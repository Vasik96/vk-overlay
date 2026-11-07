// overlay_ui.hpp - telemetry collection, passive HUD, and interaction-mode settings window.
// Pure ImGui + libc: knows nothing about Vulkan handles, so it is trivially testable.
#pragma once

#include <cstdint>

namespace vko::ui {

// Everything the window wants to print about the Vulkan side. Filled by the renderer.
struct SceneInfo {
    const char* gpu_name = "unknown";
    uint32_t    vk_api_version = 0;       // VK_MAKE_API_VERSION packed
    uint32_t    width = 0, height = 0;    // swapchain extent
    int         format = 0;               // VkFormat as int
    uint32_t    swapchain_images = 0;
};

// Names of the keys bound by the input controller, shown in the settings window.
struct KeyHints {
    const char* cycle = "Insert";
};

struct Settings {
    bool show_fps = true;
    bool show_graph = true;
    bool show_display_info = true;
    bool show_gpu_info = true;
    bool show_system_info = true;
    float hud_opacity = 0.70f;
};

class Telemetry {
public:
    static constexpr int kHistory = 240;  // frames kept for the graph / percentile stats

    // Call once per presented frame. `now` is monotonic seconds.
    void on_frame(double now);
    // Re-reads /proc at most every 500 ms; cheap to call every frame.
    void sample_system(double now);
    void reset_history();

    double last_frame_dt() const { return dt_; }

    // --- public so the draw code can read them; written only by on_frame/sample_system
    float  frame_ms[kHistory] = {};
    int    head = 0;                      // next slot to write (ring buffer)
    int    count = 0;                     // valid samples
    double fps_smoothed = 0.0;
    double cpu_percent = 0.0;             // this process, all threads, 100 == one full core
    double rss_mib = 0.0;
    int    threads = 0;
    double uptime_s = 0.0;

private:
    double dt_ = 0.0, last_frame_ = 0.0, start_ = 0.0;
    double win_start_ = 0.0;      // start of the current FPS measurement window
    unsigned win_frames_ = 0;     // frames presented in it
    double last_sys_wall_ = 0.0;
    unsigned long long last_cpu_ticks_ = 0;
    bool   have_cpu_ = false;
};

// Draws the passive HUD and, only in interaction mode, a separate settings window.
// Returns true when the user closed that window with its X button (caller leaves interaction mode).
// Must be called between ImGui::NewFrame() and Render().
bool draw(Telemetry& t, const SceneInfo& scene, bool settings_mode, Settings& settings, const KeyHints& hints);

}  // namespace vko::ui
