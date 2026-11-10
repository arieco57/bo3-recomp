// xboxkrnl: threads, dispatcher objects, waits, critical sections, spinlocks, TLS, time.
#include "guest.h"
#include "kernel_vars.h"
#include "kobject.h"
#include "gpu/renderer.h"
#include "ktime.h"
#include "stubs.h"
#include "thread.h"

#include <array>
#include <deque>
#include <unordered_map>
#include <map>
#include <mutex>
#include <algorithm>
#include <bitset>
#include <thread>
#include <set>

// ---------------------------------------------------------------------------------------
// helpers

static std::string ObjectAttributesName(uint32_t attrPtr)
{
    if (!attrPtr) return {};
    uint32_t namePtr = GuestRead32(attrPtr + 4);
    return namePtr ? GuestAnsiString(namePtr) : std::string{};
}

// Creates or opens a named object. Returns the existing one (and sets *existed) for duplicates.
template <class T, class F>
static std::shared_ptr<T> CreateMaybeNamed(uint32_t attrPtr, bool& existed, F make)
{
    existed = false;
    std::string name = ObjectAttributesName(attrPtr);
    if (!name.empty())
        if (auto o = ObjectFindNamed(name))
        {
            existed = true;
            return std::dynamic_pointer_cast<T>(o);
        }
    auto obj = make();
    if (!name.empty()) ObjectAddNamed(name, obj);
    return obj;
}

// BO3_TRACE_HANDLE=0xF8000078 logs every operation on one handle (debugging lost wakeups).
static bool Traced(uint32_t h)
{
    static const uint32_t t = getenv("BO3_TRACE_HANDLE") ? uint32_t(strtoul(getenv("BO3_TRACE_HANDLE"), nullptr, 16)) : 0;
    return t && h == t;
}
// BO3_TRACE_WAITS=1: log the first wait of each (thread, object) pair and every signal.
static void TraceWait(PPCContext& ctx, const char* what, uint32_t id)
{
    static const bool on = getenv("BO3_TRACE_WAITS") != nullptr;
    if (!on) return;
    static std::mutex m;
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    std::lock_guard lock(m);
    if (seen.insert({ CurrentThread()->id, id }).second) Log("wait: %s 0x%08X (lr=0x%08X)", what, id, uint32_t(ctx.lr));
}
static void TraceSignal(PPCContext& ctx, const char* what, uint32_t id)
{
    static const bool on = getenv("BO3_TRACE_WAITS") != nullptr;
    if (on) Log("signal: %s 0x%08X (lr=0x%08X)", what, id, uint32_t(ctx.lr));
}
#define TRACE_H(h, what) do { if (Traced(h)) Log("handle 0x%08X: %s (lr=0x%08X)", h, what, uint32_t(ctx.lr)); } while (0)

static std::shared_ptr<KObject> HandleOrNull(uint32_t h)
{
    auto o = HandleGet(h);
    if (!o) LogWarn("invalid handle 0x%08X (lr=0x%08X)", h, uint32_t(CurrentContext().lr));
    return o;
}

// ---------------------------------------------------------------------------------------
// threads

GUEST_IMPORT(ExCreateThread)
{
    uint32_t handlePtr = ARG(3), stackSize = ARG(4), idPtr = ARG(5), xapi = ARG(6), start = ARG(7), context = ARG(8),
             flags = ARG(9);
    auto t = ThreadCreate(stackSize, xapi, start, context, (flags & 1) != 0);
    if (!t) RETURN(STATUS_NO_MEMORY);
    if (handlePtr) GuestWrite32(handlePtr, HandleCreate(t));
    if (idPtr) GuestWrite32(idPtr, t->id);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ExTerminateThread) { ThreadExitCurrent(ARG(3)); }

GUEST_IMPORT(NtResumeThread)
{
    auto t = std::dynamic_pointer_cast<GuestThread>(HandleOrNull(ARG(3)));
    if (!t) RETURN(STATUS_INVALID_HANDLE);
    uint32_t prev;
    {
        std::lock_guard lock(g_dispatchLock);
        prev = t->suspendCount;
    }
    ThreadResume(t.get());
    if (ARG(4)) GuestWrite32(ARG(4), prev);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtSuspendThread)
{
    auto t = std::dynamic_pointer_cast<GuestThread>(HandleOrNull(ARG(3)));
    if (!t) RETURN(STATUS_INVALID_HANDLE);
    uint32_t prev;
    {
        std::lock_guard lock(g_dispatchLock);
        prev = t->suspendCount;
    }
    if (t.get() != CurrentThread()) LogWarn("NtSuspendThread on another running thread %u: ignored", t->id);
    else ThreadSuspend(t.get());
    if (ARG(4)) GuestWrite32(ARG(4), prev);
    RETURN(STATUS_SUCCESS);
}

static GuestThread* ThreadFromGuest(uint32_t kthread)
{
    return dynamic_cast<GuestThread*>(ObjectFromGuest(kthread).get());
}

GUEST_IMPORT(KeSetAffinityThread)
{
    GuestThread* t = ThreadFromGuest(ARG(3));
    uint32_t prev = t ? t->affinity : 0;
    if (t) t->affinity = ARG(4);
    if (ARG(5)) GuestWrite32(ARG(5), prev);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(KeSetBasePriorityThread)
{
    GuestThread* t = ThreadFromGuest(ARG(3));
    int prev = t ? t->priority : 0;
    if (t) t->priority = (int32_t)ARG(4);
    RETURN(uint32_t(prev));
}

GUEST_IMPORT(KeQueryBasePriorityThread)
{
    GuestThread* t = ThreadFromGuest(ARG(3));
    RETURN(uint32_t(t ? t->priority : 0));
}

// Guest yields (Sleep(0) / SwitchToThread): the game's job workers spin on them while idle.
// Cheap on the console's 6 hardware threads; on a phone (2 big cores) the spinning threads
// take CPU time from the ones doing the work - after a run of yields, sleep briefly instead.
static void GuestYield()
{
#ifdef __ANDROID__
    thread_local uint32_t run = 0;
    thread_local auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    run = now - last < std::chrono::microseconds(200) ? run + 1 : 0;
    last = now;
    if (run > 16) { std::this_thread::sleep_for(std::chrono::microseconds(100)); last = std::chrono::steady_clock::now(); return; }
#endif
    std::this_thread::yield();
}

GUEST_IMPORT(NtYieldExecution)
{
    GuestYield();
    RETURN(STATUS_SUCCESS);
}

// BO3_WAIT_PROFILE=1: time spent blocked per (guest thread, caller), logged every 2 s.
namespace
{
struct WaitProf
{
    std::chrono::steady_clock::time_point t0;
    uint32_t key;
    const char* what;
    const PPCContext* ctx;
    WaitProf(const PPCContext& c, const char* w) : t0(std::chrono::steady_clock::now()), key(uint32_t(c.lr)), what(w), ctx(&c) {}
    ~WaitProf();
};
std::mutex s_wpLock;
std::map<std::pair<uint32_t, uint32_t>, std::pair<double, uint32_t>> s_wp;   // (tid, lr) -> (ms, calls)
WaitProf::~WaitProf()
{
    // BO3_LONG_WAIT=<ms>: every single wait longer than that (thread, call, guest backtrace)
    static const double longMs = getenv("BO3_LONG_WAIT") ? atof(getenv("BO3_LONG_WAIT")) : 0;
    static const bool on = getenv("BO3_WAIT_PROFILE") != nullptr;
    if (!on && longMs <= 0) return;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    GuestThread* gt = CurrentThread();
    if (longMs > 0 && ms > longMs)
        Log("longwait t%u %s %.1f ms: %s", gt ? gt->id : 0, what, ms, GuestBacktrace(*ctx, 8).c_str());
    if (!on) return;
    std::lock_guard lock(s_wpLock);
    auto& e = s_wp[{ gt ? gt->id : 0, key }];
    e.first += ms;
    e.second++;
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(2))
    {
        last = std::chrono::steady_clock::now();
        std::vector<std::pair<double, std::string>> v;
        for (auto& [k, val] : s_wp)
        {
            char b[96];
            snprintf(b, sizeof(b), "t%u lr 0x%08X: %.0f ms in %u waits", k.first, k.second, val.first / 2, val.second / 2);
            v.push_back({ val.first, b });
        }
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 40; i++) Log("waitprof (per s): %s", v[i].second.c_str());
        s_wp.clear();
    }
}
}

GUEST_IMPORT(KeDelayExecutionThread)
{
    auto ns = GuestTimeoutToNs(ARG(5));
    if (static const char* bt = getenv("BO3_BT_DELAY"); bt)   // diagnostics: who sleeps (frame limiter)
    {
        static const auto t0 = std::chrono::steady_clock::now();
        static int n = 0;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > atof(bt) && n++ < 6)
            Log("delay %lld ns: %s", ns ? (long long)*ns : -1ll, GuestBacktrace(ctx, 16).c_str());
    }
    WaitProf wp(ctx, "KeDelayExecutionThread");
    if (!ns || *ns == 0) GuestYield();
    else std::this_thread::sleep_for(std::chrono::nanoseconds(*ns));
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(KeGetCurrentProcessType) { RETURN(1); }   // X_PROCTYPE_USER
GUEST_IMPORT(KeSetCurrentProcessType) { RETURN(0); }

// ---------------------------------------------------------------------------------------
// handles / objects

GUEST_IMPORT(NtClose)
{
    RETURN(HandleClose(ARG(3)) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
}

GUEST_IMPORT(NtDuplicateObject)
{
    uint32_t h = HandleDuplicate(ARG(3));
    if (!h) RETURN(STATUS_INVALID_HANDLE);
    if (ARG(4)) GuestWrite32(ARG(4), h);
    if (ARG(5) & 1) HandleClose(ARG(3));   // DUPLICATE_CLOSE_SOURCE
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ObReferenceObjectByHandle)
{
    auto o = HandleOrNull(ARG(3));
    if (!o) RETURN(STATUS_INVALID_HANDLE);
    if (!o->guestBody) ObjectCreateGuestBody(o.get(), DISP_NOTIFICATION_EVENT);
    if (ARG(5)) GuestWrite32(ARG(5), o->guestBody);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ObDereferenceObject) { RETURN(0); }

// ---------------------------------------------------------------------------------------
// events

GUEST_IMPORT(NtCreateEvent)
{
    bool existed;
    bool manual = ARG(5) == 0, initial = ARG(6) != 0;
    auto ev = CreateMaybeNamed<KEvent>(ARG(4), existed, [&] {
        auto e = std::make_shared<KEvent>(manual, initial);
        ObjectCreateGuestBody(e.get(), manual ? DISP_NOTIFICATION_EVENT : DISP_SYNCHRONIZATION_EVENT);
        e->SyncGuest();
        return e;
    });
    if (!ev) RETURN(STATUS_OBJECT_TYPE_MISMATCH);
    uint32_t h = HandleCreate(ev);
    GuestWrite32(ARG(3), h);
    if (Traced(h)) Log("handle 0x%08X: NtCreateEvent manual=%d initial=%d (lr=0x%08X)", h, manual, initial, uint32_t(ctx.lr));
    RETURN(existed ? STATUS_OBJECT_NAME_EXISTS : STATUS_SUCCESS);
}

// Pulse: the threads waiting right now get the event (all of them for a notification event,
// one for a synchronization event), then it is non-signalled again - handed over directly
// instead of yielding and hoping they run before the reset.
static uint32_t SetEvent(KEvent* e, bool pulse)
{
    static const bool globalWake = getenv("BO3_GLOBAL_WAKE") != nullptr;   // diagnostics: old behaviour
    if (globalWake)
    {
        uint32_t prev;
        {
            std::lock_guard lock(g_dispatchLock);
            prev = e->signaled;
            e->signaled = true;
            e->SyncGuest();
        }
        DispatchNotify();
        if (pulse)
        {
            std::this_thread::yield();
            std::lock_guard lock(g_dispatchLock);
            e->signaled = false;
            e->SyncGuest();
        }
        return prev;
    }
    std::unique_lock lock(g_dispatchLock);
    uint32_t prev = e->signaled;
    if (pulse)
    {
        bool handed = false;
        for (KWaiter* w : e->waiters)
            if (!w->granted)
            {
                w->granted = e;
                w->cv.notify_one();
                handed = true;
                if (!e->manualReset) break;
            }
        // Nobody waiting yet: a thread may be between checking its work queue and waiting
        // (a much wider window here than on the console) - the game's job system pulses its
        // workers and would lose the wakeup, the worker sleeping until its timeout (frames of
        // up to ~1.7 s while moving). A synchronization event stays signalled for the next
        // waiter; a notification event stays signalled for a moment, as before.
        if (handed) e->signaled = false;
        else if (!e->manualReset) e->signaled = true;
        else
        {
            e->signaled = true;
            e->SyncGuest();
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
            e->signaled = false;
        }
    }
    else
    {
        e->signaled = true;
        DispatchNotifyObject(e);
    }
    e->SyncGuest();
    return prev;
}

static uint32_t ResetEvent(KEvent* e)
{
    std::lock_guard lock(g_dispatchLock);
    uint32_t prev = e->signaled;
    e->signaled = false;
    e->SyncGuest();
    return prev;
}

// Signals an event handle from host code (asynchronous APIs completing: XNetQosLookup...).
void HostSetEventHandle(uint32_t handle)
{
    if (auto e = std::dynamic_pointer_cast<KEvent>(HandleOrNull(handle))) SetEvent(e.get(), false);
}

GUEST_IMPORT(NtSetEvent)
{
    TRACE_H(ARG(3), "NtSetEvent");
    TraceSignal(ctx, "NtSetEvent", ARG(3));
    auto e = std::dynamic_pointer_cast<KEvent>(HandleOrNull(ARG(3)));
    if (!e) RETURN(STATUS_INVALID_HANDLE);
    uint32_t prev = SetEvent(e.get(), false);
    if (ARG(4)) GuestWrite32(ARG(4), prev);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtPulseEvent)
{
    TRACE_H(ARG(3), "NtPulseEvent");
    auto e = std::dynamic_pointer_cast<KEvent>(HandleOrNull(ARG(3)));
    if (!e) RETURN(STATUS_INVALID_HANDLE);
    uint32_t prev = SetEvent(e.get(), true);
    if (ARG(4)) GuestWrite32(ARG(4), prev);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtClearEvent)
{
    TRACE_H(ARG(3), "NtClearEvent");
    auto e = std::dynamic_pointer_cast<KEvent>(HandleOrNull(ARG(3)));
    if (!e) RETURN(STATUS_INVALID_HANDLE);
    ResetEvent(e.get());
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(KeSetEvent)
{
    TraceSignal(ctx, "KeSetEvent", ARG(3));
    auto e = std::dynamic_pointer_cast<KEvent>(ObjectFromGuest(ARG(3)));
    if (!e) RETURN(0);
    RETURN(SetEvent(e.get(), false));
}

GUEST_IMPORT(KeResetEvent)
{
    auto e = std::dynamic_pointer_cast<KEvent>(ObjectFromGuest(ARG(3)));
    if (!e) RETURN(0);
    RETURN(ResetEvent(e.get()));
}

// ---------------------------------------------------------------------------------------
// semaphores / mutants

GUEST_IMPORT(NtCreateSemaphore)
{
    bool existed;
    int32_t initial = (int32_t)ARG(5), maximum = (int32_t)ARG(6);
    auto s = CreateMaybeNamed<KSemaphore>(ARG(4), existed, [&] {
        auto o = std::make_shared<KSemaphore>(initial, maximum);
        ObjectCreateGuestBody(o.get(), DISP_SEMAPHORE);
        return o;
    });
    if (!s) RETURN(STATUS_OBJECT_TYPE_MISMATCH);
    GuestWrite32(ARG(3), HandleCreate(s));
    RETURN(existed ? STATUS_OBJECT_NAME_EXISTS : STATUS_SUCCESS);
}

GUEST_IMPORT(NtReleaseSemaphore)
{
    TraceSignal(ctx, "NtReleaseSemaphore", ARG(3));
    auto s = std::dynamic_pointer_cast<KSemaphore>(HandleOrNull(ARG(3)));
    if (!s) RETURN(STATUS_INVALID_HANDLE);
    int32_t prev;
    {
        std::lock_guard lock(g_dispatchLock);
        prev = s->count;
        if (s->count + (int32_t)ARG(4) > s->maximum) RETURN(STATUS_SEMAPHORE_LIMIT_EXCEEDED);
        s->count += (int32_t)ARG(4);
        DispatchNotifyObject(s.get());
    }
    if (ARG(5)) GuestWrite32(ARG(5), uint32_t(prev));
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtCreateMutant)
{
    bool existed;
    bool owned = ARG(5) != 0;
    auto m = CreateMaybeNamed<KMutant>(ARG(4), existed, [&] {
        auto o = std::make_shared<KMutant>();
        ObjectCreateGuestBody(o.get(), DISP_MUTANT);
        if (owned) { o->owner = CurrentThread(); o->recursion = 1; }
        return o;
    });
    if (!m) RETURN(STATUS_OBJECT_TYPE_MISMATCH);
    GuestWrite32(ARG(3), HandleCreate(m));
    RETURN(existed ? STATUS_OBJECT_NAME_EXISTS : STATUS_SUCCESS);
}

GUEST_IMPORT(NtReleaseMutant)
{
    auto m = std::dynamic_pointer_cast<KMutant>(HandleOrNull(ARG(3)));
    if (!m) RETURN(STATUS_INVALID_HANDLE);
    {
        std::lock_guard lock(g_dispatchLock);
        if (m->owner != CurrentThread()) RETURN(STATUS_MUTANT_NOT_OWNED);
        if (ARG(4)) GuestWrite32(ARG(4), m->recursion);
        if (--m->recursion == 0) { m->owner = nullptr; DispatchNotifyObject(m.get()); }
    }
    RETURN(STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------------------
// timers

GUEST_IMPORT(NtCreateTimer)
{
    bool existed;
    bool manual = ARG(5) == 0;   // 0 = NotificationTimer, 1 = SynchronizationTimer
    auto t = CreateMaybeNamed<KTimer>(ARG(4), existed, [&] {
        auto o = std::make_shared<KTimer>(manual);
        ObjectCreateGuestBody(o.get(), manual ? DISP_NOTIFICATION_TIMER : DISP_SYNCHRONIZATION_TIMER, 0x28);
        return o;
    });
    if (!t) RETURN(STATUS_OBJECT_TYPE_MISMATCH);
    GuestWrite32(ARG(3), HandleCreate(t));
    RETURN(existed ? STATUS_OBJECT_NAME_EXISTS : STATUS_SUCCESS);
}

GUEST_IMPORT(NtSetTimerEx)
{
    // (handle, due_time*, apc_routine, apc_mode, apc_arg, resume, period_ms, prev_state*)
    auto t = std::dynamic_pointer_cast<KTimer>(HandleOrNull(ARG(3)));
    if (!t) RETURN(STATUS_INVALID_HANDLE);
    if (ARG(5)) LogWarn("NtSetTimerEx: APC routine 0x%08X not supported", ARG(5));
    auto due = GuestTimeoutToNs(ARG(4));
    uint32_t period = ARG(9);
    {
        std::lock_guard lock(g_dispatchLock);
        if (ARG(10)) GuestWrite32(ARG(10), t->signaled);
        t->signaled = false;
        t->dueNs = KTimeHostNs() + (due ? *due : 0) + 1;
        t->periodNs = uint64_t(period) * 1000000ull;
    }
    DispatchNotify();
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtCancelTimer)
{
    auto t = std::dynamic_pointer_cast<KTimer>(HandleOrNull(ARG(3)));
    if (!t) RETURN(STATUS_INVALID_HANDLE);
    std::lock_guard lock(g_dispatchLock);
    if (ARG(4)) GuestWrite32(ARG(4), t->signaled);
    t->dueNs = 0;
    t->periodNs = 0;
    RETURN(STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------------------
// waits

GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    if (Traced(ARG(3))) { static int n = 0; if (n++ < 5) TRACE_H(ARG(3), "NtWaitForSingleObjectEx"); }
    TraceWait(ctx, "NtWaitForSingleObjectEx", ARG(3));
    auto o = HandleOrNull(ARG(3));
    if (!o || !o->Waitable()) RETURN(STATUS_INVALID_HANDLE);
    WaitProf wp(ctx, "NtWaitForSingleObjectEx");
    if (static const char* bt = getenv("BO3_BT_WAIT"); bt)   // diagnostics: "<seconds>,<guest tid>"
    {
        static const auto t0 = std::chrono::steady_clock::now();
        static const uint32_t tid = strchr(bt, ',') ? uint32_t(atoi(strchr(bt, ',') + 1)) : 0;
        static int n = 0;
        GuestThread* gt = CurrentThread();
        if (gt && gt->id == tid && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > atof(bt) && n++ < 8)
            Log("wait 0x%X: %s", ARG(3), GuestBacktrace(ctx, 16).c_str());
    }
    uint32_t st = WaitForObjects({ o }, false, GuestTimeoutToNs(ARG(6)), ARG(5) != 0);
    if (static const double longMs = getenv("BO3_LONG_WAIT") ? atof(getenv("BO3_LONG_WAIT")) : 0; longMs > 0 &&
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wp.t0).count() > longMs)
    {
        auto e = std::dynamic_pointer_cast<KEvent>(o);
        Log("longwait handle 0x%X type %d %s status 0x%X timeout %s body 0x%08X", ARG(3), int(o->type), e ? (e->manualReset ? "manual" : "auto") : "-", st,
            ARG(6) ? std::to_string((long long)GuestRead64(ARG(6))).c_str() : "none", o->guestBody);
    }
    RETURN(st);
}

GUEST_IMPORT(KeWaitForSingleObject)
{
    TraceWait(ctx, "KeWaitForSingleObject", ARG(3));
    auto o = ObjectFromGuest(ARG(3));
    WaitProf wp(ctx, "KeWaitForSingleObject");
    RETURN(WaitForObjects({ o }, false, GuestTimeoutToNs(ARG(7)), ARG(6) != 0));
}

GUEST_IMPORT(KeWaitForMultipleObjects)
{
    TraceWait(ctx, "KeWaitForMultipleObjects", ARG(4) ? GuestRead32(ARG(4)) : 0);
    // (count, objects**, wait_type (0 = all, 1 = any), reason, mode, alertable, timeout*, wait_blocks)
    std::vector<std::shared_ptr<KObject>> objs;
    for (uint32_t i = 0; i < ARG(3); i++) objs.push_back(ObjectFromGuest(GuestRead32(ARG(4) + i * 4)));
    WaitProf wp(ctx, "KeWaitForMultipleObjects");
    RETURN(WaitForObjects(objs, ARG(5) == 0, GuestTimeoutToNs(ARG(9)), ARG(8) != 0));
}

// ---------------------------------------------------------------------------------------
// critical sections (X_RTL_CRITICAL_SECTION: header 0x10, LockCount +0x10,
// RecursionCount +0x14, OwningThread +0x18). Owner = KTHREAD address, like the real kernel.

namespace
{
constexpr size_t kStripes = 64;
// Waiters of each contended critical section (FIFO with their start time): threads that
// re-take a lock in a loop (the job system) could otherwise starve one that sleeps - the
// game's Main thread waited up to ~1.5 s for one, stalling frames while moving.
struct CsWaiter { uint32_t kthread; std::chrono::steady_clock::time_point since; };
struct Stripe { std::mutex m; std::condition_variable cv; std::unordered_map<uint32_t, std::deque<CsWaiter>> waiters; };
const std::chrono::microseconds kCsStarve{ int64_t((getenv("BO3_CS_STARVE_MS") ? atof(getenv("BO3_CS_STARVE_MS")) : 5.0) * 1000) };
std::array<Stripe, kStripes> s_csStripes;
Stripe& StripeFor(uint32_t cs) { return s_csStripes[(cs >> 3) % kStripes]; }
}

static void CsInit(uint32_t cs)
{
    GuestWrite8(cs + 0, DISP_SYNCHRONIZATION_EVENT);
    GuestWrite32(cs + 0x10, 0xFFFFFFFF);
    GuestWrite32(cs + 0x14, 0);
    GuestWrite32(cs + 0x18, 0);
}

GUEST_IMPORT(RtlInitializeCriticalSection) { CsInit(ARG(3)); RETURN(0); }
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount) { CsInit(ARG(3)); RETURN(STATUS_SUCCESS); }

static bool CsTryEnter(uint32_t cs, uint32_t self)
{
    uint32_t owner = GuestRead32(cs + 0x18);
    if (owner == 0 || owner == self)
    {
        GuestWrite32(cs + 0x18, self);
        GuestWrite32(cs + 0x14, GuestRead32(cs + 0x14) + 1);
        GuestWrite32(cs + 0x10, GuestRead32(cs + 0x10) + 1);
        return true;
    }
    return false;
}

// A free lock may be taken by anyone (fast, like the console) unless its oldest waiter has
// waited longer than kCsStarve: then only that waiter (handed over at release).
static bool CsMayTake(Stripe& s, uint32_t cs, uint32_t self)
{
    if (GuestRead32(cs + 0x18) == self) return true;   // recursive enter
    auto it = s.waiters.find(cs);
    if (it == s.waiters.end() || it->second.empty()) return true;
    const CsWaiter& front = it->second.front();
    return front.kthread == self || std::chrono::steady_clock::now() - front.since < kCsStarve;
}

static void CsDequeue(Stripe& s, uint32_t cs, uint32_t self)
{
    auto it = s.waiters.find(cs);
    if (it == s.waiters.end()) return;
    std::erase_if(it->second, [&](const CsWaiter& w) { return w.kthread == self; });
    if (it->second.empty()) s.waiters.erase(it);
}

GUEST_IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = ARG(3), self = CurrentThread()->kthread;
    Stripe& s = StripeFor(cs);
    std::unique_lock lock(s.m);
    if (CsMayTake(s, cs, self) && CsTryEnter(cs, self)) RETURN(0);
    s.waiters[cs].push_back({ self, std::chrono::steady_clock::now() });
    CurrentThread()->waitingCs = cs;
    s.cv.wait(lock, [&] {
        if (GuestRead32(cs + 0x18) == self) return true;   // handed over by the releasing thread
        return CsMayTake(s, cs, self) && CsTryEnter(cs, self);
    });
    CsDequeue(s, cs, self);
    CurrentThread()->waitingCs = 0;
    RETURN(0);
}

GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    uint32_t cs = ARG(3), self = CurrentThread()->kthread;
    Stripe& s = StripeFor(cs);
    std::lock_guard lock(s.m);
    RETURN(CsMayTake(s, cs, self) && CsTryEnter(cs, self) ? 1 : 0);
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ARG(3);
    Stripe& s = StripeFor(cs);
    bool released = false;
    {
        std::lock_guard lock(s.m);
        if (GuestRead32(cs + 0x18) != CurrentThread()->kthread)
            LogWarn("RtlLeaveCriticalSection 0x%08X not owned by this thread", cs);
        uint32_t rec = GuestRead32(cs + 0x14) - 1;
        GuestWrite32(cs + 0x14, rec);
        GuestWrite32(cs + 0x10, GuestRead32(cs + 0x10) - 1);
        if (rec == 0)
        {
            released = true;
            auto it = s.waiters.find(cs);
            if (it != s.waiters.end() && !it->second.empty() &&
                std::chrono::steady_clock::now() - it->second.front().since >= kCsStarve && !AbOld("csfair"))
            {
                // starving waiter: it owns the lock now (as if its enter had succeeded)
                GuestWrite32(cs + 0x18, it->second.front().kthread);
                GuestWrite32(cs + 0x14, 1);
                GuestWrite32(cs + 0x10, GuestRead32(cs + 0x10) + 1);
            }
            else GuestWrite32(cs + 0x18, 0);
        }
    }
    if (released) s.cv.notify_all();
    RETURN(0);
}

// ---------------------------------------------------------------------------------------
// spinlocks (guest u32: 0 = free, else owner KTHREAD) and IRQL no-ops

static void SpinAcquire(uint32_t lock)
{
    auto* p = reinterpret_cast<std::atomic<uint32_t>*>(g_base + lock);
    uint32_t self = __builtin_bswap32(CurrentThread()->kthread);
    for (uint32_t expected = 0, spins = 0; !p->compare_exchange_weak(expected, self, std::memory_order_acquire); expected = 0)
        if (++spins > 64) std::this_thread::yield();
}

static void SpinRelease(uint32_t lock)
{
    reinterpret_cast<std::atomic<uint32_t>*>(g_base + lock)->store(0, std::memory_order_release);
}

GUEST_IMPORT(KfAcquireSpinLock) { SpinAcquire(ARG(3)); RETURN(0); }
GUEST_IMPORT(KfReleaseSpinLock) { SpinRelease(ARG(3)); RETURN(0); }
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(ARG(3)); RETURN(0); }
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql) { SpinRelease(ARG(3)); RETURN(0); }
GUEST_IMPORT(KeEnterCriticalRegion) { RETURN(0); }
GUEST_IMPORT(KeLeaveCriticalRegion) { RETURN(0); }
GUEST_IMPORT(KeLockL2) { RETURN(0); }
GUEST_IMPORT(KeUnlockL2) { RETURN(0); }

// ---------------------------------------------------------------------------------------
// TLS (dynamic slots, host side)

static std::mutex s_tlsLock;
static std::bitset<1024> s_tlsUsed;

GUEST_IMPORT(KeTlsAlloc)
{
    std::lock_guard lock(s_tlsLock);
    for (uint32_t i = 0; i < s_tlsUsed.size(); i++)
        if (!s_tlsUsed[i]) { s_tlsUsed[i] = true; RETURN(i); }
    RETURN(0xFFFFFFFF);
}

GUEST_IMPORT(KeTlsFree)
{
    std::lock_guard lock(s_tlsLock);
    if (ARG(3) < s_tlsUsed.size()) s_tlsUsed[ARG(3)] = false;
    RETURN(1);
}

GUEST_IMPORT(KeTlsGetValue)
{
    auto& slots = CurrentThread()->tlsSlots;
    RETURN(ARG(3) < slots.size() ? slots[ARG(3)] : 0);
}

GUEST_IMPORT(KeTlsSetValue)
{
    auto& slots = CurrentThread()->tlsSlots;
    if (ARG(3) >= slots.size()) RETURN(0);
    slots[ARG(3)] = ARG(4);
    RETURN(1);
}

// ---------------------------------------------------------------------------------------
// time

GUEST_IMPORT(KeQueryPerformanceFrequency) { RETURN(kPerfFrequency); }

GUEST_IMPORT(KeQuerySystemTime)
{
    if (ARG(3)) GuestWrite64(ARG(3), KTimeSystemTime100ns());
    RETURN(0);
}

// TIME_FIELDS: u16 year, month, day, hour, minute, second, milliseconds, weekday
GUEST_IMPORT(RtlTimeToTimeFields)
{
    uint64_t t = GuestRead64(ARG(3));
    time_t secs = time_t((t - kFiletimeUnixEpoch) / 10000000ull);
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    uint32_t f = ARG(4);
    GuestWrite16(f + 0, uint16_t(tmv.tm_year + 1900));
    GuestWrite16(f + 2, uint16_t(tmv.tm_mon + 1));
    GuestWrite16(f + 4, uint16_t(tmv.tm_mday));
    GuestWrite16(f + 6, uint16_t(tmv.tm_hour));
    GuestWrite16(f + 8, uint16_t(tmv.tm_min));
    GuestWrite16(f + 10, uint16_t(tmv.tm_sec));
    GuestWrite16(f + 12, uint16_t((t / 10000) % 1000));
    GuestWrite16(f + 14, uint16_t(tmv.tm_wday));
    RETURN(0);
}

GUEST_IMPORT(RtlTimeFieldsToTime)
{
    uint32_t f = ARG(3);
    struct tm tmv{};
    tmv.tm_year = GuestRead16(f + 0) - 1900;
    tmv.tm_mon = GuestRead16(f + 2) - 1;
    tmv.tm_mday = GuestRead16(f + 4);
    tmv.tm_hour = GuestRead16(f + 6);
    tmv.tm_min = GuestRead16(f + 8);
    tmv.tm_sec = GuestRead16(f + 10);
    time_t secs = timegm(&tmv);
    if (secs == (time_t)-1) RETURN(0);
    GuestWrite64(ARG(4), kFiletimeUnixEpoch + uint64_t(secs) * 10000000ull + uint64_t(GuestRead16(f + 12)) * 10000ull);
    RETURN(1);
}
