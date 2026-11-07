// Logic test for the Wayland listener wrappers. No libwayland involved: we play the role of
// both the game (registers listeners) and libwayland (invokes impl[opcode]).
// Listener tables are heap-allocated with EXACTLY the entries the bound version guarantees,
// so AddressSanitizer flags any read past the end.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "input_backend.hpp"

using namespace vko::input;
using Fn = void (*)();

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// ---- the "game" -----------------------------------------------------------------------
struct Game {
    std::vector<std::vector<uint32_t>> keys;     // {key, state}
    std::vector<std::vector<uint32_t>> buttons;  // {button, state}
    int motions = 0, axes = 0, rel = 0, leaves = 0, keymaps = 0;
    void* data_seen = nullptr;
} G;

static void g_keymap(void* d, void*, uint32_t, int32_t, uint32_t) { G.keymaps++; G.data_seen = d; }
static void g_enter(void*, void*, uint32_t, void*, void*) {}
static void g_leave(void*, void*, uint32_t, void*) { G.leaves++; }
static void g_key(void* d, void*, uint32_t, uint32_t, uint32_t key, uint32_t st) { G.keys.push_back({key, st}); G.data_seen = d; }
static void g_mods(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t) {}
static void g_rep(void*, void*, int32_t, int32_t) {}
static void p_enter(void*, void*, uint32_t, void*, int32_t, int32_t) {}
static void p_leave(void*, void*, uint32_t, void*) {}
static void p_motion(void*, void*, uint32_t, int32_t, int32_t) { G.motions++; }
static void p_button(void*, void*, uint32_t, uint32_t, uint32_t b, uint32_t st) { G.buttons.push_back({b, st}); }
static void p_axis(void*, void*, uint32_t, uint32_t, int32_t) { G.axes++; }
static void p_frame(void*, void*) {}
static void r_motion(void*, void*, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t) { G.rel++; }

// ---- fake libwayland ------------------------------------------------------------------
static void (**g_registered)(void) = nullptr;
static void* g_registered_data = nullptr;
static int real_add(void*, void (**impl)(void), void* data) { g_registered = impl; g_registered_data = data; return 0; }

static Fn* make_table(std::initializer_list<Fn> fns) {
    Fn* t = static_cast<Fn*>(std::malloc(sizeof(Fn) * fns.size()));  // exact size for ASan
    size_t i = 0;
    for (Fn f : fns) t[i++] = f;
    return t;
}
#define FN(x) reinterpret_cast<Fn>(x)

static int dummy_data = 42;
static char kb_proxy, ptr_proxy, rel_proxy;

template <class F, class... A> static void call(int idx, void* proxy, A... a) {
    reinterpret_cast<F>(g_registered[idx])(g_registered_data, proxy, a...);
}

int main() {
    // ---------------------------------------------------------------- keyboard (v7: 6 entries)
    detail::wl_reset_for_test();
    Fn* kb = make_table({FN(g_keymap), FN(g_enter), FN(g_leave), FN(g_key), FN(g_mods), FN(g_rep)});
    CHECK(detail::wl_handle_add_listener("wl_keyboard", 7, &kb_proxy, kb, &dummy_data, real_add) == 0);
    CHECK(g_registered != kb);                          // libwayland got OUR table
    CHECK(g_registered_data == &dummy_data);            // game's user_data passed through untouched
    CHECK(g_registered[0] == kb[0] && g_registered[1] == kb[1] && g_registered[4] == kb[4] && g_registered[5] == kb[5]);
    CHECK(g_registered[3] != kb[3] && g_registered[2] != kb[2]);  // key + leave wrapped
    CHECK(wayland_active());

    using KeyFn = void (*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    // normal key: forwarded, game's data pointer intact
    call<KeyFn>(3, &kb_proxy, 1u, 100u, 17u /*W*/, 1u);
    CHECK(G.keys.size() == 1 && G.keys[0][0] == 17 && G.keys[0][1] == 1 && G.data_seen == &dummy_data);
    // hotkeys while NOT capturing: edge raised AND still forwarded to the game
    call<KeyFn>(3, &kb_proxy, 2u, 101u, 110u /*Insert*/, 1u);
    call<KeyFn>(3, &kb_proxy, 3u, 102u, 110u, 0u);
    call<KeyFn>(3, &kb_proxy, 4u, 103u, 88u /*F12*/, 1u);
    RawInput s;
    s = poll_wayland(false, 1920, 1080);
    CHECK(s.available && (s.pressed & key_bit(Key::Insert)) && (s.pressed & key_bit(Key::F12)));
    CHECK(!(s.down & key_bit(Key::Insert)) && (s.down & key_bit(Key::F12)));   // Insert was released, F12 still held
    RawInput s2;
    s2 = poll_wayland(false, 1920, 1080);
    CHECK(s2.pressed == 0);                                 // edges are consumed once
    CHECK(s2.down & key_bit(Key::F12));                     // ...but "held" is a level, not an edge
    CHECK(G.keys.size() == 4);

    // capture ON: W is still held from the game's view
    size_t before = G.keys.size();
    set_capture_wayland(true);
    call<KeyFn>(3, &kb_proxy, 5u, 110u, 30u /*A*/, 1u);            // new press: swallowed
    call<KeyFn>(3, &kb_proxy, 6u, 111u, 30u, 0u);                  // its release: swallowed (game never saw press)
    call<KeyFn>(3, &kb_proxy, 7u, 112u, 17u /*W*/, 0u);            // release of game-held key: FORWARDED
    CHECK(G.keys.size() == before + 1 && G.keys.back()[0] == 17 && G.keys.back()[1] == 0);
    call<KeyFn>(3, &kb_proxy, 8u, 113u, 110u /*Insert*/, 1u);      // hotkey still detected while swallowing
    s = poll_wayland(true, 1920, 1080);
    CHECK(s.pressed & key_bit(Key::Insert));
    CHECK(G.keys.size() == before + 1);                            // ...but not forwarded
    set_capture_wayland(false);
    using LeaveFn = void (*)(void*, void*, uint32_t, void*);
    call<LeaveFn>(2, &kb_proxy, 10u, nullptr);              // focus lost: nothing may stay "held"
    s = poll_wayland(false, 1920, 1080);
    CHECK(s.down == 0);
    call<KeyFn>(3, &kb_proxy, 9u, 114u, 31u, 1u);
    CHECK(G.keys.back()[0] == 31);                                 // flows again

    // ---------------------------------------------------------------- pointer v7 (9 entries)
    Fn* ptr = make_table({FN(p_enter), FN(p_leave), FN(p_motion), FN(p_button), FN(p_axis), FN(p_frame),
                          nullptr /*axis_source*/, nullptr /*axis_stop*/, nullptr /*axis_discrete*/});
    CHECK(detail::wl_handle_add_listener("wl_pointer", 7, &ptr_proxy, ptr, &dummy_data, real_add) == 0);
    CHECK(g_registered[5] == ptr[5]);                      // frame untouched
    using MotionFn = void (*)(void*, void*, uint32_t, int32_t, int32_t);
    using ButtonFn = void (*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    using AxisFn = void (*)(void*, void*, uint32_t, uint32_t, int32_t);
    call<MotionFn>(2, &ptr_proxy, 1u, 100 * 256, 50 * 256);  // wl_fixed 100.0, 50.0
    CHECK(G.motions == 1);
    s = poll_wayland(true, 1920, 1080);
    CHECK(s.mouse_x == 100.0f && s.mouse_y == 50.0f);

    call<ButtonFn>(3, &ptr_proxy, 1u, 1u, 0x110u, 1u);     // left press, forwarded (not capturing)
    set_capture_wayland(true);
    call<MotionFn>(2, &ptr_proxy, 2u, 200 * 256, 60 * 256);
    CHECK(G.motions == 1);                                 // swallowed...
    s = poll_wayland(true, 1920, 1080);
    CHECK(s.mouse_x == 200.0f && s.mouse_y == 60.0f);      // ...but the overlay cursor still moved
    call<ButtonFn>(3, &ptr_proxy, 2u, 2u, 0x111u, 1u);     // right press: swallowed, overlay sees it
    s = poll_wayland(true, 1920, 1080);
    CHECK(s.button[0] && s.button[1] && G.buttons.size() == 1);
    call<ButtonFn>(3, &ptr_proxy, 3u, 3u, 0x110u, 0u);     // release of game-held left: forwarded
    CHECK(G.buttons.size() == 2 && G.buttons.back()[1] == 0);
    call<AxisFn>(4, &ptr_proxy, 4u, 0u, 10 * 256);         // one notch down
    CHECK(G.axes == 0);
    s = poll_wayland(true, 1920, 1080);
    CHECK(s.wheel < -0.99f && s.wheel > -1.01f);
    s = poll_wayland(true, 1920, 1080);
    CHECK(s.wheel == 0.0f);                                // consumed
    set_capture_wayland(false);

    // ---------------------------------------------------------------- old pointer v3: only 5 entries
    detail::wl_reset_for_test();
    Fn* ptr3 = make_table({FN(p_enter), FN(p_leave), FN(p_motion), FN(p_button), FN(p_axis)});
    CHECK(detail::wl_handle_add_listener("wl_pointer", 3, &ptr_proxy, ptr3, &dummy_data, real_add) == 0);
    call<MotionFn>(2, &ptr_proxy, 1u, 256, 256);           // ASan: must not touch entries 5..8 of an exact-size table

    // ---------------------------------------------------------------- relative pointer
    detail::wl_reset_for_test();
    G.rel = 0;
    Fn* rel = make_table({FN(r_motion)});
    CHECK(detail::wl_handle_add_listener("zwp_relative_pointer_v1", 1, &rel_proxy, rel, &dummy_data, real_add) == 0);
    using RelFn = void (*)(void*, void*, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t);
    call<RelFn>(0, &rel_proxy, 0u, 0u, 5 * 256, -3 * 256, 5 * 256, -3 * 256);
    CHECK(G.rel == 1);                                      // not capturing: forwarded
    s = poll_wayland(false, 800, 600);                       // (not interactive: no centring)
    set_capture_wayland(true);
    s = poll_wayland(true, 800, 600);                        // entering capture with no absolute pos -> centred
    CHECK(s.mouse_x == 400.0f && s.mouse_y == 300.0f);
    call<RelFn>(0, &rel_proxy, 0u, 0u, 10 * 256, 20 * 256, 0, 0);
    s = poll_wayland(true, 800, 600);
    CHECK(G.rel == 1);                                      // swallowed
    CHECK(s.mouse_x == 410.0f && s.mouse_y == 320.0f);      // integrated from relative deltas
    call<RelFn>(0, &rel_proxy, 0u, 0u, 9999 * 256, 0, 0, 0);
    s = poll_wayland(true, 800, 600);
    CHECK(s.mouse_x == 800.0f);                             // clamped to the screen
    set_capture_wayland(false);

    // ---------------------------------------------------------------- non-input interface untouched
    detail::wl_reset_for_test();
    Fn* other = make_table({FN(g_keymap)});
    CHECK(detail::wl_handle_add_listener("wl_output", 4, &kb_proxy, other, &dummy_data, real_add) == 0);
    CHECK(g_registered == other);                           // passed straight through
    CHECK(!wayland_active());

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
