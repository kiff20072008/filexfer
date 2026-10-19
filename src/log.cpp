#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace filexfer {

namespace {
bool g_progress_active = false;   // на экране строка прогресса — перед сообщением перейти на новую строку
}

void log_msg(const char* role, const char* fmt, ...) {
    if (g_progress_active) { std::printf("\n"); g_progress_active = false; }
    time_t t = time(nullptr);
    tm tmv;
    localtime_r(&t, &tmv);
    char ts[16];
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    std::printf("[%s] [%s] ", ts, role);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
}

void log_progress(const char* fmt, ...) {
    std::printf("\r");
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
    g_progress_active = true;
}

}  // namespace filexfer
