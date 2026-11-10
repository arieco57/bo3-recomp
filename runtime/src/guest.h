#pragma once
// Common include for runtime code: the per-target XenonRecomp context + helpers to implement
// kernel/XAM imports and to call guest code.
#include <ppc_config.h>
#include <ppc_context.h>

#include <string>

#include "log.h"
#include "memory.h"

PPC_EXTERN_FUNC(_xstart);

// Implement an import: GUEST_IMPORT(NtClose) { ... ctx.r3 ... }
// With BO3_TRACE_IMPORTS=<guest thread id|all> every implemented import call on that thread is
// logged with its first arguments, caller and result.
extern int g_traceImportsThread;   // -1 off, 0 all threads
bool TraceImportsHere();
void TraceImportCall(const char* name, const PPCContext& ctx);
void TraceImportResult(const char* name, const PPCContext& ctx);
#define GUEST_IMPORT(name)                                                              \
    static void name##_impl(PPCContext& __restrict ctx, uint8_t* base);                 \
    PPC_FUNC(__imp__##name)                                                             \
    {                                                                                   \
        if (g_traceImportsThread >= 0 && TraceImportsHere()) [[unlikely]]               \
        {                                                                               \
            TraceImportCall(#name, ctx);                                                \
            name##_impl(ctx, base);                                                     \
            TraceImportResult(#name, ctx);                                              \
            return;                                                                     \
        }                                                                               \
        name##_impl(ctx, base);                                                         \
    }                                                                                   \
    static void name##_impl(PPCContext& __restrict ctx, uint8_t* base)
// Weak fallback for imports without an implementation (generated import_stubs.cpp).
#define GUEST_IMPORT_WEAK(name) __attribute__((weak)) PPC_FUNC(__imp__##name)

// Arguments r3..r10 and return value.
#define ARG(n) (ctx.r##n.u32)
#define ARG64(n) (ctx.r##n.u64)
// Arguments 9+ (0-based index n >= 8) are passed on the stack in 8-byte slots at r1+0x50.
#define STACK_ARG(n) GuestRead32(ctx.r1.u32 + 0x54 + ((n) - 8) * 8)
// 32-bit results are sign-extended into the 64-bit register like compiled PPC code leaves them.
#define RETURN(v)                                                                      \
    do                                                                                 \
    {                                                                                  \
        auto _rv = (v);                                                                \
        if constexpr (sizeof(_rv) <= 4) ctx.r3.s64 = int32_t(uint32_t(_rv));           \
        else ctx.r3.u64 = uint64_t(_rv);                                               \
        return;                                                                        \
    } while (0)

// NTSTATUS
constexpr uint32_t STATUS_SUCCESS = 0x00000000;
constexpr uint32_t STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr uint32_t STATUS_USER_APC = 0x000000C0;
constexpr uint32_t STATUS_ALERTED = 0x00000101;
constexpr uint32_t STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t STATUS_PENDING = 0x00000103;
constexpr uint32_t STATUS_OBJECT_NAME_EXISTS = 0x40000000;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t STATUS_NOT_SUPPORTED = 0xC00000BB;

// Win32 errors (XAM)
constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_FILE_NOT_FOUND = 2;
constexpr uint32_t ERROR_PATH_NOT_FOUND = 3;
constexpr uint32_t ERROR_ACCESS_DENIED = 5;
constexpr uint32_t ERROR_INVALID_HANDLE = 6;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
constexpr uint32_t ERROR_ALREADY_EXISTS = 183;
constexpr uint32_t ERROR_IO_PENDING = 997;
constexpr uint32_t ERROR_NOT_FOUND = 1168;
constexpr uint32_t ERROR_NO_SUCH_USER = 1317;
constexpr uint32_t ERROR_FUNCTION_FAILED = 1627;
constexpr uint32_t ERROR_CANCELLED = 1223;
constexpr uint32_t ERROR_NONE_MAPPED = 1332;

// Calling guest code from the host.
inline PPCFunc* GuestLookup(uint32_t addr)
{
    if (addr < PPC_CODE_BASE || addr >= PPC_CODE_BASE + PPC_CODE_SIZE) return nullptr;
    return PPC_LOOKUP_FUNC(g_base, addr);
}

// Call a guest function on the given context (arguments already in r3..).
void GuestCall(PPCContext& ctx, uint32_t addr);

// printf-style formatting of a guest format string with varargs starting at argument
// index firstArg (0 = r3).
std::string GuestFormat(const char* fmt, PPCContext& ctx, uint32_t firstArg);
// "lr caller caller..." from the guest stack back chain (callers save LR at backchain-8).
std::string GuestBacktrace(const PPCContext& ctx, int depth = 12);

// Read a guest C string / UTF-16BE string.
std::string GuestString(uint32_t addr);
std::string GuestAnsiString(uint32_t ansiStringPtr);       // ANSI_STRING*
std::u16string GuestWString(uint32_t addr);
std::string Utf16ToUtf8(const std::u16string& s);
