// main.cpp - shared-object entry points.
//
// LOADING MODEL
// -------------
//   LD_PRELOAD=/path/to/libvk_overlay.so ./vulkan_app
//
// ld.so maps the library into the process *before* main() and runs every
// __attribute__((constructor)) while the process is still single-threaded. That is the
// only moment at which installing inline hooks cannot race with the application's own
// threads, and it is early enough to see the app's vkCreateInstance/vkCreateDevice calls.
//
// Injecting into an already-running process (gdb/ptrace + dlopen) executes the same
// constructor, but cannot recover VkDevice/VkQueue/swapchain handles that were created
// earlier - see STABILITY.md ("late injection").

// late injection - gave up, ignore any mentions of it, delete it if you want to, it isnt necessary.


#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.hpp"
#include "input.hpp"
#include "process_filter.hpp"
#include "vk_hooks.hpp"

// Registered with a NULL dso handle on purpose. std::atexit() from inside a shared object
// is tied to that object and would *also* fire on dlclose(), which is exactly the case we
// need to distinguish. A NULL-dso __cxa_atexit entry runs only at real process exit.
extern "C" int __cxa_atexit(void (*)(void*), void*, void*);

namespace {

// Set by an exit-time callback. Lets the destructor tell "process is terminating"
// (do NOT touch the GPU: the driver may already be torn down) from "somebody dlclose()d
// us" (clean up everything, the application keeps running).
std::atomic<bool> g_process_exiting{false};

void on_process_exit(void*) { g_process_exiting.store(true, std::memory_order_release); }

}  // namespace

__attribute__((constructor)) static void vko_load() {
    if (!vko::process_allowed(/*at_load=*/true)) return;
    if (vko::verbose_enabled()) {
        char comm[32];
        vko::current_comm(comm);
        VKO_VLOG("loaded into '%s' (pid %d)", comm, static_cast<int>(::getpid()));
    }
    __cxa_atexit(on_process_exit, nullptr, nullptr);

    // Never let a failure escape: a throwing constructor of an LD_PRELOAD'ed library
    // would take the whole application down with it.
    try {
        vko::install_vulkan_hooks();
        vko::input::install_wayland_hook();  // before the game registers Wayland input handlers
    } catch (...) {
        VKO_LOG("initialisation failed; overlay inactive");
    }
}

__attribute__((destructor)) static void vko_unload() {
    try {
        vko::remove_vulkan_hooks(g_process_exiting.load(std::memory_order_acquire));
    } catch (...) {
    }
}
