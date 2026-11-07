#include "process_filter.hpp"

#include <strings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vko {
namespace {

bool is_wine_loader(const char* comm) { return strncasecmp(comm, "wine", 4) == 0; }

// Processes that load a preloaded library but can never present a Vulkan frame. Skipping them
// avoids pointless libvulkan loads and hook patching in every shell, runtime helper and
// Steam process of a launch.
bool is_infrastructure(const char* comm) {
    static const char* const kSkip[] = {
        "steam",   "steamwebhelper", "steamerrorrepor", "bash", "sh", "dash", "zsh", "fish", "python", "python3",
        "env",     "bwrap",          "srt-bwrap",       "pv-adverb", "pressure-vessel", "reaper", "wineserver",
        "xdg-desktop-por", "dbus-daemon", "pulseaudio", "pipewire",
    };
    for (const char* name : kSkip)
        if (strcasecmp(comm, name) == 0) return true;
    return false;
}

}  // namespace

void current_comm(char (&out)[32]) {
    out[0] = '\0';
    if (FILE* f = std::fopen("/proc/self/comm", "re")) {
        if (!std::fgets(out, sizeof out, f)) out[0] = '\0';
        std::fclose(f);
    }
    if (char* nl = std::strchr(out, '\n')) *nl = '\0';
}

bool process_name_matches_list(const char* comm, const char* list) {
    const char* p = list;
    while (p && *p) {
        while (*p == ' ' || *p == ',') ++p;
        const char* end = p;
        while (*end && *end != ',') ++end;
        const char* stop = end;
        while (stop > p && stop[-1] == ' ') --stop;
        size_t n = static_cast<size_t>(stop - p);
        if (n > 15) n = 15;  // comm is truncated by the kernel
        if (n > 0 && std::strlen(comm) == n && strncasecmp(comm, p, n) == 0) return true;
        p = end;
    }
    return false;
}

bool process_allowed(bool at_load) {
    if (std::getenv("VKOVERLAY_DISABLE")) return false;
    char comm[32];
    current_comm(comm);

    if (const char* want = std::getenv("VKOVERLAY_PROCESS"); want && *want) {
        if (process_name_matches_list(comm, want)) return true;
        return at_load && is_wine_loader(comm);  // final name not set yet: decide again at the first frame
    }
    return !is_infrastructure(comm);
}

}  // namespace vko
