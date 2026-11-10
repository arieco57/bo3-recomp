#pragma once
// Online mode (BO3_LIVE) hooks shared by the MP and ZM executables, instantiated with each one's
// addresses by LIVE_DW_HOOKS(...) in mp/hooks.cpp and zm/hooks.cpp.
//
//  fetchDone   IsDemonwareFetchingDone(controller, u32* flags, bool): every online fetch is done
//              (DW sign-in, playlists, FFOTD, online stats and loadouts...). Logs the flags.
//  pubVerify   publisher file signature check (type 0 playlists / 1 motd / 2 featured cards, data,
//              size) -> the text after the signed header line, or 0. Files from our Demonware server
//              (dw_server.cpp) are unsigned and start with "BO3RECOMP\n": accepted as they are.
//  ffotdFail   FFOTD (fastfile of the day) download failed: our server has none, so also set the
//              FFOTD-fetched byte (ffotdFlag) that the online menus wait for.
//  eventFilter bdEventLog category filter (service, category) -> drop the event? Offline from the
//              real servers every event is dropped, and the game then hands back a locally completed
//              task whose last reference it releases while still using it (freed memory -> crash,
//              e.g. when spending an unlock token). Nothing is dropped: the events go to our server,
//              which answers ok.
//  hostBandwidth  "enough upload bandwidth to host this lobby?" (lobby size, u32* reason). Without
//              Demonware's bandwidth test server the measured value says no (reason 7,
//              HOST_SERVER_HAS_NOT_NECESSARY_BANDWIDTH) and public matchmaking searches forever
//              instead of hosting when it finds no game: hosting is allowed.
#include "guest.h"
#include <cstring>
#include <cstdlib>
#include <string>

bool LiveSignedIn();   // xam.cpp
// called once the online stats are fetched (first "fetching done"); each game defines it (hooks.cpp)
void LiveStatsReady(PPCContext& ctx);

#define LIVE_DW_HOOKS(fetchDone, pubVerify, ffotdFail, ffotdFlag, eventFilter, hostBandwidth)                                          \
    PPC_FUNC_IMPL(__imp__sub_##fetchDone);                                                                  \
    PPC_FUNC(sub_##fetchDone)                                                                               \
    {                                                                                                       \
        uint32_t out = ctx.r4.u32;                                                                          \
        __imp__sub_##fetchDone(ctx, base);                                                                  \
        static uint32_t last = 0xFFFFFFFF;                                                                  \
        uint32_t f = GuestRead32(out);                                                                      \
        if (f != last) { Log("live: online fetch flags 0x%08X%s", f, ctx.r3.u32 ? " - done" : ""); last = f; } \
        static bool ready = false;                                                                          \
        if (ctx.r3.u32 && !ready) { ready = true; LiveStatsReady(ctx); }                                    \
    }                                                                                                       \
    PPC_FUNC_IMPL(__imp__sub_##pubVerify);                                                                  \
    PPC_FUNC(sub_##pubVerify)                                                                               \
    {                                                                                                       \
        uint32_t type = ctx.r3.u32, data = ctx.r4.u32, size = ctx.r5.u32;                                   \
        static const char tag[] = "BO3RECOMP\n";                                                            \
        if (data && size >= sizeof(tag) - 1 && memcmp(base + data, tag, sizeof(tag) - 1) == 0)              \
        {                                                                                                   \
            Log("live: publisher file type %u accepted (%u bytes)", type, size);                            \
            ctx.r3.u64 = data + sizeof(tag) - 1;                                                            \
            return;                                                                                         \
        }                                                                                                   \
        __imp__sub_##pubVerify(ctx, base);                                                                  \
        Log("live: publisher file type %u signature check -> 0x%X", type, ctx.r3.u32);                      \
    }                                                                                                       \
    PPC_FUNC_IMPL(__imp__sub_##ffotdFail);                                                                  \
    PPC_FUNC(sub_##ffotdFail)                                                                               \
    {                                                                                                       \
        __imp__sub_##ffotdFail(ctx, base);                                                                  \
        GuestWrite8(ffotdFlag, 1);                                                                          \
        Log("live: no FFOTD on the server, continuing without it");                                        \
    }                                                                                                       \
    PPC_FUNC_IMPL(__imp__sub_##eventFilter);                                                                \
    PPC_FUNC(sub_##eventFilter)                                                                             \
    {                                                                                                       \
        if (!LiveSignedIn()) { __imp__sub_##eventFilter(ctx, base); return; }                              \
        ctx.r3.u64 = 0;                                                                                     \
    }                                                                                                       \
    PPC_FUNC_IMPL(__imp__sub_##hostBandwidth);                                                              \
    PPC_FUNC(sub_##hostBandwidth)                                                                           \
    {                                                                                                       \
        uint32_t reason = ctx.r4.u32;                                                                       \
        __imp__sub_##hostBandwidth(ctx, base);                                                              \
        if (LiveSignedIn() && !ctx.r3.u32 && reason && GuestRead32(reason) == 7)                            \
        {                                                                                                   \
            GuestWrite32(reason, 0);                                                                        \
            ctx.r3.u64 = 1;                                                                                 \
        }                                                                                                   \
    }

// Engine.PrintInfo / PrintWarning / PrintError (one stubbed native in release builds): with
// BO3_LUA_LOG=1 the UI / lobby scripts' messages go to the log ("lua: <label> <text>") - the lobby
// state machine reports every matchmaking step there (search results, QoS, join, host checks).
// Havok Script stack: L+40 base, L+36 top, 8-byte values {value, type}; a string's text is at +12.
#define LUA_PRINT_HOOK(printFn)                                                                             \
    PPC_FUNC_IMPL(__imp__sub_##printFn);                                                                    \
    PPC_FUNC(sub_##printFn)                                                                                 \
    {                                                                                                       \
        static const bool on = getenv("BO3_LUA_LOG") && *getenv("BO3_LUA_LOG") == '1';                     \
        if (on)                                                                                             \
        {                                                                                                   \
            uint32_t L = ctx.r3.u32, top = GuestRead32(L + 36), bas = GuestRead32(L + 40);                 \
            std::string out;                                                                                \
            for (uint32_t p = bas; p < top && p < bas + 64; p += 8)                                         \
            {                                                                                               \
                uint32_t v = GuestRead32(p), t = GuestRead32(p + 4) & 0xF;                                  \
                if (t == 4)                                                                                 \
                    for (uint32_t i = 0; i < 400; i++)                                                      \
                    {                                                                                       \
                        char c = char(GuestRead8(v + 12 + i));                                              \
                        if (!c) break;                                                                      \
                        out += (c == '\n' || c == '\r') ? ' ' : c;                                          \
                    }                                                                                       \
                else if (t == 3) { float f; memcpy(&f, &v, 4); out += std::to_string(int(f)) + " "; }       \
            }                                                                                               \
            Log("lua: %s", out.c_str());                                                                    \
        }                                                                                                   \
        __imp__sub_##printFn(ctx, base);                                                                    \
    }
