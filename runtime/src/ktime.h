#pragma once
#include <chrono>
#include <cstdint>

// Xbox 360 timebase: 50 MHz (KeQueryPerformanceFrequency). System time is a FILETIME
// (100 ns units since 1601-01-01).
constexpr uint64_t kPerfFrequency = 50000000ull;
constexpr uint64_t kFiletimeUnixEpoch = 116444736000000000ull;

inline uint64_t KTimeHostNs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t KTimeStartNs();   // process start (steady clock)

inline uint64_t KTimeInterruptTime100ns() { return (KTimeHostNs() - KTimeStartNs()) / 100; }
inline uint32_t KTimeTickCountMs() { return uint32_t((KTimeHostNs() - KTimeStartNs()) / 1000000); }
inline uint64_t KTimePerfCounter() { return (KTimeHostNs() - KTimeStartNs()) / 20; } // 50 MHz
inline uint64_t KTimeSystemTime100ns()
{
    auto sys = std::chrono::system_clock::now().time_since_epoch();
    return kFiletimeUnixEpoch + (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(sys).count() / 100;
}

void KTimeUpdateBundle();   // refresh KeTimeStampBundle (kernel_vars.cpp)
