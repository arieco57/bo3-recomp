#include "ktime.h"

#include <ctime>

static const uint64_t s_start = KTimeHostNs();
uint64_t KTimeStartNs() { return s_start; }

// mftb in recompiled code: 50 MHz guest time base (see PPC_QUERY_TIMEBASE in ppc_context.h).
extern "C" uint64_t PPCQueryTimebase()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec)) / 20;
}
