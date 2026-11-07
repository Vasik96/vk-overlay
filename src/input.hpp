// input.hpp - the unified input controller (public API used by the renderer).
//
// Layering:
//     backend (X11 poll / Wayland hook)   facts only: keys down, keys pressed, pointer, wheel
//                  |                      (input_backend.hpp, key codes via input_keys.hpp)
//     controller  (input.cpp)             ALL meaning: hotkey bindings, the overlay mode cycle,
//                  |                      when the game is cut off from input
//     renderer / UI                       asks update() once per frame, draws accordingly
//
// Overlay modes (identical on X11, XWayland and Wayland). ONE key does everything:
//
//     Insert   HUD  ->  hidden  ->  interaction  ->  HUD  -> ...
//
//     HUD          passive, click-through statistics; the game keeps all input
//     hidden       nothing drawn, nothing intercepted (stays loaded and costs almost nothing)
//     interaction  HUD + settings window; the overlay owns keyboard and mouse, the game gets none.
//                  The window's X button also leaves this mode (back to HUD).
//
// Rebinding: VKOVERLAY_KEY = F1..F12, Insert, Delete, Home, End, PageUp, PageDown, Pause,
// ScrollLock. VKOVERLAY_START_HIDDEN=1 starts in the hidden mode.
#pragma once

#include <cstdint>

#include "input_keys.hpp"

namespace vko::input {

enum class Mode : uint8_t { Hud, Hidden, Settings };
const char* mode_name(Mode m);

struct Bindings {
    Key cycle = Key::Insert;  // HUD -> hidden -> interaction -> HUD
};

// The whole hotkey policy as a pure function (unit-tested). `pressed` = keys that went down
// since the previous poll.
Mode apply_hotkeys(Mode current, KeyMask pressed, const Bindings& b);

struct Frame {
    Mode mode = Mode::Hud;
    bool mode_changed = false;          // true only on the frame the mode switched
    bool input_available = false;       // some backend delivers pointer input
    float mouse_x = 0, mouse_y = 0;     // overlay cursor, swapchain pixels (meaningful in Settings)
    bool button[3] = {false, false, false};  // ImGui order: left, right, middle
    float wheel = 0;                    // scroll delta for this frame, ImGui units
    const char* backend = "none";       // "x11" / "wayland" / "none"
};

// Call once per presented frame with the swapchain extent. Polls the backend at most
// ~60 Hz (XQueryKeymap is a synchronous X round trip), applies hotkeys, and switches game
// input capture on/off when the mode changes.
Frame update(float width, float height);

// Leave interaction mode and give input back to the game (X button, swapchain/device teardown).
void release_capture();

const char* cycle_key_name();     // for on-screen hints

// Best-effort display-system label; X11/XWayland are inferred from session state.
const char* display_server_name();

bool install_wayland_hook();      // idempotent; call from the library constructor
void shutdown();

}  // namespace vko::input
