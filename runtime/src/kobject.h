#pragma once
// Kernel objects (events, semaphores, mutants, timers, threads, files) and handles.
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct GuestThread;
struct KObject;

// A thread blocked in WaitForObjects: registered on each object it waits for, woken alone when
// one of them is signalled (one shared condition variable woke every waiting thread at every
// signal). `granted`: a pulse already handed it that object.
struct KWaiter
{
    std::condition_variable cv;
    KObject* granted = nullptr;
};

enum class KType { Event, Semaphore, Mutant, Timer, Thread, File, Other };

// Dispatcher header types (guest X_DISPATCHER_HEADER.Type)
enum : uint8_t
{
    DISP_NOTIFICATION_EVENT = 0, DISP_SYNCHRONIZATION_EVENT = 1, DISP_MUTANT = 2,
    DISP_SEMAPHORE = 5, DISP_THREAD = 6, DISP_NOTIFICATION_TIMER = 8, DISP_SYNCHRONIZATION_TIMER = 9,
};

struct KObject : std::enable_shared_from_this<KObject>
{
    explicit KObject(KType t) : type(t) {}
    virtual ~KObject() = default;
    KType type;
    uint32_t guestBody = 0;   // guest memory with a dispatcher header (0 if none)
    std::string name;
    std::vector<KWaiter*> waiters;   // (g_dispatchLock)

    // Waitable interface. Called with g_dispatchLock held.
    virtual bool Waitable() const { return false; }
    virtual bool IsSignaled(GuestThread*) const { return false; }
    virtual void Acquire(GuestThread*) {}
};

// One lock for all dispatcher state. WaitForObjects waiters sleep on their own KWaiter; other
// waits (thread start / exit) on g_dispatchCv.
extern std::mutex g_dispatchLock;
extern std::condition_variable g_dispatchCv;
// Something changed (not holding g_dispatchLock): wake every waiter to re-check.
void DispatchNotify();
// `o` was signalled (holding g_dispatchLock): wake the threads waiting for it.
void DispatchNotifyObject(KObject* o);

struct KEvent : KObject
{
    KEvent(bool manual, bool initial) : KObject(KType::Event), manualReset(manual), signaled(initial) {}
    bool manualReset, signaled;
    bool Waitable() const override { return true; }
    bool IsSignaled(GuestThread*) const override { return signaled; }
    void Acquire(GuestThread*) override { if (!manualReset) signaled = false; }
    void SyncGuest();          // mirror SignalState into the guest header
};

struct KSemaphore : KObject
{
    KSemaphore(int32_t c, int32_t m) : KObject(KType::Semaphore), count(c), maximum(m) {}
    int32_t count, maximum;
    bool Waitable() const override { return true; }
    bool IsSignaled(GuestThread*) const override { return count > 0; }
    void Acquire(GuestThread*) override { count--; }
};

struct KMutant : KObject
{
    KMutant() : KObject(KType::Mutant) {}
    GuestThread* owner = nullptr;
    uint32_t recursion = 0;
    bool abandoned = false;
    bool Waitable() const override { return true; }
    bool IsSignaled(GuestThread* t) const override { return owner == nullptr || owner == t; }
    void Acquire(GuestThread* t) override { owner = t; recursion++; }
};

struct KTimer : KObject
{
    explicit KTimer(bool manual) : KObject(KType::Timer), manualReset(manual) {}
    bool manualReset, signaled = false;
    uint64_t dueNs = 0, periodNs = 0;   // dueNs = 0: not armed
    uint32_t generation = 0;
    bool Waitable() const override { return true; }
    bool IsSignaled(GuestThread*) const override;
    void Acquire(GuestThread*) override;
};

// Handles ------------------------------------------------------------------------------
uint32_t HandleCreate(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> HandleGet(uint32_t handle);
bool HandleClose(uint32_t handle);
uint32_t HandleDuplicate(uint32_t handle);

// Special handle values
constexpr uint32_t CURRENT_THREAD_HANDLE = 0xFFFFFFFE;
constexpr uint32_t CURRENT_PROCESS_HANDLE = 0xFFFFFFFF;

// Guest-pointer objects (dispatcher headers living in guest memory).
// Returns the object bound to a guest body, creating it from the header type if needed.
std::shared_ptr<KObject> ObjectFromGuest(uint32_t guestPtr);
void ObjectBindGuest(uint32_t guestPtr, std::shared_ptr<KObject> obj);
// Allocates a guest body (dispatcher header) for a handle-based object.
void ObjectCreateGuestBody(KObject* obj, uint8_t dispType, uint32_t size = 0x20);

// Named objects
std::shared_ptr<KObject> ObjectFindNamed(const std::string& name);
void ObjectAddNamed(const std::string& name, std::shared_ptr<KObject> obj);

// Waiting. timeout: nullopt = infinite, otherwise host ns relative.
// Returns STATUS_WAIT_0 + index, STATUS_TIMEOUT, STATUS_ABANDONED_WAIT_0 + index.
uint32_t WaitForObjects(const std::vector<std::shared_ptr<KObject>>& objs, bool waitAll,
                        std::optional<uint64_t> timeoutNs, bool alertable);

// Converts a guest LARGE_INTEGER* timeout (100ns units; negative = relative, positive = absolute
// FILETIME) to a relative host timeout. guestPtr == 0 -> infinite.
std::optional<uint64_t> GuestTimeoutToNs(uint32_t guestPtr);
