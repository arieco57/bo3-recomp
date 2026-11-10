// XAM: video/AV info, loader launch data, users/sign-in, system UI, notifications.
// Policy: one local profile signed in on controller 0, no Xbox Live.
#include "guest.h"
#include "gpu/renderer.h"
#include "kobject.h"
#include "stubs.h"
#include "thread.h"

#include <cmath>
#include <mutex>
#include <unistd.h>
#include <cerrno>
#include <random>
#include <string>
#include <set>

// bo3recomp.ini value (first "key=" line), or "" - read directly: the profile is needed before
// the video options load.
// Release builds carry the online backend's defaults (CMake: BO3_DEFAULT_SUPABASE_URL / _KEY, set
// by tools/make_release.sh), used when bo3recomp.ini does not set them: a fresh install - the phone
// starts without an ini - is online out of the box. Source builds have none (no backend).
#ifndef BO3_DEFAULT_SUPABASE_URL
#define BO3_DEFAULT_SUPABASE_URL ""
#endif
#ifndef BO3_DEFAULT_SUPABASE_KEY
#define BO3_DEFAULT_SUPABASE_KEY ""
#endif
static std::string ConfigDefault(const std::string& key)
{
    if (key == "supabase_url") return BO3_DEFAULT_SUPABASE_URL;
    if (key == "supabase_key") return BO3_DEFAULT_SUPABASE_KEY;
    if (key == "live") return *BO3_DEFAULT_SUPABASE_URL ? "1" : "";
    return {};
}

std::string ConfigValue(const char* key)
{
    std::string want = std::string(key) + "=", out;
    bool found = false;
    if (FILE* f = fopen("bo3recomp.ini", "r"))
    {
        char line[512];
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, want.c_str(), want.size()))
            {
                out = line + want.size();
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
                found = true;
                break;
            }
        fclose(f);
    }
    return found ? out : ConfigDefault(key);
}

// Local profile: XUID and gamertag. The saves stay in content/E0300000126D662A/ whatever the XUID
// (content.cpp). System link drops players whose XUID is already in the session, so every
// machine gets its own: [network] xuid= in bo3recomp.ini, generated once (BO3_XUID / BO3_GAMERTAG
// override for tests).
uint64_t ProfileXuid()
{
    static const uint64_t x = []() -> uint64_t {
        if (const char* e = getenv("BO3_XUID")) return strtoull(e, nullptr, 16);
        std::string v = ConfigValue("xuid");
        if (!v.empty()) return strtoull(v.c_str(), nullptr, 16);
        std::random_device rd;
        uint64_t id = 0xE000000000000000ull | (uint64_t(rd()) << 24) | (rd() & 0xFFFFFFu);   // offline XUID range
        if (FILE* f = fopen("bo3recomp.ini", "a"))
        {
            std::string tag = ConfigValue("gamertag");
            fprintf(f, "[network]\ngamertag=%s\nxuid=%016llX\n", tag.empty() ? "Player" : tag.c_str(), (unsigned long long)id);
            fclose(f);
        }
        Log("profile: new system link id %016llX (bo3recomp.ini)", (unsigned long long)id);
        return id;
    }();
    return x;
}
const char* ProfileGamertag()
{
    static std::string g = [] {
        std::string AccountUsername();   // account.cpp: the online username is the in-game name
        std::string n = getenv("BO3_GAMERTAG") ? getenv("BO3_GAMERTAG") : AccountUsername();
        if (n.empty()) n = ConfigValue("gamertag");
        if (n.empty()) n = "Player";
        if (n.size() > 15) n.resize(15);
        return n;
    }();
    return g.c_str();
}
#define kLocalXuid ProfileXuid()
#define kGamertag ProfileGamertag()

// ---------------------------------------------------------------------------------------
// video / AV

GUEST_IMPORT(XGetAVPack) { RETURN(6); }   // HDMI
GUEST_IMPORT(XGetGameRegion) { RETURN(0xFFFF); }
GUEST_IMPORT(XGetVideoCapabilities) { RETURN(0); }

// X_VIDEO_MODE: width, height, interlaced, widescreen, hi_def, refresh (float), standard, ...
GUEST_IMPORT(XGetVideoMode)
{
    uint32_t m = ARG(3);
    memset(g_base + m, 0, 0x30);
    GuestWrite32(m + 0x00, 1280);
    GuestWrite32(m + 0x04, 720);
    GuestWrite32(m + 0x08, 0);
    GuestWrite32(m + 0x0C, 1);
    GuestWrite32(m + 0x10, 1);
    float hz = float(GpuFrameLimit());   // refresh rate the game sees (= frame rate limit)
    Log("XGetVideoMode: refresh %.0f Hz", hz);
    uint32_t hzBits;
    memcpy(&hzBits, &hz, 4);
    GuestWrite32(m + 0x14, hzBits);
    GuestWrite32(m + 0x18, 1);      // NTSC-M
    GuestWrite32(m + 0x1C, 0x4A);
    GuestWrite32(m + 0x20, 0x01);
    RETURN(0);
}

GUEST_IMPORT(XGetDisplaySize)  // (width*, height*)
{
    if (ARG(3)) GuestWrite32(ARG(3), 1280);
    if (ARG(4)) GuestWrite32(ARG(4), 720);
    RETURN(0);
}

// ---------------------------------------------------------------------------------------
// loader

// Launch data survives a title relaunch (launchdata.bin in the data folder, read once).
GUEST_IMPORT(XamLoaderGetLaunchData)   // (buffer, size)
{
    static std::string data = [] {
        std::string d;
        if (FILE* f = fopen("launchdata.bin", "rb"))
        {
            char b[4096];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) d.append(b, n);
            fclose(f);
            remove("launchdata.bin");
            Log("xam: launch data from the previous run (%zu bytes)", d.size());
        }
        return d;
    }();
    if (data.empty()) RETURN(ERROR_NOT_FOUND);
    if (ARG(3)) memcpy(g_base + ARG(3), data.data(), std::min<size_t>(data.size(), ARG(4)));
    RETURN(ERROR_SUCCESS);
}
GUEST_IMPORT(XamLoaderSetLaunchData)   // (data, size)
{
    if (FILE* f = fopen("launchdata.bin", "wb"))
    {
        if (ARG(3) && ARG(4)) fwrite(g_base + ARG(3), 1, ARG(4), f);
        fclose(f);
    }
    Log("xam: launch data set (%u bytes)", ARG(4));
    RETURN(0);
}
// XamLoaderLaunchTitle(name, flags): the game restarts itself (e.g. after signing in online) or
// switches executable (default.xex = MP, default_zm.xex = Zombies). The desktop build re-executes
// the matching binary next to this one with the same arguments; at most 3 relaunches in a row.
extern char** g_argv;
GUEST_IMPORT(XamLoaderLaunchTitle)
{
    std::string name = GuestString(ARG(3));
    Log("XamLoaderLaunchTitle(\"%s\", 0x%X) from %s", name.c_str(), ARG(4), GuestBacktrace(ctx, 16).c_str());
#ifndef __ANDROID__
    int count = getenv("BO3_RELAUNCHES") ? atoi(getenv("BO3_RELAUNCHES")) : 0;
    char self[4096] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0 && count < 3 && g_argv)
    {
        std::string exe(self, size_t(n));
        std::string dir = exe.substr(0, exe.rfind('/') + 1);
        std::string lower = name;
        for (auto& c : lower) c = char(tolower((unsigned char)c));
        std::string target = lower.find("_zm") != std::string::npos ? dir + "bo3_zm" : lower.find("default") != std::string::npos ? dir + "bo3_mp" : exe;
        setenv("BO3_RELAUNCHES", std::to_string(count + 1).c_str(), 1);
        fflush(stdout);
        execv(target.c_str(), g_argv);
        LogWarn("relaunch of %s failed (%s)", target.c_str(), strerror(errno));
    }
#endif
    Log("relaunch requested - exiting");
    QuickExit(0);
}
GUEST_IMPORT(XamLoaderTerminateTitle) { Log("XamLoaderTerminateTitle"); QuickExit(0); }
GUEST_IMPORT(XamGetExecutionId)   // (info**)
{
    static uint32_t info = 0;
    if (!info)
    {
        info = GuestAlloc(0x18);
        GuestWrite32(info + 0x0C, 0x4156091D);   // title id
    }
    GuestWrite32(ARG(3), info);
    RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(XamGetSystemVersion) { RETURN(0x20096B00); }

// ---------------------------------------------------------------------------------------
// users

static bool ValidUser(uint32_t u) { return u == 0; }

// Online mode: the profile reports itself signed in to Xbox Live (state 2), so "PLAY ONLINE" works
// against our Demonware server (dw_server.cpp). On with live=1 in the [online] section of
// bo3recomp.ini; BO3_LIVE=1/0 overrides it.
bool LiveSignedIn()
{
    static const bool on = []
    {
        if (const char* e = getenv("BO3_LIVE")) return strcmp(e, "0") != 0;
        return ConfigValue("live") == "1";
    }();
    return on;
}
static uint32_t SigninState() { return LiveSignedIn() ? 2u : 1u; }

GUEST_IMPORT(XamUserGetSigninState) { RETURN(ValidUser(ARG(3)) ? SigninState() : 0); }   // 1 = locally, 2 = Live

// XamUserGetXUID(user, type_mask, xuid*)
GUEST_IMPORT(XamUserGetXUID)
{
    if (!ValidUser(ARG(3))) RETURN(ERROR_NO_SUCH_USER);
    GuestWrite64(ARG(5), kLocalXuid);
    RETURN(ERROR_SUCCESS);
}

// XamUserGetName(user, buffer, size)
GUEST_IMPORT(XamUserGetName)
{
    if (!ValidUser(ARG(3))) RETURN(ERROR_NO_SUCH_USER);
    uint32_t n = std::min<uint32_t>(ARG(5), uint32_t(strlen(kGamertag) + 1));
    memcpy(g_base + ARG(4), kGamertag, n);
    if (n) GuestWrite8(ARG(4) + n - 1, 0);
    RETURN(ERROR_SUCCESS);
}

// XamUserGetSigninInfo(user, flags, X_USER_SIGNIN_INFO*)
//   +0x00 xuid, +0x08 unk, +0x0C signin_state, +0x10 unk, +0x14 unk, +0x18 name[16]
GUEST_IMPORT(XamUserGetSigninInfo)
{
    uint32_t info = ARG(5);
    memset(g_base + info, 0, 0x28);
    if (!ValidUser(ARG(3))) RETURN(ERROR_NO_SUCH_USER);
    GuestWrite64(info + 0x00, kLocalXuid);
    if (LiveSignedIn()) GuestWrite32(info + 0x08, 1);   // XUSER_INFO_FLAG_LIVE_ENABLED
    GuestWrite32(info + 0x0C, SigninState());
    memcpy(g_base + info + 0x18, kGamertag, strlen(kGamertag));
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserCheckPrivilege)   // (user, privilege, result*) - verified at a ZM call site
{
    if (ARG(5)) GuestWrite32(ARG(5), 1);
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserAreUsersFriends)  // (user, xuids*, count, result*, overlapped)
{
    if (ARG(6)) GuestWrite32(ARG(6), 0);
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserGetMembershipTierFromXUID) { RETURN(ERROR_NO_SUCH_USER); }
GUEST_IMPORT(XamUserGetOnlineCountryFromXUID) { RETURN(ERROR_NO_SUCH_USER); }

GUEST_IMPORT(XamGetLanguage) { RETURN(1); }           // English
GUEST_IMPORT(XamGetLocaleEx) { RETURN(0); }

// ---------------------------------------------------------------------------------------
// notifications: XamNotifyCreateListener returns a handle polled with XNotifyGetNext.
// We queue the usual boot notifications (sign-in changed, system UI closed).

namespace
{
struct KNotifyListener : KObject
{
    KNotifyListener() : KObject(KType::Other) {}
    uint64_t mask = 0;
    std::vector<std::pair<uint32_t, uint32_t>> queue;  // (id, param)
};
std::mutex s_notifyLock;
}

GUEST_IMPORT(XamNotifyCreateListener)  // (mask (u64), max_version)
{
    auto l = std::make_shared<KNotifyListener>();
    l->mask = ARG64(3);
    // XN_SYS_SIGNINCHANGED (0x0000000A), XN_SYS_UI (0x00000009) closed, XN_SYS_INPUTDEVICESCHANGED (0x12)
    l->queue.push_back({ 0x0000000A, 1 });
    l->queue.push_back({ 0x00000009, 0 });
    l->queue.push_back({ 0x00000012, 0 });
    RETURN(HandleCreate(l));
}

// XNotifyGetNext(handle, match_id, id*, param*) -> BOOL
GUEST_IMPORT(XNotifyGetNext)
{
    auto l = std::dynamic_pointer_cast<KNotifyListener>(HandleGet(ARG(3)));
    if (!l) RETURN(0);
    std::lock_guard lock(s_notifyLock);
    for (size_t i = 0; i < l->queue.size(); i++)
    {
        if (ARG(4) && l->queue[i].first != ARG(4)) continue;
        if (ARG(5)) GuestWrite32(ARG(5), l->queue[i].first);
        if (ARG(6)) GuestWrite32(ARG(6), l->queue[i].second);
        l->queue.erase(l->queue.begin() + i);
        RETURN(1);
    }
    RETURN(0);
}

GUEST_IMPORT(XNotifyPositionUI) { RETURN(0); }

// ---------------------------------------------------------------------------------------
// memory (XamAlloc(flags, size, out*) / XamFree(ptr))

GUEST_IMPORT(XamAlloc)
{
    uint32_t p = GuestAlloc(ARG(4) ? ARG(4) : 1);
    GuestWrite32(ARG(5), p);
    RETURN(p ? ERROR_SUCCESS : 0x8007000E);
}
GUEST_IMPORT(XamFree) { GuestFree(ARG(3)); RETURN(ERROR_SUCCESS); }

// ---------------------------------------------------------------------------------------
// system UI: report "cancelled" without showing anything (until a real UI exists).

static uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result)
{
    if (!overlapped) return result;
    // XOVERLAPPED: +0 InternalLow(result), +4 InternalHigh(length), +8 InternalContext, +0xC hEvent
    GuestWrite32(overlapped + 0, result);
    GuestWrite32(overlapped + 4, 0);
    uint32_t ev = GuestRead32(overlapped + 0xC);
    if (ev)
        if (auto e = std::dynamic_pointer_cast<KEvent>(HandleGet(ev)))
        {
            std::lock_guard lock(g_dispatchLock);
            e->signaled = true;
            e->SyncGuest();
        }
    DispatchNotify();
    return ERROR_IO_PENDING;
}

GUEST_IMPORT(XamShowSigninUI) { Log("xam: XamShowSigninUI(panes %u, flags 0x%X) lr=0x%X", ARG(3), ARG(4), (uint32_t)ctx.lr); RETURN(ERROR_SUCCESS); }
GUEST_IMPORT(XamShowMessageBoxUI)
{
    // (user, title, text, button_count, buttons, focus, flags, result*, overlapped)
    Log("XamShowMessageBoxUI: \"%s\" / \"%s\"", Utf16ToUtf8(GuestWString(ARG(4))).c_str(), Utf16ToUtf8(GuestWString(ARG(5))).c_str());
    uint32_t resultPtr = ARG(10);
    if (resultPtr) GuestWrite32(resultPtr, 0);
    uint32_t overlapped = STACK_ARG(8);   // 9th argument (stack)
    RETURN(CompleteOverlapped(overlapped, ERROR_SUCCESS));
}
GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    // (user, content_type, content_flags, total_requested, device_id*, overlapped)
    if (ARG(7)) GuestWrite32(ARG(7), 1);   // device 1 = hdd
    RETURN(CompleteOverlapped(ARG(8), ERROR_SUCCESS));
}
GUEST_IMPORT(XamShowDirtyDiscErrorUI) { Fatal("XamShowDirtyDiscErrorUI: the game reported a disc read error"); }

// ---------------------------------------------------------------------------------------
// XSession (system link). XamSessionCreateHandle + XamSessionRefObjByHandle give the title a
// session object; XSessionCreate / Join / Start / End ... then arrive as messages to the XGI app
// (0xFB, 0x000B00xx) through XMsgStartIORequest(Ex). Sessions only need to exist locally: the
// game finds and joins hosts itself (system link broadcasts over the virtual network).
namespace
{
struct KSession : KObject { KSession() : KObject(KType::Other) {} };
}
uint32_t NetVirtualIp();   // net.cpp (0 when the network is off)

GUEST_IMPORT(XamSessionCreateHandle)   // (handle*)
{
    auto s = std::make_shared<KSession>();
    ObjectCreateGuestBody(s.get(), 0, 0x20);
    uint32_t h = HandleCreate(s);
    if (ARG(3)) GuestWrite32(ARG(3), h);
    RETURN(ERROR_SUCCESS);
}
GUEST_IMPORT(XamSessionRefObjByHandle)   // (handle, object*)
{
    auto o = HandleGet(ARG(3));
    if (!o) RETURN(6);   // ERROR_INVALID_HANDLE
    if (ARG(4)) GuestWrite32(ARG(4), o->guestBody);
    RETURN(ERROR_SUCCESS);
}

namespace
{
uint32_t XgiMessage(uint32_t msg, uint32_t buf, uint32_t len)
{
    static std::mutex m;
    static std::set<uint32_t> seen;
    {
        std::lock_guard lock(m);
        if (seen.insert(msg).second)
        {
            std::string dump;
            char b[16];
            for (uint32_t i = 0; i < std::min<uint32_t>(len, 64); i += 4) { snprintf(b, sizeof(b), " %08X", GuestRead32(buf + i)); dump += b; }
            Log("xgi: message 0x%08X len %u:%s", msg, len, dump.c_str());
        }
    }
    if (msg == 0x000B0010 && len >= 28)
    {
        // XSessionCreate: obj, flags, public slots, private slots, xuid, XSESSION_INFO*, nonce*
        uint32_t flags = GuestRead32(buf + 4), info = GuestRead32(buf + 0x14), nonce = GuestRead32(buf + 0x18);
        static std::mt19937_64 rng{ std::random_device{}() };
        if ((flags & 1) && info)   // XSESSION_CREATE_HOST: describe our session
        {
            // XSESSION_INFO: XNKID sessionID (8), XNADDR hostAddress (0x24), XNKEY keyExchangeKey (0x10)
            uint64_t id = rng() & 0x0FFFFFFFFFFFFFFFull;   // high nibble 0: system link key
            GuestWrite64(info, id);
            uint32_t ip = NetVirtualIp();
            uint32_t a = info + 8;
            memset(g_base + a, 0, 0x24);
            GuestWrite32(a, ip);
            uint8_t mac[6] = { 0x00, 0x22, 0x48, uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip) };
            memcpy(g_base + a + 10, mac, 6);
            GuestWrite64(info + 0x2C, rng());
            GuestWrite64(info + 0x34, rng());
            Log("xgi: XSessionCreate host flags 0x%X slots %u/%u -> id %016llX", flags, GuestRead32(buf + 8), GuestRead32(buf + 12), (unsigned long long)id);
        }
        else Log("xgi: XSessionCreate join flags 0x%X slots %u/%u id %016llX", flags, GuestRead32(buf + 8), GuestRead32(buf + 12), info ? (unsigned long long)GuestRead64(info) : 0ull);
        if (nonce) GuestWrite64(nonce, rng());
    }
    return ERROR_SUCCESS;
}
}

// XMsgStartIORequest(app, message, overlapped, buffer, length) / ...Ex(app, message, overlapped, buffer, length, extra)
GUEST_IMPORT(XMsgStartIORequest)
{
    if (ARG(3) == 0xFB) RETURN(CompleteOverlapped(ARG(5), XgiMessage(ARG(4), ARG(6), ARG(7))));
    RETURN(CompleteOverlapped(ARG(5), ERROR_FUNCTION_FAILED));
}
GUEST_IMPORT(XMsgStartIORequestEx)
{
    if (ARG(3) == 0xFB) RETURN(CompleteOverlapped(ARG(5), XgiMessage(ARG(4), ARG(6), ARG(7))));
    if (ARG(3) == 0xFC)
    {
        std::string dump;
        char b[16];
        for (uint32_t i = 0; i < std::min<uint32_t>(ARG(7), 64); i += 4) { snprintf(b, sizeof(b), " %08X", GuestRead32(ARG(6) + i)); dump += b; }
        Log("xlive: XMsgStartIORequestEx message 0x%08X len %u:%s lr=0x%X", ARG(4), ARG(7), dump.c_str(), (uint32_t)ctx.lr);
    }
    RETURN(CompleteOverlapped(ARG(5), ERROR_FUNCTION_FAILED));
}
GUEST_IMPORT(XMsgCancelIORequest) { RETURN(ERROR_SUCCESS); }
// XMsgInProcessCall(app, message, param1, param2): synchronous call into a system app. Returns
// an HRESULT (callers test < 0). App 0xFC is xlivebase.
GUEST_IMPORT(XMsgInProcessCall)
{
    uint32_t app = ARG(3), msg = ARG(4), p1 = ARG(5), p2 = ARG(6);
    if (app == 0xFC && msg == 0x00058006)   // NAT type -> open
    {
        if (p1) GuestWrite32(p1, 1);
        RETURN(0);
    }
    if (app == 0xFC && msg == 0x00058007 && LiveSignedIn())
    {
        // XNetLogonGetServiceInfo(service id, XONLINE_SERVICE_INFO* {id, IN_ADDR, port u16, reserved}):
        // the Demonware lobby is an LSP service of the title (0x415607E9). Its "address" is a
        // placeholder that net.cpp routes to the in-process Demonware.
        if (p2) { GuestWrite32(p2, p1); GuestWrite32(p2 + 4, 0x0A445701u); GuestWrite16(p2 + 8, 3074); GuestWrite16(p2 + 10, 0); }
        static int n = 0;
        if (n++ < 4) Log("xlive: XNetLogonGetServiceInfo(0x%08X, 0x%X)", p1, p2);
        RETURN(0);
    }
    if (app == 0xFC && msg == 0x00058035 && p1)
    {
        // XStorageBuildServerPath(user, facility, info, info size, item name, server path*, length*):
        // params +0 user, +16 facility, +20 info, +24 size, +28 item (wide), +32 out path (wide), +36 length*
        uint32_t item = GuestRead32(p1 + 28), out = GuestRead32(p1 + 32), lenPtr = GuestRead32(p1 + 36);
        std::u16string name;
        for (uint32_t a = item; a && GuestRead16(a) && name.size() < 256; a += 2) name += char16_t(GuestRead16(a));
        std::u16string path = u"tms://title/" + name;
        uint32_t cap = lenPtr ? GuestRead32(lenPtr) : 0;
        if (!out || cap < path.size() + 1) { if (lenPtr) GuestWrite32(lenPtr, uint32_t(path.size() + 1)); RETURN(0x8007007Au); }
        for (size_t i = 0; i < path.size(); i++) GuestWrite16(out + uint32_t(i * 2), uint16_t(path[i]));
        GuestWrite16(out + uint32_t(path.size() * 2), 0);
        GuestWrite32(lenPtr, uint32_t(path.size() + 1));
        std::string n8(name.begin(), name.end());
        Log("xlive: XStorageBuildServerPath facility %u \"%s\"", GuestRead32(p1 + 16), n8.c_str());
        RETURN(0);
    }
    static std::mutex m;
    static std::set<uint64_t> seen;
    {
        std::lock_guard lock(m);
        static const bool trace = getenv("BO3_XMSG_TRACE") != nullptr;
        if (seen.insert((uint64_t(app) << 32) | msg).second || trace)
            LogWarn("XMsgInProcessCall: app 0x%X message 0x%08X (0x%X, 0x%X) not handled lr=0x%X", app, msg, p1, p2, (uint32_t)ctx.lr);
    }
    RETURN(0x80004005u);   // E_FAIL
}

// ---------------------------------------------------------------------------------------
// Profile settings. XamUserReadProfileSettings(title_id, user, xuid_count, xuids, setting_count,
// setting_ids, buffer_size*, buffer, overlapped). Every requested setting is returned with its
// type (encoded in the setting id: type = id >> 28) and a zero value marked "default".
//
// Buffer: X_USER_READ_PROFILE_SETTINGS { u32 count; u32 settings_ptr; } followed by
// X_USER_PROFILE_SETTING[count] (0x28 bytes): +0 from, +8 user_index/xuid, +0x10 setting_id,
// +0x18 X_USER_DATA { u8 type; pad[7]; value (8 bytes) or {u32 size; u32 ptr} }.
GUEST_IMPORT(XamUserReadProfileSettings)
{
    uint32_t user = ARG(4), xuidCount = ARG(5), count = ARG(7), ids = ARG(8), sizePtr = ARG(9), buf = ARG(10);
    uint32_t overlapped = STACK_ARG(8);
    uint32_t entries = count * std::max<uint32_t>(xuidCount, 1);
    uint32_t need = 8 + entries * 0x28;
    if (!buf || GuestRead32(sizePtr) < need)
    {
        GuestWrite32(sizePtr, need);
        RETURN(ERROR_INSUFFICIENT_BUFFER);
    }
    memset(g_base + buf, 0, need);
    GuestWrite32(buf, entries);
    GuestWrite32(buf + 4, buf + 8);
    uint32_t e = buf + 8;
    for (uint32_t x = 0; x < std::max<uint32_t>(xuidCount, 1); x++)
        for (uint32_t i = 0; i < count; i++, e += 0x28)
        {
            uint32_t id = GuestRead32(ids + i * 4);
            GuestWrite32(e + 0x00, 1);   // XSOURCE_DEFAULT
            if (xuidCount) GuestWrite64(e + 0x08, GuestRead64(ARG(6) + x * 8));
            else GuestWrite32(e + 0x08, user);
            GuestWrite32(e + 0x10, id);
            GuestWrite8(e + 0x18, uint8_t(id >> 28));
        }
    if (g_logImports) Log("XamUserReadProfileSettings(title %08X, %u settings)", ARG(3), count);
    RETURN(CompleteOverlapped(overlapped, ERROR_SUCCESS));
}
