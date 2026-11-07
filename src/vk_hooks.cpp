// vk_hooks.cpp - the detours themselves.
//
// Hooked functions and why:
//   vkCreateInstance      learn the VkInstance + requested API version
//   vkCreateDevice        learn VkDevice / VkPhysicalDevice / queues, then hook the
//                         device-level functions at the addresses *this* device resolves
//   vkCreateSwapchainKHR  learn format + extent; add COLOR_ATTACHMENT usage so we can draw
//   vkQueuePresentKHR     render the overlay, then present
//   vkDestroySwapchainKHR free our per-swapchain objects BEFORE the swapchain goes away
//   vkDestroyDevice       free everything on the device BEFORE the device goes away
#include "vk_hooks.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "hooks.hpp"
#include "log.hpp"
#include "renderer.hpp"

namespace vko {
namespace {

std::atomic<bool> g_enabled{false};   // detours do real work only while true
std::atomic<int> g_inflight{0};       // threads currently inside any detour
std::once_flag g_install_once;
bool g_installed = false;
void* g_vulkan_lib = nullptr;

// Counts a thread as "inside our code" for the lifetime of the object. The destructor
// (unload) path waits for this to reach zero before it frees trampolines.
struct InFlight {
    InFlight() { g_inflight.fetch_add(1, std::memory_order_acq_rel); }
    ~InFlight() { g_inflight.fetch_sub(1, std::memory_order_acq_rel); }
    InFlight(const InFlight&) = delete;
    InFlight& operator=(const InFlight&) = delete;
};

// Per-thread, per-function re-entrancy flag. The same call can reach us twice: once at
// the loader's trampoline and again at the ICD function it jumps to (both are hooked).
// The inner invocation must be a pure pass-through.
struct Reentry {
    explicit Reentry(bool& flag) : flag_(flag) { flag_ = true; }
    ~Reentry() { flag_ = false; }
    bool& flag_;
};

// ----------------------------------------------------------------------------- tags --

struct CreateInstanceTag {
    using Fn = PFN_vkCreateInstance;
    static VkResult handle(Fn orig, const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
        InFlight in;
        VkResult r = orig(ci, a, out);
        if (r == VK_SUCCESS && out && g_enabled.load(std::memory_order_relaxed)) {
            try { renderer::on_instance_created(*out, ci); } catch (...) { renderer::disable(); }
        }
        return r;
    }
};

struct PresentTag {
    using Fn = PFN_vkQueuePresentKHR;
    static VkResult handle(Fn orig, VkQueue q, const VkPresentInfoKHR* pi) {
        InFlight in;
        thread_local bool busy = false;
        if (busy || !pi || !g_enabled.load(std::memory_order_relaxed)) return orig(q, pi);
        Reentry guard(busy);  // stays set across orig(): covers the nested ICD-level hook

        VkSemaphore wait = VK_NULL_HANDLE;
        bool drew = false;
        // A C++ exception must never unwind into the application's (C) frames.
        try { drew = renderer::on_present(q, *pi, wait); } catch (...) { renderer::disable(); }

        if (drew) {
            // Our submit already waited on the app's semaphores; present waits on ours.
            VkPresentInfoKHR patched = *pi;
            patched.waitSemaphoreCount = 1;
            patched.pWaitSemaphores = &wait;
            return orig(q, &patched);
        }
        return orig(q, pi);
    }
};

struct CreateSwapchainTag {
    using Fn = PFN_vkCreateSwapchainKHR;
    static VkResult handle(Fn orig, VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                           const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
        InFlight in;
        thread_local bool busy = false;
        if (busy || !ci || !out || !g_enabled.load(std::memory_order_relaxed)) return orig(dev, ci, a, out);
        Reentry guard(busy);

        // Ask for COLOR_ATTACHMENT so a render pass can target the images. Most apps
        // already set it; if the surface refuses the extra bit we fall back untouched.
        VkSwapchainCreateInfoKHR patched = *ci;
        patched.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        bool usage_ok = true;
        VkResult r = orig(dev, &patched, a, out);
        if (r != VK_SUCCESS && patched.imageUsage != ci->imageUsage) {
            usage_ok = (ci->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
            patched = *ci;
            r = orig(dev, ci, a, out);
        }
        if (r == VK_SUCCESS) {
            try { renderer::on_swapchain_created(dev, *out, patched, usage_ok); } catch (...) { renderer::disable(); }
        }
        return r;
    }
};

struct DestroySwapchainTag {
    using Fn = PFN_vkDestroySwapchainKHR;
    static void handle(Fn orig, VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
        InFlight in;
        thread_local bool busy = false;
        if (!busy && sc && g_enabled.load(std::memory_order_relaxed)) {
            Reentry guard(busy);
            try { renderer::on_swapchain_destroying(dev, sc); } catch (...) { renderer::disable(); }
            orig(dev, sc, a);
            return;
        }
        orig(dev, sc, a);
    }
};

struct DestroyInstanceTag {
    using Fn = PFN_vkDestroyInstance;
    static void handle(Fn orig, VkInstance inst, const VkAllocationCallbacks* a) {
        InFlight in;
        thread_local bool busy = false;
        if (!busy && inst && g_enabled.load(std::memory_order_relaxed)) {
            Reentry guard(busy);
            try { renderer::on_instance_destroying(inst); } catch (...) { renderer::disable(); }
            orig(inst, a);
            return;
        }
        orig(inst, a);
    }
};

struct DestroyDeviceTag {
    using Fn = PFN_vkDestroyDevice;
    static void handle(Fn orig, VkDevice dev, const VkAllocationCallbacks* a) {
        InFlight in;
        thread_local bool busy = false;
        if (!busy && dev && g_enabled.load(std::memory_order_relaxed)) {
            Reentry guard(busy);
            try { renderer::on_device_destroying(dev); } catch (...) { renderer::disable(); }
            orig(dev, a);
            return;
        }
        orig(dev, a);
    }
};

using CreateInstanceHooks = HookSlots<CreateInstanceTag, PFN_vkCreateInstance>;
using PresentHooks = HookSlots<PresentTag, PFN_vkQueuePresentKHR>;
using CreateSwapchainHooks = HookSlots<CreateSwapchainTag, PFN_vkCreateSwapchainKHR>;
using DestroySwapchainHooks = HookSlots<DestroySwapchainTag, PFN_vkDestroySwapchainKHR>;
using DestroyDeviceHooks = HookSlots<DestroyDeviceTag, PFN_vkDestroyDevice>;
using DestroyInstanceHooks = HookSlots<DestroyInstanceTag, PFN_vkDestroyInstance>;

// vkCreateDevice is special: after it succeeds we hook the *device-level* functions at
// whatever addresses this particular device dispatches to.
struct CreateDeviceTag {
    using Fn = PFN_vkCreateDevice;
    static VkResult handle(Fn orig, VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                           const VkAllocationCallbacks* a, VkDevice* out) {
        InFlight in;
        VkResult r = orig(phys, ci, a, out);
        if (r != VK_SUCCESS || !out || !g_enabled.load(std::memory_order_relaxed)) return r;
        try {
            renderer::on_device_created(phys, *out, ci);
            // vkGetDeviceProcAddr(device, name) is what the loader itself hands to
            // applications; whatever it returns is a function the app may call directly.
            auto addr = [&](const char* n) { return reinterpret_cast<void*>(g_loader.gdpa(*out, n)); };
            PresentHooks::install(addr("vkQueuePresentKHR"), "vkQueuePresentKHR[dev]");
            CreateSwapchainHooks::install(addr("vkCreateSwapchainKHR"), "vkCreateSwapchainKHR[dev]");
            DestroySwapchainHooks::install(addr("vkDestroySwapchainKHR"), "vkDestroySwapchainKHR[dev]");
            DestroyDeviceHooks::install(addr("vkDestroyDevice"), "vkDestroyDevice[dev]");
        } catch (...) { renderer::disable(); }
        return r;
    }
};
using CreateDeviceHooks = HookSlots<CreateDeviceTag, PFN_vkCreateDevice>;

}  // namespace

// ================================================================================= API ==

bool install_vulkan_hooks() {
    std::call_once(g_install_once, [] {
        // Load (or find the already-loaded) Vulkan loader. RTLD_GLOBAL matches what most
        // engines do. Loading it *now*, from the constructor, means the loader's code is
        // patched before the app's first vkCreateInstance - the only moment we can still
        // observe the creation calls that give us the handles we need.
        const char* path = std::getenv("VKOVERLAY_LIBVULKAN");
        g_vulkan_lib = dlopen(path ? path : "libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!g_vulkan_lib) {
            VKO_LOG("cannot load %s: %s", path ? path : "libvulkan.so.1", dlerror());
            return;
        }
        g_loader.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_vulkan_lib, "vkGetInstanceProcAddr"));
        g_loader.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_vulkan_lib, "vkGetDeviceProcAddr"));
        if (!g_loader.gipa || !g_loader.gdpa) {
            VKO_LOG("loader exports missing vkGet*ProcAddr");
            return;
        }

        auto sym = [](const char* n) { return dlsym(g_vulkan_lib, n); };
        g_enabled.store(true, std::memory_order_release);  // before hooks go live

        bool ok = true;
        ok &= CreateInstanceHooks::install(sym("vkCreateInstance"), "vkCreateInstance");
        ok &= DestroyInstanceHooks::install(sym("vkDestroyInstance"), "vkDestroyInstance");
        ok &= CreateDeviceHooks::install(sym("vkCreateDevice"), "vkCreateDevice");
        // Loader-level trampolines for apps that call the exported symbols directly:
        ok &= PresentHooks::install(sym("vkQueuePresentKHR"), "vkQueuePresentKHR");
        ok &= CreateSwapchainHooks::install(sym("vkCreateSwapchainKHR"), "vkCreateSwapchainKHR");
        ok &= DestroySwapchainHooks::install(sym("vkDestroySwapchainKHR"), "vkDestroySwapchainKHR");
        ok &= DestroyDeviceHooks::install(sym("vkDestroyDevice"), "vkDestroyDevice");

        g_installed = true;
        VKO_LOG("hooks installed%s", ok ? "" : " (some failed)");
    });
    return g_installed;
}

void remove_vulkan_hooks(bool process_exiting) {
    if (!g_installed) return;

    // 1. Make every detour a pass-through. Threads that already entered finish normally.
    g_enabled.store(false, std::memory_order_release);
    renderer::disable();

    // 2. Wait (bounded) until no thread is executing inside a detour.
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(2);
    while (g_inflight.load(std::memory_order_acquire) > 0 && steady_clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(1));
    if (g_inflight.load() > 0) VKO_LOG("%d thread(s) still inside hooks after 2s - unhooking anyway", g_inflight.load());
    // Grace period for a thread that passed the g_enabled check just before step 1 but
    // has not yet bumped g_inflight (the window is a handful of instructions).
    std::this_thread::sleep_for(milliseconds(20));

    // 3. Restore original bytes (dlclose path only). NOT atomic against other threads on
    //    Linux - see hooks.hpp. Prefer never dlclose()ing the library.
    CreateInstanceHooks::remove_all();
    DestroyInstanceHooks::remove_all();
    CreateDeviceHooks::remove_all();
    PresentHooks::remove_all();
    CreateSwapchainHooks::remove_all();
    DestroySwapchainHooks::remove_all();
    DestroyDeviceHooks::remove_all();

    // 4. GPU objects: only when the process keeps running (library dlclose()d).
    renderer::shutdown(/*gpu_cleanup=*/!process_exiting);
    g_installed = false;
}

}  // namespace vko
