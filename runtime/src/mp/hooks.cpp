// Hooks into recompiled game functions (Multiplayer executable, TU1 addresses).
// Generated guest functions are weak aliases of __imp__sub_XXXXXXXX, so defining sub_XXXXXXXX
// here replaces them while still allowing the original to be called.
#include "guest.h"
#include <chrono>
#include <set>
#include "host_input.h"
#include "video_options.h"
#include "live_hooks.h"

PPC_FUNC_IMPL(__imp__sub_82649010);

// Engine assert / fatal error: (const char* file, int line, int level, const char* fmt, ...)
// e.g. ("src\\xenon\\xenon_net.cpp", 964, 1, "Unable to find an XNADDR.\n"). It longjmps
// to the error handler, so log before calling through.
PPC_FUNC(sub_82649010)
{
    std::string msg = GuestFormat(GuestString(ctx.r6.u32).c_str(), ctx, 4);
    while (!msg.empty() && msg.back() == '\n') msg.pop_back();
    LogError("GAME ERROR %s:%d (level %d): %s", GuestString(ctx.r3.u32).c_str(), ctx.r4.s32, ctx.r5.s32, msg.c_str());
    LogError("  guest backtrace: %s", GuestBacktrace(ctx).c_str());
    __imp__sub_82649010(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_826BECE0);
PPC_FUNC_IMPL(__imp__sub_822DECE0);

// Engine debug print: (const char* fmt, ...). Silent in retail builds; logged with BO3_GAMELOG=1.
PPC_FUNC(sub_826BECE0)
{
    static const bool on = getenv("BO3_GAMELOG") != nullptr;
    if (on)
    {
        std::string msg = GuestFormat(GuestString(ctx.r3.u32).c_str(), ctx, 1);
        while (!msg.empty() && msg.back() == '\n') msg.pop_back();
        Log("game: %s", msg.c_str());
    }
    __imp__sub_826BECE0(ctx, base);
}

// Script (GSC) runtime error: (inst, const char* message)
PPC_FUNC(sub_822DECE0)
{
    std::string msg = GuestString(ctx.r4.u32);
    while (!msg.empty() && msg.back() == '\n') msg.pop_back();
    LogWarn("script error: %s", msg.c_str());
    __imp__sub_822DECE0(ctx, base);
}

// --- Bink diagnostics (BO3_BINKLOG=1) ---
PPC_FUNC_IMPL(__imp__sub_82A95220);
PPC_FUNC_IMPL(__imp__sub_82A953E8);
PPC_FUNC_IMPL(__imp__sub_82A988F8);
static bool BinkLog() { static const bool on = getenv("BO3_BINKLOG") != nullptr; return on; }
PPC_FUNC(sub_82A95220)
{
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;
    __imp__sub_82A95220(ctx, base);
    if (BinkLog()) Log("bink 82A95220(0x%X \"%s\", 0x%X, 0x%X) -> 0x%X", a3, a3 > 0x10000 ? GuestString(a3).c_str() : "", a4, a5, ctx.r3.u32);
}
PPC_FUNC(sub_82A953E8)
{
    uint32_t a3 = ctx.r3.u32;
    __imp__sub_82A953E8(ctx, base);
    static int n = 0;
    if (BinkLog() && n++ < 6) Log("bink 82A953E8(0x%X) -> 0x%X", a3, ctx.r3.u32);
}
PPC_FUNC(sub_82A988F8)
{
    uint32_t a3 = ctx.r3.u32;
    __imp__sub_82A988F8(ctx, base);
    static int n = 0;
    if (BinkLog() && n++ < 6) Log("bink 82A988F8(0x%X) -> 0x%X", a3, ctx.r3.u32);
}

// Cinematic_Play(const char* name, ...). The boot logo movie plays (Bink decodes it, ~3 s); set
// BO3_SKIP_INTRO=1 to skip it.
PPC_FUNC_IMPL(__imp__sub_822C09A0);
PPC_FUNC(sub_822C09A0)
{
    std::string name = ctx.r3.u32 ? GuestString(ctx.r3.u32) : "";
    Log("cinematic: play \"%s\" (flags 0x%X)", name.c_str(), ctx.r4.u32);
    if (getenv("BO3_SKIP_INTRO") && name.find("Global_Logo") != std::string::npos)
    {
        Log("cinematic: skipping intro movie");
        return;
    }
    __imp__sub_822C09A0(ctx, base);
}

// Save-data task failure callback (GAME_SAVE_UNABLE_TO_ACCESS_DEVICE): log the task's vtable.
PPC_FUNC_IMPL(__imp__sub_826DF808);
PPC_FUNC(sub_826DF808)
{
    uint32_t vt = GuestRead32(ctx.r3.u32 + 4);
    LogError("savedata task 0x%X failed: vtable 0x%X poll 0x%X start 0x%X state %u", ctx.r3.u32, vt,
             GuestRead32(vt + 24), GuestRead32(vt + 0), GuestRead32(ctx.r3.u32 + 8));
    __imp__sub_826DF808(ctx, base);
}

// D3D: run/insert a command buffer object (r4). Diagnostics for a null fixup list.
PPC_FUNC_IMPL(__imp__sub_82701780);
PPC_FUNC(sub_82701780)
{
    uint32_t cb = ctx.r4.u32;
    if (cb && (GuestRead32(cb + 108) & 0x20) && GuestRead32(cb + 116) == 0)
    {
        static int n = 0;
        if (n++ < 4)
        {
            std::string dump;
            char b[16];
            for (uint32_t o = 0; o < 128; o += 4) { snprintf(b, sizeof(b), "%s%08X", o % 32 ? " " : "\n      ", GuestRead32(cb + o)); dump += b; }
            LogWarn("d3d cmdbuf 0x%X: flags 0x%X with null list; bt %s%s", cb, GuestRead32(cb + 108), GuestBacktrace(ctx).c_str(), dump.c_str());
        }
    }
    __imp__sub_82701780(ctx, base);
}

// Client view angles from the user command (pitch/yaw = cmd angles (r4 + 16/20, 16-bit units)
// + delta angles, pitch clamped), r3 = client state with viewangles at +0x28C. Found with a hardware write
// watchpoint (BO3_WATCH) on the view yaw; the MP copy matched by instruction pattern.
// Mouse look is added to the command angles first: direct angles instead of right-stick emulation.
PPC_FUNC_IMPL(__imp__sub_821B2E88);
PPC_FUNC(sub_821B2E88)
{
    uint32_t cl = ctx.r3.u32, cmd = ctx.r4.u32;
    HostInputApplyMouseLook(cl, cmd);   // r3 = client state, r4 = user command
    __imp__sub_821B2E88(ctx, base);
    HostInputMouseLookTrace(cl, cmd);
    static const bool test = getenv("BO3_MOUSE_TEST") != nullptr;
    static int n = 0;
    static int n2 = 0;
    if (test && (cl < 0x82000000 || cl >= 0x86000000) && n2++ % 10 == 0)
    {
        auto f = [](uint32_t a) { uint32_t v = GuestRead32(a); float x; memcpy(&x, &v, 4); return x; };
        Log("mouselook: other copy 0x%08X: view %.2f %.2f delta %.2f %.2f cmd %d %d", cl, f(cl + 0x28C), f(cl + 0x290), f(cl + 188), f(cl + 192),
            int32_t(GuestRead32(cmd + 16)), int32_t(GuestRead32(cmd + 20)));
    }
    if (test && cl >= 0x82000000 && cl < 0x86000000 && n++ % 10 == 0)
    {
        auto f = [](uint32_t a) { uint32_t v = GuestRead32(a); float x; memcpy(&x, &v, 4); return x; };
        Log("mouselook: after: view %.2f %.2f delta %.2f %.2f cmd %d %d", f(cl + 0x28C), f(cl + 0x290), f(cl + 188), f(cl + 192),
            int32_t(GuestRead32(cmd + 16)), int32_t(GuestRead32(cmd + 20)));
    }
}


// Dvar_GetInt. The main loop (sub_8267F438) reads its frame-rate dvar once (100: it waits
// 1000/100 ms after each frame's work, which caps the game near 60 fps). Returning a high
// value removes that wait; the frame rate is then set by the emulated vsync (FPS limit option).
// The game's simulation uses real elapsed time, so its speed does not change.
PPC_FUNC_IMPL(__imp__sub_826B8618);
PPC_FUNC(sub_826B8618)
{
    uint32_t dvar = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_826B8618(ctx, base);
    // client frame limiter (sub_8264DFD0: msec = 1000 / maxfps, maxfps = 60 -> 16 ms = 62.5 fps)
    if (lr == 0x8264E024)
    {
        static const bool keep = getenv("BO3_KEEP_MAXFPS") != nullptr;
        static int n = 0;
        if (n++ < 3) Log("game: client max fps dvar %d%s", ctx.r3.s32, keep ? "" : " -> 1000 (paced by the vsync / FPS limit)");
        if (!keep) ctx.r3.s64 = 1000;
    }
    (void)dvar;
    if (lr == 0x8267F4D8)
    {
        static const bool keep = getenv("BO3_KEEP_MAXFPS") != nullptr;
        static int n = 0;
        if (n++ < 2) Log("game: main loop frame-rate dvar %d%s", ctx.r3.s32, keep ? "" : " -> 1000 (paced by the vsync / FPS limit)");
        if (!keep) ctx.r3.s64 = 1000;
    }
}

// Client look from the gamepad (sub_822E3E38, CL_GamepadMove-like): per-frame view deltas are
// stored at [0x84475F74] + 12704 / 12708. diagnostics: BO3_LOOK_LOG=1
PPC_FUNC_IMPL(__imp__sub_822E3E38);
PPC_FUNC(sub_822E3E38)
{
    __imp__sub_822E3E38(ctx, base);
    HostInputApplyLookRates(GuestRead32(0x84475F74));
    static const bool on = getenv("BO3_LOOK_LOG") != nullptr;
    if (on)
    {
        uint32_t cl = GuestRead32(0x84475F74);
        auto f = [](uint32_t a) { uint32_t v = GuestRead32(a); float x; memcpy(&x, &v, 4); return x; };
        static int n = 0;
        if (cl && (f(cl + 12704) != 0 || f(cl + 12708) != 0) && n++ % 6 == 0 && n < 600)
            Log("look: cl 0x%08X d %.4f %.4f", cl, f(cl + 12704), f(cl + 12708));
    }
}

// The game's CRT memcpy (r3 = dst, r4 = src, r5 = bytes; returns dst) and memset (r3 = dst,
// r4 = byte, r5 = bytes): copying bytes needs no byte swapping, so the host's versions do it
// (the recompiled loops swap every word on load and store). memmove: overlap stays safe.
bool AbOld(const char* name);   // renderer: BO3_AB_TOGGLE measurements
PPC_FUNC_IMPL(__imp__sub_82A16B40);
PPC_FUNC(sub_82A16B40)
{
    if (AbOld("memcpy")) { __imp__sub_82A16B40(ctx, base); return; }
    memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}

PPC_FUNC(sub_82A17140)
{
    memset(base + ctx.r3.u32, ctx.r4.u8, ctx.r5.u32);
}

// ---------------------------------------------------------------------------------------
// Online mode (BO3_LIVE): Xbox Live title storage, served from online/tms/<name> in the data
// folder (empty when missing). The game reads a few files from it before Demonware
// (content_ids_1_ww.csv: DLC ids for the store) in two steps:
//   sub_82786AC8 = XStorageEnumerate(user, wide path, start, max, results size, results*, XOVERLAPPED*)
//   sub_827867E8 = XStorageDownloadToMemory(user, wide path, size, buffer, results size, results*, XOVERLAPPED*)
// Results: XSTORAGE_ENUMERATE_RESULTS {total, returned, items*} with packed XSTORAGE_FILE_INFO
// (storage size +29, installed size +33, path length +55, path* +57; 65 bytes) and
// XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS {bytes total, owner xuid, created}.
bool LiveSignedIn();
namespace
{
std::string WidePath(uint32_t a)
{
    std::string p;
    for (; a && GuestRead16(a) && p.size() < 512; a += 2) p += char(GuestRead16(a));
    return p;
}
std::string TmsFile(const std::string& path)
{
    std::string name = path.substr(path.rfind('/') + 1), data;
    if (FILE* f = fopen(("online/tms/" + name).c_str(), "rb"))
    {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
    }
    return data;
}
void CompleteOv(uint32_t ov, uint32_t result, uint32_t bytes)
{
    if (!ov) return;
    GuestWrite32(ov + 0, result);
    GuestWrite32(ov + 4, bytes);
    GuestWrite32(ov + 24, 0);
}
}

PPC_FUNC_IMPL(__imp__sub_82786AC8);
PPC_FUNC(sub_82786AC8)
{
    if (!LiveSignedIn()) { __imp__sub_82786AC8(ctx, base); return; }
    std::string path = WidePath(ctx.r4.u32), data = TmsFile(path);
    uint32_t cap = ctx.r7.u32, res = ctx.r8.u32, ov = ctx.r9.u32;
    uint32_t item = res + 12, str = item + 65, need = 12 + 65 + uint32_t(path.size() + 1) * 2;
    if (!res || cap < need) { Log("xlive: XStorageEnumerate %s: buffer %u < %u", path.c_str(), cap, need); CompleteOv(ov, 122, 0); ctx.r3.u64 = 122; return; }
    memset(base + res, 0, need);
    GuestWrite32(res + 0, 1);              // total items
    GuestWrite32(res + 4, 1);              // returned
    GuestWrite32(res + 8, item);
    GuestWrite32(item + 0, 0x4156091D);    // title id
    GuestWrite32(item + 29, uint32_t(data.size()));
    GuestWrite32(item + 33, uint32_t(data.size()));
    GuestWrite16(item + 55, uint16_t(path.size() + 1));
    GuestWrite32(item + 57, str);
    for (size_t i = 0; i < path.size(); i++) GuestWrite16(str + uint32_t(i * 2), uint16_t(uint8_t(path[i])));
    CompleteOv(ov, 0, 1);
    Log("xlive: XStorageEnumerate %s -> 1 file, %zu bytes", path.c_str(), data.size());
    ctx.r3.u64 = 0;
}

PPC_FUNC_IMPL(__imp__sub_827867E8);
PPC_FUNC(sub_827867E8)
{
    if (!LiveSignedIn()) { __imp__sub_827867E8(ctx, base); return; }
    std::string path = WidePath(ctx.r4.u32), data = TmsFile(path);
    uint32_t cap = ctx.r5.u32, buffer = ctx.r6.u32, resSize = ctx.r7.u32, res = ctx.r8.u32, ov = ctx.r9.u32;
    size_t n = std::min<size_t>(data.size(), cap);
    if (buffer && n) memcpy(base + buffer, data.data(), n);
    if (res && resSize >= 4) { memset(base + res, 0, std::min<uint32_t>(resSize, 20)); GuestWrite32(res, uint32_t(n)); }
    CompleteOv(ov, 0, uint32_t(n));
    Log("xlive: XStorageDownloadToMemory %s -> %zu bytes", path.c_str(), n);
    ctx.r3.u64 = 0;
}

// Demonware online flow (live_hooks.h)
void LiveStatsReady(PPCContext&) {}
LIVE_DW_HOOKS(8253EEE8, 8256B550, 8256BAF8, 0x844761E1, 827A2608, 8253F900)
LUA_PRINT_HOOK(825E4030)


