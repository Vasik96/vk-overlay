// input_wayland_hook.cpp - installs the inline hook on libwayland-client's
// wl_proxy_add_listener. Done in the library constructor (single-threaded under LD_PRELOAD,
// the only moment inline patching is race-free on Linux - see hooks.hpp).
//
// We dlopen libwayland-client ourselves (RTLD_GLOBAL) instead of waiting for the game:
// SDL3/GLFW load it lazily with dlopen+dlsym, which bypasses symbol interposition, but an
// inline patch of the function body is seen by everybody because the soname resolves to the
// same mapping when they dlopen it later.
#include <dlfcn.h>

#include <cstdlib>
#include <mutex>

#include <safetyhook.hpp>

#include "input_backend.hpp"
#include "log.hpp"

namespace vko::input {
namespace {

using GetClassFn = const char* (*)(void*);
using GetVersionFn = uint32_t (*)(void*);

safetyhook::InlineHook* g_hook = nullptr;  // leaked on purpose (see hooks.hpp)
safetyhook::InlineHook* g_dispatch_hook = nullptr;
GetClassFn g_get_class = nullptr;
GetVersionFn g_get_version = nullptr;

int real_add_listener(void* proxy, void (**impl)(void), void* data) {
    return g_hook->call<int>(proxy, impl, data);
}
int real_add_dispatcher(void* proxy, detail::Dispatcher dispatcher, const void* implementation, void* data) {
    return g_dispatch_hook->call<int>(proxy, dispatcher, implementation, data);
}

int dispatcher_detour(void* proxy, detail::Dispatcher dispatcher, const void* implementation, void* data) {
    try {
        if (proxy && g_get_class)
            return detail::wl_handle_add_dispatcher(g_get_class(proxy), proxy, dispatcher,
                                                     implementation, data, &real_add_dispatcher);
    } catch (...) {}
    return real_add_dispatcher(proxy, dispatcher, implementation, data);
}

int detour(void* proxy, void (**impl)(void), void* data) {
    // Never let an exception cross into the C library; on any failure behave as if unhooked.
    try {
        if (proxy && g_get_class && g_get_version)
            return detail::wl_handle_add_listener(g_get_class(proxy), g_get_version(proxy), proxy, impl, data,
                                                  &real_add_listener);
    } catch (...) {
    }
    return real_add_listener(proxy, impl, data);
}

}  // namespace

bool install_wayland_hook() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        if (!std::getenv("WAYLAND_DISPLAY") || std::getenv("VKOVERLAY_NO_WAYLAND")) return;
        void* lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (!lib) { VKO_VLOG("libwayland-client not available: %s", dlerror()); return; }
        void* add = dlsym(lib, "wl_proxy_add_listener");
        void* add_dispatcher = dlsym(lib, "wl_proxy_add_dispatcher");
        g_get_class = reinterpret_cast<GetClassFn>(dlsym(lib, "wl_proxy_get_class"));
        g_get_version = reinterpret_cast<GetVersionFn>(dlsym(lib, "wl_proxy_get_version"));
        if (!add || !g_get_class || !g_get_version) {
            VKO_LOG("libwayland-client: missing required listener symbols");
            return;
        }
        auto r = safetyhook::InlineHook::create(add, reinterpret_cast<void*>(&detour));
        if (!r) { VKO_LOG("failed to hook wl_proxy_add_listener"); return; }
        g_hook = new safetyhook::InlineHook(std::move(*r));

        // Dispatcher support is optional: older libwayland builds may not export
        // wl_proxy_add_dispatcher. Do not let that disable the listener hook.
        if (add_dispatcher) {
            auto rd = safetyhook::InlineHook::create(add_dispatcher,
                                                     reinterpret_cast<void*>(&dispatcher_detour));
            if (rd) {
                g_dispatch_hook = new safetyhook::InlineHook(std::move(*rd));
                VKO_VLOG("hooked wl_proxy_add_dispatcher at %p", add_dispatcher);
            } else {
                VKO_LOG("failed to hook wl_proxy_add_dispatcher; listener path remains active");
            }
        } else {
            VKO_VLOG("wl_proxy_add_dispatcher is not exported; listener path remains active");
        }
        ok = true;
        VKO_VLOG("hooked wl_proxy_add_listener at %p", add);
    });
    return ok;
}

}  // namespace vko::input
