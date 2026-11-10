#include "kobject.h"
#include "guest.h"
#include "ktime.h"
#include "thread.h"

#include <atomic>
#include <unordered_map>

std::mutex g_dispatchLock;
std::condition_variable g_dispatchCv;
static std::vector<KWaiter*> s_waiters;   // every blocked WaitForObjects (g_dispatchLock)

void DispatchNotify()
{
    {
        std::lock_guard lock(g_dispatchLock);
        for (KWaiter* w : s_waiters) w->cv.notify_one();
    }
    g_dispatchCv.notify_all();
}

void DispatchNotifyObject(KObject* o)
{
    for (KWaiter* w : o->waiters) w->cv.notify_one();
}

static std::mutex s_handleLock;
static std::unordered_map<uint32_t, std::shared_ptr<KObject>> s_handles;
static uint32_t s_nextHandle = 0xF8000004;
static std::unordered_map<uint32_t, std::shared_ptr<KObject>> s_guestObjects;
static std::unordered_map<std::string, std::weak_ptr<KObject>> s_named;

void KEvent::SyncGuest()
{
    if (guestBody) GuestWrite32(guestBody + 4, signaled ? 1 : 0);
}

bool KTimer::IsSignaled(GuestThread*) const
{
    return signaled || (dueNs && KTimeHostNs() >= dueNs);
}

void KTimer::Acquire(GuestThread*)
{
    // Fire due timer, re-arm periodic ones.
    if (dueNs && KTimeHostNs() >= dueNs)
    {
        signaled = true;
        dueNs = periodNs ? KTimeHostNs() + periodNs : 0;
    }
    if (!manualReset) signaled = false;
}

uint32_t HandleCreate(std::shared_ptr<KObject> obj)
{
    std::lock_guard lock(s_handleLock);
    uint32_t h = s_nextHandle;
    s_nextHandle += 4;
    s_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<KObject> HandleGet(uint32_t handle)
{
    if (handle == CURRENT_THREAD_HANDLE) return CurrentThreadObject();
    std::lock_guard lock(s_handleLock);
    auto it = s_handles.find(handle);
    return it == s_handles.end() ? nullptr : it->second;
}

bool HandleClose(uint32_t handle)
{
    std::lock_guard lock(s_handleLock);
    return s_handles.erase(handle) != 0;
}

uint32_t HandleDuplicate(uint32_t handle)
{
    auto obj = HandleGet(handle);
    return obj ? HandleCreate(obj) : 0;
}

void ObjectCreateGuestBody(KObject* obj, uint8_t dispType, uint32_t size)
{
    obj->guestBody = GuestAlloc(size);
    GuestWrite8(obj->guestBody, dispType);
    GuestWrite8(obj->guestBody + 2, uint8_t(size / 4));
    GuestWrite32(obj->guestBody + 8, obj->guestBody + 8);    // empty wait list
    GuestWrite32(obj->guestBody + 12, obj->guestBody + 8);
    std::lock_guard lock(s_handleLock);
    s_guestObjects[obj->guestBody] = obj->shared_from_this();
}

void ObjectBindGuest(uint32_t guestPtr, std::shared_ptr<KObject> obj)
{
    std::lock_guard lock(s_handleLock);
    obj->guestBody = guestPtr;
    s_guestObjects[guestPtr] = std::move(obj);
}

std::shared_ptr<KObject> ObjectFromGuest(uint32_t p)
{
    if (p < 0x10000)
    {
        // A null/invalid dispatcher pointer: hand out a throwaway signalled event instead of crashing.
        static std::atomic<int> warned{ 0 };
        if (warned++ < 5) LogWarn("ObjectFromGuest: invalid object pointer 0x%08X (further warnings suppressed)", p);
        return std::make_shared<KEvent>(true, true);
    }
    {
        std::lock_guard lock(s_handleLock);
        auto it = s_guestObjects.find(p);
        if (it != s_guestObjects.end()) return it->second;
    }
    // First use of an object the game initialised itself: build it from the header.
    uint8_t type = GuestRead8(p);
    int32_t state = (int32_t)GuestRead32(p + 4);
    std::shared_ptr<KObject> obj;
    switch (type)
    {
    case DISP_NOTIFICATION_EVENT: obj = std::make_shared<KEvent>(true, state != 0); break;
    case DISP_SYNCHRONIZATION_EVENT: obj = std::make_shared<KEvent>(false, state != 0); break;
    case DISP_SEMAPHORE: obj = std::make_shared<KSemaphore>(state, (int32_t)GuestRead32(p + 0x10)); break;
    case DISP_MUTANT: obj = std::make_shared<KMutant>(); break;
    case DISP_NOTIFICATION_TIMER: obj = std::make_shared<KTimer>(true); break;
    case DISP_SYNCHRONIZATION_TIMER: obj = std::make_shared<KTimer>(false); break;
    default:
        LogWarn("ObjectFromGuest: unknown dispatcher type %u at 0x%08X, treating as auto-reset event", type, p);
        obj = std::make_shared<KEvent>(false, state != 0);
        break;
    }
    std::lock_guard lock(s_handleLock);
    auto [it, inserted] = s_guestObjects.emplace(p, obj);
    if (inserted) obj->guestBody = p;
    return it->second;
}

std::shared_ptr<KObject> ObjectFindNamed(const std::string& name)
{
    std::lock_guard lock(s_handleLock);
    auto it = s_named.find(name);
    return it == s_named.end() ? nullptr : it->second.lock();
}

void ObjectAddNamed(const std::string& name, std::shared_ptr<KObject> obj)
{
    std::lock_guard lock(s_handleLock);
    obj->name = name;
    s_named[name] = obj;
}

std::optional<uint64_t> GuestTimeoutToNs(uint32_t ptr)
{
    if (!ptr) return std::nullopt;
    int64_t t = (int64_t)GuestRead64(ptr);
    if (t < 0) return uint64_t(-t) * 100;
    if (t == 0) return 0;
    uint64_t now = KTimeSystemTime100ns();
    return uint64_t(t) > now ? (uint64_t(t) - now) * 100 : 0;
}

uint32_t WaitForObjects(const std::vector<std::shared_ptr<KObject>>& objs, bool waitAll,
                        std::optional<uint64_t> timeoutNs, bool alertable)
{
    GuestThread* self = CurrentThread();
    auto deadline = timeoutNs ? std::chrono::steady_clock::now() + std::chrono::nanoseconds(*timeoutNs)
                              : std::chrono::steady_clock::time_point::max();
    std::unique_lock lock(g_dispatchLock);
    KWaiter w;
    bool registered = false;
    struct Unregister
    {
        KWaiter& w; const std::vector<std::shared_ptr<KObject>>& objs; bool& on;
        ~Unregister()
        {
            if (!on) return;
            if (GuestThread* t = CurrentThread()) t->waitingObj = nullptr;
            for (auto& o : objs) std::erase(o->waiters, &w);
            std::erase(s_waiters, &w);
        }
    } unregister{ w, objs, registered };
    for (;;)
    {
        if (w.granted && !waitAll)
            for (size_t i = 0; i < objs.size(); i++)
                if (objs[i].get() == w.granted) return uint32_t(STATUS_SUCCESS + i);   // pulsed (acquired by the pulse)
        w.granted = nullptr;
        if (waitAll)
        {
            bool all = true;
            for (auto& o : objs) all &= o->IsSignaled(self);
            if (all)
            {
                for (auto& o : objs) o->Acquire(self);
                return STATUS_SUCCESS;
            }
        }
        else
        {
            for (size_t i = 0; i < objs.size(); i++)
                if (objs[i]->IsSignaled(self))
                {
                    bool abandoned = objs[i]->type == KType::Mutant && static_cast<KMutant*>(objs[i].get())->abandoned;
                    objs[i]->Acquire(self);
                    return uint32_t((abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_SUCCESS) + i);
                }
        }
        if (timeoutNs && *timeoutNs == 0) return STATUS_TIMEOUT;

        // Timers need periodic re-evaluation; everything else is notified.
        bool hasTimer = false;
        for (auto& o : objs) hasTimer |= o->type == KType::Timer;
        auto wakeAt = deadline;
        if (hasTimer) wakeAt = std::min(wakeAt, std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
        if (!registered)
        {
            if (self) self->waitingObj = objs[0].get();
            for (auto& o : objs) o->waiters.push_back(&w);
            s_waiters.push_back(&w);
            registered = true;
        }
        if (w.cv.wait_until(lock, wakeAt) == std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline)
        {
            // one last check
            bool any = false;
            for (auto& o : objs) any |= o->IsSignaled(self);
            if (w.granted && !waitAll) continue;   // a pulse arrived with the timeout
            if (!any || waitAll) return STATUS_TIMEOUT;
        }
    }
}
