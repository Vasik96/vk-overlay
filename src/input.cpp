// input.cpp - the unified input controller. This file, and only this file, decides what
// keys do. Backends (input_x11.cpp, input_wayland*.cpp) just report facts.
#include "input.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "input_backend.hpp"
#include "log.hpp"

namespace vko::input {

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Hud: return "hud";
        case Mode::Hidden: return "hidden";
        case Mode::Settings: return "settings";
    }
    return "?";
}

namespace {

// HUD -> hidden -> settings -> HUD
Mode next_mode(Mode m) {
    switch (m) {
        case Mode::Hud: return Mode::Hidden;
        case Mode::Hidden: return Mode::Settings;
        case Mode::Settings: return Mode::Hud;
    }
    return Mode::Hud;
}

struct Controller {
    std::mutex mu;
    bool initialised = false;
    Bindings bindings;
    Mode mode = Mode::Hud;
    bool capture_applied = false;  // what we last told the backend
    double last_poll = -1.0;
    RawInput cached;               // last polled state (edge/wheel fields consumed once)
};
Controller C;

void bind_from_env(const char* var, Key& dst) {
    const char* v = std::getenv(var);
    if (!v || !*v) return;
    Key k;
    if (parse_key(v, &k)) dst = k;
    else VKO_LOG("%s=%s is not a supported key (F1-F12, Insert, Delete, Home, End, PageUp, PageDown, "
                 "Pause, ScrollLock, none) - keeping %s", var, v, key_name(dst));
}

void init_locked() {
    if (C.initialised) return;
    C.initialised = true;
    bind_from_env("VKOVERLAY_KEY", C.bindings.cycle);
    if (std::getenv("VKOVERLAY_START_HIDDEN")) C.mode = Mode::Hidden;
    VKO_LOG("hotkey: %s = cycle HUD / hidden / interaction", key_name(C.bindings.cycle));
}

RawInput backend_poll(bool capturing, float w, float h) {
    return wayland_active() ? poll_wayland(capturing, w, h) : poll_x11(capturing);
}

void backend_capture(bool on) {
    if (wayland_active()) set_capture_wayland(on);
    else set_capture_x11(on);
}

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Mode apply_hotkeys(Mode m, KeyMask pressed, const Bindings& b) {
    return (pressed & key_bit(b.cycle)) != 0 ? next_mode(m) : m;
}

Frame update(float w, float h) {
    std::lock_guard lk(C.mu);
    init_locked();

    const double now = now_seconds();
    const bool fresh = C.last_poll < 0.0 || now - C.last_poll >= 1.0 / 60.0;

    Frame f;
    if (fresh) {
        C.cached = backend_poll(C.mode == Mode::Settings, w, h);
        C.last_poll = now;

        const Mode next = apply_hotkeys(C.mode, C.cached.pressed, C.bindings);
        if (next != C.mode) {
            C.mode = next;
            f.mode_changed = true;
            VKO_VLOG("overlay mode: %s", mode_name(next));
        }
        const bool want_capture = C.mode == Mode::Settings;
        if (want_capture != C.capture_applied) {
            backend_capture(want_capture);
            C.capture_applied = want_capture;
        }
        f.wheel = C.cached.wheel;  // a delta: hand it out exactly once
        C.cached.wheel = 0;
        C.cached.pressed = 0;      // an edge: consumed
    }

    f.mode = C.mode;
    f.input_available = C.cached.available;
    f.mouse_x = C.cached.mouse_x;
    f.mouse_y = C.cached.mouse_y;
    for (int i = 0; i < 3; ++i) f.button[i] = C.cached.button[i];
    f.backend = C.cached.backend;
    return f;
}

void release_capture() {
    std::lock_guard lk(C.mu);
    if (C.mode == Mode::Settings) C.mode = Mode::Hud;
    if (C.capture_applied) {
        backend_capture(false);  // never leave the user's pointer grabbed / the game muted
        C.capture_applied = false;
    }
}

const char* cycle_key_name() {
    std::lock_guard lk(C.mu);
    init_locked();
    return key_name(C.bindings.cycle);
}

const char* display_server_name() {
    // Once our Wayland keyboard hook has observed a client listener/dispatcher,
    // this process is using the native Wayland input path. Otherwise infer the
    // X11/XWayland path from the session environment; environment variables alone
    // cannot distinguish every toolkit/backend combination.
    if (wayland_active()) return "Wayland (native)";

    const char* session = std::getenv("XDG_SESSION_TYPE");
    const bool wayland_session = session && std::strcmp(session, "wayland") == 0;
    const bool has_wayland_display = std::getenv("WAYLAND_DISPLAY") != nullptr;
    const bool has_x_display = std::getenv("DISPLAY") != nullptr;

    if (wayland_session && has_x_display) return "XWayland";
    if (wayland_session || has_wayland_display) return "Wayland (not yet detected)";
    if (has_x_display) return "X11";
    return "Unknown";
}

void shutdown() {
    set_capture_wayland(false);  // make sure the game is never left without input
    shutdown_x11();
}

}  // namespace vko::input
