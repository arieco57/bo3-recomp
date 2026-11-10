#pragma once
#include "kobject.h"

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

struct PPCContext;

// A guest (Xbox 360) thread, run on its own host thread.
//
// Guest-visible structures (layout from Xenia):
//   PCR  (r13): +0x00 tls_ptr, +0x30 self, +0x70 stack_base (high), +0x74 stack_limit (low),
//               +0x100 current_thread (KTHREAD*), +0x10C current_cpu, +0x150 dpc_active
//   KTHREAD   : +0x00 dispatcher header (type 6), +0x5C stack_base, +0x60 stack_limit,
//               +0x68 tls_address, +0x14C thread_id, +0x160 last_error
struct GuestThread : KObject
{
    GuestThread() : KObject(KType::Thread) {}
    ~GuestThread() override;

    uint32_t id = 0;
    uint32_t stackBase = 0, stackSize = 0;   // stackBase = lowest address
    uint32_t pcr = 0, kthread = 0, tlsStatic = 0;
    uint32_t startAddress = 0, startContext = 0, xapiStartup = 0;
    std::vector<uint32_t> tlsSlots;          // KeTlsGetValue/SetValue (host side)
    PPCContext* ctx = nullptr;

    bool terminated = false;                 // guarded by g_dispatchLock
    uint32_t exitCode = 0;
    int suspendCount = 0;                    // guarded by g_dispatchLock
    int priority = 0;
    uint32_t affinity = 0;

    bool Waitable() const override { return true; }
    bool IsSignaled(GuestThread*) const override { return terminated; }

    std::thread host;
    pthread_t pthread = 0;                   // (host thread, for diagnostics)
    std::atomic<uint64_t> sampledRip{ 0 };   // ThreadSampleAll
    std::atomic<uint32_t> waitingCs{ 0 };    // critical section being entered (diagnostics)
    std::atomic<const void*> waitingObj{ nullptr };   // KObject waited for (diagnostics)
};

// Diagnostics: log every guest thread's host instruction pointer and guest backtrace.
void ThreadSampleAll(const char* why);

// Creates a thread. suspended = CREATE_SUSPENDED. Returns nullptr on failure.
std::shared_ptr<GuestThread> ThreadCreate(uint32_t stackSize, uint32_t xapiStartup, uint32_t startAddress,
                                          uint32_t startContext, bool suspended, bool isMain = false);
void ThreadResume(GuestThread* t);    // decrements the suspend count (starts the thread)
void ThreadSuspend(GuestThread* t);

GuestThread* CurrentThread();
std::shared_ptr<KObject> CurrentThreadObject();
PPCContext& CurrentContext();

// Throws to unwind the current guest thread (ExTerminateThread).
[[noreturn]] void ThreadExitCurrent(uint32_t exitCode);

// A host-driven guest thread: has guest PCR/TLS/stack so it can call guest callbacks
// (graphics interrupts, DPCs). `body` runs on the new host thread; use GuestCallOnThread.
std::shared_ptr<GuestThread> ThreadCreateHost(const char* name, std::function<void(GuestThread&)> body);
// Calls a guest function on this host-driven thread with fresh stack pointer.
void GuestCallOnThread(GuestThread& t, uint32_t addr, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0);

// Runs the guest entry point on a new guest thread and waits for it.
int RunMainThread(uint32_t entry);

uint32_t ThreadCount();
