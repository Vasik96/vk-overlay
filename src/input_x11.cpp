#include "input_backend.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>

#include "log.hpp"

// We deliberately do not include <X11/Xlib.h>: declaring the handful of entry points we
// need keeps the build free of libx11-dev and means we resolve libX11 at runtime, only
// if the process is actually an X11/XWayland client.
namespace {

using Display = void;
using Window = unsigned long;
using KeySym = unsigned long;
using KeyCode = unsigned char;

constexpr unsigned kButton1Mask = 1u << 8, kButton2Mask = 1u << 9, kButton3Mask = 1u << 10;
constexpr unsigned long kPointerEvents = (1ul << 2) | (1ul << 3) | (1ul << 6);  // press|release|motion

struct X {
    void* lib = nullptr;
    Display* dpy = nullptr;
    bool tried = false;

    Display* (*XOpenDisplay)(const char*) = nullptr;
    int (*XCloseDisplay)(Display*) = nullptr;
    int (*XQueryKeymap)(Display*, char[32]) = nullptr;
    KeyCode (*XKeysymToKeycode)(Display*, KeySym) = nullptr;
    int (*XQueryPointer)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned*) = nullptr;
    int (*XGetInputFocus)(Display*, Window*, int*) = nullptr;
    Window (*XDefaultRootWindow)(Display*) = nullptr;
    int (*XGrabPointer)(Display*, Window, int, unsigned, int, int, Window, unsigned long, unsigned long) = nullptr;
    int (*XUngrabPointer)(Display*, unsigned long) = nullptr;
    int (*XPending)(Display*) = nullptr;
    int (*XNextEvent)(Display*, void*) = nullptr;
    int (*XFlush)(Display*) = nullptr;

    KeyCode keycode[32] = {};   // indexed by Key; 0 = not on this keyboard layout
    vko::input::KeyMask prev_down = 0;
    bool grabbed = false;
    bool want_grab_env = false;
};

X g;

template <class F>
bool load(F& out, const char* name) {
    out = reinterpret_cast<F>(dlsym(g.lib, name));
    return out != nullptr;
}

bool ensure_connected() {
    if (g.dpy) return true;
    if (g.tried) return false;  // don't retry every frame if there is no X server
    g.tried = true;

    if (!std::getenv("DISPLAY")) {
        VKO_VLOG("no DISPLAY - overlay input disabled (native Wayland?)");
        return false;
    }
    // RTLD_NOLOAD: only proceed if the host process already uses libX11 (SDL/GLFW/...).
    // This avoids pulling X11 into a pure-Wayland process and avoids version mismatches.
    g.lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (!g.lib) g.lib = dlopen("libX11.so.6", RTLD_NOW);
    if (!g.lib) return false;

    bool ok = load(g.XOpenDisplay, "XOpenDisplay") && load(g.XCloseDisplay, "XCloseDisplay") &&
              load(g.XQueryKeymap, "XQueryKeymap") && load(g.XKeysymToKeycode, "XKeysymToKeycode") &&
              load(g.XQueryPointer, "XQueryPointer") && load(g.XGetInputFocus, "XGetInputFocus") &&
              load(g.XDefaultRootWindow, "XDefaultRootWindow") && load(g.XGrabPointer, "XGrabPointer") &&
              load(g.XUngrabPointer, "XUngrabPointer") && load(g.XPending, "XPending") &&
              load(g.XNextEvent, "XNextEvent") && load(g.XFlush, "XFlush");
    if (!ok) return false;

    // A *private* connection: its own socket, own event queue, own sequence numbers.
    // Nothing we do here can disturb the host's connection state.
    g.dpy = g.XOpenDisplay(nullptr);
    if (!g.dpy) return false;
    // Resolve every watched key to the X keycode of the live keymap. Which key means what is
    // NOT decided here - see input.cpp.
    size_t n = 0;
    const vko::input::KeyInfo* table = vko::input::key_table(&n);
    for (size_t i = 0; i < n; ++i) g.keycode[static_cast<unsigned>(table[i].key)] = g.XKeysymToKeycode(g.dpy, table[i].keysym);
    g.want_grab_env = std::getenv("VKOVERLAY_GRAB") != nullptr;
    VKO_VLOG("X11 input connected (%zu keys watched)", n);
    return true;
}

inline bool key_down(const char keys[32], KeyCode kc) {
    return kc != 0 && (keys[kc >> 3] & (1 << (kc & 7)));
}

}  // namespace

namespace vko::input {

RawInput poll_x11(bool capturing) {
    RawInput s;
    if (!ensure_connected()) return s;
    s.available = true;
    s.backend = "x11";

    // Global keyboard state (works regardless of which window has focus). Report which watched
    // keys are down and which just went down; the controller decides what that means.
    char keys[32];
    g.XQueryKeymap(g.dpy, keys);
    KeyMask down = 0;
    for (unsigned k = 1; k < static_cast<unsigned>(Key::Count); ++k)
        if (key_down(keys, g.keycode[k])) down |= key_bit(static_cast<Key>(k));
    s.down = down;
    s.pressed = down & ~g.prev_down;  // edge detection: one event per physical press
    g.prev_down = down;

    // Drain anything delivered to our private connection (only non-empty while grabbing).
    // If we never read it, Xlib would buffer it forever.
    alignas(8) char ev[192];  // sizeof(XEvent) is 192 on LP64
    while (g.XPending(g.dpy) > 0) g.XNextEvent(g.dpy, ev);

    if (capturing) {
        Window focus = 0;
        int revert = 0;
        g.XGetInputFocus(g.dpy, &focus, &revert);
        if (focus <= 1) focus = g.XDefaultRootWindow(g.dpy);  // None / PointerRoot
        Window root_ret, child_ret;
        int rx, ry, wx = 0, wy = 0;
        unsigned mask = 0;
        if (g.XQueryPointer(g.dpy, focus, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask)) {
            s.mouse_x = static_cast<float>(wx);
            s.mouse_y = static_cast<float>(wy);
            s.button[0] = mask & kButton1Mask;
            s.button[1] = mask & kButton3Mask;  // X: 2 = middle, 3 = right; ImGui: 1 = right, 2 = middle
            s.button[2] = mask & kButton2Mask;
        }
    }
    return s;
}

void set_capture_x11(bool on) {
    if (!g.dpy || !g.want_grab_env) return;
    if (on && !g.grabbed) {
        Window focus = 0;
        int revert = 0;
        g.XGetInputFocus(g.dpy, &focus, &revert);
        if (focus <= 1) return;
        // GrabModeAsync = 1. owner_events=False: all pointer events go to *us*.
        const int r = g.XGrabPointer(g.dpy, focus, 0, static_cast<unsigned>(kPointerEvents), 1, 1, 0, 0, 0);
        g.grabbed = (r == 0);  // GrabSuccess
        g.XFlush(g.dpy);
    } else if (!on && g.grabbed) {
        g.XUngrabPointer(g.dpy, 0);
        g.XFlush(g.dpy);
        g.grabbed = false;
    }
}

void shutdown_x11() {
    if (g.dpy) {
        set_capture_x11(false);  // never leave the pointer grabbed behind us
        g.XCloseDisplay(g.dpy);
        g.dpy = nullptr;
    }
    g.tried = false;
}

}  // namespace vko::input
