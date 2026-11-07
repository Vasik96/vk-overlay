// Integration test against the system's REAL libwayland-client / libwayland-server.
//
//   fake compositor thread (libwayland-server)  <-- socketpair -->  "game" (libwayland-client)
//
// The game registers wl_keyboard / wl_pointer listeners exactly as SDL/GLFW do. Because
// install_wayland_hook() has inline-patched wl_proxy_add_listener inside libwayland-client,
// the compositor's events travel through the real libwayland dispatch (libffi-style
// invocation of impl[opcode]) into OUR wrappers first. This validates the parts a pure unit
// test cannot: the hook itself, wl_proxy_get_class/get_version, and argument marshalling.
//
// No headers are used: protocol structs are declared by hand, everything else via dlsym.
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "input_backend.hpp"

using namespace vko::input;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

struct WlInterface { const char* name; int version; };
using Fn = void (*)();

// ---- libwayland function pointers --------------------------------------------------
#define DECL(ret, name, args) static ret(*name) args
DECL(void*, wl_display_connect_to_fd, (int));
DECL(int, wl_display_roundtrip, (void*));
DECL(void, wl_display_disconnect, (void*));
DECL(void*, wl_proxy_marshal_flags, (void*, uint32_t, const WlInterface*, uint32_t, uint32_t, ...));
DECL(int, wl_proxy_add_listener, (void*, Fn*, void*));
DECL(void*, wl_display_create, ());
DECL(void*, wl_display_get_event_loop, (void*));
DECL(int, wl_event_loop_dispatch, (void*, int));
DECL(void, wl_display_flush_clients, (void*));
DECL(void*, wl_global_create, (void*, const WlInterface*, int, void*, void (*)(void*, void*, uint32_t, uint32_t)));
DECL(void*, wl_client_create, (void*, int));
DECL(void*, wl_resource_create, (void*, const WlInterface*, int, uint32_t));
DECL(void, wl_resource_set_implementation, (void*, const void*, void*, void*));
DECL(int, wl_resource_get_version, (void*));
DECL(void, wl_resource_post_event, (void*, uint32_t, ...));
static const WlInterface *I_registry, *I_seat, *I_keyboard, *I_pointer;

template <class T> static void sym(void* lib, T& out, const char* n) {
    out = reinterpret_cast<T>(dlsym(lib, n));
    if (!out) { std::printf("missing symbol %s\n", n); std::exit(2); }
}

// ---- fake compositor -----------------------------------------------------------------
static void *g_kb_res = nullptr, *g_ptr_res = nullptr;
static std::mutex g_q_mu;
static std::queue<std::function<void()>> g_q;
static std::atomic<bool> g_run{true};

static void dummy_release(void*, void*) {}
static Fn g_res_impl[1] = {reinterpret_cast<Fn>(dummy_release)};

static void seat_get_pointer(void* client, void* res, uint32_t id) {
    g_ptr_res = wl_resource_create(client, I_pointer, wl_resource_get_version(res), id);
    wl_resource_set_implementation(g_ptr_res, g_res_impl, nullptr, nullptr);
}
static void seat_get_keyboard(void* client, void* res, uint32_t id) {
    g_kb_res = wl_resource_create(client, I_keyboard, wl_resource_get_version(res), id);
    wl_resource_set_implementation(g_kb_res, g_res_impl, nullptr, nullptr);
}
static void seat_get_touch(void*, void*, uint32_t) {}
static Fn g_seat_impl[4] = {reinterpret_cast<Fn>(seat_get_pointer), reinterpret_cast<Fn>(seat_get_keyboard),
                            reinterpret_cast<Fn>(seat_get_touch), reinterpret_cast<Fn>(dummy_release)};
static void seat_bind(void* client, void*, uint32_t version, uint32_t id) {
    void* r = wl_resource_create(client, I_seat, static_cast<int>(version), id);
    wl_resource_set_implementation(r, g_seat_impl, nullptr, nullptr);
}

static void on_server(std::function<void()> f) { std::lock_guard lk(g_q_mu); g_q.push(std::move(f)); }

// ---- the "game" (client) ---------------------------------------------------------------
struct Seen {
    std::vector<std::vector<uint32_t>> keys, buttons;
    int motions = 0, axes = 0;
    void* last_data = nullptr;
} S;
static int g_game_data = 1234;
static void* g_seat = nullptr;

static void k_keymap(void*, void*, uint32_t, int32_t, uint32_t) {}
static void k_enter(void*, void*, uint32_t, void*, void*) {}
static void k_leave(void*, void*, uint32_t, void*) {}
static void k_key(void* d, void*, uint32_t, uint32_t, uint32_t key, uint32_t st) { S.keys.push_back({key, st}); S.last_data = d; }
static void k_mods(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t) {}
static void k_rep(void*, void*, int32_t, int32_t) {}
static void p_enter(void*, void*, uint32_t, void*, int32_t, int32_t) {}
static void p_leave(void*, void*, uint32_t, void*) {}
static void p_motion(void*, void*, uint32_t, int32_t, int32_t) { S.motions++; }
static void p_button(void*, void*, uint32_t, uint32_t, uint32_t b, uint32_t st) { S.buttons.push_back({b, st}); }
static void p_axis(void*, void*, uint32_t, uint32_t, int32_t) { S.axes++; }
static void p_frame(void*, void*) {}
static void p_src(void*, void*, uint32_t) {}
static void p_stop(void*, void*, uint32_t, uint32_t) {}
static void p_disc(void*, void*, uint32_t, int32_t) {}

static void reg_global(void*, void* reg, uint32_t name, const char* iface, uint32_t version) {
    if (std::string(iface) == "wl_seat")
        g_seat = wl_proxy_marshal_flags(reg, 0 /*bind*/, I_seat, 7, 0, name, I_seat->name, 7u, nullptr);
}
static void reg_remove(void*, void*, uint32_t) {}

template <size_t N> static Fn* exact_table(const Fn (&src)[N]) {  // exact-size heap table
    Fn* t = static_cast<Fn*>(std::malloc(sizeof(Fn) * N));
    for (size_t i = 0; i < N; ++i) t[i] = src[i];
    return t;
}
#define F(x) reinterpret_cast<Fn>(x)

int main() {
    ::setenv("WAYLAND_DISPLAY", "test-not-used", 1);
    ::setenv("VKOVERLAY_VERBOSE", "1", 1);

    // 1. Hook libwayland-client BEFORE the game touches it (this is what LD_PRELOAD guarantees).
    CHECK(install_wayland_hook());
    void* cl = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
    void* sv = dlopen("libwayland-server.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!cl || !sv) { std::printf("libwayland missing, skipping\n"); return 77; }
    sym(cl, wl_display_connect_to_fd, "wl_display_connect_to_fd");
    sym(cl, wl_display_roundtrip, "wl_display_roundtrip");
    sym(cl, wl_display_disconnect, "wl_display_disconnect");
    sym(cl, wl_proxy_marshal_flags, "wl_proxy_marshal_flags");
    sym(cl, wl_proxy_add_listener, "wl_proxy_add_listener");
    sym(sv, wl_display_create, "wl_display_create");
    sym(sv, wl_display_get_event_loop, "wl_display_get_event_loop");
    sym(sv, wl_event_loop_dispatch, "wl_event_loop_dispatch");
    sym(sv, wl_display_flush_clients, "wl_display_flush_clients");
    sym(sv, wl_global_create, "wl_global_create");
    sym(sv, wl_client_create, "wl_client_create");
    sym(sv, wl_resource_create, "wl_resource_create");
    sym(sv, wl_resource_set_implementation, "wl_resource_set_implementation");
    sym(sv, wl_resource_get_version, "wl_resource_get_version");
    sym(sv, wl_resource_post_event, "wl_resource_post_event");
    I_registry = static_cast<const WlInterface*>(dlsym(cl, "wl_registry_interface"));
    I_seat = static_cast<const WlInterface*>(dlsym(cl, "wl_seat_interface"));
    I_keyboard = static_cast<const WlInterface*>(dlsym(cl, "wl_keyboard_interface"));
    I_pointer = static_cast<const WlInterface*>(dlsym(cl, "wl_pointer_interface"));
    if (!I_registry || !I_seat || !I_keyboard || !I_pointer) { std::printf("missing interfaces\n"); return 2; }

    // 2. compositor + connection
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return 2;
    void* sdisplay = wl_display_create();
    wl_client_create(sdisplay, fds[0]);
    wl_global_create(sdisplay, I_seat, 7, nullptr, seat_bind);
    std::thread server([&] {
        void* loop = wl_display_get_event_loop(sdisplay);
        while (g_run) {
            {
                std::lock_guard lk(g_q_mu);
                while (!g_q.empty()) { g_q.front()(); g_q.pop(); }
            }
            wl_event_loop_dispatch(loop, 5);
            wl_display_flush_clients(sdisplay);
        }
    });

    void* display = wl_display_connect_to_fd(fds[1]);
    CHECK(display != nullptr);
    void* registry = wl_proxy_marshal_flags(display, 1 /*get_registry*/, I_registry, 1, 0, nullptr);
    static const Fn reg_listener[2] = {F(reg_global), F(reg_remove)};
    wl_proxy_add_listener(registry, const_cast<Fn*>(reg_listener), nullptr);
    wl_display_roundtrip(display);  // globals -> bind seat
    CHECK(g_seat != nullptr);

    // 3. the game gets the keyboard + pointer and registers listeners the way SDL/GLFW do
    void* kb = wl_proxy_marshal_flags(g_seat, 1 /*get_keyboard*/, I_keyboard, 7, 0, nullptr);
    void* ptr = wl_proxy_marshal_flags(g_seat, 0 /*get_pointer*/, I_pointer, 7, 0, nullptr);
    const Fn kbl[6] = {F(k_keymap), F(k_enter), F(k_leave), F(k_key), F(k_mods), F(k_rep)};
    const Fn ptl[9] = {F(p_enter), F(p_leave), F(p_motion), F(p_button), F(p_axis), F(p_frame), F(p_src), F(p_stop), F(p_disc)};
    CHECK(wl_proxy_add_listener(kb, exact_table(kbl), &g_game_data) == 0);
    CHECK(wl_proxy_add_listener(ptr, exact_table(ptl), &g_game_data) == 0);
    CHECK(wayland_active());  // detour saw the wl_keyboard registration
    wl_display_roundtrip(display);
    CHECK(g_kb_res && g_ptr_res);

    auto pump = [&] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); wl_display_roundtrip(display); };

    // 4a. keys through the real dispatch path
    on_server([] {
        wl_resource_post_event(g_kb_res, 3, 1u, 10u, 17u /*W*/, 1u);
        wl_resource_post_event(g_kb_res, 3, 2u, 11u, 110u /*Insert*/, 1u);
        wl_resource_post_event(g_kb_res, 3, 3u, 12u, 110u, 0u);
    });
    pump();
    CHECK(S.keys.size() == 3);
    CHECK(S.last_data == &g_game_data);  // game's user_data survived the substitution
    RawInput s;
    s = poll_wayland(false, 1920, 1080);
    CHECK(s.pressed & key_bit(Key::Insert));

    // 4b. capture: game stops seeing input, but still gets the release of the held W
    set_capture_wayland(true);
    on_server([] {
        wl_resource_post_event(g_kb_res, 3, 4u, 20u, 30u /*A*/, 1u);
        wl_resource_post_event(g_kb_res, 3, 5u, 21u, 17u /*W*/, 0u);
        wl_resource_post_event(g_ptr_res, 2, 22u, 300 * 256, 200 * 256);              // motion
        wl_resource_post_event(g_ptr_res, 3, 6u, 23u, 0x110u, 1u);                    // left press
        wl_resource_post_event(g_ptr_res, 4, 24u, 0u, 20 * 256);                      // 2 notches down
    });
    pump();
    CHECK(S.keys.size() == 4 && S.keys.back()[0] == 17 && S.keys.back()[1] == 0);
    CHECK(S.motions == 0 && S.buttons.empty() && S.axes == 0);
    s = poll_wayland(true, 1920, 1080);
    CHECK(s.mouse_x == 300.0f && s.mouse_y == 200.0f && s.button[0]);
    CHECK(s.wheel < -1.9f && s.wheel > -2.1f);

    // 4c. release capture: events reach the game again
    set_capture_wayland(false);
    on_server([] { wl_resource_post_event(g_ptr_res, 2, 30u, 10 * 256, 10 * 256); });
    pump();
    CHECK(S.motions == 1);

    g_run = false;
    server.join();
    wl_display_disconnect(display);
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    std::fflush(stdout);
    std::_Exit(g_fail ? 1 : 0);  // skip libwayland-server teardown (single-process test)
}
