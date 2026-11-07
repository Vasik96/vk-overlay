// renderer.cpp - tracks the host application's Vulkan objects and draws the ImGui
// overlay into its swapchain images right before presentation.
//
// ======================================================================================
// HOW THE OVERLAY IS COMPOSITED (the part that must be exactly right)
// ======================================================================================
// The application calls  vkQueuePresentKHR(queue, {waitSemaphores W, swapchain S, image i}).
// W are binary semaphores signalled by the app's last rendering submit; presenting
// without waiting on them would show a half-rendered frame. We therefore:
//
//   1. vkQueueSubmit(queue, wait = W (stage COLOR_ATTACHMENT_OUTPUT),
//                           cmd  = [begin RP (loadOp LOAD) ; ImGui draw ; end RP],
//                           signal = ourSemaphore[i])
//   2. call the real vkQueuePresentKHR with wait = { ourSemaphore[i] } only.
//
// W is consumed by *our* submit, ourSemaphore by the present - every binary semaphore
// still has exactly one signal and one wait, which is what validation layers check.
//
// The render pass declares initialLayout = finalLayout = PRESENT_SRC_KHR (that is the
// layout the app leaves the image in) and loadOp = LOAD so the app's pixels are kept.
//
// Resources are indexed two ways, deliberately:
//   * semaphore + framebuffer  by *swapchain image index* - an image can only be
//     re-acquired after its previous present finished, so its semaphore is free again.
//   * command buffer + fence   by a *frame ring* (our own counter). The fence tells us
//     when the GPU is done with that slot, so we never re-record a command buffer, or
//     overwrite ImGui's per-frame vertex/index buffers, while the GPU still reads them.
// ======================================================================================
#include "renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "input.hpp"
#include "log.hpp"
#include "overlay_ui.hpp"
#include "process_filter.hpp"

namespace vko {

LoaderFns g_loader;

namespace renderer {
namespace {

// -------------------------------------------------------------------------------------
// Device-level function table, resolved with the real vkGetDeviceProcAddr. Calling
// through these pointers (instead of link-time vk* symbols) means we can never be
// re-routed into our own hooks.
// -------------------------------------------------------------------------------------
#define VKO_DEVICE_FNS(X)                                                                  \
    X(GetDeviceQueue) X(QueueSubmit) X(DeviceWaitIdle) X(CreateRenderPass)                 \
    X(DestroyRenderPass) X(CreateImageView) X(DestroyImageView) X(CreateFramebuffer)       \
    X(DestroyFramebuffer) X(CreateCommandPool) X(DestroyCommandPool)                       \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer)                  \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass)    \
    X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence)                  \
    X(WaitForFences) X(ResetFences) X(CreateDescriptorPool) X(DestroyDescriptorPool)       \
    X(GetSwapchainImagesKHR)

struct DeviceFns {
#define X(n) PFN_vk##n n = nullptr;
    VKO_DEVICE_FNS(X)
#undef X
    bool load(VkDevice d) {
        bool ok = true;
#define X(n)                                                                               \
    n = reinterpret_cast<PFN_vk##n>(g_loader.gdpa(d, "vk" #n));                            \
    if (!n) { VKO_LOG("missing device function vk" #n); ok = false; }
        VKO_DEVICE_FNS(X)
#undef X
        return ok;
    }
};

struct InstanceCtx {
    VkInstance handle = VK_NULL_HANDLE;
    uint32_t api = VK_API_VERSION_1_0;
    PFN_vkEnumeratePhysicalDevices enum_pd = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_props = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qf = nullptr;
};

struct FrameSlot {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;  // created SIGNALED so the first wait returns at once
};

struct SwapchainCtx {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    bool usage_ok = true;  // swapchain was created with COLOR_ATTACHMENT usage
    bool built = false;
    bool broken = false;
    uint32_t family = 0;
    VkRenderPass rp = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;
    std::vector<VkSemaphore> done_sems;  // indexed by image index
    std::vector<FrameSlot> ring;         // indexed by frame counter
    uint64_t counter = 0;
};

struct DeviceCtx {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    InstanceCtx* inst = nullptr;
    DeviceFns fn;
    std::unordered_map<VkQueue, uint32_t> queue_family;
    std::vector<VkQueueFlags> family_flags;
    std::unordered_map<uint32_t, VkCommandPool> pools;  // per queue family
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    std::string gpu_name = "unknown";
    std::unordered_map<VkSwapchainKHR, SwapchainCtx> swapchains;
};

struct State {
    std::mutex mu;
    std::vector<std::unique_ptr<InstanceCtx>> instances;
    std::unordered_map<VkDevice, std::unique_ptr<DeviceCtx>> devices;
    std::unordered_map<VkSwapchainKHR, DeviceCtx*> sc_owner;

    // The overlay lives on exactly one swapchain at a time (the first one that presents).
    DeviceCtx* active_dev = nullptr;
    VkSwapchainKHR active_sc = VK_NULL_HANDLE;

    bool imgui_ctx = false;
    bool backend_up = false;
    ui::Telemetry telem;
    ui::Settings settings;   // overlay mode / hotkeys are owned by input::update() (input.cpp)
    bool warned_unknown = false, warned_queue = false, warned_extra_sc = false;
    std::vector<VkPipelineStageFlags> wait_stages;  // reused every frame: no per-frame malloc
};

// Intentionally leaked: static destructors run at an unpredictable point during process
// exit (possibly while another thread is still inside a hook). A leaked singleton can
// never be observed in a half-destroyed state.
State& S() {
    static State* s = new State;
    return *s;
}

std::atomic<bool> g_disabled{false};

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ teardown helpers --

void wait_ring(DeviceCtx& d, SwapchainCtx& sc, uint64_t timeout_ns) {
    VkFence fences[16];
    uint32_t n = 0;
    for (auto& f : sc.ring)
        if (f.fence && n < 16) fences[n++] = f.fence;
    if (n) d.fn.WaitForFences(d.dev, n, fences, VK_TRUE, timeout_ns);
}

void destroy_swapchain_resources(DeviceCtx& d, SwapchainCtx& sc) {
    auto& f = d.fn;
    wait_ring(d, sc, 2'000'000'000ull);  // GPU must be done with our command buffers
    VkCommandPool pool = VK_NULL_HANDLE;
    if (auto it = d.pools.find(sc.family); it != d.pools.end()) pool = it->second;
    for (auto& s : sc.ring) {
        if (s.cb && pool) f.FreeCommandBuffers(d.dev, pool, 1, &s.cb);
        if (s.fence) f.DestroyFence(d.dev, s.fence, nullptr);
    }
    for (auto s : sc.done_sems) if (s) f.DestroySemaphore(d.dev, s, nullptr);
    for (auto fb : sc.fbs) if (fb) f.DestroyFramebuffer(d.dev, fb, nullptr);
    for (auto v : sc.views) if (v) f.DestroyImageView(d.dev, v, nullptr);
    if (sc.rp) f.DestroyRenderPass(d.dev, sc.rp, nullptr);
    sc.ring.clear(); sc.done_sems.clear(); sc.fbs.clear(); sc.views.clear(); sc.images.clear();
    sc.rp = VK_NULL_HANDLE;
    sc.built = false;
}

void backend_shutdown(State& s) {
    if (s.backend_up) {
        ImGui_ImplVulkan_Shutdown();  // frees pipeline, font texture, vertex buffers, ...
        s.backend_up = false;
    }
}

void deactivate(State& s) {
    s.active_dev = nullptr;
    s.active_sc = VK_NULL_HANDLE;
    input::release_capture();  // never leave the user's pointer grabbed or the game muted
}

void destroy_device(State& s, DeviceCtx& d) {
    d.fn.DeviceWaitIdle(d.dev);
    if (s.active_dev == &d) {
        backend_shutdown(s);
        deactivate(s);
    }
    for (auto& [h, sc] : d.swapchains) {
        destroy_swapchain_resources(d, sc);
        s.sc_owner.erase(h);
    }
    d.swapchains.clear();
    if (d.dpool) d.fn.DestroyDescriptorPool(d.dev, d.dpool, nullptr);
    for (auto& [fam, pool] : d.pools) d.fn.DestroyCommandPool(d.dev, pool, nullptr);
    d.pools.clear();
}

// ------------------------------------------------------------------ build helpers -----

VkCommandPool get_pool(DeviceCtx& d, uint32_t family) {
    if (auto it = d.pools.find(family); it != d.pools.end()) return it->second;
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;  // we re-record per frame
    ci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (d.fn.CreateCommandPool(d.dev, &ci, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    d.pools[family] = pool;
    return pool;
}

// Our own descriptor pool for ImGui (font atlas + any user textures). FREE_DESCRIPTOR_SET
// is mandatory: the backend frees/recreates the font descriptor set.
VkDescriptorPool get_dpool(DeviceCtx& d) {
    if (d.dpool) return d.dpool;
    VkDescriptorPoolSize sz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets = 64;
    ci.poolSizeCount = 1;
    ci.pPoolSizes = &sz;
    if (d.fn.CreateDescriptorPool(d.dev, &ci, nullptr, &d.dpool) != VK_SUCCESS) d.dpool = VK_NULL_HANDLE;
    return d.dpool;
}

bool build_swapchain(DeviceCtx& d, SwapchainCtx& sc, uint32_t family) {
    auto& f = d.fn;
    VkCommandPool pool = get_pool(d, family);
    if (!pool) return false;
    sc.family = family;

    uint32_t n = 0;
    if (f.GetSwapchainImagesKHR(d.dev, sc.handle, &n, nullptr) != VK_SUCCESS || n == 0) return false;
    sc.images.resize(n);
    if (f.GetSwapchainImagesKHR(d.dev, sc.handle, &n, sc.images.data()) < VK_SUCCESS) return false;
    sc.images.resize(n);

    // --- render pass: keep the app's pixels, draw on top, hand the image back as-is ----
    VkAttachmentDescription att{};
    att.format = sc.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1; rpci.pAttachments = &att;
    rpci.subpassCount = 1;    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    if (f.CreateRenderPass(d.dev, &rpci, nullptr, &sc.rp) != VK_SUCCESS) return false;

    // --- per image: view, framebuffer, "overlay finished" semaphore ---------------------
    sc.views.assign(n, VK_NULL_HANDLE);
    sc.fbs.assign(n, VK_NULL_HANDLE);
    sc.done_sems.assign(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = sc.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sc.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (f.CreateImageView(d.dev, &vi, nullptr, &sc.views[i]) != VK_SUCCESS) return false;

        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = sc.rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &sc.views[i];
        fi.width = sc.extent.width;
        fi.height = sc.extent.height;
        fi.layers = 1;
        if (f.CreateFramebuffer(d.dev, &fi, nullptr, &sc.fbs[i]) != VK_SUCCESS) return false;

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (f.CreateSemaphore(d.dev, &si, nullptr, &sc.done_sems[i]) != VK_SUCCESS) return false;
    }

    // --- frame ring ---------------------------------------------------------------------
    const uint32_t ring_n = std::clamp<uint32_t>(n, 3, 8);
    std::vector<VkCommandBuffer> cbs(ring_n);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = ring_n;
    if (f.AllocateCommandBuffers(d.dev, &ai, cbs.data()) != VK_SUCCESS) return false;
    sc.ring.assign(ring_n, {});
    for (uint32_t i = 0; i < ring_n; ++i) {
        sc.ring[i].cb = cbs[i];
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (f.CreateFence(d.dev, &fci, nullptr, &sc.ring[i].fence) != VK_SUCCESS) return false;
    }
    sc.built = true;
    VKO_VLOG("built overlay resources: %ux%u fmt=%d images=%u ring=%u", sc.extent.width,
             sc.extent.height, static_cast<int>(sc.format), n, ring_n);
    return true;
}

PFN_vkVoidFunction imgui_loader(const char* name, void* user) {
    return g_loader.gipa(static_cast<VkInstance>(user), name);
}

bool init_backend(State& s, DeviceCtx& d, SwapchainCtx& sc, VkQueue queue, uint32_t family) {
    if (!s.imgui_ctx) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // never write imgui.ini into the game's directory
        io.LogFilename = nullptr;
        io.BackendPlatformName = "vk_overlay (Wayland hook / polled X11)";
        ImGui::StyleColorsClassic();
        ImGuiStyle& st = ImGui::GetStyle();   // square corners everywhere
        st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = 0.0f;
        st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0.0f;
        s.imgui_ctx = true;
    }
    // The backend was compiled with IMGUI_IMPL_VULKAN_NO_PROTOTYPES, so it needs us to
    // hand it every Vulkan function pointer before Init().
    if (!ImGui_ImplVulkan_LoadFunctions(d.inst->api, imgui_loader, d.inst->handle)) {
        VKO_LOG("ImGui_ImplVulkan_LoadFunctions failed");
        return false;
    }
    VkDescriptorPool dpool = get_dpool(d);
    if (!dpool) return false;

    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = d.inst->api;
    ii.Instance = d.inst->handle;
    ii.PhysicalDevice = d.phys;
    ii.Device = d.dev;
    ii.QueueFamily = family;
    ii.Queue = queue;
    ii.DescriptorPool = dpool;
    ii.RenderPass = sc.rp;
    ii.MinImageCount = 2;
    // ImGui keeps one vertex/index buffer set per "image count"; matching it to our frame
    // ring size means its buffer N is only rewritten after our fence N has signalled.
    ii.ImageCount = static_cast<uint32_t>(sc.ring.size());
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        VKO_LOG("ImGui_ImplVulkan_Init failed");
        return false;
    }
    s.backend_up = true;
    return true;
}

}  // namespace

// ======================================================================================
// lifecycle events
// ======================================================================================

void on_instance_created(VkInstance instance, const VkInstanceCreateInfo* ci) {
    auto ic = std::make_unique<InstanceCtx>();
    ic->handle = instance;
    if (ci && ci->pApplicationInfo && ci->pApplicationInfo->apiVersion)
        ic->api = ci->pApplicationInfo->apiVersion;
    ic->enum_pd = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(g_loader.gipa(instance, "vkEnumeratePhysicalDevices"));
    ic->get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(g_loader.gipa(instance, "vkGetPhysicalDeviceProperties"));
    ic->get_qf = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(g_loader.gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!ic->enum_pd || !ic->get_props || !ic->get_qf) return;
    std::lock_guard lk(S().mu);
    S().instances.push_back(std::move(ic));
}

// Instances come and go (DXVK/Wine create short-lived ones). A destroyed instance MUST leave the
// list: on_device_created() calls vkEnumeratePhysicalDevices on every tracked instance to find a
// physical device's owner, and calling that on a dead handle is undefined behaviour.
void on_instance_destroying(VkInstance instance) {
    State& s = S();
    std::lock_guard lk(s.mu);
    auto& v = s.instances;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if ((*it)->handle != instance) continue;
        for (auto& [h, d] : s.devices)
            if (d->inst == it->get()) { VKO_VLOG("instance destroyed while a tracked device still uses it"); return; }
        v.erase(it);
        return;
    }
}

void on_device_created(VkPhysicalDevice phys, VkDevice device, const VkDeviceCreateInfo* ci) {
    auto d = std::make_unique<DeviceCtx>();
    d->dev = device;
    d->phys = phys;

    std::lock_guard lk(S().mu);

    // Which instance does this physical device belong to? VkPhysicalDevice has no
    // "get owner" query, so enumerate every known instance and compare handles.
    for (auto& ic : S().instances) {
        uint32_t n = 0;
        ic->enum_pd(ic->handle, &n, nullptr);
        std::vector<VkPhysicalDevice> pds(n);
        ic->enum_pd(ic->handle, &n, pds.data());
        if (std::find(pds.begin(), pds.begin() + n, phys) != pds.begin() + n) { d->inst = ic.get(); break; }
    }
    if (!d->inst) { VKO_VLOG("device %p: owning instance unknown (created before injection?)", (void*)device); return; }
    if (!d->fn.load(device)) return;

    VkPhysicalDeviceProperties props{};
    d->inst->get_props(phys, &props);
    d->gpu_name = props.deviceName;

    uint32_t nq = 0;
    d->inst->get_qf(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    d->inst->get_qf(phys, &nq, qf.data());
    for (auto& p : qf) d->family_flags.push_back(p.queueFlags);

    // Map every queue handle to its family. vkGetDeviceQueue on the same (family, index)
    // always returns the same handle, so asking now yields exactly the handles the app
    // will later pass to vkQueuePresentKHR - without hooking vkGetDeviceQueue itself.
    if (ci) {
        for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
            const auto& q = ci->pQueueCreateInfos[i];
            if (q.flags != 0) continue;  // protected queues need vkGetDeviceQueue2
            for (uint32_t j = 0; j < q.queueCount; ++j) {
                VkQueue h = VK_NULL_HANDLE;
                d->fn.GetDeviceQueue(device, q.queueFamilyIndex, j, &h);
                if (h) d->queue_family[h] = q.queueFamilyIndex;
            }
        }
    }
    VKO_VLOG("device tracked: %s (%zu queues)", d->gpu_name.c_str(), d->queue_family.size());
    S().devices[device] = std::move(d);
}

void on_swapchain_created(VkDevice device, VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR& ci,
                          bool color_attachment_ok) {
    std::lock_guard lk(S().mu);
    auto it = S().devices.find(device);
    if (it == S().devices.end()) return;
    DeviceCtx& d = *it->second;
    SwapchainCtx s;
    s.handle = sc;
    s.format = ci.imageFormat;
    s.extent = ci.imageExtent;
    s.usage_ok = color_attachment_ok;
    d.swapchains[sc] = std::move(s);
    S().sc_owner[sc] = &d;
}

void on_swapchain_destroying(VkDevice device, VkSwapchainKHR sc) {
    State& s = S();
    std::lock_guard lk(s.mu);
    auto o = s.sc_owner.find(sc);
    if (o == s.sc_owner.end()) return;
    DeviceCtx& d = *o->second;
    auto it = d.swapchains.find(sc);
    if (it != d.swapchains.end()) {
        if (s.active_sc == sc) {
            wait_ring(d, it->second, 2'000'000'000ull);
            backend_shutdown(s);  // pipeline was built against this swapchain's render pass
            deactivate(s);
        }
        destroy_swapchain_resources(d, it->second);
        d.swapchains.erase(it);
    }
    s.sc_owner.erase(o);
}

void on_device_destroying(VkDevice device) {
    State& s = S();
    std::lock_guard lk(s.mu);
    auto it = s.devices.find(device);
    if (it == s.devices.end()) return;
    destroy_device(s, *it->second);
    s.devices.erase(it);
}

// ======================================================================================
// the per-frame path
// ======================================================================================

bool on_present(VkQueue queue, const VkPresentInfoKHR& pi, VkSemaphore& out_wait) {
    if (g_disabled.load(std::memory_order_relaxed)) return false;

    // The process name is final by the time a frame is presented (Wine sets it late), so this is
    // where VKOVERLAY_PROCESS is really enforced. Decided once per process.
    static std::atomic<int> proc_state{0};  // 0 = undecided, 1 = allowed, 2 = filtered out
    int ps = proc_state.load(std::memory_order_acquire);
    if (ps == 0) {
        char comm[32];
        current_comm(comm);
        ps = process_allowed(/*at_load=*/false) ? 1 : 2;
        proc_state.store(ps, std::memory_order_release);
        VKO_VLOG("process '%s' presents Vulkan frames: overlay %s", comm, ps == 1 ? "enabled" : "disabled by VKOVERLAY_PROCESS");
    }
    if (ps == 2) return false;

    State& s = S();
    std::lock_guard lk(s.mu);

    // ---- 1. which of the presented swapchains do we draw on? ---------------------------
    DeviceCtx* d = nullptr;
    SwapchainCtx* sc = nullptr;
    uint32_t image_index = 0;
    for (uint32_t i = 0; i < pi.swapchainCount; ++i) {
        auto o = s.sc_owner.find(pi.pSwapchains[i]);
        if (o == s.sc_owner.end()) continue;
        if (s.active_sc != VK_NULL_HANDLE && pi.pSwapchains[i] != s.active_sc) {
            if (!s.warned_extra_sc) { s.warned_extra_sc = true; VKO_VLOG("ignoring additional swapchain"); }
            continue;
        }
        d = o->second;
        sc = &d->swapchains[pi.pSwapchains[i]];
        image_index = pi.pImageIndices[i];
        break;
    }
    if (!sc) {
        if (!s.warned_unknown) {
            s.warned_unknown = true;
            VKO_VLOG("present on a swapchain we did not see being created - was the library "
                     "injected after vkCreateSwapchainKHR? Start the app with LD_PRELOAD instead.");
        }
        return false;
    }
    if (!sc->usage_ok || sc->broken || sc->extent.width == 0 || sc->extent.height == 0) return false;

    // ---- 2. does this queue support graphics (needed for a render pass)? ---------------
    auto qf = d->queue_family.find(queue);
    if (qf == d->queue_family.end() || qf->second >= d->family_flags.size() ||
        !(d->family_flags[qf->second] & VK_QUEUE_GRAPHICS_BIT)) {
        if (!s.warned_queue) { s.warned_queue = true; VKO_VLOG("present queue unknown or without graphics support"); }
        return false;
    }
    const uint32_t family = qf->second;

    // ---- 3. telemetry + input (always, even while the overlay is hidden) ---------------
    const double now = now_seconds();
    s.telem.on_frame(now);
    s.telem.sample_system(now);

    // One call: polls the backend (rate-limited inside), applies the hotkey policy, and
    // switches game-input capture when the mode changes. The renderer only reacts to the result.
    const input::Frame in = input::update(static_cast<float>(sc->extent.width), static_cast<float>(sc->extent.height));
    if (in.mode == input::Mode::Hidden) return false;
    const bool settings_mode = in.mode == input::Mode::Settings;

    // ---- 4. lazy construction of GPU resources -----------------------------------------
    if (!sc->built) {
        if (!build_swapchain(*d, *sc, family)) {
            VKO_LOG("failed to build overlay resources - overlay disabled for this swapchain");
            sc->broken = true;
            destroy_swapchain_resources(*d, *sc);
            return false;
        }
    }
    if (image_index >= sc->images.size()) return false;
    if (s.active_sc == VK_NULL_HANDLE) {
        if (!init_backend(s, *d, *sc, queue, family)) {
            sc->broken = true;
            backend_shutdown(s);
            destroy_swapchain_resources(*d, *sc);
            return false;
        }
        s.active_dev = d;
        s.active_sc = sc->handle;
        VKO_LOG("overlay active on %s (%ux%u)", d->gpu_name.c_str(), sc->extent.width, sc->extent.height);
    }

    // ---- 5. frame ring: wait until the GPU finished the *previous* use of this slot ----
    FrameSlot& slot = sc->ring[sc->counter % sc->ring.size()];
    if (d->fn.WaitForFences(d->dev, 1, &slot.fence, VK_TRUE, 500'000'000ull) != VK_SUCCESS)
        return false;  // GPU stalled / device lost: skip the overlay rather than hang the app
    ++sc->counter;

    // ---- 6. build the ImGui frame ------------------------------------------------------
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(sc->extent.width), static_cast<float>(sc->extent.height));
    io.DeltaTime = static_cast<float>(std::max(s.telem.last_frame_dt(), 1.0 / 1000.0));
    const bool live = settings_mode && in.input_available;
    io.AddMousePosEvent(live ? in.mouse_x : -FLT_MAX, live ? in.mouse_y : -FLT_MAX);
    for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, live && in.button[b]);
    if (live && in.wheel != 0.0f) io.AddMouseWheelEvent(0.0f, in.wheel);
    io.MouseDrawCursor = settings_mode;  // the game's cursor is hidden/locked: draw our own

    ImGui_ImplVulkan_NewFrame();  // lazily uploads the font atlas on first call
    ImGui::NewFrame();
    ui::SceneInfo si;
    si.gpu_name = d->gpu_name.c_str();
    si.vk_api_version = d->inst->api;
    si.width = sc->extent.width;
    si.height = sc->extent.height;
    si.format = static_cast<int>(sc->format);
    si.swapchain_images = static_cast<uint32_t>(sc->images.size());
    ui::KeyHints hints;
    hints.cycle = input::cycle_key_name();
    const bool closed_by_x = ui::draw(s.telem, si, settings_mode, s.settings, hints);
    ImGui::Render();
    if (closed_by_x) input::release_capture();   // X button: back to HUD, input returned to the game

    // ---- 7. record -----------------------------------------------------------------------
    auto& f = d->fn;
    f.ResetCommandBuffer(slot.cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (f.BeginCommandBuffer(slot.cb, &bi) != VK_SUCCESS) return false;
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = sc->rp;
    rb.framebuffer = sc->fbs[image_index];
    rb.renderArea = {{0, 0}, sc->extent};
    f.CmdBeginRenderPass(slot.cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), slot.cb);
    f.CmdEndRenderPass(slot.cb);
    if (f.EndCommandBuffer(slot.cb) != VK_SUCCESS) return false;

    // ---- 8. submit: wait for the app's semaphores, signal ours, fence the slot ----------
    s.wait_stages.assign(pi.waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.waitSemaphoreCount = pi.waitSemaphoreCount;
    sub.pWaitSemaphores = pi.pWaitSemaphores;
    sub.pWaitDstStageMask = s.wait_stages.data();
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &slot.cb;
    sub.signalSemaphoreCount = 1;
    sub.pSignalSemaphores = &sc->done_sems[image_index];

    f.ResetFences(d->dev, 1, &slot.fence);  // only now: a reset fence MUST be followed by a submit
    const VkResult r = f.QueueSubmit(queue, 1, &sub, slot.fence);
    if (r != VK_SUCCESS) {
        // The fence is now unsignalled with nothing pending; any later wait on it would
        // hang. Disable permanently - the app keeps running, just without the overlay.
        VKO_LOG("vkQueueSubmit failed (%d) - disabling overlay", static_cast<int>(r));
        g_disabled.store(true);
        return false;
    }
    out_wait = sc->done_sems[image_index];
    return true;
}

void disable() { g_disabled.store(true); }

void shutdown(bool gpu_cleanup) {
    g_disabled.store(true);
    State& s = S();
    std::lock_guard lk(s.mu);
    if (!gpu_cleanup) return;  // process exit: driver state may already be gone
    for (auto& [h, d] : s.devices) destroy_device(s, *d);
    s.devices.clear();
    s.sc_owner.clear();
    backend_shutdown(s);
    if (s.imgui_ctx) { ImGui::DestroyContext(); s.imgui_ctx = false; }
    input::shutdown();
}

}  // namespace renderer
}  // namespace vko
