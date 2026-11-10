// Demonware emulation (online mode, BO3_LIVE). Requests arrive through the emulated XHTTP
// (xhttp.cpp) and, later, the lobby connection.
//
// auth3 (POST https://ops3-xbox360-auth3.prod.demonware.net/auth/): JSON
//   {"auth_task": "42", "iv_seed": "...", "title_id": "5574", "extra_data": "..."}
// answered with {"auth_task", "code": "700" (ok), "iv_seed", "client_ticket", "server_ticket"}.
// The client ticket is a 128-byte bdAuthTicket, little-endian and packed:
//   magic 0xEFBDADDE, type u8, title id, time issued, time expires, license id u64, user id u64,
//   user name[64], session key[24], hash magic[3], hash[4]
// The game skips decrypting it when it already starts with the magic (sub_827B5F08), so it goes
// in plain; the session key is ours (the lobby connection will use it).
#include "dw_server.h"
#include "json_mini.h"
#include "log.h"
#include "xhttp.h"

#include <algorithm>
#include <filesystem>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>

uint64_t ProfileXuid();           // xam.cpp
const char* ProfileGamertag();
using namespace json_mini;

namespace
{
std::mutex s_lock;
DwSession s_session;

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string B64(const std::string& in)
{
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
    }
    if (i + 1 == in.size()) { uint32_t v = uint8_t(in[i]) << 16; out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += "=="; }
    else if (i + 2 == in.size()) { uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8); out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += '='; }
    return out;
}

template <class T> void PutLE(std::string& s, T v) { for (size_t i = 0; i < sizeof(T); i++) s += char((uint64_t(v) >> (8 * i)) & 0xFF); }

std::string Ticket(uint32_t titleId, uint64_t userId, const std::string& name, const uint8_t* key)
{
    std::string t;
    uint32_t now = uint32_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    PutLE<uint32_t>(t, 0xEFBDADDE);
    t += char(0);                         // type
    PutLE<uint32_t>(t, titleId);
    PutLE<uint32_t>(t, now);
    PutLE<uint32_t>(t, now + 24 * 3600);
    PutLE<uint64_t>(t, 1);                // license id
    PutLE<uint64_t>(t, userId);
    std::string n = name.substr(0, 63);
    n.resize(64, '\0');
    t += n;
    t.append(reinterpret_cast<const char*>(key), 24);
    t.append(7, '\0');                    // hash magic + hash
    return t;                             // 128 bytes
}

HttpLocalResponse Auth3(const HttpLocalRequest& req)
{
    std::string task = JsonField(req.body, "auth_task"), iv = JsonField(req.body, "iv_seed"), title = JsonField(req.body, "title_id");
    std::lock_guard g(s_lock);
    static std::mt19937_64 rng{ std::random_device{}() };
    for (auto& b : s_session.sessionKey) b = uint8_t(rng());
    s_session.titleId = uint32_t(strtoul(title.c_str(), nullptr, 10));
    s_session.userId = ProfileXuid();
    s_session.userName = ProfileGamertag();
    s_session.ivSeed = uint32_t(strtoul(iv.c_str(), nullptr, 10));
    s_session.authenticated = true;
    std::string client = Ticket(s_session.titleId, s_session.userId, s_session.userName, s_session.sessionKey);
    std::string server(128, '\0');
    for (size_t i = 0; i < server.size(); i++) server[i] = char(0xA0 + (i & 15));   // recognizable (the client hands it to the lobby)
    HttpLocalResponse r;
    r.status = 200;
    // the reply's task is the request's + 1 (42 -> 43: checked by the parser's caller)
    std::string replyTask = std::to_string((task.empty() ? 42 : atoi(task.c_str())) + 1);
    r.body = "{\"auth_task\": \"" + replyTask + "\", \"code\": \"700\", \"iv_seed\": \"" + iv +
             "\", \"client_ticket\": \"" + B64(client) + "\", \"server_ticket\": \"" + B64(server) +
             "\", \"extra_data\": \"{\\\"dpi\\\": \\\"0\\\", \\\"extended_data\\\": \\\"\\\"}\"}";   // (sub_827B4260 requires extra_data)
    Log("dw: auth3 reply %s", r.body.c_str());
    Log("dw: auth3 task %s title %s -> signed in as \"%s\" (%016llX)", task.c_str(), title.c_str(), s_session.userName.c_str(), (unsigned long long)s_session.userId);
    return r;
}
}

DwSession DwCurrentSession()
{
    std::lock_guard g(s_lock);
    return s_session;
}

HttpLocalResponse HttpLocalHandle(const HttpLocalRequest& req)
{
    if (req.host.find("auth3") != std::string::npos && req.path.rfind("/auth", 0) == 0) return Auth3(req);
    Log("dw: unhandled %s %s%s (%zu bytes): %.*s", req.method.c_str(), req.host.c_str(), req.path.c_str(), req.body.size(), int(std::min<size_t>(req.body.size(), 400)), req.body.c_str());
    HttpLocalResponse r;
    r.status = 404;
    r.body = "{}";
    return r;
}

// ---------------------------------------------------------------------------------------
// Lobby (bdLobby over the LSP "TCP" connection), as the 360 client speaks it:
//   client: C8 00 00 00 + u32 (header), then [u32 size][u8 0 = plain][bit buffer: auth with the
//           server ticket]
//   frames: [u32 size][u8 flag: 0 plain, 1/2 encrypted (+8 bytes)][size-1 bytes]
//   plain message = [u8 type][payload]: 1 task reply, 2 ?, 3 u32, 4 login done (u64 user id,
//   typed byte buffer), 5 push (sub_827AEA68).
#include <thread>
#include <unistd.h>

namespace
{
std::string Frame(uint8_t type, const std::string& payload)
{
    std::string f;
    PutLE<uint32_t>(f, uint32_t(payload.size() + 2));
    f += char(0);        // plain
    f += char(type);
    f += payload;
    return f;
}

void Send(int fd, const std::string& d)
{
    size_t off = 0;
    while (off < d.size()) { ssize_t n = write(fd, d.data() + off, d.size() - off); if (n <= 0) return; off += size_t(n); }
}

std::string Hex(const char* p, size_t n, size_t max = 256)
{
    std::string h;
    char b[4];
    for (size_t i = 0; i < n && i < max; i++) { snprintf(b, sizeof(b), "%02X", (unsigned char)p[i]); h += b; }
    return h;
}
}

// Typed byte buffer (bdByteBuffer with type tags: 1 bool, 3 ubyte, 7 int32, 8 uint32, 10 uint64,
// 16 string, 19 blob, 100+n arrays).
struct DwWriter
{
    std::string b;
    void U8(uint8_t v) { b += char(3); b += char(v); }
    void U32(uint32_t v) { b += char(8); PutLE(b, v); }
    void I32(int32_t v) { b += char(7); PutLE(b, uint32_t(v)); }
    void U64(uint64_t v) { b += char(10); PutLE(b, v); }
    void Bool(bool v) { b += char(1); b += char(v ? 1 : 0); }
    void F32(float v) { uint32_t u; memcpy(&u, &v, 4); b += char(13); PutLE(b, u); }
    void Str(const std::string& v) { b += char(16); b += v; b += char(0); }
    void Blob(const std::string& v) { b += char(19); U32(uint32_t(v.size())); b += v; }
};

struct DwReader
{
    const std::string& b;
    size_t p = 0;
    bool ok = true;
    explicit DwReader(const std::string& s, size_t start = 0) : b(s), p(start) {}
    bool Tag(uint8_t t) { if (p >= b.size() || uint8_t(b[p]) != t) { ok = false; return false; } p++; return true; }
    template <class T> T LE(size_t n) { T v = 0; if (p + n > b.size()) { ok = false; return 0; } for (size_t i = 0; i < n; i++) v |= T(uint8_t(b[p + i])) << (8 * i); p += n; return v; }
    uint8_t U8() { return Tag(3) ? LE<uint8_t>(1) : 0; }
    bool Bool() { return Tag(1) ? LE<uint8_t>(1) != 0 : false; }
    uint16_t U16() { return Tag(6) ? LE<uint16_t>(2) : 0; }
    uint32_t U32() { return Tag(8) ? LE<uint32_t>(4) : 0; }
    int32_t I32() { return Tag(7) ? int32_t(LE<uint32_t>(4)) : 0; }
    uint64_t U64() { return Tag(10) ? LE<uint64_t>(8) : 0; }
    std::string Str() { if (!Tag(16)) return {}; size_t e = b.find('\0', p); if (e == std::string::npos) { ok = false; return {}; } std::string v = b.substr(p, e - p); p = e + 1; return v; }
    std::string Blob() { if (!Tag(19)) return {}; uint32_t n = U32(); if (p + n > b.size()) { ok = false; return {}; } std::string v = b.substr(p, n); p += n; return v; }
};

// The typed values from body[from] up to the end of the message (a 0 tag or the end of the data).
std::string TypedValues(const std::string& b, size_t from)
{
    size_t p = from;
    while (p < b.size())
    {
        uint8_t t = uint8_t(b[p]);
        size_t n = 0;
        switch (t)
        {
        case 1: case 2: case 3: n = 1; break;      // bool, char, ubyte
        case 5: case 6: n = 2; break;              // int16, uint16
        case 7: case 8: case 13: n = 4; break;     // int32, uint32, float
        case 9: case 10: case 14: n = 8; break;    // int64, uint64, double
        case 16: { size_t e = b.find('\0', p + 1); n = e == std::string::npos ? b.size() - p - 1 : e - p; break; }
        case 19:                                   // blob: [19][8][u32 size][data]
            if (p + 6 > b.size()) return b.substr(from, p - from);
            n = 5 + (uint8_t(b[p + 2]) | (uint8_t(b[p + 3]) << 8) | (uint8_t(b[p + 4]) << 16) | (uint32_t(uint8_t(b[p + 5])) << 24));
            break;
        default: return b.substr(from, p - from);
        }
        if (p + 1 + n > b.size()) return b.substr(from, p - from);
        p += 1 + n;
    }
    return b.substr(from, p - from);
}

// bdFileInfo result: size, id, created, modified, private, owner, name
std::string FileInfo(const std::string& name, uint32_t size, uint64_t owner, bool priv)
{
    DwWriter w;
    uint64_t id = 1469598103934665603ull;
    for (unsigned char c : name) { id ^= c; id *= 1099511628211ull; }
    uint32_t now = uint32_t(time(nullptr));
    w.U32(size);
    w.U64(id);
    w.U32(now);
    w.U32(now);
    w.Bool(priv);
    w.U64(owner);
    w.Str(name);
    return w.b;
}

std::string ReadAll(const std::string& path, bool& found)
{
    std::string d;
    found = false;
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
        found = true;
        char b[16384];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) d.append(b, n);
        fclose(f);
    }
    return d;
}
// Demonware user files (online stats, loadouts...) live in the profile's save folder, so the cloud
// sync (cloud.cpp) carries them to Supabase with the rest of the saves.
std::string UserDir()
{
    const char* x = getenv("BO3_XUID");
    std::string d = std::string("content/") + (x ? x : "E0300000126D662A") + "/4156091D/online";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}
void WriteAll(const std::string& path, const std::string& data)
{
    std::string tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) { fwrite(data.data(), 1, data.size(), f); fclose(f); rename(tmp.c_str(), path.c_str()); }
}
std::string SafeName(std::string n) { for (auto& c : n) if (c == '/' || c == '\\' || c == ':') c = '_'; return n; }

// task reply (message type 1): transaction, error, task, result count [, count, results...]
std::string TaskReply(uint8_t task, uint32_t error, uint32_t count = 0, const std::string& results = {})
{
    static uint64_t tx = 0;
    DwWriter w;
    w.U64(++tx);
    w.U32(error);
    w.U8(task);
    if (error == 0)
    {
        w.U32(count);
        if (count) { w.U32(count); w.b += results; }
    }
    else w.U64(tx);
    return Frame(1, w.b);
}

constexpr uint32_t BD_NO_FILE = 1000;

// zlib stream with stored (uncompressed) blocks: what the game inflates for the .gz publisher files
std::string ZlibStored(const std::string& d)
{
    std::string z = "\x78\x01";
    size_t off = 0;
    do
    {
        size_t n = std::min<size_t>(d.size() - off, 65535);
        bool last = off + n == d.size();
        z += char(last ? 1 : 0);
        z += char(n & 0xFF); z += char(n >> 8);
        z += char(~n & 0xFF); z += char((~n >> 8) & 0xFF);
        z.append(d, off, n);
        off += n;
    } while (off < d.size());
    uint32_t a = 1, b = 0;
    for (unsigned char c : d) { a = (a + c) % 65521; b = (b + a) % 65521; }
    uint32_t ad = (b << 16) | a;
    for (int i = 3; i >= 0; i--) z += char((ad >> (8 * i)) & 0xFF);
    return z;
}

// Built-in publisher files (ours, unsigned: the game hook accepts the "BO3RECOMP" header).
// online/publisher/<name> overrides them.
// Matchmaking playlists (publisher file playlists*.gz), in the game's playlists.info text format
// (parser: MP sub_826A4D70). Top-level blocks, one keyword per line:
//   gametype <name>        a game mode; "script <gametype script>", "rule <dvar> <value>"
//   maplist <name>         followed by one map per line ("<map> [free]")
//   supercategory <name>   top menu level: sessionmode / name / description / icon
//   category <name>        menu category: supercategory <name> / sessionmode / name / description
//                          / icon. sessionmode (mp, zm or cp) is required: only categories of the
//                          running mode are listed. filter: the menu that lists it (playermatch =
//                          Public Match, arenamatch = Arena). Zombies lists one category per map:
//                          its name must be the map's (zm_zod). Categories must come before the
//                          playlists: inside a playlist, "category <name>" assigns it instead.
//   playlist <id 0-249>    name / description / icon / category / minparty / maxparty / rule,
//                          then entries "<map or maplist>,<gametype>,<weight>[,free]" (comma
//                          separated: these lines are tokenized on commas)
//   "name english <text>" (same for description): text for that language, english = fallback.
static const char kPlaylists[] = R"(version 1
gametype tdm
script tdm
gametype dm
script dm
gametype dom
script dom
gametype koth
script koth
gametype conf
script conf
gametype dem
script dem
gametype escort
script escort
gametype ball
script ball
gametype gun
script gun
gametype zclassic
script zclassic

maplist core
mp_apartments
mp_biodome
mp_chinatown
mp_ethiopia
mp_havoc
mp_infection
mp_metro
mp_redwood
mp_sector
mp_spire
mp_stronghold
mp_veiled

supercategory matchmaking
sessionmode mp
name english "Matchmaking"
description english "Online public matches."

supercategory zombies
sessionmode zm
name english "Zombies"
description english "Online Zombies matches."

category core
filter playermatch
supercategory matchmaking
sessionmode mp
name english "Core"
icon playlist_teamtactical
description english "The classic Black Ops III modes."

category party
filter playermatch
supercategory matchmaking
sessionmode mp
name english "Party Games"
icon playlist_generic_01
description english "Free-for-all and party modes."

category zm_zod
filter playermatch
supercategory zombies
sessionmode zm
name english "Shadows of Evil"
description english "Take to the streets of Morg City to combat the undead. Up to 4 players."
icon img_t7_menu_zm_preview_zod

category zm_factory
filter playermatch
supercategory zombies
sessionmode zm
name english "The Giant"
description english "Return to Der Riese, where it all began. Up to 4 players."
icon img_mapvote_zm_factory

category zm_castle
filter playermatch
supercategory zombies
sessionmode zm
name english "Der Eisendrache"
description english "A castle in the Austrian Alps hides the dragon's secrets. Up to 4 players."
icon img_t7_menu_zm_preview_castle

playlist 1
name english "Team Deathmatch"
icon playlist_tdm
description english "Two teams. The first to reach the score limit wins."
category core
minparty 1
maxparty 6
core,tdm,1

playlist 2
name english "Domination"
icon playlist_domination
description english "Capture and hold the flags."
category core
minparty 1
maxparty 6
core,dom,1

playlist 3
name english "Kill Confirmed"
icon playlist_kill_confirm
description english "Collect the dog tags of your kills."
category core
minparty 1
maxparty 6
core,conf,1

playlist 4
name english "Hardpoint"
icon playlist_koth
description english "Hold the rotating hardpoint zone."
category core
minparty 1
maxparty 6
core,koth,1

playlist 5
name english "Demolition"
icon playlist_demolition
description english "Plant and defend bombs."
category core
minparty 1
maxparty 6
core,dem,1

playlist 6
name english "Safeguard"
icon playlist_escort
description english "Escort the robot to the enemy base."
category core
minparty 1
maxparty 6
core,escort,1

playlist 7
name english "Uplink"
icon playlist_ball
description english "Throw or carry the ball into the uplink."
category core
minparty 1
maxparty 6
core,ball,1

playlist 8
name english "Free-for-All"
icon playlist_ffa
description english "Every player for themselves."
category party
minparty 1
maxparty 1
core,dm,1

playlist 9
name english "Gun Game"
icon playlist_gungame
description english "Advance through the weapons, one kill at a time."
category party
minparty 1
maxparty 1
core,gun,1

playlist 20
name english "Shadows of Evil"
description english "Classic Zombies in Morg City."
icon img_t7_menu_zm_preview_zod
category zm_zod
minparty 1
maxparty 4
zm_zod,zclassic,1

playlist 21
name english "The Giant"
description english "Classic Zombies at Der Riese."
icon img_mapvote_zm_factory
category zm_factory
minparty 1
maxparty 4
zm_factory,zclassic,1

playlist 22
name english "Der Eisendrache"
description english "Classic Zombies in the castle."
icon img_t7_menu_zm_preview_castle
category zm_castle
minparty 1
maxparty 4
zm_castle,zclassic,1
)";

bool BuiltinPublisherFile(const std::string& name, std::string& out)
{
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    std::string text;
    if (has("playlists") && has(".gz"))
    {
        // online/playlists.txt (plain text, playlists.info format) replaces the built-in list
        bool f;
        text = ReadAll("online/playlists.txt", f);
        if (!f) text = kPlaylists;
    }
    else if (has("motd") && has(".gz")) text = "";
    else if (has("featured_cards") && has(".gz")) text = "";
    else return false;
    out = ZlibStored("BO3RECOMP\n" + text + std::string(1, '\0'));
    return true;
}

std::string PublisherFile(const std::string& name, bool& found)
{
    std::string data = ReadAll("online/publisher/" + SafeName(name), found);
    if (!found) found = BuiltinPublisherFile(name, data);
    return data;
}

// one service call -> its reply frame
void NetSetSession(const std::string& info);   // net.cpp: the public match we host
std::vector<std::string> NetPeerSessions();      // net.cpp: the public matches of the other instances

std::string DwService(uint8_t service, uint8_t task, const std::string& body)
{
    DwReader r(body, 3);
    DwSession ses = DwCurrentSession();
    if (service == 10)   // bdStorage
    {
        if (task == 21)   // publisher file (unk, name)
        {
            std::string unk = r.Str(), name = r.Str();
            bool found;
            std::string data = PublisherFile(name, found);
            Log("dw: storage: publisher file \"%s\" (%s) -> %s", name.c_str(), unk.c_str(), found ? "sent" : "missing");
            if (!found) return TaskReply(task, BD_NO_FILE);
            DwWriter w;
            w.Blob(data);
            return TaskReply(task, 0, 1, w.b);
        }
        if (task == 20)   // list publisher files (unk, date, count, offset, name)
        {
            std::string unk = r.Str();
            r.U32(); r.U16(); r.U16();
            std::string name = r.Str();
            bool found;
            std::string data = PublisherFile(name, found);
            Log("dw: storage: list publisher files \"%s\" -> %s", name.c_str(), found ? "1" : "none");
            if (!found) return TaskReply(task, 0);
            return TaskReply(task, 0, 1, FileInfo(name, uint32_t(data.size()), 0, false));
        }
        if (task == 12)   // user file by name (context, name, owner, ?): the file or NO_FILE
        {
            std::string ctxs = r.Str(), name = r.Str();
            bool found;
            std::string data = ReadAll(UserDir() + "/" + SafeName(name), found);
            Log("dw: storage: user file \"%s\" -> %s", name.c_str(), found ? "sent" : "missing");
            if (!found) return TaskReply(task, BD_NO_FILE);
            DwWriter w;
            w.Blob(data);
            return TaskReply(task, 0, 1, w.b);
        }
        if (task == 16)   // get files: context, [u64 owner, string account] x n, [string name] x n
        {
            std::string context = r.Str();
            uint32_t n = r.U32();
            std::vector<std::pair<uint64_t, std::string>> users;
            for (uint32_t i = 0; i < n && r.ok; i++) { uint64_t u = r.U64(); users.emplace_back(u, r.Str()); }
            uint32_t m = r.U32();
            DwWriter w;
            std::string names;
            for (uint32_t i = 0; i < m && r.ok; i++)
            {
                std::string name = r.Str();
                uint64_t owner = i < users.size() ? users[i].first : 0;
                bool found = false;
                std::string data;
                if (owner == 0 || owner == ses.userId) data = ReadAll(UserDir() + "/" + SafeName(name), found);
                w.U64(owner);
                w.Str(i < users.size() ? users[i].second : "");
                w.Str(name);
                w.U32(found ? 0 : BD_NO_FILE);
                w.Blob(data);
                names += " " + name + (found ? "" : "(new)");
            }
            Log("dw: storage: get files%s", names.c_str());
            return TaskReply(task, 0, m, w.b);
        }
        if (task == 18 || task == 24)   // upload files: game, owner, account, n x (name, data, u32, bool private)
        {
            std::string game = r.Str();
            uint64_t owner = r.U64();
            std::string account = r.Str();
            uint32_t n = r.U32();
            DwWriter w;
            std::string names;
            uint32_t now = uint32_t(time(nullptr));
            for (uint32_t i = 0; i < n && r.ok; i++)
            {
                std::string name = r.Str(), data = r.Blob();
                uint32_t ver = r.U32();
                bool priv = r.Bool();
                if (!r.ok) break;
                WriteAll(UserDir() + "/" + SafeName(name), data);
                names += " " + name + "(" + std::to_string(data.size()) + ")";
                if (task == 18) { w.U32(now); w.U32(now); w.Bool(priv); w.U64(owner); w.Str(account); w.Str(name); }
                else { w.U32(0); w.U32(0); w.U32(0); w.Bool(false); w.U64(owner); w.Str(account); w.Str(name); w.Blob(data); }
                (void)ver;
            }
            Log("dw: storage: upload files%s", names.c_str());
            return TaskReply(task, 0, n, w.b);
        }
        if (task == 13) return TaskReply(task, 0);   // user file list: none
        if (task == 10)   // set user file (game, name, private, data, owner)
        {
            std::string game = r.Str(), name = r.Str();
            bool priv = r.Bool();
            std::string data = r.Blob();
            uint64_t owner = r.U64();
            WriteAll(UserDir() + "/" + SafeName(name), data);
            Log("dw: storage: saved user file \"%s\" (%s, %zu bytes)", name.c_str(), game.c_str(), data.size());
            return TaskReply(task, 0, 1, FileInfo(name, uint32_t(data.size()), owner ? owner : ses.userId, priv));
        }
    }
    if (service == 21)   // bdMatchMaking: public match sessions, shared over the virtual network (net.cpp)
    {
        // Create / update carry the game's MatchMakingInfo serialization: host address blob, game
        // type, max players, then the game's own fields (security id and key, netcode / playlist
        // values, skill, geo, host xuid). A search result is read back as host address blob,
        // session id blob, game type, max players, player count, then the same game fields - that
        // is what the session is announced as.
        if (task == 1 || task == 2 || task == 12)   // create / update (session id, ...) / update with player count
        {
            static uint64_t s_sessionId = 0;
            static uint32_t s_players = 1;
            if (task != 1)
            {
                r.Blob();                                    // session id (ours)
                if (task == 12) s_players = r.U32();         // players in the lobby
            }
            else
            {
                s_sessionId = (uint64_t(std::random_device{}()) << 32) | std::random_device{}();
                s_players = 1;
            }
            std::string host = r.Blob();
            uint32_t gameType = r.U32(), maxPlayers = r.U32();
            std::string rest = TypedValues(body, r.p);
            if (r.ok)
            {
                DwWriter w;
                w.Blob(host);
                w.Blob(std::string(reinterpret_cast<const char*>(&s_sessionId), 8));
                w.U32(gameType);
                w.U32(maxPlayers);
                w.U32(s_players);
                NetSetSession(w.b + rest);
            }
            Log("dw: matchmaking: %s session (%u/%u players)%s", task == 1 ? "create" : "update", s_players, maxPlayers, r.ok ? "" : " - unreadable");
            if (task != 1) return TaskReply(task, 0);
            DwWriter w;
            w.Blob(std::string(reinterpret_cast<const char*>(&s_sessionId), 8));
            return TaskReply(task, 0, 1, w.b);
        }
        if (task == 3)   // delete session
        {
            NetSetSession({});
            Log("dw: matchmaking: delete session");
            return TaskReply(task, 0);
        }
        if (task == 5)   // find sessions (query id, offset, count, query values...)
        {
            std::vector<std::string> found = NetPeerSessions();
            std::string all;
            for (auto& f : found) all += f;
            static size_t s_lastFound = size_t(-1);
            if (found.size() != s_lastFound) { Log("dw: matchmaking: search -> %zu session(s)", found.size()); s_lastFound = found.size(); }
            return TaskReply(task, 0, uint32_t(found.size()), all);
        }
    }
    if (service == 27 && task == 2)   // bdDML: user location (raw data)
    {
        DwWriter w;
        w.Str("AR"); w.Str("Argentina"); w.Str("Buenos Aires"); w.Str("Buenos Aires");
        w.F32(-34.6f); w.F32(-58.4f);
        w.U32(0); w.Str("-03:00");
        Log("dw: dml task %u -> location", task);
        return TaskReply(task, 0, 1, w.b);
    }
    if (service == 12 && task == 6)   // bdTitleUtilities: server time
    {
        DwWriter w;
        w.U32(uint32_t(time(nullptr)));
        return TaskReply(task, 0, 1, w.b);
    }
    Log("dw: service %u task %u (%zu bytes) -> not implemented, ok/no results", service, task, body.size() - 3);
    return TaskReply(task, 0);
}

void DwLobbyAccept(int fd)
{
    std::thread([fd] {
        pthread_setname_np(pthread_self(), "dw-lobby");
        std::string in;
        char buf[8192];
        bool authed = false;
        for (;;)
        {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            in.append(buf, size_t(n));
            // frames: the 8-byte header first (size field 0xC8), then [size][flag][body]
            for (;;)
            {
                if (in.size() < 4) break;
                uint32_t size = uint8_t(in[0]) | (uint8_t(in[1]) << 8) | (uint8_t(in[2]) << 16) | (uint32_t(uint8_t(in[3])) << 24);
                if (size == 0xC8 && in.size() >= 8) { Log("dw: lobby <- client header %s", Hex(in.data(), 8).c_str()); in.erase(0, 8); continue; }
                if (size == 0)   // keep-alive: answered with an empty frame
                {
                    in.erase(0, 4);
                    Send(fd, std::string(4, '\0'));
                    continue;
                }
                if (in.size() < 4 + size) break;
                uint8_t flag = uint8_t(in[4]);
                std::string body = in.substr(5, size - 1);
                in.erase(0, 4 + size);
                Log("dw: lobby <- frame flag %u, %u bytes: %s", flag, size - 1, Hex(body.data(), body.size(), 96).c_str());
                if (authed && flag == 0 && body.size() >= 3)
                {
                    // service call: [service id][typed ubyte task][typed arguments]
                    uint8_t service = uint8_t(body[0]), task = uint8_t(body[1]) == 3 ? uint8_t(body[2]) : 0;
                    std::string reply;
                    try { reply = DwService(service, task, body); }
                    catch (const std::exception& e)
                    {
                        // a malformed / unexpected request must never take the game down
                        LogWarn("dw: service %u task %u failed (%s), answering ok/empty", service, task, e.what());
                        reply = TaskReply(task, 0);
                    }
                    Send(fd, reply);
                    continue;
                }
                if (!authed && flag == 0)
                {
                    // the auth (server ticket): login done for our user
                    authed = true;
                    DwSession ses = DwCurrentSession();
                    std::string p;
                    p += char(10);   // typed uint64
                    PutLE<uint64_t>(p, ses.userId);
                    Send(fd, Frame(4, p));
                    Log("dw: lobby -> login done (user %016llX)", (unsigned long long)ses.userId);
                }
            }
        }
        Log("dw: lobby connection closed");
        close(fd);
    }).detach();
}
