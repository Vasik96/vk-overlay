// input_backend.hpp - what an input backend is allowed to do: report FACTS.
//
// A backend answers "which watched keys are down / were pressed since the last poll, where
// is the pointer, which buttons are held, how far was the wheel turned" and can be asked to
// stop the game from seeing input ("capture"). It never decides what any of that means -
// no hotkey constants, no mode logic. That lives in input.cpp (the controller).
//
//   Wayland (native clients, e.g. SDL3 / GLFW 3.4 with the Wayland backend)
//       Wayland deliberately has NO global input state: a client only ever receives events
//       for its own focused surface, and cannot poll or grab. So instead of polling we sit
//       *inside the game's own libwayland-client*: wl_proxy_add_listener() is inline-hooked
//       and, whenever the game registers a listener on a wl_keyboard / wl_pointer /
//       zwp_relative_pointer_v1, we substitute a copy of its listener table whose key /
//       button / motion / axis entries are our wrappers. Wrappers (1) observe the events,
//       (2) forward them to the game unless the overlay currently owns the input
//       ("capture" mode). Only events the compositor already delivers to the game are used,
//       so no extra permissions, no evdev, no protocol extensions and no
//       compositor-specific code are needed.
//       Both wl_proxy_add_listener and wl_proxy_add_dispatcher registration paths are
//       intercepted. The library must be loaded before the game registers proxies/listeners
//       (LD_PRELOAD); unsupported protocol events are passed through unchanged.
//
//   X11 / XWayland: polling of a private X connection (see input_x11.cpp).
#pragma once

#include <cstdint>

#include "input_keys.hpp"

namespace vko::input {

struct RawInput {
    bool available = false;            // this backend delivers input at all
    KeyMask down = 0;                  // watched keys currently held
    KeyMask pressed = 0;               // watched keys pressed since the previous poll (latched edge)
    float mouse_x = 0, mouse_y = 0;    // pointer in swapchain pixels (only meaningful while capturing)
    bool  button[3] = {false, false, false};  // ImGui order: left, right, middle
    float wheel = 0;                   // scroll accumulated since the previous poll, ImGui units
    const char* backend = "none";
};

// `capturing` tells the backend whether the overlay currently owns the pointer, so it can
// track/centre the cursor and report wheel movement only then.
RawInput poll_x11(bool capturing);
void set_capture_x11(bool on);
void shutdown_x11();

bool install_wayland_hook();   // idempotent; call from the library constructor
bool wayland_active();         // a wl_keyboard listener/dispatcher has been observed
RawInput poll_wayland(bool capturing, float width, float height);
void set_capture_wayland(bool on);

namespace detail {  // exposed for the unit/integration tests
using RealAddListener = int (*)(void* proxy, void (**impl)(void), void* data);
// Wayland's dispatcher ABI. message/args are opaque here to avoid requiring wayland headers.
using Dispatcher = int (*)(const void* implementation, void* target, uint32_t opcode,
                           const void* message, void* args);
using RealAddDispatcher = int (*)(void* proxy, Dispatcher dispatcher,
                                  const void* implementation, void* data);
// Core of the wl_proxy_add_listener detour. `cls` is wl_proxy_get_class(proxy).
int wl_handle_add_listener(const char* cls, uint32_t version, void* proxy, void (**impl)(void),
                           void* data, RealAddListener real);
int wl_handle_add_dispatcher(const char* cls, void* proxy, Dispatcher dispatcher,
                             const void* implementation, void* data, RealAddDispatcher real);
void wl_reset_for_test();
}  // namespace detail

}  // namespace vko::input
