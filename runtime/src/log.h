#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// Thread-safe-enough logging (single fprintf per line). Prefix shows the guest thread id.
void LogImpl(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define Log(...)     LogImpl("I", __VA_ARGS__)
#define LogWarn(...) LogImpl("W", __VA_ARGS__)
#define LogError(...) LogImpl("E", __VA_ARGS__)
#define Fatal(...)   do { LogImpl("F", __VA_ARGS__); std::abort(); } while (0)

extern bool g_logImports;   // --trace: log every import call

// Leave the process immediately: guest threads are still running, so static destructors
// (handle table, etc.) must not run.
[[noreturn]] void QuickExit(int code);
