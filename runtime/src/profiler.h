#pragma once
#include <cstdint>
// BO3_PROFILE=<start_s>,<duration_s>: sample the calling thread (see profiler.cpp).
// BO3_PROFILE_THREAD=<name> samples the guest thread of that name instead of the GPU CP thread
// (attached when the game names it, `named` = true).
void ProfilerAttachThisThread(bool named = false);
void ProfilerStartWatch();   // BO3_WATCH=<seconds>,<guest addr>
void ProfilerWatchAddress(uint32_t addr, double startDelaySeconds);   // hardware write watchpoint for 3 s
