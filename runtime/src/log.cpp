#include "log.h"
#include "thread.h"

#include <chrono>
#ifdef __ANDROID__
#include <android/log.h>
#include <cstdio>
#endif
#include <mutex>

bool g_logImports = false;

void LogImpl(const char* level, const char* fmt, ...)
{
    static std::mutex m;
    static auto t0 = std::chrono::steady_clock::now();
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    GuestThread* th = CurrentThread();
    std::lock_guard lock(m);
    fprintf(stderr, "[%9.4f %s t%-2u] %s\n", t, level, th ? th->id : 0, buf);
#ifdef __ANDROID__
    // stderr goes nowhere on Android: logcat (tag BO3) and bo3.log in the data folder
    __android_log_print(level[0] == 'E' || level[0] == 'F' ? ANDROID_LOG_ERROR : level[0] == 'W' ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "BO3", "%s", buf);
    static FILE* f = fopen("bo3.log", "w");
    if (f) { fprintf(f, "[%9.4f %s t%-2u] %s\n", t, level, th ? th->id : 0, buf); fflush(f); }
#endif
}

#include <unistd.h>
void QuickExit(int code)
{
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}
