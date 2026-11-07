# Stability guide

## Build and run

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
    LD_PRELOAD=$PWD/build/libvk_overlay.so vkcube
    # Steam: set the launch option  LD_PRELOAD=/path/libvk_overlay.so %command%

**Insert** is the only hotkey: HUD -> hidden -> interaction -> HUD ... Interaction mode shows a
normal, resizable settings window (X button = back to HUD) and captures mouse/keyboard for the
overlay; the other modes never capture. Rebind with `VKOVERLAY_KEY=<F1-F12|Insert|Delete|Home|End|
PageUp|PageDown|Pause|ScrollLock>`. Theme: ImGui Classic, square corners.

Env vars: `VKOVERLAY_VERBOSE=1` (log), `VKOVERLAY_LOGFILE=/tmp/vko.log` (also write each process's
log to `/tmp/vko.log.<pid>`), `VKOVERLAY_DISABLE=1`, `VKOVERLAY_START_HIDDEN=1`,
`VKOVERLAY_PROCESS=a,b` (comma-separated process names, case-insensitive, first 15 characters
compared), `VKOVERLAY_GRAB=1` (best-effort X11 pointer grab in settings mode),
`VKOVERLAY_LIBVULKAN=<path>`, `VKOVERLAY_SCALE`, `VKOVERLAY_NO_WAYLAND=1`.

## Proton / Wine

Vulkan calls come from inside the Wine process (DXVK/VKD3D -> winevulkan -> host libvulkan), so the
preload works there, but a launch spawns many processes (launcher, services, the game). Notes:
* Wine processes are named `wine64-preload` when the library loads and get their final name later,
  so `VKOVERLAY_PROCESS` is enforced at the first presented frame, not only at load.
* Launchers (Rockstar launcher etc.) that use Vulkan/D3D get their own overlay unless filtered:
  `VKOVERLAY_PROCESS=GTA5.exe`.
* Short-lived VkInstances (DXVK probing) are tracked and dropped on `vkDestroyInstance`.
* Games that render with OpenGL (e.g. cocos2d-x titles such as Geometry Dash, unverified) never call
  Vulkan, so there is nothing to hook. `VKOVERLAY_VERBOSE=1` shows whether any Vulkan present occurs.
* Diagnose with `VKOVERLAY_VERBOSE=1 VKOVERLAY_LOGFILE=/tmp/vko.log` and look at the per-pid files.
* Only for offline/single-player use; never with anti-cheat.

Requirements: x86/x86_64 Linux, GCC >= 12 or Clang >= 16 (safetyhook needs `std::expected`),
CMake >= 3.28. Own code is C++20; the target builds as C++23 because safetyhook demands it.

## Injection timing and "late injection"

Recommended workflow when you may want the overlay mid-session: always launch with
`LD_PRELOAD=... VKOVERLAY_START_HIDDEN=1`. It stays invisible (only a cheap per-frame timestamp
and a 60 Hz key poll) until you press **Insert**.

Attaching to an already-running process (gdb/ptrace + dlopen) is deliberately NOT supported:
1. Vulkan cannot report the `VkDevice`/`VkInstance` behind an existing queue or swapchain, nor a
   swapchain's format/extent; only the creation calls reveal them.
2. Apps such as LWJGL call `vkGetDeviceProcAddr`, so the real entry points are driver
   functions that can only be located through a device.
3. Inline-patching code that other threads execute is not atomic, and on Linux safetyhook does
   not stop them. In a JVM with ~40 threads that is a crash lottery.
Every Vulkan overlay layer (MangoHud, Steam, ReShade) has the same constraint for the same reasons.

## Injection timing (the most important rule)

Load with `LD_PRELOAD` (or as an early `dlopen`). The overlay can only learn
`VkInstance`/`VkDevice`/queue handles by observing `vkCreateInstance`/`vkCreateDevice`.
Vulkan has no query that returns the device behind a `VkQueue`, so a **late** injection
(ptrace + `dlopen` into a running app) sees presents for swapchains it never saw created and
stays inactive (one verbose log line explains why). Supporting that needs the app to be
re-created, or a layer-style approach (implicit Vulkan layer), which is the sturdier design
if you control installation.

## Thread safety

| Hazard | Mitigation in this code |
|---|---|
| Hook installation racing app threads | Runs in the ELF constructor (process still single-threaded under `LD_PRELOAD`); `std::call_once`. **safetyhook does not stop other threads on Linux**, so patching a running multi-threaded process is unsafe (see "Late injection"). |
| Several app threads present/destroy concurrently | One renderer mutex; **no hooked Vulkan call is made while it is held except our own fn-table calls**, which bypass the hooks. |
| Same call hits two hooked addresses (loader trampoline then ICD) | One slot per distinct address + `thread_local` re-entrancy flag; inner hook is pass-through. |
| Vulkan external synchronisation of `VkQueue` | We submit on the app's own queue *inside* its present call, i.e. on the thread that already owns that queue. We never use another queue or thread. |
| Swapchain/device destroyed while we reference it | `vkDestroySwapchainKHR`/`vkDestroyDevice` are hooked and free our objects **before** forwarding. |
| C++ exceptions crossing into C frames | Every detour wraps renderer calls in `try/catch`; failure calls `renderer::disable()`. |

## GPU synchronisation (validation-clean)

* Our submit waits on the app's present-wait semaphores; the real present then waits only on
  our semaphore. Each binary semaphore keeps exactly one signal and one wait.
* Per-image semaphore + framebuffer; per-ring-slot command buffer + fence. A slot is reused
  only after its fence signals, so command buffers and ImGui's vertex/index buffers are never
  rewritten while in flight.
* The fence is reset immediately before `vkQueueSubmit`; a failed submit disables the overlay
  permanently so nobody waits on an unsignalled fence.
* Render pass uses `loadOp=LOAD` with `PRESENT_SRC_KHR` initial/final layout, matching what
  the app leaves the image in.
* Every fence wait has a timeout (500 ms); a lost device skips the overlay instead of hanging.

## Safe unhooking (destructor path)

1. `g_enabled=false` turns every detour into a pass-through.
2. Wait (<= 2 s) for the in-flight counter to reach zero, plus a 20 ms grace period.
3. `HookSlots::remove_all()` restores original bytes (dlclose path only, NOT atomic vs. other
   threads on Linux). At process exit nothing is un-patched: hooks stay as pass-throughs.
4. `__cxa_atexit(…, nullptr)` distinguishes process exit from `dlclose`: on exit nothing GPU
   related is touched (driver may be gone); on `dlclose` we `vkDeviceWaitIdle`, free all
   objects, `ImGui_ImplVulkan_Shutdown`, and destroy the context.

Residual risk: a thread preempted for more than the grace period between passing the
enabled-check and entering the counter. For maximum safety, never `dlclose` the library;
unloading at process exit is the supported path.

## Avoiding crashes in the host

* Vulkan function pointers come from `dlsym`/`vkGet*ProcAddr` on the real loader and ImGui is
  built with `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`, so we never resolve symbols through the host's
  symbol namespace. `-Bsymbolic`, hidden visibility and `--exclude-libs` keep our symbols private.
* `libstdc++`/`libgcc` linked statically (games often ship an old bundled libstdc++).
* No `imgui.ini`, no stdio in hot paths (raw `write(2)`/`read(2)`), no per-frame heap growth.

## Input architecture

Backends (X11 polling, Wayland listener hooks) only report facts: keys down/pressed, pointer,
wheel (`RawInput`). `input.cpp` is the single controller that decides what they mean (hotkeys,
mode cycle, capture), so behaviour is identical on X11, XWayland and Wayland. Key names/codes
live in one table (`input_keys.cpp`). `tests/test_input_controller.cpp` checks this.

## Input backends (Wayland and X11)

Wayland has no global input state, so native clients cannot be polled. Instead the library
inline-hooks `wl_proxy_add_listener` in libwayland-client (installed from the constructor,
before the game registers its listeners) and wraps the game's `wl_keyboard`, `wl_pointer` and
`zwp_relative_pointer_v1` listener tables. Works with SDL3 / GLFW 3.4 native Wayland, on any
compositor (Cinnamon, GNOME, KDE, Sway), with no extra permissions.

* Hotkeys fire only while the game window has keyboard focus.
* In capture mode the game stops receiving keys, buttons, motion and scroll; releases of keys
  or buttons the game saw pressed are still forwarded, so nothing sticks.
* The overlay cursor follows absolute motion, or relative deltas when the game has locked the
  pointer (mouse-look), so capture works even then.
* Hotkeys are also delivered to the game when not capturing (they may do something in
  the game).
* Fractional scaling: pointer coordinates are logical pixels; set `VKOVERLAY_SCALE=1.5` etc.
  if the overlay cursor does not line up with the swapchain.
* Games using `wl_proxy_add_dispatcher` (Rust wayland-client) are bridged too. Not covered:
  anything that bound its keyboard before the overlay loaded (late injection); the overlay then
  stays display-only.
* `VKOVERLAY_NO_WAYLAND=1` disables the Wayland hook.

Tests: `cmake -DVKO_BUILD_TESTS=ON` builds `test_wayland_logic` (ASan/UBSan) and
`test_wayland_e2e`, which runs a fake compositor on the system's real libwayland-server and
drives the hooked libwayland-client through it. Neither replaces a real compositor test.
* X11/XWayland input uses a *private* connection and polling, so the game's event stream is
  untouched. Trade-off: without `VKOVERLAY_GRAB` the game also sees clicks made on the overlay.

## Known limitations / next steps

* Only the first swapchain that presents gets the overlay; secondary swapchains are ignored.
* Swapchain recreation (resize) destroys and lazily rebuilds the overlay resources; ImGui's
  pipeline is rebuilt then, which costs a frame.
* Not tested here against a real windowed Vulkan app (no GPU/display in the build sandbox).
  Please run it first with `vkcube` and the validation layer
  (`VK_LOADER_LAYERS_ENABLE=*validation*`) before using it on anything else.
* Do not use it on online games with anti-cheat; injected hooks can get accounts banned.
* Pin `GIT_TAG` for safetyhook to a commit hash for reproducible builds.
