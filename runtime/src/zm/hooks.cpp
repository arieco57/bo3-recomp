// Hooks into recompiled game functions (Zombies executable, TU1 addresses).
// Generated guest functions are weak aliases of __imp__sub_XXXXXXXX, so defining sub_XXXXXXXX
// here replaces them while still allowing the original to be called.
#include "guest.h"
#include "live_hooks.h"
#include "host_input.h"
#include "video_options.h"
#include "memory.h"
#include <string>
#include <vector>

PPC_FUNC_IMPL(__imp__sub_8263FF40);

// Engine assert / fatal error: (const char* file, int line, int level, const char* fmt, ...)
// e.g. ("src\\xenon\\xenon_net.cpp", 964, 1, "Unable to find an XNADDR.\n"). It longjmps
// to the error handler, so log before calling through.
PPC_FUNC(sub_8263FF40)
{
    std::string msg = GuestFormat(GuestString(ctx.r6.u32).c_str(), ctx, 4);
    while (!msg.empty() && msg.back() == '\n') msg.pop_back();
    LogError("GAME ERROR %s:%d (level %d): %s", GuestString(ctx.r3.u32).c_str(), ctx.r4.s32, ctx.r5.s32, msg.c_str());
    __imp__sub_8263FF40(ctx, base);
}

// Client view angles from the user command (pitch/yaw = cmd angles (r4 + 16/20, 16-bit units)
// + delta angles, pitch clamped), r3 = client state with viewangles at +0x28C. Found with a hardware write
// watchpoint (BO3_WATCH) on the view yaw; the Zombies copy matched by instruction pattern.
// Mouse look is added to the command angles first: direct angles instead of right-stick emulation.
PPC_FUNC_IMPL(__imp__sub_821B2F40);
PPC_FUNC(sub_821B2F40)
{
    uint32_t cl = ctx.r3.u32, cmd = ctx.r4.u32;
    HostInputApplyMouseLook(cl, cmd);   // r3 = client state, r4 = user command
    __imp__sub_821B2F40(ctx, base);
    HostInputMouseLookTrace(cl, cmd);
}

// Frame-rate caps of the game (see mp/hooks.cpp): the client frame limiter (sub_82644DC8:
// msec = 1000 / 60) and the main loop (sub_82675xxx: 1000 / 100) read their dvars through
// Dvar_GetInt once. Returning 1000 removes both waits; the frame rate is then set by the
// emulated vsync (FPS limit option). The simulation runs on real time: no speed change.
PPC_FUNC_IMPL(__imp__sub_826AC940);
PPC_FUNC(sub_826AC940)
{
    uint32_t lr = uint32_t(ctx.lr), fovDvar = ctx.r3.u32;
    __imp__sub_826AC940(ctx, base);
    if (lr == 0x82644E1C || lr == 0x8267541C)
    {
        static const bool keep = getenv("BO3_KEEP_MAXFPS") != nullptr;
        static int n = 0;
        if (n++ < 4) Log("game: %s frame-rate dvar %d%s", lr == 0x82644E1C ? "client" : "main loop", ctx.r3.s32,
            keep ? "" : " -> 1000 (paced by the vsync / FPS limit)");
        if (!keep) ctx.r3.s64 = 1000;
    }
}

// Client look from the gamepad (MP sub_822E3E38, matched by instruction pattern): per-frame view
// turn rates at [0x84439358] + 12704 / 12708 - mouse look is added there (host_input.cpp).
PPC_FUNC_IMPL(__imp__sub_822DFD78);
PPC_FUNC(sub_822DFD78)
{
    __imp__sub_822DFD78(ctx, base);
    HostInputApplyLookRates(GuestRead32(0x84439358));
}

// The game's CRT memcpy (r3 = dst, r4 = src, r5 = bytes; returns dst) and memset (r3 = dst,
// r4 = byte, r5 = bytes): copying bytes needs no byte swapping, so the host's versions do it
// (the recompiled loops swap every word on load and store). memmove: overlap stays safe.
bool AbOld(const char* name);   // renderer: BO3_AB_TOGGLE measurements
PPC_FUNC_IMPL(__imp__sub_82A08D60);
PPC_FUNC(sub_82A08D60)
{
    if (AbOld("memcpy")) { __imp__sub_82A08D60(ctx, base); return; }
    memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}

PPC_FUNC(sub_82A09360)
{
    memset(base + ctx.r3.u32, ctx.r4.u8, ctx.r5.u32);
}

// Demonware online flow (live_hooks.h)
LIVE_DW_HOOKS(8253E030, 8256A100, 8256A6A8, 0x844395A5, 82794680, 8253EA48)
LUA_PRINT_HOOK(825E24D0)


// Online mode: all GobbleGums and Liquid Divinium. Mega GobbleGum counts are the online stats
// ItemStats[item].stats.bgbConsumablesGained - bgbConsumablesUsed and Liquid Divinium is
// PlayerStatsList.BGB_TOKENS_GAINED - BGB_TOKENS_USED (menus and the in-game GSC both read them).
// Once the online stats are fetched, the game's own DDL functions set them: 999 of every gum and
// 999 Liquid Divinium (the menu counter has 3 digits), refilled every launch (the game uploads the stats with its next write).
//   sub_82563AC8(controller, location, 1) -> stats buffer      sub_8255C2B0(location) -> DDL root
//   sub_82327960(root, state*, count, const char** path) -> ok sub_82327B00(state*, buffer, value)
namespace
{
bool ZmStatSet(PPCContext& c, uint32_t buf, uint32_t root, uint32_t scratch, const std::vector<std::string>& path, uint32_t value)
{
    uint32_t state = scratch, arr = scratch + 0x40, strs = scratch + 0x80;
    memset(g_base + state, 0, 0x20);
    GuestWrite32(state + 8, 0xFFFFFFFFu);
    uint32_t at = strs;
    for (size_t i = 0; i < path.size(); i++)
    {
        memcpy(g_base + at, path[i].c_str(), path[i].size() + 1);
        GuestWrite32(arr + uint32_t(i) * 4, at);
        at += uint32_t(path[i].size() + 1 + 3) & ~3u;
    }
    c.r3.u64 = root; c.r4.u64 = state; c.r5.u64 = path.size(); c.r6.u64 = arr;
    GuestCall(c, 0x82327960);
    if (!(c.r3.u32 & 0xFF)) return false;
    c.r3.u64 = state; c.r4.u64 = buf; c.r5.u64 = value;
    GuestCall(c, 0x82327B00);
    return (c.r3.u32 & 0xFF) != 0;
}
}

void LiveStatsReady(PPCContext& ctx)
{
    static uint32_t scratch = GuestAlloc(0x400);
    PPCContext c = ctx;
    c.r1.u64 = (ctx.r1.u32 - 0x2000) & ~0xFu;   // room below the caller's frame
    c.r3.u64 = 0; c.r4.u64 = 0; c.r5.u64 = 1;
    GuestCall(c, 0x82563AC8);
    uint32_t buf = c.r3.u32;
    c.r3.u64 = 0;
    GuestCall(c, 0x8255C2B0);
    uint32_t root = c.r3.u32;
    if (!buf || !root) { LogWarn("live: no online stats buffer, GobbleGums not set"); return; }
    bool tg = ZmStatSet(c, buf, root, scratch, { "PlayerStatsList", "BGB_TOKENS_GAINED", "statValue" }, 999);
    bool tu = ZmStatSet(c, buf, root, scratch, { "PlayerStatsList", "BGB_TOKENS_USED", "statValue" }, 0);
    int items = 0;
    for (int i = 0; i < 1024; i++)
    {
        std::string idx = std::to_string(i);
        if (!ZmStatSet(c, buf, root, scratch, { "ItemStats", idx, "stats", "bgbConsumablesGained", "statValue" }, 999)) break;
        ZmStatSet(c, buf, root, scratch, { "ItemStats", idx, "stats", "bgbConsumablesUsed", "statValue" }, 0);
        items++;
    }
    Log("live: GobbleGums refilled (%d items), Liquid Divinium %s", items, tg && tu ? "999" : "not set");
}

// Classic GobbleGums unlock with the player's rank; in online mode every GobbleGum is unlocked.
// sub_821C9AC8(mode, stats buffer, item) -> locked; sub_821C7658(mode, item, 3) -> the item's name
// (GobbleGums: "zmui_bgb_*").
PPC_FUNC_IMPL(__imp__sub_821C9AC8);
PPC_FUNC(sub_821C9AC8)
{
    uint32_t mode = ctx.r3.u32, item = ctx.r5.u32;
    PPCContext saved = ctx;
    __imp__sub_821C9AC8(ctx, base);
    if (!(ctx.r3.u32 & 0xFF) || mode >= 4 || item >= 256 || !LiveSignedIn()) return;
    static int8_t gum[4][256];
    static bool init = false;
    if (!init) { memset(gum, -1, sizeof(gum)); init = true; }
    if (gum[mode][item] < 0)
    {
        PPCContext c = saved;
        c.r1.u64 = (saved.r1.u32 - 0x1000) & ~0xFu;
        c.r3.u64 = mode; c.r4.u64 = item; c.r5.u64 = 3;
        GuestCall(c, 0x821C7658);
        std::string name = c.r3.u32 ? GuestString(c.r3.u32) : std::string();
        gum[mode][item] = name.find("_bgb_") != std::string::npos ? 1 : 0;
    }
    if (gum[mode][item] == 1) ctx.r3.u64 = 0;
}
