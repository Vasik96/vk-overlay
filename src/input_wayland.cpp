// input_wayland.cpp - listener-table substitution (core logic, no hooking, no libwayland).
//
// Wayland ABI facts this file relies on (stable protocol ABI):
//   * A listener is a plain C struct of function pointers, one per event, in protocol order.
//     libwayland calls impl[opcode](user_data, proxy, args...) for each event. It never
//     passes the struct size, so how many entries are valid is derived from the *bound
//     interface version* (wl_proxy_get_version): an app compiled against old headers
//     supplies a shorter struct, and reading past it would be an out-of-bounds read.
//   * user_data is one value per proxy; we pass the game's own value through unchanged, so
//     every entry we do NOT wrap keeps working untouched, and our wrappers find their
//     bookkeeping by looking up the proxy pointer (their 2nd argument).
//
// Event tables (opcode : signature after (data, proxy)):
//   wl_keyboard  0 keymap(u32 fmt, i32 fd, u32 size)  1 enter(u32 serial, obj surface, array*)
//                2 leave(u32 serial, obj surface)     3 key(u32 serial, u32 time, u32 key, u32 state)
//                4 modifiers(u32 x4)                  5 repeat_info(i32, i32)           [v4+]
//   wl_pointer   0 enter(u32, obj, fixed, fixed)      1 leave(u32, obj)
//                2 motion(u32 time, fixed, fixed)     3 button(u32 serial, u32 time, u32 button, u32 state)
//                4 axis(u32 time, u32 axis, fixed)    5 frame()  6 axis_source(u32)  7 axis_stop(u32, u32)
//                8 axis_discrete(u32, i32)            9 axis_value120(u32, i32) [v8]  10 axis_relative_direction(u32,u32) [v9]
//   zwp_relative_pointer_v1  0 relative_motion(u32 hi, u32 lo, fixed dx, dy, dx_unaccel, dy_unaccel)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "input_backend.hpp"
#include "log.hpp"

namespace vko::input {
namespace {

// Linux evdev codes - what Wayland delivers in wl_pointer.button (keyboard codes: input_keys.cpp).
constexpr uint32_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112;

using fixed_t = int32_t;                          // wl_fixed_t: signed 24.8
inline double fx(fixed_t v) { return v / 256.0; }

enum class Kind { None, Keyboard, Pointer, Relative };
using Fn = void (*)();

struct Ctx {
    Kind kind = Kind::None;
    std::vector<Fn> orig;  // the game's table (copied: only `n` valid entries)
    std::vector<Fn> ours;  // table handed to libwayland; must outlive the proxy
};

std::mutex g_mu;                                   // guards everything below except atomics
std::unordered_map<void*, Ctx*> g_ctx;             // proxy -> ctx (ctx objects are never freed)
std::vector<uint32_t> g_game_keys, g_game_buttons; // currently held *from the game's point of view*

struct Mouse {
    double x = 0, y = 0;
    bool btn[3] = {false, false, false};
    double wheel = 0;
    int64_t last_abs_ns = 0;
    float w = 0, h = 0;
} g_mouse;
bool g_was_interactive = false;

std::atomic<bool> g_seen_keyboard{false};
std::atomic<bool> g_swallow{false};
// Watched-key state. Written by the compositor-event wrappers (any thread), consumed by the
// controller. `g_pressed` latches presses so a tap shorter than one poll interval is not lost.
std::atomic<KeyMask> g_down{0}, g_pressed{0};

// The ONLY thing the wrappers know about keys: record the fact. Meaning lives in input.cpp.
void note_key(uint32_t evdev_code, uint32_t state) {
    const KeyMask bit = key_bit(key_from_evdev(evdev_code));
    if (!bit) return;
    if (state == 1) {  // pressed (compositor-side repeat is not sent as events)
        g_down.fetch_or(bit, std::memory_order_acq_rel);
        g_pressed.fetch_or(bit, std::memory_order_acq_rel);
    } else {
        g_down.fetch_and(~bit, std::memory_order_acq_rel);
    }
}

int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

double scale() {
    // Pointer coordinates are surface-local (logical) pixels; the swapchain is in physical
    // pixels. With fractional/integer scaling set VKOVERLAY_SCALE (e.g. 1.5).
    static const double s = [] {
        const char* e = std::getenv("VKOVERLAY_SCALE");
        const double v = e ? std::atof(e) : 1.0;
        return v > 0.1 && v < 16.0 ? v : 1.0;
    }();
    return s;
}

Ctx* find_ctx(void* proxy) {
    std::lock_guard lk(g_mu);
    auto it = g_ctx.find(proxy);
    return it == g_ctx.end() ? nullptr : it->second;
}

bool contains(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }
void add(std::vector<uint32_t>& v, uint32_t x) { if (!contains(v, x)) v.push_back(x); }
void del(std::vector<uint32_t>& v, uint32_t x) { v.erase(std::remove(v.begin(), v.end(), x), v.end()); }

// Decide whether an event reaches the game. While capturing, everything is swallowed EXCEPT
// the release of something the game saw pressed - otherwise the game would keep that key or
// button "held" forever (stuck movement keys after toggling the overlay mid-sprint).
bool should_forward(std::vector<uint32_t>& held, uint32_t code, uint32_t state) {
    std::lock_guard lk(g_mu);
    const bool was_held = contains(held, code);
    const bool forward = !g_swallow.load(std::memory_order_acquire) || (state == 0 && was_held);
    if (forward) {
        if (state == 1) add(held, code);
        else del(held, code);
    }
    return forward;
}

// ------------------------------------------------------------------ wl_keyboard wrappers --

void kb_leave(void* data, void* kb, uint32_t serial, void* surface) {
    Ctx* c = find_ctx(kb);
    if (!c) return;
    { std::lock_guard lk(g_mu); g_game_keys.clear(); }  // focus lost: the game resets its own state too
    g_down.store(0, std::memory_order_release);          // ...and so do held-key facts
    if (auto f = c->orig[2]) reinterpret_cast<void (*)(void*, void*, uint32_t, void*)>(f)(data, kb, serial, surface);
}

void kb_key(void* data, void* kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    Ctx* c = find_ctx(kb);
    if (!c) return;
    note_key(key, state);
    if (!should_forward(g_game_keys, key, state)) return;
    if (auto f = c->orig[3])
        reinterpret_cast<void (*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t)>(f)(data, kb, serial, time, key, state);
}

// ------------------------------------------------------------------ wl_pointer wrappers ---

void ptr_motion(void* data, void* p, uint32_t time, fixed_t sx, fixed_t sy) {
    Ctx* c = find_ctx(p);
    if (!c) return;
    {
        std::lock_guard lk(g_mu);
        g_mouse.x = fx(sx) * scale();
        g_mouse.y = fx(sy) * scale();
        g_mouse.last_abs_ns = now_ns();
    }
    if (g_swallow.load(std::memory_order_acquire)) return;
    if (auto f = c->orig[2]) reinterpret_cast<void (*)(void*, void*, uint32_t, fixed_t, fixed_t)>(f)(data, p, time, sx, sy);
}

void ptr_button(void* data, void* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    Ctx* c = find_ctx(p);
    if (!c) return;
    {
        std::lock_guard lk(g_mu);
        const int i = button == kBtnLeft ? 0 : button == kBtnRight ? 1 : button == kBtnMiddle ? 2 : -1;
        if (i >= 0) g_mouse.btn[i] = (state == 1);
    }
    if (!should_forward(g_game_buttons, button, state)) return;
    if (auto f = c->orig[3])
        reinterpret_cast<void (*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t)>(f)(data, p, serial, time, button, state);
}

void ptr_axis(void* data, void* p, uint32_t time, uint32_t axis, fixed_t value) {
    Ctx* c = find_ctx(p);
    if (!c) return;
    if (axis == 0) {  // vertical. Wayland: +value = scroll down, ~10.0 per wheel notch. ImGui: +y = up.
        std::lock_guard lk(g_mu);
        g_mouse.wheel -= fx(value) / 10.0;
    }
    if (g_swallow.load(std::memory_order_acquire)) return;
    if (auto f = c->orig[4]) reinterpret_cast<void (*)(void*, void*, uint32_t, uint32_t, fixed_t)>(f)(data, p, time, axis, value);
}

// The other scroll-related events carry no information we need: swallow them with the rest
// of the scroll gesture while capturing, otherwise pass through.
void ptr_axis_source(void* data, void* p, uint32_t src) {
    Ctx* c = find_ctx(p);
    if (!c || g_swallow.load(std::memory_order_acquire)) return;
    if (auto f = c->orig[6]) reinterpret_cast<void (*)(void*, void*, uint32_t)>(f)(data, p, src);
}
void ptr_axis_stop(void* data, void* p, uint32_t time, uint32_t axis) {
    Ctx* c = find_ctx(p);
    if (!c || g_swallow.load(std::memory_order_acquire)) return;
    if (auto f = c->orig[7]) reinterpret_cast<void (*)(void*, void*, uint32_t, uint32_t)>(f)(data, p, time, axis);
}
void ptr_axis_discrete(void* data, void* p, uint32_t axis, int32_t d) {
    Ctx* c = find_ctx(p);
    if (!c || g_swallow.load(std::memory_order_acquire)) return;
    if (auto f = c->orig[8]) reinterpret_cast<void (*)(void*, void*, uint32_t, int32_t)>(f)(data, p, axis, d);
}
void ptr_axis_value120(void* data, void* p, uint32_t axis, int32_t d) {
    Ctx* c = find_ctx(p);
    if (!c || g_swallow.load(std::memory_order_acquire)) return;
    if (auto f = c->orig[9]) reinterpret_cast<void (*)(void*, void*, uint32_t, int32_t)>(f)(data, p, axis, d);
}
void ptr_axis_rel_dir(void* data, void* p, uint32_t axis, uint32_t dir) {
    Ctx* c = find_ctx(p);
    if (!c || g_swallow.load(std::memory_order_acquire)) return;
    if (auto f = c->orig[10]) reinterpret_cast<void (*)(void*, void*, uint32_t, uint32_t)>(f)(data, p, axis, dir);
}

// ------------------------------------------------------- zwp_relative_pointer_v1 wrapper --

void rel_motion(void* data, void* obj, uint32_t hi, uint32_t lo, fixed_t dx, fixed_t dy, fixed_t dxu, fixed_t dyu) {
    Ctx* c = find_ctx(obj);
    if (!c) return;
    {
        // A locked pointer (FPS-style mouse look) never produces absolute motion, so the overlay
        // cursor is moved from relative deltas - but only if no absolute motion arrived lately,
        // otherwise both sources would double-count.
        std::lock_guard lk(g_mu);
        if (now_ns() - g_mouse.last_abs_ns > 200'000'000) {
            g_mouse.x += fx(dx);
            g_mouse.y += fx(dy);
            if (g_mouse.w > 0) g_mouse.x = std::clamp(g_mouse.x, 0.0, static_cast<double>(g_mouse.w));
            if (g_mouse.h > 0) g_mouse.y = std::clamp(g_mouse.y, 0.0, static_cast<double>(g_mouse.h));
        }
    }
    if (g_swallow.load(std::memory_order_acquire)) return;
    if (auto f = c->orig[0])
        reinterpret_cast<void (*)(void*, void*, uint32_t, uint32_t, fixed_t, fixed_t, fixed_t, fixed_t)>(f)(data, obj, hi, lo, dx, dy, dxu, dyu);
}

// Number of valid listener entries for an interface version.
size_t entry_count(Kind k, uint32_t version) {
    switch (k) {
        case Kind::Keyboard: return version >= 4 ? 6 : 5;
        case Kind::Pointer:  return version >= 9 ? 11 : version >= 8 ? 10 : version >= 5 ? 9 : 5;
        case Kind::Relative: return 1;
        default: return 0;
    }
}

Kind classify(const char* cls) {
    if (!cls) return Kind::None;
    if (!std::strcmp(cls, "wl_keyboard")) return Kind::Keyboard;
    if (!std::strcmp(cls, "wl_pointer")) return Kind::Pointer;
    if (!std::strcmp(cls, "zwp_relative_pointer_v1")) return Kind::Relative;
    return Kind::None;
}

}  // namespace

// =====================================================================================
namespace detail {

int wl_handle_add_listener(const char* cls, uint32_t version, void* proxy, void (**impl)(void), void* data,
                           RealAddListener real) {
    const Kind kind = classify(cls);
    const size_t n = entry_count(kind, version);
    if (kind == Kind::None || !impl || n == 0) return real(proxy, impl, data);

    auto* c = new Ctx;
    c->kind = kind;
    c->orig.assign(impl, impl + n);  // exactly the entries the app's struct is guaranteed to have
    c->ours = c->orig;
    switch (kind) {
        case Kind::Keyboard:
            c->ours[2] = reinterpret_cast<Fn>(&kb_leave);
            c->ours[3] = reinterpret_cast<Fn>(&kb_key);
            break;
        case Kind::Pointer:
            c->ours[2] = reinterpret_cast<Fn>(&ptr_motion);
            c->ours[3] = reinterpret_cast<Fn>(&ptr_button);
            c->ours[4] = reinterpret_cast<Fn>(&ptr_axis);
            if (n > 6) c->ours[6] = reinterpret_cast<Fn>(&ptr_axis_source);
            if (n > 7) c->ours[7] = reinterpret_cast<Fn>(&ptr_axis_stop);
            if (n > 8) c->ours[8] = reinterpret_cast<Fn>(&ptr_axis_discrete);
            if (n > 9) c->ours[9] = reinterpret_cast<Fn>(&ptr_axis_value120);
            if (n > 10) c->ours[10] = reinterpret_cast<Fn>(&ptr_axis_rel_dir);
            break;
        case Kind::Relative:
            c->ours[0] = reinterpret_cast<Fn>(&rel_motion);
            break;
        default: break;
    }
    {
        std::lock_guard lk(g_mu);
        g_ctx[proxy] = c;  // registered before the real call: events can only arrive after it
    }
    const int rc = real(proxy, c->ours.data(), data);
    if (rc != 0) {  // libwayland refused (listener already set): undo
        std::lock_guard lk(g_mu);
        g_ctx.erase(proxy);
        delete c;
        return rc;
    }
    if (kind == Kind::Keyboard) {
        if (!g_seen_keyboard.exchange(true)) VKO_LOG("native Wayland input active (listener hook)");
    }
    return rc;
}


// Rust's wayland-client and other generated clients can register a single dispatcher
// instead of a wl_message listener table. The dispatcher receives protocol arguments
// in the stable wl_argument union layout. We wrap only the input events we understand,
// then call the game's original dispatcher unchanged when the overlay is not capturing.
struct WlMessageCompat { const char* name; const char* signature; const void* types; };
union WlArgumentCompat {
    int32_t i; uint32_t u; int32_t f; const char* s; void* o;
    uint32_t n; void* a; int32_t h;
};
struct DispatchCtx {
    Kind kind = Kind::None;
    Dispatcher original = nullptr;
    const void* implementation = nullptr;
};
std::unordered_map<void*, DispatchCtx*> g_dispatch_ctx;

int dispatch_bridge(const void* opaque, void* target, uint32_t opcode,
                    const void* message_opaque, void* args_opaque) {
    auto* c = const_cast<DispatchCtx*>(static_cast<const DispatchCtx*>(opaque));
    if (!c || !c->original || !message_opaque || !args_opaque) return 0;
    const auto* message = static_cast<const WlMessageCompat*>(message_opaque);
    auto* a = static_cast<WlArgumentCompat*>(args_opaque);
    const char* name = message->name ? message->name : "";
    bool forward = true;

    if (c->kind == Kind::Keyboard) {
        if (!std::strcmp(name, "leave")) {
            { std::lock_guard lk(g_mu); g_game_keys.clear(); }
            g_down.store(0, std::memory_order_release);
        } else if (!std::strcmp(name, "key")) {
            const uint32_t key = a[2].u, state = a[3].u;
            note_key(key, state);
            forward = should_forward(g_game_keys, key, state);
        }
    } else if (c->kind == Kind::Pointer) {
        if (!std::strcmp(name, "motion")) {
            {
                std::lock_guard lk(g_mu);
                g_mouse.x = fx(a[1].f) * scale(); g_mouse.y = fx(a[2].f) * scale();
                g_mouse.last_abs_ns = now_ns();
            }
            forward = !g_swallow.load(std::memory_order_acquire);
        } else if (!std::strcmp(name, "button")) {
            const uint32_t button = a[2].u, state = a[3].u;
            {
                std::lock_guard lk(g_mu);
                const int i = button == kBtnLeft ? 0 : button == kBtnRight ? 1 : button == kBtnMiddle ? 2 : -1;
                if (i >= 0) g_mouse.btn[i] = (state == 1);
            }
            forward = should_forward(g_game_buttons, button, state);
        } else if (!std::strcmp(name, "axis")) {
            if (a[1].u == 0) { std::lock_guard lk(g_mu); g_mouse.wheel -= fx(a[2].f) / 10.0; }
            forward = !g_swallow.load(std::memory_order_acquire);
        } else if (!std::strcmp(name, "axis_source") || !std::strcmp(name, "axis_stop") ||
                   !std::strcmp(name, "axis_discrete") || !std::strcmp(name, "axis_value120") ||
                   !std::strcmp(name, "axis_relative_direction")) {
            forward = !g_swallow.load(std::memory_order_acquire);
        }
    } else if (c->kind == Kind::Relative && !std::strcmp(name, "relative_motion")) {
        {
            std::lock_guard lk(g_mu);
            if (now_ns() - g_mouse.last_abs_ns > 200'000'000) {
                g_mouse.x += fx(a[2].f); g_mouse.y += fx(a[3].f);
                if (g_mouse.w > 0) g_mouse.x = std::clamp(g_mouse.x, 0.0, static_cast<double>(g_mouse.w));
                if (g_mouse.h > 0) g_mouse.y = std::clamp(g_mouse.y, 0.0, static_cast<double>(g_mouse.h));
            }
        }
        forward = !g_swallow.load(std::memory_order_acquire);
    }
    return forward ? c->original(c->implementation, target, opcode, message_opaque, args_opaque) : 0;
}

int wl_handle_add_dispatcher(const char* cls, void* proxy, Dispatcher dispatcher,
                             const void* implementation, void* data, RealAddDispatcher real) {
    const Kind kind = classify(cls);
    if (kind == Kind::None || !dispatcher || !proxy) return real(proxy, dispatcher, implementation, data);
    auto* c = new DispatchCtx{kind, dispatcher, implementation};
    {
        std::lock_guard lk(g_mu);
        g_dispatch_ctx[proxy] = c;
    }
    const int rc = real(proxy, &dispatch_bridge, c, data);
    if (rc != 0) {
        std::lock_guard lk(g_mu); g_dispatch_ctx.erase(proxy); delete c;
        return rc;
    }
    if (kind == Kind::Keyboard && !g_seen_keyboard.exchange(true))
        VKO_LOG("native Wayland input active (dispatcher hook)");
    return rc;
}

void wl_reset_for_test() {
    std::lock_guard lk(g_mu);
    g_ctx.clear();
    g_dispatch_ctx.clear();
    g_game_keys.clear();
    g_game_buttons.clear();
    g_mouse = Mouse{};
    g_was_interactive = false;
    g_seen_keyboard = false;
    g_swallow = false;
    g_down = 0;
    g_pressed = 0;
}

}  // namespace detail

// =====================================================================================

bool wayland_active() { return g_seen_keyboard.load(std::memory_order_acquire); }

void set_capture_wayland(bool on) { g_swallow.store(on, std::memory_order_release); }

RawInput poll_wayland(bool capturing, float w, float h) {
    RawInput s;
    s.available = true;
    s.backend = "wayland";
    s.down = g_down.load(std::memory_order_acquire);
    s.pressed = g_pressed.exchange(0, std::memory_order_acq_rel);

    std::lock_guard lk(g_mu);
    g_mouse.w = w;
    g_mouse.h = h;
    if (capturing && !g_was_interactive) {
        // Entering capture. If the pointer was locked there is no absolute position: start centred.
        if (now_ns() - g_mouse.last_abs_ns > 500'000'000) { g_mouse.x = w * 0.5; g_mouse.y = h * 0.5; }
    }
    g_was_interactive = capturing;
    s.mouse_x = static_cast<float>(std::clamp(g_mouse.x, 0.0, static_cast<double>(w)));
    s.mouse_y = static_cast<float>(std::clamp(g_mouse.y, 0.0, static_cast<double>(h)));
    for (int i = 0; i < 3; ++i) s.button[i] = g_mouse.btn[i];
    s.wheel = capturing ? static_cast<float>(g_mouse.wheel) : 0.0f;
    g_mouse.wheel = 0;
    return s;
}

}  // namespace vko::input
