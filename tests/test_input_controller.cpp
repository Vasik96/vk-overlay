// Tests the unified controller (input.cpp) against a SCRIPTED fake backend.
//
// The point of the unification: the same physical key presses must produce the same overlay
// behaviour no matter which backend reported them. The fake backend below stands in for both
// X11 and Wayland (they differ only in `backend` and in how `pressed` is obtained); the
// controller is the only code that interprets keys.
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "input.hpp"
#include "input_backend.hpp"

using namespace vko::input;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// ---- scripted fake backend (the controller links against these) --------------------------
static bool g_wayland = false;
static RawInput g_next;                       // what the next poll returns
static std::vector<bool> g_capture_calls;     // capture on/off requests, in order
static int g_polls = 0;
static bool g_capturing_arg = false;

namespace vko::input {
RawInput poll_x11(bool capturing) { ++g_polls; g_capturing_arg = capturing; RawInput r = g_next; r.backend = "x11"; g_next = {}; return r; }
RawInput poll_wayland(bool capturing, float, float) { ++g_polls; g_capturing_arg = capturing; RawInput r = g_next; r.backend = "wayland"; g_next = {}; return r; }
bool wayland_active() { return g_wayland; }
void set_capture_x11(bool on) { g_capture_calls.push_back(on); }
void set_capture_wayland(bool on) { g_capture_calls.push_back(on); }
void shutdown_x11() {}
}  // namespace vko::input

static void wait_poll_interval() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }  // > 1/60 s

static Frame press(Key k) {
    g_next = {};
    g_next.available = true;
    g_next.pressed = key_bit(k);
    g_next.down = key_bit(k);
    wait_poll_interval();
    return update(1920, 1080);
}

int main() {
    // ============================================================ pure policy
    const Bindings b;  // Insert cycles everything
    CHECK(apply_hotkeys(Mode::Hud, key_bit(Key::Insert), b) == Mode::Hidden);        // HUD -> hidden
    CHECK(apply_hotkeys(Mode::Hidden, key_bit(Key::Insert), b) == Mode::Settings);   // hidden -> interaction
    CHECK(apply_hotkeys(Mode::Settings, key_bit(Key::Insert), b) == Mode::Hud);      // interaction -> HUD (repeat)
    CHECK(apply_hotkeys(Mode::Hud, 0, b) == Mode::Hud);                              // nothing pressed
    CHECK(apply_hotkeys(Mode::Hud, key_bit(Key::F12), b) == Mode::Hud);              // F12 is no longer bound
    Bindings off{Key::None};
    CHECK(apply_hotkeys(Mode::Hud, key_bit(Key::Insert), off) == Mode::Hud);         // disabled binding is inert
    Mode m = Mode::Hud;
    for (int i = 0; i < 3; ++i) m = apply_hotkeys(m, key_bit(Key::Insert), b);
    CHECK(m == Mode::Hud);                                                           // a full lap returns to the start

    // ============================================================ key table
    Key k = Key::None;
    CHECK(parse_key("f12", &k) && k == Key::F12);
    CHECK(parse_key("INSERT", &k) && k == Key::Insert);
    CHECK(parse_key("none", &k) && k == Key::None);
    k = Key::Home;
    CHECK(!parse_key("banana", &k) && k == Key::Home);   // unknown name leaves the value alone
    CHECK(key_from_evdev(88) == Key::F12 && key_from_evdev(110) == Key::Insert && key_from_evdev(30) == Key::None);
    size_t n = 0;
    const KeyInfo* t = key_table(&n);
    for (size_t i = 0; i < n; ++i)                         // table is self-consistent (no duplicate codes)
        for (size_t j = i + 1; j < n; ++j) CHECK(t[i].evdev != t[j].evdev && t[i].keysym != t[j].keysym && t[i].key != t[j].key);

    // ============================================================ controller, X11-style backend
    ::unsetenv("VKOVERLAY_START_HIDDEN");
    ::unsetenv("VKOVERLAY_KEY");
    g_wayland = false;
    CHECK(std::string(cycle_key_name()) == "Insert");

    g_next = {}; g_next.available = true;
    Frame f = update(1920, 1080);
    CHECK(f.mode == Mode::Hud && !f.mode_changed && f.input_available && std::string(f.backend) == "x11");

    f = press(Key::Insert);
    CHECK(f.mode == Mode::Hidden && f.mode_changed);
    CHECK(g_capture_calls.empty());                         // hidden never captures
    f = press(Key::Insert);
    CHECK(f.mode == Mode::Settings && f.mode_changed);
    CHECK(!g_capture_calls.empty() && g_capture_calls.back() == true);   // settings captures game input
    f = press(Key::Insert);
    CHECK(f.mode == Mode::Hud && f.mode_changed);
    CHECK(g_capture_calls.back() == false);                // ...and releases it again

    // ============================================================ identical behaviour on Wayland
    g_wayland = true;
    g_capture_calls.clear();
    f = press(Key::Insert);  CHECK(f.mode == Mode::Hidden   && std::string(f.backend) == "wayland");
    f = press(Key::Insert);  CHECK(f.mode == Mode::Settings && g_capture_calls.back() == true);
    f = press(Key::Insert); CHECK(f.mode == Mode::Hud    && g_capture_calls.back() == false);
    // (now in HUD after the lap above)

    // the backend is told whether we are capturing, so it can centre the cursor / report scroll
    f = press(Key::Insert); f = press(Key::Insert);        // HUD -> hidden -> interaction
    CHECK(f.mode == Mode::Settings);
    g_next = {}; g_next.available = true;
    wait_poll_interval(); update(1920, 1080);
    CHECK(g_capturing_arg == true);

    // ============================================================ rate limiting + one-shot fields
    const int polls_before = g_polls;
    g_next = {}; g_next.available = true; g_next.wheel = 3.0f; g_next.pressed = key_bit(Key::Insert);
    wait_poll_interval();
    f = update(1920, 1080);                                // fresh poll: wheel handed out once, Insert acted on
    CHECK(f.wheel == 3.0f && f.mode == Mode::Hud);         // interaction -> HUD
    Frame f2 = update(1920, 1080);                         // immediately again: cached, no new poll
    CHECK(g_polls == polls_before + 1);
    CHECK(f2.wheel == 0.0f && !f2.mode_changed && f2.mode == Mode::Hud);   // neither wheel nor key edge repeats

    // ============================================================ release_capture (X button / teardown)
    g_capture_calls.clear();
    f = press(Key::Insert); f = press(Key::Insert);        // -> hidden -> interaction
    CHECK(f.mode == Mode::Settings && g_capture_calls.back() == true);
    release_capture();                                     // X button clicked
    CHECK(g_capture_calls.back() == false);
    wait_poll_interval();
    g_next = {}; g_next.available = true;
    f = update(1920, 1080);
    CHECK(f.mode == Mode::Hud);                            // back to HUD, input returned to the game

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
