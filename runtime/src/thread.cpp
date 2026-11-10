#include "thread.h"
#include "arch.h"
#include "guest.h"
#include "loader.h"

#include <pthread.h>
#include <csignal>

namespace
{
thread_local GuestThread* t_thread = nullptr;
thread_local std::shared_ptr<GuestThread> t_threadRef;
std::atomic<uint32_t> s_nextId{ 1 };
std::atomic<uint32_t> s_live{ 0 };
std::mutex s_threadsLock;
std::vector<std::shared_ptr<GuestThread>> s_threads;   // keeps running threads alive

struct ThreadExit { uint32_t code; };
}

GuestThread::~GuestThread()
{
    delete ctx;
    if (host.joinable()) host.detach();
}

GuestThread* CurrentThread() { return t_thread; }
std::shared_ptr<KObject> CurrentThreadObject() { return t_threadRef; }
PPCContext& CurrentContext() { return *t_thread->ctx; }
uint32_t ThreadCount() { return s_live; }

void GuestCall(PPCContext& ctx, uint32_t addr)
{
    PPCFunc* f = GuestLookup(addr);
    if (!f) Fatal("call to unknown guest function 0x%08X (lr=0x%08llX)", addr, (unsigned long long)ctx.lr);
    f(ctx, g_base);
}

[[noreturn]] void ThreadExitCurrent(uint32_t exitCode) { throw ThreadExit{ exitCode }; }

static void ThreadEntry(std::shared_ptr<GuestThread> t)
{
    t_thread = t.get();
    t_threadRef = t;
    // Alternate signal stack so the crash handler works even if the host stack overflows.
    static thread_local std::vector<uint8_t> altStack(256 * 1024);
    stack_t ss{};
    ss.ss_sp = altStack.data();
    ss.ss_size = altStack.size();
    sigaltstack(&ss, nullptr);
    char name[16];
    snprintf(name, sizeof(name), "guest-%u", t->id);
    pthread_setname_np(pthread_self(), name);
    t->pthread = pthread_self();

    // Wait while suspended.
    {
        std::unique_lock lock(g_dispatchLock);
        g_dispatchCv.wait(lock, [&] { return t->suspendCount <= 0; });
    }

    PPCContext& ctx = *t->ctx;
    ctx.fpscr.loadFromHost();
    ctx.fpscr.storeFromGuest(PPC_ROUND_NEAREST);
    ctx.fpscr.disableFlushModeUnconditional();

    uint32_t code = 0;
    try
    {
        if (t->xapiStartup)
        {
            ctx.r3.u64 = t->startAddress;
            ctx.r4.u64 = t->startContext;
            GuestCall(ctx, t->xapiStartup);
        }
        else
        {
            ctx.r3.u64 = t->startContext;
            GuestCall(ctx, t->startAddress);
        }
        code = ctx.r3.u32;
    }
    catch (const ThreadExit& e)
    {
        code = e.code;
    }
    Log("thread %u exited (code 0x%X)", t->id, code);
    {
        std::lock_guard lock(g_dispatchLock);
        t->exitCode = code;
        t->terminated = true;
        GuestWrite32(t->guestBody + 4, 1);
    }
    DispatchNotify();
    s_live--;
    std::lock_guard lock(s_threadsLock);
    std::erase(s_threads, t);
}

static bool SetupGuestThread(GuestThread* tp, uint32_t stackSize);

std::shared_ptr<GuestThread> ThreadCreate(uint32_t stackSize, uint32_t xapiStartup, uint32_t startAddress,
                                          uint32_t startContext, bool suspended, bool isMain)
{
    auto t = std::make_shared<GuestThread>();
    t->id = s_nextId++;
    t->xapiStartup = xapiStartup;
    t->startAddress = startAddress;
    t->startContext = startContext;
    t->suspendCount = suspended ? 1 : 0;
    if (!SetupGuestThread(t.get(), stackSize)) return nullptr;

    {
        std::lock_guard lock(s_threadsLock);
        s_threads.push_back(t);
    }
    s_live++;
    Log("thread %u created: entry 0x%08X ctx 0x%08X xapi 0x%08X stack 0x%08X+0x%X%s%s", t->id, startAddress,
        startContext, xapiStartup, t->stackBase, t->stackSize, suspended ? " (suspended)" : "", isMain ? " [main]" : "");
    t->host = std::thread(ThreadEntry, t);
    t->host.detach();
    return t;
}

static bool SetupGuestThread(GuestThread* tp, uint32_t stackSize)
{
    GuestThread* t = tp;
    t->tlsSlots.assign(1024, 0);

    if (stackSize == 0) stackSize = g_module.stackSize;
    stackSize = std::max<uint32_t>((stackSize + 0xFFF) & ~0xFFFu, 0x10000);
    t->stackSize = stackSize;
    t->stackBase = g_virtualHeap.Alloc(stackSize, 0x10000);
    if (!t->stackBase) return false;

    // KTHREAD (also the thread's dispatcher object)
    ObjectCreateGuestBody(t, DISP_THREAD, 0x200);
    t->kthread = t->guestBody;

    // Static TLS: copy of the module's TLS template.
    uint32_t tlsSize = std::max<uint32_t>(g_module.tlsDataSize, 4);
    t->tlsStatic = GuestAlloc(tlsSize);
    if (g_module.tlsRawSize && g_module.tlsRawAddress)
        memcpy(g_base + t->tlsStatic, g_base + g_module.tlsRawAddress, g_module.tlsRawSize);

    t->pcr = GuestAlloc(0x1000);
    uint32_t top = t->stackBase + stackSize;
    GuestWrite32(t->pcr + 0x00, t->tlsStatic);
    GuestWrite32(t->pcr + 0x30, t->pcr);
    GuestWrite32(t->pcr + 0x70, top);
    GuestWrite32(t->pcr + 0x74, t->stackBase);
    GuestWrite32(t->pcr + 0x100, t->kthread);
    GuestWrite8(t->pcr + 0x10C, uint8_t(t->id % 6));

    GuestWrite32(t->kthread + 0x5C, top);
    GuestWrite32(t->kthread + 0x60, t->stackBase);
    GuestWrite32(t->kthread + 0x68, t->tlsStatic);
    GuestWrite32(t->kthread + 0x14C, t->id);

    t->ctx = new PPCContext{};
    t->ctx->r1.u64 = top - 0x100;   // leave room for the back chain / red zone
    t->ctx->r13.u64 = t->pcr;
    return true;
}

std::shared_ptr<GuestThread> ThreadCreateHost(const char* name, std::function<void(GuestThread&)> body)
{
    auto t = std::make_shared<GuestThread>();
    t->id = s_nextId++;
    if (!SetupGuestThread(t.get(), 0x40000)) Fatal("cannot create host thread %s", name);
    {
        std::lock_guard lock(s_threadsLock);
        s_threads.push_back(t);
    }
    std::string n = name;
    t->host = std::thread([t, body, n] {
        t_thread = t.get();
        t_threadRef = t;
        static thread_local std::vector<uint8_t> altStack(256 * 1024);
        stack_t ss{};
        ss.ss_sp = altStack.data();
        ss.ss_size = altStack.size();
        sigaltstack(&ss, nullptr);
        pthread_setname_np(pthread_self(), n.substr(0, 15).c_str());
        t->pthread = pthread_self();
        t->ctx->fpscr.loadFromHost();
        body(*t);
    });
    t->host.detach();
    Log("host thread %u (%s) created", t->id, name);
    return t;
}

void GuestCallOnThread(GuestThread& t, uint32_t addr, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6)
{
    PPCContext& ctx = *t.ctx;
    ctx.r1.u64 = t.stackBase + t.stackSize - 0x100;
    ctx.r13.u64 = t.pcr;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    GuestCall(ctx, addr);
}

void ThreadResume(GuestThread* t)
{
    {
        std::lock_guard lock(g_dispatchLock);
        if (t->suspendCount > 0) t->suspendCount--;
    }
    DispatchNotify();
}

void ThreadSuspend(GuestThread* t)
{
    std::lock_guard lock(g_dispatchLock);
    t->suspendCount++;
    // NOTE: only takes effect for threads that haven't started yet; suspending a running
    // host thread at an arbitrary point isn't supported.
}

int RunMainThread(uint32_t entry)
{
    auto t = ThreadCreate(g_module.stackSize, 0, entry, 0, false, true);
    if (!t) Fatal("cannot create main thread");
    std::unique_lock lock(g_dispatchLock);
    g_dispatchCv.wait(lock, [&] { return t->terminated; });
    return int(t->exitCode);
}

static void OnSampleSignal(int, siginfo_t*, void* uc)
{
    if (GuestThread* t = CurrentThread()) t->sampledRip = HostPc(uc);
}

void ThreadSampleAll(const char* why)
{
    static bool installed = [] {
        struct sigaction sa{};
        sa.sa_sigaction = OnSampleSignal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGUSR2, &sa, nullptr);
        return true;
    }();
    (void)installed;
    std::vector<std::shared_ptr<GuestThread>> ts;
    {
        std::lock_guard lock(s_threadsLock);
        ts = s_threads;
    }
    for (auto& t : ts) { t->sampledRip = 0; if (t->pthread) pthread_kill(t->pthread, SIGUSR2); }
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    for (auto& t : ts)
    {
        std::string extra;
        if (uint32_t cs = t->waitingCs.load())
        {
            uint32_t owner = GuestRead32(cs + 0x18);
            uint32_t ownerId = 0;
            for (auto& o : ts) if (o->kthread == owner) ownerId = o->id;
            char b[96];
            snprintf(b, sizeof(b), " [entering CS 0x%08X owned by t%u]", cs, ownerId);
            extra = b;
        }
        if (const void* o = t->waitingObj.load())
        {
            auto* ko = static_cast<const KObject*>(o);
            char b[96];
            snprintf(b, sizeof(b), " [waiting for %s %p body 0x%08X]", ko->type == KType::Event ? "event" : ko->type == KType::Semaphore ? "semaphore" : ko->type == KType::Thread ? "thread" : "object", o, ko->guestBody);
            extra += b;
        }
        Log("stall (%s): t%u rip 0x%llx%s guest %s", why, t->id, (unsigned long long)t->sampledRip.load(), extra.c_str(), t->ctx ? GuestBacktrace(*t->ctx, 10).c_str() : "-");
    }
}
