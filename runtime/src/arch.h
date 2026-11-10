#pragma once
// Host architecture helpers (x86-64 desktop, ARM64 Android).
#include <cstdint>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

// Spin-wait hint.
inline void CpuRelax()
{
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

// Instruction pointer of an interrupted thread (signal handler context).
inline uint64_t& HostPc(void* uc)
{
#if defined(__x86_64__)
    return reinterpret_cast<uint64_t&>(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return reinterpret_cast<uint64_t&>(static_cast<ucontext_t*>(uc)->uc_mcontext.pc);
#endif
}

// memfd_create (bionic only has it from API 30).
inline int HostMemfd(const char* name)
{
    return int(syscall(__NR_memfd_create, name, 1u /* MFD_CLOEXEC */));
}
