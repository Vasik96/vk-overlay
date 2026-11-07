// renderer.hpp - Vulkan-side state tracking + ImGui rendering.
//
// The hook layer (vk_hooks.cpp) forwards a handful of lifecycle events here:
//
//     vkCreateInstance      -> on_instance_created      (remember instance + API version)
//     vkCreateDevice        -> on_device_created        (load fn table, enumerate queues)
//     vkCreateSwapchainKHR  -> on_swapchain_created     (remember format / extent)
//     vkQueuePresentKHR     -> on_present               (record + submit the overlay)
//     vkDestroySwapchainKHR -> on_swapchain_destroying  (free per-swapchain resources)
//     vkDestroyDevice       -> on_device_destroying     (free everything on that device)
//
// All functions are thread-safe (one internal mutex). None of them calls back into a
// hooked Vulkan entry point while holding that mutex, so they cannot deadlock against
// the hook layer.
#pragma once

#include <vulkan/vulkan.h>

namespace vko {

// Real (un-hooked) loader entry points, resolved with dlsym from libvulkan.so.1.
struct LoaderFns {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
};
extern LoaderFns g_loader;

namespace renderer {

void on_instance_created(VkInstance instance, const VkInstanceCreateInfo* ci);
void on_instance_destroying(VkInstance instance);
void on_device_created(VkPhysicalDevice phys, VkDevice device, const VkDeviceCreateInfo* ci);
void on_device_destroying(VkDevice device);
void on_swapchain_created(VkDevice device, VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR& ci,
                          bool color_attachment_ok);
void on_swapchain_destroying(VkDevice device, VkSwapchainKHR sc);

// Called from inside vkQueuePresentKHR *before* the real present. If it returns true an
// overlay was rendered and the real present must wait on `out_wait` (a semaphore our
// submit signals) INSTEAD of on the application's original wait semaphores - our submit
// already waited on those.
bool on_present(VkQueue queue, const VkPresentInfoKHR& info, VkSemaphore& out_wait);

// Permanently disables rendering (used after an exception or an unrecoverable error).
void disable();

// gpu_cleanup == true : wait for the GPU and destroy every Vulkan object we created
//                       (library is being dlclose()d while the app keeps running).
// gpu_cleanup == false: process is exiting; the driver may already be torn down, so
//                       touch nothing and let the kernel reclaim it.
void shutdown(bool gpu_cleanup);

}  // namespace renderer
}  // namespace vko
