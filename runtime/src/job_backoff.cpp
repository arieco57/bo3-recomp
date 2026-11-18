// The game's job system keeps jobs that wait for a dependency in the queues: a worker takes one,
// sees it is not ready (or not meant for it) and puts it back - and takes the next. With nothing
// ready the workers do just that: on a phone in Freerun ~400k jobs a second put back against ~40k
// queued, each a handful of atomics on cache lines shared by every core. On the console that only
// cost hardware threads nobody else used; here it keeps two cores busy, takes them from the game's
// Main / Backend / Server threads and heats a phone up (lower clocks).
// After a run of jobs put back without one finishing, a thread yields, and after a longer run sleeps
// a moment (Adreno 618, Zombies in game: 20 -> 23.6 fps; JQ workers 63% -> 30% of a core each).
// BO3_JQ_BACKOFF=<yield after>,<sleep after>,<sleep us> (default 64,256,50; 0 = off).
// BO3_JQ_STATS=1 logs the queue traffic every 2 s.
#include "job_backoff.h"
#include "log.h"
#include "thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace
{
struct Config
{
    uint32_t yieldAfter = 64, sleepAfter = 256, sleepUs = 50;
    bool stats = false;
    Config()
    {
        if (const char* e = getenv("BO3_JQ_BACKOFF"))
        {
            if (atoi(e) == 0) yieldAfter = sleepAfter = UINT32_MAX;
            else sscanf(e, "%u,%u,%u", &yieldAfter, &sleepAfter, &sleepUs);
        }
        stats = getenv("BO3_JQ_STATS") != nullptr;
    }
};
const Config& Cfg() { static Config c; return c; }

thread_local uint32_t t_streak = 0;   // jobs put back since one last finished
thread_local bool t_putBack = false;  // the current job went back into a queue

std::atomic<uint64_t> s_searches{ 0 }, s_found{ 0 }, s_putBack{ 0 }, s_new{ 0 }, s_yields{ 0 }, s_sleeps{ 0 };

void Stats()
{
    static std::atomic<int64_t> last{ 0 };
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t l = last.load(std::memory_order_relaxed);
    if (now - l < 2000 || !last.compare_exchange_strong(l, now)) return;
    auto per = [](std::atomic<uint64_t>& a) { return (unsigned long long)(a.exchange(0) / 2); };
    unsigned long long se = per(s_searches), fo = per(s_found), pb = per(s_putBack), ne = per(s_new), yi = per(s_yields), sl = per(s_sleeps);
    Log("jobs/s: %llu searches (%llu found), %llu put back, %llu other pushes; backoff %llu yields, %llu sleeps", se, fo, pb, ne, yi, sl);
}
}

void JobBeforeSearch()
{
    // the previous job finished (it was not put back): the workers are making progress
    if (!t_putBack) t_streak = 0;
    t_putBack = false;
}

void JobAfterSearch(bool found)
{
    // jobs the search itself put back (meant for another worker) count in the streak, but whether
    // the job it took finishes decides the next reset
    t_putBack = false;
    if (!Cfg().stats) return;
    s_searches++;
    if (found) s_found++;
    Stats();
}

void JobAfterPush(bool putBack)
{
    const Config& c = Cfg();
    if (c.stats) (putBack ? s_putBack : s_new)++;
    if (!putBack) return;
    t_putBack = true;
    if (++t_streak < c.yieldAfter) return;
    if (t_streak < c.sleepAfter)
    {
        if (c.stats) s_yields++;
        std::this_thread::yield();
    }
    else
    {
        if (c.stats) s_sleeps++;
        std::this_thread::sleep_for(std::chrono::microseconds(c.sleepUs));
    }
}
