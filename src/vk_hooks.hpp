// vk_hooks.hpp - installs / removes the Vulkan inline hooks.
#pragma once

namespace vko {

// Resolves the real Vulkan loader, then inline-hooks the instance/device creation and
// swapchain/present entry points. Idempotent; thread-safe.
bool install_vulkan_hooks();

// Disables the overlay, waits for threads still inside our detours, restores the original
// bytes of every hooked function and (if `process_exiting` is false) frees GPU resources.
void remove_vulkan_hooks(bool process_exiting);

}  // namespace vko
