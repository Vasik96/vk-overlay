cmake_minimum_required(VERSION 3.28)
project(vk_overlay LANGUAGES C CXX)

# --------------------------------------------------------------------------------------
# Notes on language level
#   * Our own code is written to C++20.
#   * safetyhook (inline hooking engine) uses std::expected, i.e. it needs C++23 library
#     support (GCC >= 12 / Clang >= 16 with libstdc++ 12+). Because safetyhook publishes
#     that requirement on its target, anything linking it is built as C++23. That is a
#     superset of C++20, so nothing in our sources depends on 23-only features.
#   * Target architecture: x86 / x86_64 Linux only (safetyhook + Zydis limitation).
# --------------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)       # export nothing by default
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- Vulkan headers ------------------------------------------------------------------
# We only need *headers*: every Vulkan entry point is resolved at runtime from the
# loader that the target process already has mapped, so we never link libvulkan.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
  set(VKO_VULKAN_INCLUDE "${Vulkan_INCLUDE_DIRS}")
else()
  message(STATUS "System Vulkan headers not found - fetching Vulkan-Headers")
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.4.309
    GIT_SHALLOW    TRUE)
  FetchContent_GetProperties(vulkan_headers)
  if(NOT vulkan_headers_POPULATED)
    FetchContent_Populate(vulkan_headers)
  endif()
  set(VKO_VULKAN_INCLUDE "${vulkan_headers_SOURCE_DIR}/include")
endif()

# ---- Dear ImGui ----------------------------------------------------------------------
# Pinned: the Vulkan backend's InitInfo layout changed in 1.92 (RenderPass moved into
# PipelineInfoMain). The code in renderer.cpp targets 1.91.x.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9
  GIT_SHALLOW    TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
endif()

add_library(vko_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(vko_imgui PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${VKO_VULKAN_INCLUDE})
# The ImGui backend must NOT call vk* through link-time symbols: it gets its function
# pointers from ImGui_ImplVulkan_LoadFunctions() (see renderer.cpp). This also guarantees
# the backend can never accidentally re-enter our own hooks via PLT symbols.
target_compile_definitions(vko_imgui PUBLIC
  IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1
  VK_NO_PROTOTYPES=1)

# ---- safetyhook (+ Zydis) ------------------------------------------------------------
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
  GIT_REPOSITORY https://github.com/cursey/safetyhook.git
  GIT_TAG        origin/main)          # TODO: pin to a commit hash for reproducible builds
FetchContent_MakeAvailable(safetyhook)

# ---- the overlay library -------------------------------------------------------------
add_library(vk_overlay SHARED
  src/main.cpp
  src/vk_hooks.cpp
  src/renderer.cpp
  src/overlay_ui.cpp
  src/input.cpp
  src/input_keys.cpp
  src/process_filter.cpp
  src/input_x11.cpp
  src/input_wayland.cpp
  src/input_wayland_hook.cpp)

find_package(Threads REQUIRED)
target_include_directories(vk_overlay PRIVATE src ${VKO_VULKAN_INCLUDE})
target_link_libraries(vk_overlay PRIVATE vko_imgui safetyhook::safetyhook ${CMAKE_DL_LIBS} Threads::Threads)

target_compile_options(vk_overlay PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-plt)
# -fno-plt/-fvisibility=hidden keep our own symbols out of the target's symbol namespace.
set_target_properties(vk_overlay PROPERTIES
  OUTPUT_NAME "vk_overlay"
  PREFIX "lib"
  # Do not let the dynamic linker bind *our* internal calls to anything in the host app.
  LINK_FLAGS "-Wl,-Bsymbolic -Wl,--exclude-libs,ALL -Wl,-z,defs")

# Make the library self-contained: libstdc++/libgcc are linked statically so we never
# collide with (or depend on) whatever C++ runtime the target process loaded. Games
# shipped with an old bundled libstdc++ are the classic crash source for injected libs.
option(VKO_STATIC_RUNTIME "Statically link libstdc++/libgcc into the overlay" ON)
if(VKO_STATIC_RUNTIME)
  target_link_options(vk_overlay PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- tests (optional): cmake -DVKO_BUILD_TESTS=ON ------------------------------------
option(VKO_BUILD_TESTS "Build input-path tests" OFF)
if(VKO_BUILD_TESTS)
  # Pure-logic test of the Wayland listener wrappers (ASan/UBSan catch over-reads).
  add_executable(test_wayland_logic tests/test_wayland_logic.cpp src/input_wayland.cpp src/input_keys.cpp)
  target_include_directories(test_wayland_logic PRIVATE src)
  target_compile_options(test_wayland_logic PRIVATE -fsanitize=address,undefined -g)
  target_link_options(test_wayland_logic PRIVATE -fsanitize=address,undefined)
  # Integration test against the system's real libwayland-client/-server (fake compositor).
  add_executable(test_wayland_e2e tests/test_wayland_e2e.cpp src/input_wayland.cpp src/input_wayland_hook.cpp src/input_keys.cpp)
  target_include_directories(test_wayland_e2e PRIVATE src)
  target_link_libraries(test_wayland_e2e PRIVATE safetyhook::safetyhook ${CMAKE_DL_LIBS} Threads::Threads)
  # The unified controller (hotkey policy, mode cycle, capture) against a scripted fake backend.
  add_executable(test_input_controller tests/test_input_controller.cpp src/input.cpp src/input_keys.cpp)
  target_include_directories(test_input_controller PRIVATE src)
  target_compile_options(test_input_controller PRIVATE -fsanitize=address,undefined -g)
  target_link_options(test_input_controller PRIVATE -fsanitize=address,undefined)
  add_executable(test_process_filter tests/test_process_filter.cpp src/process_filter.cpp)
  target_include_directories(test_process_filter PRIVATE src)
  add_test(NAME process_filter COMMAND test_process_filter)
  enable_testing()
  add_test(NAME input_controller COMMAND test_input_controller)
  add_test(NAME wayland_logic COMMAND test_wayland_logic)
  # Per-proxy wrapper contexts are immortal by design (libwayland keeps the pointer for the proxy's lifetime) and
  # the test never frees its listener tables, so leak detection would only report intended allocations.
  set_tests_properties(wayland_logic PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  add_test(NAME wayland_e2e COMMAND test_wayland_e2e)
endif()
