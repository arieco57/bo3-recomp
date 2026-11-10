// XHTTP (the console's WinHTTP-like client, NetDll_XHttp*). Requests never leave the machine:
// they go to an in-process handler (dw_server.cpp - the Demonware emulation for the online mode).
// The API is asynchronous like WinHTTP: results arrive through the status callback, which this
// emulation calls from XHttpDoWork on the game's own thread (the callback values are WinHTTP's:
// HEADERS_AVAILABLE 0x20000, READ_COMPLETE 0x80000, WRITE_COMPLETE 0x100000, REQUEST_ERROR
// 0x200000, SENDREQUEST_COMPLETE 0x400000).
#include "guest.h"
#include "log.h"
#include "memory.h"
#include "write_watch.h"
#include "xhttp.h"

#include <deque>
#include <map>
#include <mutex>

#define XTRACE(n) do { static int _c = 0; if (_c++ < 8) Log("xhttp: %s(0x%X, 0x%X, 0x%X, 0x%X) lr=0x%X", n, ARG(4), ARG(5), ARG(6), ARG(7), (uint32_t)ctx.lr); } while (0)

namespace
{
constexpr uint32_t ST_HEADERS = 0x20000, ST_READ = 0x80000, ST_WRITE = 0x100000, ST_ERROR = 0x200000, ST_SENT = 0x400000;

struct Handle
{
    int kind = 0;                 // 1 session, 2 connection, 3 request
    uint32_t parent = 0;
    std::string host, verb, path;
    uint16_t port = 0;
    bool secure = false;
    uint32_t callback = 0, context = 0;
    std::string reqHeaders, body;
    uint32_t total = 0;
    HttpLocalResponse resp;
    bool answered = false;
    size_t readPos = 0;
    uint32_t scratch = 0;         // guest DWORD for callback info
};
struct Event { uint32_t handle, status, info, len; };

std::mutex s_lock;
std::map<uint32_t, Handle> s_handles;
std::deque<Event> s_events;
uint32_t s_next = 0x7E000010;

std::string GStr(uint32_t a, size_t max = 4096)
{
    if (!a) return {};
    const char* p = reinterpret_cast<const char*>(g_base + a);
    return std::string(p, strnlen(p, max));
}

uint32_t NewHandle(Handle h)
{
    std::lock_guard g(s_lock);
    uint32_t id = s_next;
    s_next += 4;
    h.scratch = GuestAlloc(16);
    s_handles[id] = std::move(h);
    return id;
}

Handle* Get(uint32_t h)
{
    auto it = s_handles.find(h);
    return it == s_handles.end() ? nullptr : &it->second;
}

void Queue(uint32_t h, uint32_t status, uint32_t info = 0, uint32_t len = 0) { s_events.push_back({ h, status, info, len }); }

// Runs the request through the local handler once its body is complete.
void Answer(uint32_t id, Handle& r)
{
    if (r.answered) return;
    r.answered = true;
    Handle* c = Get(r.parent);
    HttpLocalRequest q;
    q.method = r.verb.empty() ? "GET" : r.verb;
    q.host = c ? c->host : "";
    q.port = c ? c->port : 0;
    q.secure = c && c->secure;
    q.path = r.path;
    q.headers = r.reqHeaders;
    q.body = r.body;
    r.resp = HttpLocalHandle(q);
    Log("xhttp: %s https://%s%s (%zu bytes) -> %d (%zu bytes)", q.method.c_str(), q.host.c_str(), q.path.c_str(), q.body.size(), r.resp.status, r.resp.body.size());
}

// Calls the guest status callback (handle, context, status, info, length) on this thread.
void Deliver(PPCContext& ctx, const Event& e)
{
    uint32_t cb, context;
    {
        std::lock_guard g(s_lock);
        Handle* h = Get(e.handle);
        if (!h) return;
        cb = h->callback;
        context = h->context;
    }
    if (!cb) return;
    PPCContext saved = ctx;
    ctx.r1.u64 = saved.r1.u64 - 0x400;   // below the caller's frame
    ctx.r3.u64 = e.handle;
    ctx.r4.u64 = context;
    ctx.r5.u64 = e.status;
    ctx.r6.u64 = e.info;
    ctx.r7.u64 = e.len;
    GuestCall(ctx, cb);
    ctx = saved;
}
}

GUEST_IMPORT(NetDll_XHttpStartup) {
    XTRACE("NetDll_XHttpStartup"); RETURN(1); }
GUEST_IMPORT(NetDll_XHttpShutdown) {
    XTRACE("NetDll_XHttpShutdown"); RETURN(1); }

// XHttpOpen(caller, user agent, access type, proxy, bypass, flags)
GUEST_IMPORT(NetDll_XHttpOpen) {
    XTRACE("NetDll_XHttpOpen"); Handle h; h.kind = 1; RETURN(NewHandle(std::move(h))); }

// XHttpConnect(caller, session, server name, port, flags)
GUEST_IMPORT(NetDll_XHttpConnect)
{
    XTRACE("NetDll_XHttpConnect");
    Handle h;
    h.kind = 2;
    h.parent = ARG(4);
    h.host = GStr(ARG(5));
    h.port = uint16_t(ARG(6));
    h.secure = (ARG(7) & 0x00800000) != 0;
    RETURN(NewHandle(std::move(h)));
}

// XHttpOpenRequest(caller, connection, verb, object name, version, referrer, accept types, flags)
GUEST_IMPORT(NetDll_XHttpOpenRequest)
{
    XTRACE("NetDll_XHttpOpenRequest");
    Handle h;
    h.kind = 3;
    h.parent = ARG(4);
    h.verb = ARG(5) ? GStr(ARG(5)) : "GET";
    h.path = ARG(6) ? GStr(ARG(6)) : "/";
    RETURN(NewHandle(std::move(h)));
}

// XHttpSetStatusCallback(caller, handle, callback, notification flags, reserved) -> previous
GUEST_IMPORT(NetDll_XHttpSetStatusCallback)
{
    XTRACE("NetDll_XHttpSetStatusCallback");
    std::lock_guard g(s_lock);
    Handle* h = Get(ARG(4));
    if (!h) RETURN(0xFFFFFFFFu);
    uint32_t prev = h->callback;
    h->callback = ARG(5);
    RETURN(prev);
}

// XHttpSendRequest(caller, request, headers, headers length, optional, optional length, total length, context)
GUEST_IMPORT(NetDll_XHttpSendRequest)
{
    XTRACE("NetDll_XHttpSendRequest");
    std::lock_guard g(s_lock);
    Handle* r = Get(ARG(4));
    if (!r) RETURN(0);
    if (ARG(5)) r->reqHeaders = ARG(6) && ARG(6) != 0xFFFFFFFFu ? std::string(reinterpret_cast<const char*>(g_base + ARG(5)), ARG(6)) : GStr(ARG(5));
    if (ARG(7) && ARG(8)) r->body.assign(reinterpret_cast<const char*>(g_base + ARG(7)), ARG(8));
    r->total = ARG(9);
    r->context = ARG(10);
    Queue(ARG(4), ST_SENT);
    RETURN(1);
}

// XHttpWriteData(caller, request, buffer, length, written*)
GUEST_IMPORT(NetDll_XHttpWriteData)
{
    XTRACE("NetDll_XHttpWriteData");
    std::lock_guard g(s_lock);
    Handle* r = Get(ARG(4));
    if (!r) RETURN(0);
    r->body.append(reinterpret_cast<const char*>(g_base + ARG(5)), ARG(6));
    if (ARG(7)) GuestWrite32(ARG(7), ARG(6));
    GuestWrite32(r->scratch, ARG(6));
    Queue(ARG(4), ST_WRITE, r->scratch, 4);
    RETURN(1);
}

// XHttpReceiveResponse(caller, request, reserved)
GUEST_IMPORT(NetDll_XHttpReceiveResponse)
{
    XTRACE("NetDll_XHttpReceiveResponse");
    std::lock_guard g(s_lock);
    Handle* r = Get(ARG(4));
    if (!r) RETURN(0);
    Answer(ARG(4), *r);
    Queue(ARG(4), ST_HEADERS);
    RETURN(1);
}

// XHttpQueryHeaders(caller, request, info level, name, buffer, buffer length*, index*)
GUEST_IMPORT(NetDll_XHttpQueryHeaders)
{
    XTRACE("NetDll_XHttpQueryHeaders");
    std::lock_guard g(s_lock);
    Handle* r = Get(ARG(4));
    if (!r) RETURN(0);
    uint32_t level = ARG(5) & 0xFFFF, asNumber = ARG(5) & 0x20000000, buf = ARG(7), lenPtr = ARG(8);
    std::string value;
    if (level == 19 || level == 0xFFFE) value = std::to_string(r->resp.status);   // STATUS_CODE (0xFFFE on the console)
    else if (level == 5 || level == 9) value = std::to_string(r->resp.body.size());   // CONTENT_LENGTH (9 on the console)
    else if (level == 1) value = r->resp.contentType;                       // CONTENT_TYPE
    else if (level == 22) value = "HTTP/1.1 " + std::to_string(r->resp.status) + "\r\nContent-Length: " + std::to_string(r->resp.body.size()) + "\r\n\r\n";   // RAW_HEADERS_CRLF
    else { Log("xhttp: QueryHeaders level %u (name %s) not provided", level, GStr(ARG(6)).c_str()); RETURN(0); }
    if (asNumber)
    {
        if (!buf) RETURN(0);
        GuestWrite32(buf, uint32_t(strtoul(value.c_str(), nullptr, 10)));
        if (lenPtr) GuestWrite32(lenPtr, 4);
        RETURN(1);
    }
    uint32_t cap = lenPtr ? GuestRead32(lenPtr) : 0;
    if (!buf || cap < value.size() + 1) { if (lenPtr) GuestWrite32(lenPtr, uint32_t(value.size() + 1)); RETURN(0); }
    memcpy(g_base + buf, value.c_str(), value.size() + 1);
    GuestWrite32(lenPtr, uint32_t(value.size()));
    RETURN(1);
}

// XHttpReadData(caller, request, buffer, size, read*): READ_COMPLETE with the bytes (0 = end)
GUEST_IMPORT(NetDll_XHttpReadData)
{
    XTRACE("NetDll_XHttpReadData");
    std::lock_guard g(s_lock);
    Handle* r = Get(ARG(4));
    if (!r) RETURN(0);
    size_t n = std::min<size_t>(ARG(6), r->resp.body.size() - std::min(r->readPos, r->resp.body.size()));
    if (n)
    {
        WatchHostWrite hw(ARG(5), uint32_t(n));
        memcpy(g_base + ARG(5), r->resp.body.data() + r->readPos, n);
    }
    r->readPos += n;
    if (ARG(7)) GuestWrite32(ARG(7), uint32_t(n));
    Queue(ARG(4), ST_READ, ARG(5), uint32_t(n));
    RETURN(1);
}

// XHttpDoWork(caller, session, timeout): delivers the pending callbacks
GUEST_IMPORT(NetDll_XHttpDoWork)
{
    for (int i = 0; i < 64; i++)
    {
        Event e;
        {
            std::lock_guard g(s_lock);
            if (s_events.empty()) break;
            e = s_events.front();
            s_events.pop_front();
        }
        Deliver(ctx, e);
    }
    RETURN(1);
}

GUEST_IMPORT(NetDll_XHttpCloseHandle)
{
    XTRACE("NetDll_XHttpCloseHandle");
    std::lock_guard g(s_lock);
    s_handles.erase(ARG(4));
    for (auto it = s_events.begin(); it != s_events.end();) it = it->handle == ARG(4) ? s_events.erase(it) : it + 1;
    RETURN(1);
}

// XHttpCrackUrl(caller, url, length, flags, URL_COMPONENTS*) - WinHTTP's 60-byte layout:
// +0 size, +4/+8 scheme, +12 nScheme (1 http, 2 https), +16/+20 host, +24 port (u16),
// +28/+32 user, +36/+40 password, +44/+48 path, +52/+56 extra info. A component with a buffer
// gets a copy; one without a buffer (length != 0) gets a pointer into the URL.
GUEST_IMPORT(NetDll_XHttpCrackUrl)
{
    uint32_t url = ARG(4), uc = ARG(7);
    std::string u = ARG(5) ? std::string(reinterpret_cast<const char*>(g_base + url), strnlen(reinterpret_cast<const char*>(g_base + url), ARG(5))) : GStr(url);
    size_t se = u.find("://");
    if (se == std::string::npos || !uc) RETURN(0);
    std::string scheme = u.substr(0, se);
    size_t hs = se + 3, pe = u.find('/', hs);
    if (pe == std::string::npos) pe = u.size();
    std::string hostPort = u.substr(hs, pe - hs);
    size_t q = u.find_first_of("?#", pe);
    size_t pathEnd = q == std::string::npos ? u.size() : q;
    uint16_t port = scheme == "https" ? 443 : 80;
    std::string host = hostPort;
    if (size_t c = hostPort.rfind(':'); c != std::string::npos) { host = hostPort.substr(0, c); port = uint16_t(atoi(hostPort.c_str() + c + 1)); }
    auto put = [&](uint32_t ptrOff, uint32_t lenOff, size_t start, size_t len) {
        uint32_t p = GuestRead32(uc + ptrOff), cap = GuestRead32(uc + lenOff);
        if (p && cap)
        {
            size_t n = std::min<size_t>(len, cap ? cap - 1 : 0);
            memcpy(g_base + p, u.data() + start, n);
            GuestWrite8(p + uint32_t(n), 0);
            GuestWrite32(uc + lenOff, uint32_t(n));
        }
        else if (cap)
        {
            GuestWrite32(uc + ptrOff, url + uint32_t(start));
            GuestWrite32(uc + lenOff, uint32_t(len));
        }
    };
    put(4, 8, 0, scheme.size());
    GuestWrite32(uc + 12, scheme == "https" ? 2 : 1);
    put(16, 20, hs, host.size());
    GuestWrite16(uc + 24, port);
    put(44, 48, pe, pathEnd - pe);
    put(52, 56, pathEnd, u.size() - pathEnd);
    Log("xhttp: CrackUrl %s -> host %s port %u path %s", u.c_str(), host.c_str(), port, u.substr(pe, pathEnd - pe).c_str());
    RETURN(1);
}
