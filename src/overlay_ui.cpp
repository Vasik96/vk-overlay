#include "overlay_ui.hpp"
#include "input.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <imgui.h>

namespace vko::ui {

namespace {

// Reads a whole small /proc file into buf using raw syscalls (no stdio => no locks, no
// allocation). Returns bytes read or -1.
ssize_t slurp(const char* path, char* buf, size_t cap) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = ::read(fd, buf, cap - 1);
    ::close(fd);
    if (n >= 0) buf[n] = '\0';
    return n;
}

const char* format_name(int f) {
    // Only the formats that realistically show up as swapchain formats.
    switch (f) {
        case 37: return "R8G8B8A8_UNORM";
        case 43: return "R8G8B8A8_SRGB";
        case 44: return "B8G8R8A8_UNORM";
        case 50: return "B8G8R8A8_SRGB";
        case 64: return "A2B10G10R10_UNORM";
        case 58: return "A2R10G10B10_UNORM";
        case 97: return "R16G16B16A16_SFLOAT";
        default: return "other";
    }
}

}  // namespace

void Telemetry::on_frame(double now) {
    if (start_ == 0.0) start_ = now;
    uptime_s = now - start_;
    if (last_frame_ != 0.0) {
        dt_ = now - last_frame_;
        const float ms = static_cast<float>(dt_ * 1000.0);
        frame_ms[head] = ms;
        head = (head + 1) % kHistory;
        if (count < kHistory) ++count;
        // FPS = frames presented / wall time, over a 0.5 s window (what MangoHud and in-game
        // counters do). Averaging per-frame 1/dt values instead overestimates whenever frame
        // times vary (mean of 1/x > 1/mean of x), which is why the first version read ~100 FPS
        // too high in CS2.
        if (win_start_ == 0.0) win_start_ = last_frame_;
        ++win_frames_;
        const double span = now - win_start_;
        if (span >= 0.5) {
            fps_smoothed = static_cast<double>(win_frames_) / span;
            win_frames_ = 0;
            win_start_ = now;
        }
    }
    last_frame_ = now;
}

void Telemetry::reset_history() {
    std::memset(frame_ms, 0, sizeof frame_ms);
    head = count = 0;
    fps_smoothed = 0.0;
    win_frames_ = 0;
    win_start_ = 0.0;
}

void Telemetry::sample_system(double now) {
    if (now - last_sys_wall_ < 0.5) return;

    char buf[1024];
    // /proc/self/stat: "pid (comm) state ppid ... utime(14) stime(15) ... num_threads(20)"
    // comm can contain spaces and parentheses, so anchor on the *last* ')'.
    if (slurp("/proc/self/stat", buf, sizeof buf) > 0) {
        if (const char* p = std::strrchr(buf, ')')) {
            char state;
            long ppid, pgrp, session, tty, tpgid;
            unsigned long flags, minflt, cminflt, majflt, cmajflt, utime, stime;
            long cutime, cstime, prio, nice, nthreads;
            if (std::sscanf(p + 2, "%c %ld %ld %ld %ld %ld %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld",
                            &state, &ppid, &pgrp, &session, &tty, &tpgid, &flags, &minflt, &cminflt,
                            &majflt, &cmajflt, &utime, &stime, &cutime, &cstime, &prio, &nice,
                            &nthreads) == 18) {
                threads = static_cast<int>(nthreads);
                const unsigned long long ticks = utime + stime;
                const double hz = static_cast<double>(::sysconf(_SC_CLK_TCK));
                if (have_cpu_ && now > last_sys_wall_ && hz > 0) {
                    const double cpu_s = static_cast<double>(ticks - last_cpu_ticks_) / hz;
                    cpu_percent = 100.0 * cpu_s / (now - last_sys_wall_);
                }
                last_cpu_ticks_ = ticks;
                have_cpu_ = true;
            }
        }
    }
    // /proc/self/statm: "size resident shared ..." in pages.
    if (slurp("/proc/self/statm", buf, sizeof buf) > 0) {
        unsigned long size = 0, resident = 0;
        if (std::sscanf(buf, "%lu %lu", &size, &resident) == 2)
            rss_mib = static_cast<double>(resident) * static_cast<double>(::sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
    }
    last_sys_wall_ = now;
}

bool draw(Telemetry& t, const SceneInfo& scene, bool settings_mode, Settings& settings, const KeyHints& hints) {
    static char comm[64] = {};
    if (!comm[0]) {
        if (slurp("/proc/self/comm", comm, sizeof comm) > 0) {
            if (char* nl = std::strchr(comm, '\n')) *nl = '\0';
        }
        if (!comm[0]) std::snprintf(comm, sizeof comm, "?");
    }

    // ---- statistics over the history window -----------------------------------------
    float avg = 0, mn = FLT_MAX, mx = 0, p99 = 0;
    if (t.count > 0) {
        float tmp[Telemetry::kHistory];
        for (int i = 0; i < t.count; ++i) {
            const float v = t.frame_ms[i];
            tmp[i] = v;
            avg += v; mn = std::min(mn, v); mx = std::max(mx, v);
        }
        avg /= static_cast<float>(t.count);
        const int k = std::min(t.count - 1, static_cast<int>(static_cast<float>(t.count) * 0.99f));
        std::nth_element(tmp, tmp + k, tmp + t.count);
        p99 = tmp[k];
    } else {
        mn = 0;
    }

    // The HUD is deliberately a passive, click-through window in BOTH visible modes.
    // Controls live in the separate settings window below, so the HUD never changes size
    // or steals clicks when the user enters interaction mode.
    ImGui::SetNextWindowBgAlpha(settings.hud_opacity);
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    const ImGuiWindowFlags hud_flags = ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar;

    if (ImGui::Begin("Performance HUD", nullptr, hud_flags)) {
        const ImVec4 good(0.4f, 1.0f, 0.5f, 1), warn(1.0f, 0.85f, 0.3f, 1), bad(1.0f, 0.4f, 0.4f, 1);
        const double fps = t.fps_smoothed;
        if (settings.show_fps) {
            ImGui::TextColored(fps >= 55 ? good : fps >= 30 ? warn : bad, "%6.1f FPS", fps);
            ImGui::SameLine();
            ImGui::Text("  %5.2f ms", fps > 0.0 ? 1000.0 / fps : 0.0);
        }

        if (settings.show_graph && t.count > 1) {
            ImGui::PlotLines("##frametime", t.frame_ms, Telemetry::kHistory, t.head, nullptr, 0.0f,
                             std::max(mx * 1.15f, 8.0f), ImVec2(260, 56));
        }
        if (settings.show_fps) {
            ImGui::Text("avg %.2f  min %.2f  max %.2f ms", avg, mn, mx);
            if (p99 > 0.0f) ImGui::Text("1%% low: %.1f FPS", 1000.0f / p99);
        }

        if (settings.show_display_info || settings.show_gpu_info || settings.show_system_info)
            ImGui::Separator();
        if (settings.show_display_info) {
            ImGui::Text("Display %s", input::display_server_name());
            ImGui::Text("SC    %ux%u  %s  x%u", scene.width, scene.height, format_name(scene.format), scene.swapchain_images);
        }
        if (settings.show_gpu_info) {
            ImGui::Text("GPU   %s", scene.gpu_name);
            ImGui::Text("API   Vulkan %u.%u.%u", (scene.vk_api_version >> 22) & 0x7f,
                        (scene.vk_api_version >> 12) & 0x3ff, scene.vk_api_version & 0xfff);
        }
        if (settings.show_system_info) {
            ImGui::Separator();
            ImGui::Text("Proc  %s (pid %d)", comm, static_cast<int>(::getpid()));
            ImGui::Text("CPU   %.0f%%   RSS %.0f MiB   %d threads", t.cpu_percent, t.rss_mib, t.threads);
            ImGui::Text("Up    %02d:%02d:%02d", static_cast<int>(t.uptime_s) / 3600,
                        static_cast<int>(t.uptime_s) / 60 % 60, static_cast<int>(t.uptime_s) % 60);
        }

        ImGui::Separator();
        ImGui::Text("Version: %s", "1.4");
    }
    ImGui::End();

    // A genuinely separate ImGui window, rendered in the same Vulkan frame. It only exists
    // while the hotkey has selected interaction mode; no native platform window is created.
    // A normal ImGui window: title bar with an X button, draggable, resizable from its edges.
    bool closed = false;
    if (settings_mode) {
        bool open = true;
        ImGui::SetNextWindowPos(ImVec2(320, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340, 360), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(260, 160), ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::Begin("VK Overlay Settings", &open, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextDisabled("Interaction mode");
            ImGui::Separator();
            ImGui::Text("HUD content");
            ImGui::Checkbox("Show FPS and frame times", &settings.show_fps);
            ImGui::Checkbox("Frame-time graph", &settings.show_graph);
            ImGui::Checkbox("Display and swapchain info", &settings.show_display_info);
            ImGui::Checkbox("GPU and Vulkan info", &settings.show_gpu_info);
            ImGui::Checkbox("Process and system info", &settings.show_system_info);
            ImGui::Spacing();
            ImGui::Text("HUD appearance");
            ImGui::SliderFloat("Background opacity", &settings.hud_opacity, 0.10f, 1.0f, "%.2f");
            ImGui::Spacing();
            if (ImGui::Button("Reset statistics")) t.reset_history();
            ImGui::SameLine();
            if (ImGui::Button("Reset settings")) settings = Settings{};
            ImGui::Separator();
            ImGui::TextDisabled("%s: next mode (HUD -> hidden -> interaction)", hints.cycle);
        }
        ImGui::End();
        closed = !open;
    }
    return closed;
}

}  // namespace vko::ui
