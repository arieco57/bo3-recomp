#pragma once
#include <cstdint>

// CPU write tracking of guest physical memory in 64 KiB granules (texture change detection):
// armed granules are write-protected in all three physical views; the first guest store
// faults, marks the granule dirty and unprotects it.
void WatchInit();
bool WatchEnabled();
// true: no write into [phys, phys + size) since this caller's previous check (`stamp`, 0 at
// first). Otherwise the range is armed again (before the caller reads it), `stamp` updated and
// false returned.
bool WatchCheckAndArm(uint32_t phys, uint32_t size, uint32_t& stamp);
// Diagnostics: still no write since `stamp` (without arming).
bool WatchClean(uint32_t phys, uint32_t size, uint32_t stamp);
// The GPU wrote the range (resolve to memory): next check reports it changed.
void WatchMarkDirty(uint32_t phys, uint32_t size);
// A syscall writes guest memory (pread / recv cannot take the fault): the range is unprotected
// and cannot be armed until the scope ends.
void WatchBeginHostWrite(uint32_t guest, uint32_t size);
void WatchEndHostWrite(uint32_t guest, uint32_t size);
struct WatchHostWrite
{
    uint32_t guest, size;
    WatchHostWrite(uint32_t g, uint32_t n) : guest(g), size(n) { WatchBeginHostWrite(g, n); }
    ~WatchHostWrite() { WatchEndHostWrite(guest, size); }
};
// The host is about to store into [phys, phys + size) (even from a signal handler): unprotected now, marked written.
void WatchHostWriteNow(uint32_t phys, uint32_t size);
// SIGSEGV on an armed granule: unprotect it and return true (the store is retried).
bool WatchHandleFault(void* addr);

// GPUs that see guest memory without snooping the CPU caches (Android: KGSL user memory imported
// without IO-coherency, kernel 4.14): every granule the CPU may have written since the last GPU
// submission is cleaned from the CPU caches before the next one (WatchFlushForGpu, called right
// before each vkQueueSubmit); clean granules are write-protected so the next store marks them.
// By default only what the GPU reads is cleaned, when it is about to (WatchRequestUpload, as in copy
// mode); BO3_FLUSH_ALL=1: every written granule before each submission (the old way).
void WatchEnableGpuFlush();
void WatchFlushForGpu();
// copy mode or cleaning on request: the renderer reports what each draw / texture upload reads
bool WatchNeedsRequests();
// some granule of [phys, phys + size) was written by the CPU and not cleaned / uploaded since
bool WatchAnyDirty(uint32_t phys, uint32_t size);

// Copy mode (drivers that cannot share memory with the CPU: Adreno stock drivers, many Mali / PowerVR):
// the GPU has its own copy of guest physical memory (`gpu`, mapped); WatchFlushForGpu copies the
// granules the CPU wrote into it. `flush` (optional) makes CPU writes of a non-coherent mapping visible.
void WatchEnableCopyMode(uint8_t* gpu, void (*flush)(uint32_t offset, uint32_t size));
bool WatchCopyMode();
// Copy mode: the GPU is about to read [phys, phys + size) (a draw's vertex / index buffers): the granules
// of it the CPU wrote go up now (only what draws read is copied, not every CPU write).
void WatchRequestUpload(uint32_t phys, uint32_t size);
uint64_t WatchCopyTakeStats();
uint64_t WatchCopyPeekStats();   // bytes uploaded since the last call
// The GPU will write [phys, phys + size) of its copy: the range goes up now and a snapshot of it is
// returned; those granules are not uploaded again (they would overwrite the GPU's data) until
// WatchGpuWriteDone copies back the words the GPU changed (compared with the snapshot - the rest
// of the range may hold newer CPU data).
#include <memory>
#include <vector>
std::shared_ptr<std::vector<uint32_t>> WatchGpuWriteBegin(uint32_t phys, uint32_t size);
void WatchGpuWriteDone(uint32_t phys, uint32_t size, const std::shared_ptr<std::vector<uint32_t>>& snap,
                       void (*invalidate)(uint32_t offset, uint32_t size));
