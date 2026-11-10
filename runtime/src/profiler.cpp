// Tiny sampling profiler for one thread (diagnostics).
// BO3_PROFILE=<start_s>,<duration_s>: the thread that calls ProfilerAttachThisThread() is
// sampled every 1 ms of its CPU time from start_s for duration_s seconds; the instruction
// pointers go to bo3_profile.txt with /proc/self/maps (resolve with tools/profile_report.py).
// BO3_PROFILE_STACKS=1 also records a call stack per sample (report: BO3_CALLERS=<function>).
#include "profiler.h"
#include "log.h"
#include "arch.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <dirent.h>
#if !defined(__ANDROID__)
#include <execinfo.h>
#else
static int backtrace(void**, int) { return 0; }   // (bionic has it from API 33: no stacks on Android)
#endif
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <map>
#include <vector>
#include "memory.h"

namespace
{
constexpr size_t kMax = 1 << 20;
constexpr size_t kStackMax = 1 << 16, kDepth = 16;
void** s_stacks = nullptr;   // kStackMax x kDepth (BO3_PROFILE_STACKS)
uint64_t* s_ips = nullptr;
std::atomic<size_t> s_n{ 0 };
std::atomic<bool> s_on{ false };

void OnProf(int, siginfo_t*, void* uc)
{
    if (!s_on.load(std::memory_order_relaxed)) return;
    size_t i = s_n.fetch_add(1, std::memory_order_relaxed);
    if (i < kMax) s_ips[i] = HostPc(uc);
    if (s_stacks && i < kStackMax)
    {
        void** st = s_stacks + i * kDepth;
        int n = backtrace(st, int(kDepth));
        for (int k = n; k < int(kDepth); k++) st[k] = nullptr;
    }
}
}

void ProfilerAttachThisThread(bool named)
{
    const char* e = getenv("BO3_PROFILE");
    if (!e) return;
    if (named != (getenv("BO3_PROFILE_THREAD") != nullptr)) return;
    double start = atof(e), dur = 10;
    if (const char* c = strchr(e, ',')) dur = atof(c + 1);
    s_ips = new uint64_t[kMax];
    if (getenv("BO3_PROFILE_STACKS"))
    {
        s_stacks = new void*[kStackMax * kDepth]();
        void* warm[4];
        backtrace(warm, 4);   // loads the unwinder now (not async-signal-safe the first time)
    }
    struct sigaction sa{};
    sa.sa_sigaction = OnProf;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    clockid_t clk;
    pthread_getcpuclockid(pthread_self(), &clk);
    sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = pid_t(syscall(SYS_gettid));
    timer_t timer;
    if (timer_create(clk, &sev, &timer) != 0) { LogWarn("profiler: timer_create failed"); return; }
    itimerspec its{};
    its.it_interval.tv_nsec = its.it_value.tv_nsec = 1000000;
    timer_settime(timer, 0, &its, nullptr);
    std::thread([start, dur] {
        std::this_thread::sleep_for(std::chrono::duration<double>(start));
        s_on = true;
        Log("profiler: sampling");
        std::this_thread::sleep_for(std::chrono::duration<double>(dur));
        s_on = false;
        size_t n = std::min(s_n.load(), kMax);
        FILE* f = fopen("bo3_profile.txt", "w");
        if (!f) return;
        fprintf(f, "samples %zu\n", n);
        for (size_t i = 0; i < n; i++) fprintf(f, "%lx\n", (unsigned long)s_ips[i]);
        if (s_stacks)
        {
            size_t ns = std::min(n, kStackMax);
            fprintf(f, "stacks %zu\n", ns);
            for (size_t i = 0; i < ns; i++)
            {
                for (size_t k = 0; k < kDepth && s_stacks[i * kDepth + k]; k++) fprintf(f, "%lx ", (unsigned long)s_stacks[i * kDepth + k]);
                fprintf(f, "\n");
            }
        }
        fprintf(f, "maps\n");
        if (FILE* m = fopen("/proc/self/maps", "r")) { char line[512]; while (fgets(line, sizeof(line), m)) fputs(line, f); fclose(m); }
        fclose(f);
        Log("profiler: %zu samples written to bo3_profile.txt", n);
    }).detach();
}

// BO3_WATCH=<seconds>,<guest addr hex>: hardware write watchpoint on that guest address in
// every thread for 3 seconds; logs the host instruction pointers that wrote it (resolve with
// nm: the recompiled function names carry the guest address).
void ProfilerWatchAddress(uint32_t addr, double start);
void ProfilerStartWatch()
{
    const char* e = getenv("BO3_WATCH");
    if (!e || !strchr(e, ',')) return;
    ProfilerWatchAddress(uint32_t(strtoul(strchr(e, ',') + 1, nullptr, 16)), atof(e));
}

void ProfilerWatchAddress(uint32_t addr, double start)
{
    std::thread([start, addr] {
        std::this_thread::sleep_for(std::chrono::duration<double>(start));
        struct Ev { int fd; void* ring; };
        std::vector<Ev> evs;
        DIR* dir = opendir("/proc/self/task");
        while (dirent* de = dir ? readdir(dir) : nullptr)
        {
            int tid = atoi(de->d_name);
            if (tid <= 0) continue;
            perf_event_attr a{};
            a.type = PERF_TYPE_BREAKPOINT;
            a.size = sizeof(a);
            static const bool rw = getenv("BO3_WATCH_RW") != nullptr;
            a.bp_type = rw ? HW_BREAKPOINT_RW : HW_BREAKPOINT_W;
            a.bp_addr = uint64_t(g_base + addr);
            a.bp_len = HW_BREAKPOINT_LEN_4;
            a.sample_period = 1;
            a.sample_type = PERF_SAMPLE_IP;
            a.exclude_kernel = 1;
            a.exclude_hv = 1;
            int fd = int(syscall(SYS_perf_event_open, &a, tid, -1, -1, 0));
            if (fd < 0) continue;
            void* ring = mmap(nullptr, 9 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            evs.push_back({ fd, ring == MAP_FAILED ? nullptr : ring });
        }
        if (dir) closedir(dir);
        Log("watch: %zu threads watching 0x%08X", evs.size(), addr);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::map<uint64_t, int> ips;
        for (auto& ev : evs)
        {
            if (!ev.ring) continue;
            auto* mp = static_cast<perf_event_mmap_page*>(ev.ring);
            uint64_t head = mp->data_head, tail = mp->data_tail;
            uint8_t* data = static_cast<uint8_t*>(ev.ring) + 4096;
            while (tail < head)
            {
                auto* h = reinterpret_cast<perf_event_header*>(data + (tail % (8 * 4096)));
                if (h->type == PERF_RECORD_SAMPLE) ips[*reinterpret_cast<uint64_t*>(h + 1)]++;
                tail += h->size;
            }
            close(ev.fd);
        }
        for (auto& [ip, n] : ips) Log("watch: write from host ip 0x%llx (%d times)", (unsigned long long)ip, n);
        Log("watch: done");
    }).detach();
}
