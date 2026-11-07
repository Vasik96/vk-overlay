// log.hpp - tiny stderr logger. Deliberately avoids iostream/locale machinery and any
// allocation so it is safe to call from hook detours, constructors and destructors.
#pragma once
#include <fcntl.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace vko {

inline bool verbose_enabled() {
    // Evaluated once; getenv is not guaranteed thread-safe against setenv, but the
    // first call happens in our constructor while the process is still single-threaded.
    static const bool v = std::getenv("VKOVERLAY_VERBOSE") != nullptr;
    return v;
}

// VKOVERLAY_LOGFILE=/tmp/vko.log  ->  every line is ALSO appended to /tmp/vko.log.<pid>.
// Proton/Wine run dozens of processes and Steam hides their stderr; one file per process shows
// exactly which of them loaded the overlay and what each one did.
inline int log_file_fd() {
    static const int fd = [] {
        const char* base = std::getenv("VKOVERLAY_LOGFILE");
        if (!base || !*base) return -1;
        char path[512];
        std::snprintf(path, sizeof path, "%s.%d", base, static_cast<int>(::getpid()));
        return ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }();
    return fd;
}

__attribute__((format(printf, 1, 2)))
inline void log(const char* fmt, ...) {
    char buf[512];
    int n = std::snprintf(buf, sizeof buf, "[vkoverlay:%d] ", static_cast<int>(::getpid()));
    va_list ap;
    va_start(ap, fmt);
    n += std::vsnprintf(buf + n, sizeof buf - static_cast<size_t>(n) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<size_t>(n) > sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    // One write(2) => lines from different threads don't interleave mid-line, and we
    // don't touch stdio's locks (the host app may hold them when we're called).
    (void)!::write(STDERR_FILENO, buf, static_cast<size_t>(n));
    if (const int fd = log_file_fd(); fd >= 0) (void)!::write(fd, buf, static_cast<size_t>(n));
}

}  // namespace vko

#define VKO_LOG(...) ::vko::log(__VA_ARGS__)
#define VKO_VLOG(...) do { if (::vko::verbose_enabled()) ::vko::log(__VA_ARGS__); } while (0)
