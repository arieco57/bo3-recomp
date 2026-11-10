// xboxkrnl: strings, debug output, exceptions, XEX module queries, config settings, CRT printf.
#include "guest.h"
#include "loader.h"
#include "stubs.h"
#include "profiler.h"
#include "thread.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <xex.h>

// ---------------------------------------------------------------------------------------
// Guest printf. Arguments come either from registers/stack (varargs call) or from a va_list,
// which on the Xbox 360 is an array of 8-byte big-endian slots.

namespace
{
struct ArgSource
{
    PPCContext* ctx = nullptr;   // varargs: first arg index `next` into r3..r10, then stack
    uint32_t next = 0;
    uint32_t vaList = 0;         // va_list pointer (if non-zero)

    uint64_t Next()
    {
        uint32_t i = next++;
        if (vaList) return GuestRead64(vaList + i * 8);
        switch (i)
        {
        case 0: return ctx->r3.u64;
        case 1: return ctx->r4.u64;
        case 2: return ctx->r5.u64;
        case 3: return ctx->r6.u64;
        case 4: return ctx->r7.u64;
        case 5: return ctx->r8.u64;
        case 6: return ctx->r9.u64;
        case 7: return ctx->r10.u64;
        }
        // Arguments past r10 live in 8-byte slots from r1+0x50 (verified at an NtCreateFile
        // call site: the 9th argument is stored with `stw rX,84(r1)`).
        return GuestRead64(ctx->r1.u32 + 0x50 + (i - 8) * 8);
    }
};

std::string Format(const char* fmt, ArgSource& args)
{
    std::string out;
    for (const char* p = fmt; *p; p++)
    {
        if (*p != '%') { out += *p; continue; }
        std::string spec = "%";
        p++;
        while (*p && strchr("-+ #0", *p)) spec += *p++;
        if (*p == '*') { spec += std::to_string((int32_t)args.Next()); p++; }
        else while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.')
        {
            spec += *p++;
            if (*p == '*') { spec += std::to_string((int32_t)args.Next()); p++; }
            else while (*p >= '0' && *p <= '9') spec += *p++;
        }
        bool isLong64 = false, isShort = false, wide = false;
        for (;;)
        {
            if (*p == 'l') { if (p[1] == 'l') { isLong64 = true; p++; } p++; continue; }
            if (*p == 'h') { isShort = true; p++; continue; }
            if (*p == 'I' && p[1] == '6' && p[2] == '4') { isLong64 = true; p += 3; continue; }
            if (*p == 'z' || *p == 't' || *p == 'j') { p++; continue; }
            break;
        }
        char conv = *p;
        if (!conv) break;
        char buf[512];
        switch (conv)
        {
        case '%': out += '%'; break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
        {
            uint64_t v = args.Next();
            if (isLong64) snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (long long)v);
            else if (conv == 'd' || conv == 'i') snprintf(buf, sizeof(buf), (spec + conv).c_str(), isShort ? (int)(int16_t)v : (int)(int32_t)v);
            else snprintf(buf, sizeof(buf), (spec + conv).c_str(), isShort ? (unsigned)(uint16_t)v : (unsigned)(uint32_t)v);
            out += buf;
            break;
        }
        case 'p':
            snprintf(buf, sizeof(buf), "%08X", (uint32_t)args.Next());
            out += buf;
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        {
            uint64_t bits = args.Next();
            double d;
            memcpy(&d, &bits, 8);
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
            out += buf;
            break;
        }
        case 's': case 'S':
        {
            uint32_t ptr = (uint32_t)args.Next();
            wide = conv == 'S' || (conv == 's' && false);
            std::string s = !ptr ? "(null)" : wide ? Utf16ToUtf8(GuestWString(ptr)) : GuestString(ptr);
            snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        case 'n': args.Next(); break;
        default: out += spec; out += conv; break;
        }
    }
    return out;
}

uint32_t WriteOut(uint32_t buffer, int64_t count, const std::string& s)
{
    // _snprintf semantics: no terminator if truncated, returns -1.
    if (count >= 0 && int64_t(s.size()) > count)
    {
        memcpy(g_base + buffer, s.data(), count);
        return 0xFFFFFFFF;
    }
    memcpy(g_base + buffer, s.c_str(), s.size() + 1);
    return uint32_t(s.size());
}
}

std::string GuestFormat(const char* fmt, PPCContext& ctx, uint32_t firstArg)
{
    ArgSource a{ &ctx, firstArg };
    return Format(fmt, a);
}

GUEST_IMPORT(sprintf)
{
    ArgSource a{ &ctx, 2 };
    RETURN(WriteOut(ARG(3), -1, Format(GuestString(ARG(4)).c_str(), a)));
}

GUEST_IMPORT(_snprintf)
{
    ArgSource a{ &ctx, 3 };
    RETURN(WriteOut(ARG(3), ARG(4), Format(GuestString(ARG(5)).c_str(), a)));
}

GUEST_IMPORT(_vsnprintf)
{
    ArgSource a{ nullptr, 0, ARG(6) };
    RETURN(WriteOut(ARG(3), ARG(4), Format(GuestString(ARG(5)).c_str(), a)));
}

GUEST_IMPORT(DbgPrint)
{
    ArgSource a{ &ctx, 1 };
    std::string s = Format(GuestString(ARG(3)).c_str(), a);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    Log("DbgPrint: %s", s.c_str());
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(DbgBreakPoint) { LogWarn("DbgBreakPoint (lr=0x%08X)", uint32_t(ctx.lr)); RETURN(0); }

GUEST_IMPORT(KeBugCheck) { Fatal("KeBugCheck(0x%X) lr=0x%08X", ARG(3), uint32_t(ctx.lr)); }
GUEST_IMPORT(KeBugCheckEx)
{
    Fatal("KeBugCheckEx(0x%X, 0x%X, 0x%X, 0x%X, 0x%X) lr=0x%08X", ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), uint32_t(ctx.lr));
}

// ---------------------------------------------------------------------------------------
// exceptions

GUEST_IMPORT(RtlRaiseException)
{
    uint32_t rec = ARG(3);
    uint32_t code = GuestRead32(rec);
    if (code == 0x406D1388)   // MSVC "SetThreadName"
    {
        uint32_t namePtr = GuestRead32(rec + 0x14 + 4);
        uint32_t tid = GuestRead32(rec + 0x14 + 8);
        std::string name = GuestString(namePtr);
        Log("thread name: 0x%X = \"%s\"", tid, name.c_str());
        if (const char* want = getenv("BO3_PROFILE_THREAD"); want && tid == 0xFFFFFFFFu && name == want) ProfilerAttachThisThread(true);
        RETURN(0);
    }
    LogError("RtlRaiseException code 0x%08X flags 0x%X at 0x%08X (lr=0x%08X) - exceptions are not supported",
             code, GuestRead32(rec + 4), GuestRead32(rec + 0xC), uint32_t(ctx.lr));
    RETURN(0);
}

// ---------------------------------------------------------------------------------------
// strings

GUEST_IMPORT(RtlInitAnsiString)
{
    uint32_t dst = ARG(3), src = ARG(4);
    uint16_t len = src ? uint16_t(strlen((const char*)(g_base + src))) : 0;
    GuestWrite16(dst, len);
    GuestWrite16(dst + 2, src ? len + 1 : 0);
    GuestWrite32(dst + 4, src);
    RETURN(0);
}

GUEST_IMPORT(RtlInitUnicodeString)
{
    uint32_t dst = ARG(3), src = ARG(4);
    uint16_t len = src ? uint16_t(GuestWString(src).size() * 2) : 0;
    GuestWrite16(dst, len);
    GuestWrite16(dst + 2, src ? len + 2 : 0);
    GuestWrite32(dst + 4, src);
    RETURN(0);
}

GUEST_IMPORT(RtlFreeAnsiString)
{
    uint32_t buf = GuestRead32(ARG(3) + 4);
    if (buf) GuestFree(buf);
    GuestWrite32(ARG(3) + 4, 0);
    GuestWrite16(ARG(3), 0);
    RETURN(0);
}

// (ANSI_STRING* dst, UNICODE_STRING* src, BOOLEAN alloc)
GUEST_IMPORT(RtlUnicodeStringToAnsiString)
{
    uint32_t dst = ARG(3), src = ARG(4);
    uint16_t srcLen = GuestRead16(src) / 2;
    uint32_t srcBuf = GuestRead32(src + 4);
    std::string s;
    for (uint16_t i = 0; i < srcLen; i++) { uint16_t c = GuestRead16(srcBuf + i * 2); s += c < 0x80 ? char(c) : '?'; }
    uint32_t buf;
    if (ARG(5))
    {
        buf = GuestAlloc(uint32_t(s.size() + 1));
        GuestWrite16(dst + 2, uint16_t(s.size() + 1));
        GuestWrite32(dst + 4, buf);
    }
    else
    {
        buf = GuestRead32(dst + 4);
        if (GuestRead16(dst + 2) < s.size() + 1) RETURN(0x80000005);   // STATUS_BUFFER_OVERFLOW
    }
    memcpy(g_base + buf, s.c_str(), s.size() + 1);
    GuestWrite16(dst, uint16_t(s.size()));
    RETURN(STATUS_SUCCESS);
}

// (dst wchar*, dst_bytes, written*, src char*, src_bytes) - Latin-1 conversion
GUEST_IMPORT(RtlMultiByteToUnicodeN)
{
    uint32_t n = std::min(ARG(4) / 2, ARG(7));
    for (uint32_t i = 0; i < n; i++) GuestWrite16(ARG(3) + i * 2, GuestRead8(ARG(6) + i));
    if (ARG(5)) GuestWrite32(ARG(5), n * 2);
    RETURN(STATUS_SUCCESS);
}

// (dst char*, dst_bytes, written*, src wchar*, src_bytes)
GUEST_IMPORT(RtlUnicodeToMultiByteN)
{
    uint32_t n = std::min(ARG(4), ARG(7) / 2);
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = GuestRead16(ARG(6) + i * 2);
        GuestWrite8(ARG(3) + i, c < 0x100 ? uint8_t(c) : '?');
    }
    if (ARG(5)) GuestWrite32(ARG(5), n);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(RtlFillMemoryUlong)
{
    for (uint32_t i = 0; i + 4 <= ARG(4); i += 4) GuestWrite32(ARG(3) + i, ARG(5));
    RETURN(0);
}

GUEST_IMPORT(RtlCompareMemoryUlong)
{
    uint32_t i = 0;
    for (; i + 4 <= ARG(4); i += 4)
        if (GuestRead32(ARG(3) + i) != ARG(5)) break;
    RETURN(i);
}

GUEST_IMPORT(RtlNtStatusToDosError)
{
    uint32_t s = ARG(3);
    switch (s)
    {
    case STATUS_SUCCESS: RETURN(ERROR_SUCCESS);
    case STATUS_NO_SUCH_FILE: case STATUS_OBJECT_NAME_NOT_FOUND: RETURN(ERROR_FILE_NOT_FOUND);
    case STATUS_OBJECT_PATH_NOT_FOUND: RETURN(ERROR_PATH_NOT_FOUND);
    case STATUS_ACCESS_DENIED: RETURN(ERROR_ACCESS_DENIED);
    case STATUS_INVALID_HANDLE: RETURN(ERROR_INVALID_HANDLE);
    case STATUS_NO_MORE_FILES: RETURN(ERROR_NO_MORE_FILES);
    case STATUS_OBJECT_NAME_COLLISION: RETURN(ERROR_ALREADY_EXISTS);
    case STATUS_END_OF_FILE: RETURN(38);           // ERROR_HANDLE_EOF
    case STATUS_PENDING: RETURN(ERROR_IO_PENDING);
    case STATUS_TIMEOUT: RETURN(1460);             // ERROR_TIMEOUT
    case STATUS_INVALID_PARAMETER: RETURN(ERROR_INVALID_PARAMETER);
    default:
        if ((s & 0xFFFF0000) == 0x80070000) RETURN(s & 0xFFFF);
        RETURN(s == 0 ? 0 : 317);                  // ERROR_MR_MID_NOT_FOUND
    }
}

// ---------------------------------------------------------------------------------------
// XEX module queries

GUEST_IMPORT(RtlImageXexHeaderField) { RETURN(XexHeaderFieldGuest(ARG(4))); }

GUEST_IMPORT(XexGetModuleHandle)
{
    std::string name = ARG(3) ? GuestString(ARG(3)) : std::string{};
    if (name.empty() || name == "default.xex" || name == "default_zm.xex")
    {
        GuestWrite32(ARG(4), g_module.ldrEntryGuest);
        RETURN(STATUS_SUCCESS);
    }
    static bool warned = false;
    if (!warned) { warned = true; LogWarn("XexGetModuleHandle(\"%s\"): not found (further warnings suppressed)", name.c_str()); }
    GuestWrite32(ARG(4), 0);
    RETURN(STATUS_NO_SUCH_FILE);
}

GUEST_IMPORT(XexGetProcedureAddress)
{
    LogWarn("XexGetProcedureAddress(0x%X, ordinal %u)", ARG(3), ARG(4));
    GuestWrite32(ARG(5), 0);
    RETURN(STATUS_NOT_IMPLEMENTED);
}

GUEST_IMPORT(XexCheckExecutablePrivilege)
{
    // privilege bit in XEX_HEADER_SYSTEM_FLAGS (only the first 32 privileges are in that field)
    uint32_t priv = ARG(3);
    uint32_t flags = XexHeaderFieldGuest(XEX_HEADER_SYSTEM_FLAGS);
    RETURN(priv < 32 ? (flags >> priv) & 1 : 0);
}

GUEST_IMPORT(XexLoadImage)
{
    LogWarn("XexLoadImage(\"%s\") not supported", GuestString(ARG(3)).c_str());
    RETURN(STATUS_NOT_SUPPORTED);
}

GUEST_IMPORT(XexUnloadImage) { RETURN(STATUS_SUCCESS); }

// ---------------------------------------------------------------------------------------
// config

// ExGetXConfigSetting(category, setting, buffer, buffer_size, required_size*)
GUEST_IMPORT(ExGetXConfigSetting)
{
    uint16_t cat = uint16_t(ARG(3)), id = uint16_t(ARG(4));
    uint32_t buf = ARG(5), size = ARG(6);
    uint8_t data[8] = {};
    uint32_t len = 4;
    uint32_t v = 0;
    bool known = true;
    switch ((cat << 16) | id)
    {
    case 0x00020009: v = 1; break;            // XCONFIG_USER_LANGUAGE: English
    case 0x0002000A: v = 0x00000300; break;   // XCONFIG_USER_VIDEO_FLAGS: widescreen + 720p-ish
    case 0x00020002: v = 0x00010000; break;   // XCONFIG_USER_TIME_ZONE_BIAS?
    case 0x00020007: v = 0; break;            // XCONFIG_USER_AUDIO_FLAGS
    case 0x00020003: v = 0; break;            // time zone standard name...
    case 0x00030006: v = 0; len = 2; break;   // XCONFIG_CONSOLE_... (2 bytes)
    case 0x00030005: v = 0xFF; len = 1; break;// XCONFIG_CONSOLE_GAME_REGION? (all)
    default: known = false; break;
    }
    if (!known) LogWarn("ExGetXConfigSetting(%u, %u) unknown, returning zeros", cat, id);
    if (len == 4) { data[0] = v >> 24; data[1] = v >> 16; data[2] = v >> 8; data[3] = v; }
    else if (len == 2) { data[0] = v >> 8; data[1] = v; }
    else data[0] = uint8_t(v);
    if (ARG(7)) GuestWrite16(ARG(7), uint16_t(len));
    if (buf && size) memcpy(g_base + buf, data, std::min(size, len));
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(HalReturnToFirmware)
{
    Log("HalReturnToFirmware(%u): game requested exit", ARG(3));
    QuickExit(0);
}

GUEST_IMPORT(ExRegisterTitleTerminateNotification) { RETURN(0); }
GUEST_IMPORT(KiApcNormalRoutineNop) { RETURN(0); }

std::string GuestBacktrace(const PPCContext& ctx, int depth)
{
    char b[16];
    snprintf(b, sizeof(b), "%08X", (uint32_t)ctx.lr);
    std::string bt = b;
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < depth; i++)
    {
        uint32_t next = GuestRead32(sp);
        if (next <= sp || next - sp > 0x100000) break;
        uint32_t lr = GuestRead32(next - 8);
        if (lr < 0x82000000 || lr >= 0x84000000) break;
        snprintf(b, sizeof(b), " %08X", lr);
        bt += b;
        sp = next;
    }
    return bt;
}

int g_traceImportsThread = []
{
    const char* e = getenv("BO3_TRACE_IMPORTS");
    if (!e) return -1;
    return strcmp(e, "all") == 0 ? 0 : atoi(e);
}();

bool TraceImportsHere()
{
    if (g_traceImportsThread == 0) return true;
    GuestThread* t = CurrentThread();
    return t && int(t->id) == g_traceImportsThread;
}

void TraceImportCall(const char* name, const PPCContext& ctx)
{
    Log("> %s(0x%X, 0x%X, 0x%X, 0x%X, 0x%X) lr=0x%X", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        ctx.r7.u32, (uint32_t)ctx.lr);
}

void TraceImportResult(const char* name, const PPCContext& ctx) { Log("< %s -> 0x%X", name, ctx.r3.u32); }
