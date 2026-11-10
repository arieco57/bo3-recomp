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
// SIGSEGV on an armed granule: unprotect it and return true (the store is retried).
bool WatchHandleFault(void* addr);
