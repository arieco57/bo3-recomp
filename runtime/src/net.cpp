// Networking. Default: system link (LAN) through a virtual network - every UDP socket of the
// game is virtual and travels encapsulated over one real UDP socket per instance, so two copies
// on one PC (or on other machines of the LAN) see each other without real port clashes, and the
// same transport can later reach peers over the internet. BO3_NET=off restores the offline mode
// (no link: the game skips system link / Xbox Live). TCP sockets stay on loopback.
//
// Virtual network: each instance has a virtual IP (10.x.y.z, BO3_VIP to force one) that is its
// XNADDR / "secure" address. Instances announce themselves every second (broadcast on the LAN
// ports 47600..47615 and the same ports on localhost) and learn each other's real endpoint.
// BO3_PEERS="host:port,..." adds peers that broadcasts do not reach.
#include "guest.h"
#include "memory.h"
#include "stubs.h"
#include "write_watch.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <algorithm>

uint64_t ProfileXuid();          // xam.cpp
const char* ProfileGamertag();
std::string ConfigValue(const char* key);   // bo3recomp.ini (xam.cpp)

constexpr uint32_t WSAENETDOWN = 10050;
constexpr uint32_t kDwLobbyAddr = 0x0A445701;   // placeholder LSP address of the Demonware lobby (xam.cpp hands it out)
bool LiveSignedIn();                             // xam.cpp
void DwLobbyAccept(int fd);                      // dw_server.cpp: serves one lobby connection
void HostSetEventHandle(uint32_t handle);        // kernel_sync.cpp
void RendezvousStart();                          // rendezvous.cpp: internet peers (online mode)
constexpr uint32_t SOCKET_ERROR = 0xFFFFFFFF;

namespace
{
// ---------------------------------------------------------------------------------------
// guest socket table

struct GSock
{
    int fd = -1;           // what the guest reads / polls: the socket itself (TCP) or the app end of a datagram pair (virtual UDP)
    int feed = -1;         // virtual UDP: the end the network thread writes received packets to
    bool virt = false;
    uint16_t port = 0;     // virtual: bound port (host order, 0 = unbound)
    uint32_t peerIp = 0;   // virtual: connect()ed default destination
    uint16_t peerPort = 0;
};

std::mutex s_sockLock;
std::unordered_map<uint32_t, GSock> s_sockets;      // guest SOCKET -> host side
std::unordered_map<uint16_t, uint32_t> s_vports;    // virtual port -> guest SOCKET
uint32_t s_nextSocket = 0x100;
uint16_t s_nextEphemeral = 49152;
thread_local uint32_t t_wsaError = 0;

bool NetEnabled()
{
    static const bool on = !(getenv("BO3_NET") && (!strcmp(getenv("BO3_NET"), "off") || !strcmp(getenv("BO3_NET"), "0")));
    return on;
}

int HostFd(uint32_t s)
{
    std::lock_guard lock(s_sockLock);
    auto it = s_sockets.find(s);
    return it == s_sockets.end() ? -1 : it->second.fd;
}

uint32_t WsaFromErrno(int e)
{
    switch (e)
    {
    case EWOULDBLOCK: return 10035;      // WSAEWOULDBLOCK
    case EINPROGRESS: return 10036;
    case EBADF: return 10038;            // WSAENOTSOCK
    case EMSGSIZE: return 10040;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETUNREACH: return 10051;
    case ECONNRESET: return 10054;
    case ENOTCONN: return 10057;
    case ECONNREFUSED: return 10061;
    case EINVAL: return 10022;
    default: return WSAENETDOWN;
    }
}

uint32_t Fail(int e = errno) { t_wsaError = WsaFromErrno(e); return SOCKET_ERROR; }

// Guest sockaddr_in: family is a big-endian u16; port and address are network order already.
bool ToHostAddr(uint32_t g, uint32_t len, sockaddr_in& out)
{
    if (!g || len < 8) return false;
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    memcpy(&out.sin_port, g_base + g + 2, 2);
    memcpy(&out.sin_addr, g_base + g + 4, 4);
    return true;
}

void ToGuestAddr(const sockaddr_in& a, uint32_t g, uint32_t lenPtr)
{
    if (!g) return;
    memset(g_base + g, 0, 16);
    GuestWrite16(g, 2);   // AF_INET
    memcpy(g_base + g + 2, &a.sin_port, 2);
    memcpy(g_base + g + 4, &a.sin_addr, 4);
    if (lenPtr) GuestWrite32(lenPtr, 16);
}

// ---------------------------------------------------------------------------------------
// virtual network transport

constexpr uint32_t kMagic = 0x42335654;   // "B3VT"
constexpr uint16_t kBasePort = 47600, kPortCount = 16;
enum : uint8_t { PKT_HELLO = 1, PKT_DATA = 2, PKT_SESSION = 3, PKT_QOS = 4 };
#pragma pack(push, 1)
struct PktHeader
{
    uint32_t magic;
    uint8_t type, version;
    uint16_t srcPort;    // virtual (network order)
    uint32_t srcIp;      // virtual (network order)
    uint32_t dstIp;
    uint16_t dstPort;
    uint16_t reserved;
};
struct HelloBody { uint64_t xuid; char gamertag[16]; };
#pragma pack(pop)

struct Peer
{
    sockaddr_in addr;
    std::chrono::steady_clock::time_point seen;
    std::string name;
    std::string session;                                  // its advertised online matchmaking session (PKT_SESSION)
    std::chrono::steady_clock::time_point sessionSeen;
    std::string qos;                                      // its QoS reply data (XNetQosListen, PKT_QOS)
    std::chrono::steady_clock::time_point qosSeen;
};

std::mutex s_netLock;
int s_host = -1;              // the real UDP socket
uint16_t s_hostPort = 0;
uint32_t s_vip = 0;           // our virtual IP (host order)
std::unordered_map<uint32_t, Peer> s_peers;   // virtual IP -> real endpoint
std::vector<sockaddr_in> s_extraPeers;        // BO3_PEERS
std::vector<sockaddr_in> s_rvPeers;           // online players found through the rendezvous (rendezvous.cpp)
// STUN (our public address as the internet sees the vnet socket): the reply arrives on the net thread
std::mutex s_stunLock;
std::condition_variable s_stunCv;
uint8_t s_stunTid[12];
uint32_t s_stunIp = 0;      // host order, 0 = no answer yet
uint16_t s_stunPort = 0;
std::atomic<uint64_t> s_txPkts{ 0 }, s_rxPkts{ 0 }, s_txBytes{ 0 }, s_rxBytes{ 0 };
std::string s_session;        // the matchmaking session we host (dw_server.cpp), announced to every peer
std::string s_qosData;        // our QoS reply data (XNetQosListen): what a peer's QoS probe of us returns
bool s_qosOn = false;

uint32_t VirtualIp()
{
    return s_vip;
}

void SendHello(const sockaddr_in& to)
{
    uint8_t buf[sizeof(PktHeader) + sizeof(HelloBody)] = {};
    PktHeader h{ kMagic, PKT_HELLO, 1, 0, htonl(s_vip), 0, 0, 0 };
    HelloBody b{};
    b.xuid = ProfileXuid();
    strncpy(b.gamertag, ProfileGamertag(), sizeof(b.gamertag) - 1);
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &b, sizeof(b));
    sendto(s_host, buf, sizeof(buf), MSG_NOSIGNAL, (const sockaddr*)&to, sizeof(to));
}

void Announce()
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    for (uint16_t p = kBasePort; p < kBasePort + kPortCount; p++)
    {
        a.sin_port = htons(p);
        static const bool noLan = getenv("BO3_NET_NOLAN") && *getenv("BO3_NET_NOLAN") == '1';   // testing the internet path
        if (noLan) break;
        a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        SendHello(a);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (p != s_hostPort) SendHello(a);
    }
    for (auto& e : s_extraPeers) SendHello(e);
    // online players from the rendezvous: our hellos and theirs open both NATs (UDP hole punching)
    for (auto& e : s_rvPeers) SendHello(e);
    // and every peer learned from its packets (behind NAT, e.g. a phone on another router: our
    // hellos keep its mapping open and keep it from timing us out)
    for (auto& [ip, p] : s_peers) SendHello(p.addr);
    // our online matchmaking session (public match host): the other instances' Demonware
    // servers list it in their session searches
    if (!s_session.empty())
    {
        std::vector<uint8_t> pkt(sizeof(PktHeader) + s_session.size());
        PktHeader h{ kMagic, PKT_SESSION, 1, 0, htonl(s_vip), 0, 0, 0 };
        memcpy(pkt.data(), &h, sizeof(h));
        memcpy(pkt.data() + sizeof(h), s_session.data(), s_session.size());
        for (auto& e : s_extraPeers) sendto(s_host, pkt.data(), pkt.size(), MSG_NOSIGNAL, (const sockaddr*)&e, sizeof(e));
        for (auto& [ip, p] : s_peers) sendto(s_host, pkt.data(), pkt.size(), MSG_NOSIGNAL, (const sockaddr*)&p.addr, sizeof(p.addr));
    }
    // and our QoS reply data (lobby state: free slots, match ending...): what the peers' QoS
    // probes of us (XNetQosLookup) return
    if (s_qosOn)
    {
        std::vector<uint8_t> pkt(sizeof(PktHeader) + s_qosData.size());
        PktHeader h{ kMagic, PKT_QOS, 1, 0, htonl(s_vip), 0, 0, 0 };
        memcpy(pkt.data(), &h, sizeof(h));
        if (!s_qosData.empty()) memcpy(pkt.data() + sizeof(h), s_qosData.data(), s_qosData.size());
        for (auto& e : s_extraPeers) sendto(s_host, pkt.data(), pkt.size(), MSG_NOSIGNAL, (const sockaddr*)&e, sizeof(e));
        for (auto& [ip, p] : s_peers) sendto(s_host, pkt.data(), pkt.size(), MSG_NOSIGNAL, (const sockaddr*)&p.addr, sizeof(p.addr));
    }
}

void Deliver(uint32_t srcIp, uint16_t srcPort, uint16_t dstPort, const uint8_t* data, size_t n)
{
    int feed = -1;
    {
        std::lock_guard lock(s_sockLock);
        auto it = s_vports.find(dstPort);
        if (it != s_vports.end()) { auto st = s_sockets.find(it->second); if (st != s_sockets.end()) feed = st->second.feed; }
    }
    static const bool trace = getenv("BO3_NET_TRACE") != nullptr;
    if (trace) Log("net: rx %u bytes %08X:%u -> port %u%s", unsigned(n), srcIp, srcPort, dstPort, feed < 0 ? " (no socket: dropped)" : "");
    if (feed < 0) return;
    uint8_t hdr[6];
    uint32_t ip = htonl(srcIp);
    uint16_t pt = htons(srcPort);
    memcpy(hdr, &ip, 4);
    memcpy(hdr + 4, &pt, 2);
    iovec iov[2] = { { hdr, 6 }, { (void*)data, n } };
    msghdr m{};
    m.msg_iov = iov;
    m.msg_iovlen = 2;
    sendmsg(feed, &m, MSG_DONTWAIT | MSG_NOSIGNAL);   // full receive queue: dropped, like UDP
}

void NetThread()
{
    pthread_setname_np(pthread_self(), "vnet");
    std::vector<uint8_t> buf(65536);
    auto lastAnnounce = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    for (;;)
    {
        auto now = std::chrono::steady_clock::now();
        if (now - lastAnnounce >= std::chrono::seconds(1))
        {
            lastAnnounce = now;
            static int tick = 0;
            if (++tick % 5 == 0 && (s_txPkts || s_rxPkts))
                Log("net: game traffic out %llu pkts / %llu KB, in %llu pkts / %llu KB (last 5 s)", (unsigned long long)s_txPkts.exchange(0),
                    (unsigned long long)s_txBytes.exchange(0) / 1024, (unsigned long long)s_rxPkts.exchange(0), (unsigned long long)s_rxBytes.exchange(0) / 1024);
            std::lock_guard lock(s_netLock);
            Announce();
            for (auto it = s_peers.begin(); it != s_peers.end();)
                if (now - it->second.seen > std::chrono::seconds(10)) { Log("net: peer %s (%s) gone", inet_ntoa(in_addr{ htonl(it->first) }), it->second.name.c_str()); it = s_peers.erase(it); }
                else ++it;
        }
        pollfd pf{ s_host, POLLIN, 0 };
        if (poll(&pf, 1, 200) <= 0) continue;
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(s_host, buf.data(), buf.size(), 0, (sockaddr*)&from, &fl);
        // STUN binding response (rendezvous.cpp asked for our public address)
        if (n >= 20 && buf[0] == 0x01 && buf[1] == 0x01 && buf[4] == 0x21 && buf[5] == 0x12 && buf[6] == 0xA4 && buf[7] == 0x42)
        {
            std::lock_guard lock(s_stunLock);
            if (memcmp(buf.data() + 8, s_stunTid, 12) == 0)
                for (size_t p = 20; p + 4 <= size_t(n);)
                {
                    uint16_t t = uint16_t(buf[p] << 8 | buf[p + 1]), l = uint16_t(buf[p + 2] << 8 | buf[p + 3]);
                    if (p + 4 + l > size_t(n)) break;
                    if ((t == 0x0020 || t == 0x0001) && l >= 8 && buf[p + 5] == 1)   // (XOR-)MAPPED-ADDRESS, IPv4
                    {
                        uint16_t port = uint16_t(buf[p + 6] << 8 | buf[p + 7]);
                        uint32_t ip = uint32_t(buf[p + 8]) << 24 | uint32_t(buf[p + 9]) << 16 | uint32_t(buf[p + 10]) << 8 | buf[p + 11];
                        if (t == 0x0020) { port ^= 0x2112; ip ^= 0x2112A442u; }
                        s_stunIp = ip;
                        s_stunPort = port;
                        s_stunCv.notify_all();
                        if (t == 0x0020) break;
                    }
                    p += 4 + ((l + 3u) & ~3u);
                }
            continue;
        }
        if (n < ssize_t(sizeof(PktHeader))) continue;
        PktHeader h;
        memcpy(&h, buf.data(), sizeof(h));
        if (h.magic != kMagic) continue;
        uint32_t src = ntohl(h.srcIp);
        if (src == s_vip || !src) continue;   // our own broadcast
        {
            std::lock_guard lock(s_netLock);
            auto it = s_peers.find(src);
            bool isNew = it == s_peers.end();
            Peer& p = s_peers[src];
            p.addr = from;
            p.seen = std::chrono::steady_clock::now();
            if (h.type == PKT_HELLO && size_t(n) >= sizeof(PktHeader) + sizeof(HelloBody))
            {
                HelloBody b;
                memcpy(&b, buf.data() + sizeof(PktHeader), sizeof(b));
                b.gamertag[15] = 0;
                p.name = b.gamertag;
            }
            if (h.type == PKT_SESSION)
            {
                std::string sess(reinterpret_cast<const char*>(buf.data()) + sizeof(PktHeader), size_t(n) - sizeof(PktHeader));
                if (sess != p.session) Log("net: peer \"%s\" %s a public match", p.name.c_str(), sess.empty() ? "stopped hosting" : "hosts");
                p.session = std::move(sess);
                p.sessionSeen = p.seen;
            }
            if (h.type == PKT_QOS)
            {
                p.qos.assign(reinterpret_cast<const char*>(buf.data()) + sizeof(PktHeader), size_t(n) - sizeof(PktHeader));
                p.qosSeen = p.seen;
            }
            if (isNew)
            {
                { char va[20], ra[20]; inet_ntop(AF_INET, &h.srcIp, va, sizeof(va)); inet_ntop(AF_INET, &from.sin_addr, ra, sizeof(ra));
                  Log("net: peer %s \"%s\" at %s:%u", va, p.name.c_str(), ra, ntohs(from.sin_port)); }
                SendHello(from);   // answer right away so the other side learns us too
            }
        }
        if (h.type == PKT_DATA)
        {
            uint32_t dst = ntohl(h.dstIp);
            if (dst == s_vip || dst == 0xFFFFFFFFu)
                s_rxPkts++, s_rxBytes += size_t(n), Deliver(src, ntohs(h.srcPort), ntohs(h.dstPort), buf.data() + sizeof(PktHeader), size_t(n) - sizeof(PktHeader));
        }
    }
}

void NetInit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (!NetEnabled()) return;
        std::random_device rd;
        s_vip = (10u << 24) | (rd() & 0x00FFFF00u) | (1u + rd() % 254u);   // last byte 1..254 (255 reads as broadcast)
        if (const char* v = getenv("BO3_VIP")) { in_addr a{}; if (inet_aton(v, &a)) s_vip = ntohl(a.s_addr); }
        s_host = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(s_host, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        int big = 4 << 20;
        setsockopt(s_host, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
        setsockopt(s_host, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
        uint16_t want = getenv("BO3_NET_PORT") ? uint16_t(atoi(getenv("BO3_NET_PORT"))) : 0;
        for (uint16_t p = want ? want : kBasePort; p < (want ? want + 1 : kBasePort + kPortCount); p++)
        {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(p);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(s_host, (sockaddr*)&a, sizeof(a)) == 0) { s_hostPort = p; break; }
        }
        if (!s_hostPort) { LogWarn("net: no free UDP port in %u..%u - network off", kBasePort, kBasePort + kPortCount - 1); close(s_host); s_host = -1; return; }
        std::string peerList = getenv("BO3_PEERS") ? getenv("BO3_PEERS") : ConfigValue("peers");
        if (!peerList.empty())
        {
            std::string all = peerList;
            size_t pos = 0;
            while (pos < all.size())
            {
                size_t e = all.find(',', pos);
                if (e == std::string::npos) e = all.size();
                std::string item = all.substr(pos, e - pos);
                pos = e + 1;
                std::string host = item, port = std::to_string(kBasePort);
                if (size_t c = item.rfind(':'); c != std::string::npos) { host = item.substr(0, c); port = item.substr(c + 1); }
                addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0 && res)
                {
                    s_extraPeers.push_back(*(sockaddr_in*)res->ai_addr);
                    freeaddrinfo(res);
                }
                else LogWarn("net: cannot resolve peer %s", item.c_str());
            }
        }
        Log("net: system link on - virtual IP %s, UDP port %u", inet_ntoa(in_addr{ htonl(s_vip) }), s_hostPort);
        std::thread(NetThread).detach();
        RendezvousStart();
    });
}

// Sends one datagram of a virtual socket (dst: host order IP; 255.255.255.255 = every peer).
ssize_t VirtualSend(uint16_t srcPort, uint32_t dstIp, uint16_t dstPort, const uint8_t* data, size_t n)
{
    if (s_host < 0) { errno = ENETUNREACH; return -1; }
    if (n > 65000) { errno = EMSGSIZE; return -1; }
    if (dstIp == s_vip || (dstIp >> 24) == 127)
    {
        Deliver(s_vip, srcPort, dstPort, data, n);
        return ssize_t(n);
    }
    s_txPkts++;
    s_txBytes += n;
    if (static const bool trace = getenv("BO3_NET_TRACE") != nullptr; trace)
        Log("net: tx %u bytes port %u -> %08X:%u", unsigned(n), srcPort, dstIp, dstPort);
    std::vector<uint8_t> pkt(sizeof(PktHeader) + n);
    PktHeader h{ kMagic, PKT_DATA, 1, htons(srcPort), htonl(s_vip), htonl(dstIp), htons(dstPort), 0 };
    memcpy(pkt.data(), &h, sizeof(h));
    memcpy(pkt.data() + sizeof(h), data, n);
    std::lock_guard lock(s_netLock);
    if (dstIp == 0xFFFFFFFFu || (dstIp & 0xFF) == 0xFF)
    {
        for (auto& [ip, p] : s_peers) sendto(s_host, pkt.data(), pkt.size(), MSG_NOSIGNAL, (sockaddr*)&p.addr, sizeof(p.addr));
        return ssize_t(n);
    }
    auto it = s_peers.find(dstIp);
    if (it == s_peers.end()) return ssize_t(n);   // unknown peer: lost like a UDP packet
    sendto(s_host, pkt.data(), pkt.size(), MSG_NOSIGNAL, (sockaddr*)&it->second.addr, sizeof(it->second.addr));
    return ssize_t(n);
}

// binds a virtual socket (port 0 = ephemeral); call with s_sockLock held
bool VirtualBind(uint32_t s, GSock& g, uint16_t port)
{
    if (!port)
    {
        for (int tries = 0; tries < 16384; tries++)
        {
            uint16_t p = s_nextEphemeral++;
            if (s_nextEphemeral < 49152) s_nextEphemeral = 49152;
            if (!s_vports.count(p)) { port = p; break; }
        }
        if (!port) return false;
    }
    else if (s_vports.count(port)) return false;
    if (g.port) s_vports.erase(g.port);
    g.port = port;
    s_vports[port] = s;
    return true;
}
}

GUEST_IMPORT(NetDll_WSAGetLastError) { RETURN(t_wsaError); }

// socket(caller, af, type, protocol). UDP and VDP (protocol 254, Xbox voice/data) become virtual
// sockets when the network is on; TCP stays a host socket on loopback.
GUEST_IMPORT(NetDll_socket)
{
    NetInit();
    GSock g;
    if (ARG(5) != 1 && NetEnabled() && s_host >= 0)
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) < 0) RETURN(Fail());
        int big = 1 << 20;
        setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
        setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
        g.fd = sv[0]; g.feed = sv[1]; g.virt = true;
    }
    else
    {
        int fd = socket(AF_INET, (ARG(5) == 1 ? SOCK_STREAM : SOCK_DGRAM) | SOCK_CLOEXEC, 0);
        if (fd < 0) RETURN(Fail());
        g.fd = fd;
    }
    std::lock_guard lock(s_sockLock);
    uint32_t s = s_nextSocket++;
    s_sockets[s] = g;
    Log("socket(af=%u, type=%u, proto=%u) -> 0x%X%s", ARG(4), ARG(5), ARG(6), s, g.virt ? " (virtual)" : "");
    RETURN(s);
}

GUEST_IMPORT(NetDll_closesocket)
{
    std::lock_guard lock(s_sockLock);
    auto it = s_sockets.find(ARG(4));
    if (it == s_sockets.end()) RETURN(Fail(EBADF));
    if (it->second.port) s_vports.erase(it->second.port);
    close(it->second.fd);
    if (it->second.feed >= 0) close(it->second.feed);
    s_sockets.erase(it);
    RETURN(0);
}

// setsockopt(caller, s, level, optname, optval, optlen): accept everything, apply the few
// options that matter on the host.
GUEST_IMPORT(NetDll_setsockopt)
{
    int fd = HostFd(ARG(4));
    if (fd < 0) RETURN(Fail(EBADF));
    uint32_t level = ARG(5), opt = ARG(6), val = ARG(8) >= 4 ? GuestRead32(ARG(7)) : (ARG(8) ? GuestRead8(ARG(7)) : 0);
    int v = int(val);
    if (level == 0xFFFF)
    {
        if (opt == 0x0004) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
        else if (opt == 0x0020) setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &v, sizeof(v));
        else if (opt == 0x1001) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
        else if (opt == 0x1002) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    }
    RETURN(0);
}

// ioctlsocket(caller, s, cmd, argp)
GUEST_IMPORT(NetDll_ioctlsocket)
{
    int fd = HostFd(ARG(4));
    if (fd < 0) RETURN(Fail(EBADF));
    if (ARG(5) == 0x8004667E)   // FIONBIO
    {
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, GuestRead32(ARG(6)) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
        RETURN(0);
    }
    if (ARG(5) == 0x4004667F)   // FIONREAD: bytes of the next datagram (minus our address header)
    {
        int n = 0;
        ioctl(fd, FIONREAD, &n);
        bool virt;
        { std::lock_guard lock(s_sockLock); virt = s_sockets[ARG(4)].virt; }
        if (virt) n = n > 6 ? n - 6 : 0;
        GuestWrite32(ARG(6), uint32_t(n));
        RETURN(0);
    }
    LogWarn("ioctlsocket: cmd 0x%X ignored", ARG(5));
    RETURN(0);
}

// bind(caller, s, addr, len): virtual sockets take the virtual port; host sockets go to loopback
GUEST_IMPORT(NetDll_bind)
{
    sockaddr_in a;
    if (!ToHostAddr(ARG(5), ARG(6), a)) RETURN(Fail(EINVAL));
    {
        std::lock_guard lock(s_sockLock);
        auto it = s_sockets.find(ARG(4));
        if (it == s_sockets.end()) RETURN(Fail(EBADF));
        if (it->second.virt)
        {
            if (!VirtualBind(ARG(4), it->second, ntohs(a.sin_port))) RETURN(Fail(EADDRINUSE));
            Log("bind(0x%X) -> virtual port %u", ARG(4), it->second.port);
            RETURN(0);
        }
    }
    int fd = HostFd(ARG(4));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0)
    {
        // port taken (e.g. a second instance): fall back to any port
        a.sin_port = 0;
        if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) RETURN(Fail());
    }
    Log("bind(0x%X) -> 127.0.0.1:%u", ARG(4), ntohs(a.sin_port));
    RETURN(0);
}

// getsockname(caller, s, addr, len*)
GUEST_IMPORT(NetDll_getsockname)
{
    {
        std::lock_guard lock(s_sockLock);
        auto it = s_sockets.find(ARG(4));
        if (it == s_sockets.end()) RETURN(Fail(EBADF));
        if (it->second.virt)
        {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(it->second.port);
            a.sin_addr.s_addr = htonl(VirtualIp());
            ToGuestAddr(a, ARG(5), ARG(6));
            RETURN(0);
        }
    }
    int fd = HostFd(ARG(4));
    sockaddr_in a{};
    socklen_t l = sizeof(a);
    if (fd < 0 || getsockname(fd, (sockaddr*)&a, &l) < 0) RETURN(Fail());
    ToGuestAddr(a, ARG(5), ARG(6));
    RETURN(0);
}

namespace
{
// virtual send: (s, buf, len, dest or null for the connect()ed peer)
uint32_t SendVirtual(uint32_t s, uint32_t buf, uint32_t len, const sockaddr_in* to)
{
    uint16_t src;
    uint32_t dip;
    uint16_t dport;
    {
        std::lock_guard lock(s_sockLock);
        auto it = s_sockets.find(s);
        if (it == s_sockets.end()) return Fail(EBADF);
        if (!it->second.port && !VirtualBind(s, it->second, 0)) return Fail(EADDRINUSE);   // implicit bind
        src = it->second.port;
        if (to) { dip = ntohl(to->sin_addr.s_addr); dport = ntohs(to->sin_port); }
        else { dip = it->second.peerIp; dport = it->second.peerPort; if (!dport) return Fail(ENOTCONN); }
    }
    ssize_t r = VirtualSend(src, dip, dport, g_base + buf, len);
    return r < 0 ? Fail() : uint32_t(r);
}

// virtual receive into guest memory: the network thread prefixed each datagram with its source
uint32_t RecvVirtual(int fd, uint32_t buf, uint32_t len, uint32_t from, uint32_t fromLen)
{
    uint8_t hdr[6];
    WatchHostWrite hostWrite(buf, len);
    iovec iov[2] = { { hdr, 6 }, { g_base + buf, len } };
    msghdr m{};
    m.msg_iov = iov;
    m.msg_iovlen = 2;
    ssize_t r = recvmsg(fd, &m, 0);
    if (r < 0) return Fail();
    if (r < 6) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr, hdr, 4);
    memcpy(&a.sin_port, hdr + 4, 2);
    ToGuestAddr(a, from, fromLen);
    return uint32_t(r - 6);
}

bool IsVirtual(uint32_t s, int& fd)
{
    std::lock_guard lock(s_sockLock);
    auto it = s_sockets.find(s);
    if (it == s_sockets.end()) { fd = -1; return false; }
    fd = it->second.fd;
    return it->second.virt;
}
}

// sendto(caller, s, buf, len, flags, to, tolen)
GUEST_IMPORT(NetDll_sendto)
{
    int fd;
    sockaddr_in a;
    bool hasTo = ToHostAddr(ARG(8), ARG(9), a);   // (caller r3, s, buf, len, flags, to r8, tolen r9)
    if (IsVirtual(ARG(4), fd)) RETURN(SendVirtual(ARG(4), ARG(5), ARG(6), hasTo ? &a : nullptr));
    if (fd < 0) RETURN(Fail(EBADF));
    ssize_t r = hasTo ? sendto(fd, g_base + ARG(5), ARG(6), MSG_NOSIGNAL, (sockaddr*)&a, sizeof(a)) : send(fd, g_base + ARG(5), ARG(6), MSG_NOSIGNAL);
    RETURN(r < 0 ? Fail() : uint32_t(r));
}

// recvfrom(caller, s, buf, len, flags, from, fromlen*)
GUEST_IMPORT(NetDll_recvfrom)
{
    int fd;
    if (IsVirtual(ARG(4), fd)) RETURN(RecvVirtual(fd, ARG(5), ARG(6), ARG(8), ARG(9)));   // (caller, s, buf, len, flags, from r8, fromlen* r9)
    if (fd < 0) RETURN(Fail(EBADF));
    sockaddr_in a{};
    socklen_t l = sizeof(a);
    WatchHostWrite hostWrite(ARG(5), ARG(6));
    ssize_t r = recvfrom(fd, g_base + ARG(5), ARG(6), 0, (sockaddr*)&a, &l);
    if (r < 0) RETURN(Fail());
    ToGuestAddr(a, ARG(8), ARG(9));
    RETURN(uint32_t(r));
}

GUEST_IMPORT(NetDll_send)   // (caller, s, buf, len, flags)
{
    if (static int n = 0; getenv("BO3_NET_TRACE") && n++ < 6) Log("net: send(0x%X, %u bytes) from %s", ARG(4), ARG(6), GuestBacktrace(ctx, 14).c_str());
    int fd;
    if (IsVirtual(ARG(4), fd)) RETURN(SendVirtual(ARG(4), ARG(5), ARG(6), nullptr));
    if (fd < 0) RETURN(Fail(EBADF));
    ssize_t r = send(fd, g_base + ARG(5), ARG(6), MSG_NOSIGNAL);
    RETURN(r < 0 ? Fail() : uint32_t(r));
}

GUEST_IMPORT(NetDll_recv)   // (caller, s, buf, len, flags)
{
    if (static const bool tr = getenv("BO3_NET_TRACE") != nullptr; tr)
    {
        int fd0 = HostFd(ARG(4));
        pollfd pf{ fd0, POLLIN, 0 };
        static int n = 0;
        if (fd0 >= 0 && poll(&pf, 1, 0) > 0 && n++ < 6) Log("net: recv(0x%X, %u) with data from %s", ARG(4), ARG(6), GuestBacktrace(ctx, 14).c_str());
    }
    int fd;
    if (IsVirtual(ARG(4), fd)) RETURN(RecvVirtual(fd, ARG(5), ARG(6), 0, 0));
    if (fd < 0) RETURN(Fail(EBADF));
    WatchHostWrite hostWrite(ARG(5), ARG(6));
    ssize_t r = recv(fd, g_base + ARG(5), ARG(6), 0);
    RETURN(r < 0 ? Fail() : uint32_t(r));
}

// connect(caller, s, addr, len): a virtual UDP socket just remembers its default peer
GUEST_IMPORT(NetDll_connect)
{
    sockaddr_in a;
    std::lock_guard lock(s_sockLock);
    auto it = s_sockets.find(ARG(4));
    if (it == s_sockets.end()) RETURN(Fail(EBADF));
    if (it->second.virt && ToHostAddr(ARG(5), ARG(6), a))
    {
        it->second.peerIp = ntohl(a.sin_addr.s_addr);
        it->second.peerPort = ntohs(a.sin_port);
        RETURN(0);
    }
    if (ToHostAddr(ARG(5), ARG(6), a))
    {
        char b[20];
        inet_ntop(AF_INET, &a.sin_addr, b, sizeof(b));
        if (ntohl(a.sin_addr.s_addr) == kDwLobbyAddr && LiveSignedIn())
        {
            // the Demonware lobby (LSP service): a stream pair, the other end served in-process
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) RETURN(Fail());
            int fl = fcntl(it->second.fd, F_GETFL);
            close(it->second.fd);
            it->second.fd = sv[0];
            if (fl & O_NONBLOCK) fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
            Log("net: TCP connect to the Demonware lobby (%s:%u)", b, ntohs(a.sin_port));
            DwLobbyAccept(sv[1]);
            RETURN(0);
        }
        Log("net: TCP connect to %s:%u refused (no online service) lr=0x%X", b, ntohs(a.sin_port), (uint32_t)ctx.lr);
    }
    RETURN(Fail(ENETUNREACH));
}
GUEST_IMPORT(NetDll_listen) { RETURN(Fail(ENETUNREACH)); }
// (TCP connect attempts are logged above: Xbox Live / Demonware traffic would start there)
GUEST_IMPORT(NetDll_accept) { RETURN(Fail(EWOULDBLOCK)); }

// fd_set (Winsock layout): u32 count, u32 sockets[64]
namespace
{
void FdSetRead(uint32_t set, std::vector<uint32_t>& out)
{
    out.clear();
    if (!set) return;
    uint32_t n = std::min<uint32_t>(GuestRead32(set), 64);
    for (uint32_t i = 0; i < n; i++) out.push_back(GuestRead32(set + 4 + i * 4));
}
void FdSetWrite(uint32_t set, const std::vector<uint32_t>& v)
{
    if (!set) return;
    GuestWrite32(set, uint32_t(v.size()));
    for (size_t i = 0; i < v.size(); i++) GuestWrite32(set + 4 + uint32_t(i) * 4, v[i]);
}
}

// select(caller, nfds, readfds, writefds, exceptfds, timeval*)
GUEST_IMPORT(NetDll_select)
{
    std::vector<uint32_t> rd, wr, ex;
    FdSetRead(ARG(5), rd);
    FdSetRead(ARG(6), wr);
    FdSetRead(ARG(7), ex);
    std::vector<pollfd> pfds;
    std::vector<std::pair<uint32_t, int>> map;   // guest socket, kind
    auto add = [&](const std::vector<uint32_t>& v, short ev, int kind) {
        for (uint32_t s : v) { pfds.push_back({ HostFd(s), ev, 0 }); map.push_back({ s, kind }); }
    };
    add(rd, POLLIN, 0);
    add(wr, POLLOUT, 1);
    add(ex, POLLPRI, 2);
    int timeoutMs = -1;
    if (ARG(8)) timeoutMs = int(GuestRead32(ARG(8)) * 1000 + GuestRead32(ARG(8) + 4) / 1000);
    int r = poll(pfds.data(), pfds.size(), timeoutMs);
    if (r < 0) RETURN(Fail());
    std::vector<uint32_t> ord, owr, oex;
    for (size_t i = 0; i < pfds.size(); i++)
        if (pfds[i].revents)
            (map[i].second == 0 ? ord : map[i].second == 1 ? owr : oex).push_back(map[i].first);
    FdSetWrite(ARG(5), ord);
    FdSetWrite(ARG(6), owr);
    FdSetWrite(ARG(7), oex);
    RETURN(uint32_t(ord.size() + owr.size() + oex.size()));
}

GUEST_IMPORT(NetDll___WSAFDIsSet)   // (s, fd_set*)
{
    std::vector<uint32_t> v;
    FdSetRead(ARG(4), v);
    RETURN(std::find(v.begin(), v.end(), ARG(3)) != v.end() ? 1 : 0);
}

GUEST_IMPORT(NetDll_inet_addr)   // (const char*)
{
    unsigned a, b, c, d;
    if (sscanf(GuestString(ARG(3)).c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) RETURN(0xFFFFFFFF);
    RETURN((a << 24) | (b << 16) | (c << 8) | d);   // network order == guest big-endian
}

GUEST_IMPORT(NetDll_XNetStartup) { RETURN(0); }
GUEST_IMPORT(NetDll_WSAStartup)   // (caller, version, WSADATA*)
{
    if (ARG(5))
    {
        memset(g_base + ARG(5), 0, 0x190);
        GuestWrite16(ARG(5), uint16_t(ARG(4)));
        GuestWrite16(ARG(5) + 2, 0x0202);
    }
    RETURN(0);
}

// Link up while the virtual network runs (active | 100 Mbit | full duplex); otherwise no cable,
// which keeps the game in its offline modes.
GUEST_IMPORT(NetDll_XNetGetEthernetLinkStatus) { NetInit(); RETURN(NetEnabled() && s_host >= 0 ? 0x0Bu : 0u); }

namespace
{
// XNADDR: ina(4) inaOnline(4) wPortOnline(2) abEnet[6] abOnline[20]. Ours: the virtual IP, a MAC
// derived from it; the "secure" IN_ADDR of a peer is simply its virtual IP.
void WriteXnAddr(uint32_t a, uint32_t ip)
{
    memset(g_base + a, 0, 0x24);
    GuestWrite32(a + 0, ip);
    uint8_t mac[6] = { 0x00, 0x22, 0x48, uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip) };
    memcpy(g_base + a + 10, mac, 6);
}
}

uint32_t NetVirtualIp() { NetInit(); return s_host >= 0 ? VirtualIp() : 0; }   // (xam.cpp: XSESSION_INFO)

// Internet play (rendezvous.cpp). NetStun: our public IP / port (host order) as a STUN server sees
// the virtual network's UDP socket - the endpoint other players must send to.
bool NetStun(const char* server, uint16_t port, uint32_t& ip, uint16_t& pt)
{
    NetInit();
    if (s_host < 0) return false;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server, std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
    sockaddr_in to = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    uint8_t req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
    std::unique_lock lock(s_stunLock);
    std::random_device rd;
    for (auto& b : s_stunTid) b = uint8_t(rd());
    memcpy(req + 8, s_stunTid, 12);
    s_stunIp = 0;
    for (int attempt = 0; attempt < 3 && !s_stunIp; attempt++)
    {
        sendto(s_host, req, sizeof(req), MSG_NOSIGNAL, (sockaddr*)&to, sizeof(to));
        s_stunCv.wait_for(lock, std::chrono::milliseconds(700), [] { return s_stunIp != 0; });
    }
    if (!s_stunIp) return false;
    ip = s_stunIp;
    pt = s_stunPort;
    return true;
}
uint16_t NetHostPort() { NetInit(); return s_host >= 0 ? s_hostPort : 0; }
// The online players' endpoints (public and LAN, host order): hellos go to them every second.
void NetSetRendezvousPeers(const std::vector<std::pair<uint32_t, uint16_t>>& eps)
{
    std::vector<sockaddr_in> v;
    for (auto& [ip, port] : eps)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(ip);
        a.sin_port = htons(port);
        v.push_back(a);
    }
    std::lock_guard lock(s_netLock);
    s_rvPeers = std::move(v);
}

// Online matchmaking (dw_server.cpp, bdMatchMaking): the session we host (empty = none) is announced
// to every peer of the virtual network; the peers' sessions are what our session searches find.
void NetSetSession(const std::string& info)
{
    NetInit();
    std::lock_guard lock(s_netLock);
    if (!info.empty()) { if (s_session.empty()) Log("net: hosting a public match, announcing it to the peers"); }
    else if (!s_session.empty()) Log("net: public match closed");
    s_session = info;
    if (s_host < 0) return;
    if (info.empty())   // tell the peers right away
    {
        PktHeader h{ kMagic, PKT_SESSION, 1, 0, htonl(s_vip), 0, 0, 0 };
        for (auto& [ip, p] : s_peers) sendto(s_host, &h, sizeof(h), MSG_NOSIGNAL, (sockaddr*)&p.addr, sizeof(p.addr));
    }
}
std::vector<std::string> NetPeerSessions()
{
    std::vector<std::string> out;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(s_netLock);
    for (auto& [ip, p] : s_peers)
        if (!p.session.empty() && now - p.sessionSeen < std::chrono::seconds(5)) out.push_back(p.session);
    return out;
}

// XNetGetTitleXnAddr(caller, XNADDR*). BO3 asserts ("Unable to find an XNADDR.",
// xenon_net.cpp:964) if the NONE bit is set, so offline it reports a static loopback address.
GUEST_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    NetInit();
    if (ARG(4)) WriteXnAddr(ARG(4), s_host >= 0 ? VirtualIp() : 0x7F000001u);
    RETURN(0x00000006);   // XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_STATIC
}

// XNetXnAddrToInAddr(caller, XNADDR*, XNKID*, IN_ADDR*)
GUEST_IMPORT(NetDll_XNetXnAddrToInAddr)
{
    if (!ARG(4) || !ARG(6)) RETURN(10022);
    GuestWrite32(ARG(6), GuestRead32(ARG(4)));
    RETURN(0);
}

// Registered session keys (XNetRegisterKey): a secure address belongs to the session whose key it
// was made with - system link has one at a time, the latest.
namespace { std::mutex s_keyLock; std::vector<uint64_t> s_keys; }

// XNetInAddrToXnAddr(caller, IN_ADDR, XNADDR*, XNKID*)
GUEST_IMPORT(NetDll_XNetInAddrToXnAddr)
{
    if (ARG(5)) WriteXnAddr(ARG(5), ARG(4));
    if (ARG(6))
    {
        std::lock_guard lock(s_keyLock);
        GuestWrite64(ARG(6), s_keys.empty() ? 0 : s_keys.back());
    }
    RETURN(0);
}

// XNetInAddrToString(caller, IN_ADDR, char*, size)
GUEST_IMPORT(NetDll_XNetInAddrToString)
{
    uint32_t ip = ARG(4);
    char b[20];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    if (ARG(5) && ARG(6)) { uint32_t n = std::min<uint32_t>(ARG(6) - 1, uint32_t(strlen(b))); memcpy(g_base + ARG(5), b, n); GuestWrite8(ARG(5) + n, 0); }
    RETURN(0);
}

// XNetXnAddrToMachineId(caller, XNADDR*, u64*)
GUEST_IMPORT(NetDll_XNetXnAddrToMachineId)
{
    if (!ARG(4) || !ARG(5)) RETURN(10022);
    GuestWrite64(ARG(5), 0xFA00000000000000ull | GuestRead32(ARG(4)));
    RETURN(0);
}

GUEST_IMPORT(NetDll_XNetRegisterKey)   // (caller, XNKID*, XNKEY*)
{
    if (!ARG(4)) RETURN(10022);
    uint64_t id = GuestRead64(ARG(4));
    std::lock_guard lock(s_keyLock);
    s_keys.erase(std::remove(s_keys.begin(), s_keys.end(), id), s_keys.end());
    s_keys.push_back(id);
    Log("net: XNetRegisterKey %016llX", (unsigned long long)id);
    RETURN(0);
}
GUEST_IMPORT(NetDll_XNetUnregisterKey)   // (caller, XNKID*)
{
    if (!ARG(4)) RETURN(10022);
    uint64_t id = GuestRead64(ARG(4));
    std::lock_guard lock(s_keyLock);
    s_keys.erase(std::remove(s_keys.begin(), s_keys.end(), id), s_keys.end());
    RETURN(0);
}
GUEST_IMPORT(NetDll_XNetUnregisterInAddr) { RETURN(0); }     // (caller, IN_ADDR)
GUEST_IMPORT(NetDll_XNetConnect) { RETURN(0); }              // (caller, IN_ADDR)
GUEST_IMPORT(NetDll_XNetGetConnectStatus) { RETURN(2); }     // XNET_CONNECT_STATUS_CONNECTED
// XNetServerToInAddr(caller, server IN_ADDR, service id, secure IN_ADDR*): LSP servers. The only
// one is the in-process Demonware lobby (online mode); its secure address is itself.
GUEST_IMPORT(NetDll_XNetServerToInAddr)
{
    Log("net: XNetServerToInAddr(0x%08X, service 0x%X)", ARG(4), ARG(5));
    if (ARG(4) != kDwLobbyAddr || !LiveSignedIn()) RETURN(WSAENETDOWN);
    if (ARG(6)) GuestWrite32(ARG(6), kDwLobbyAddr);
    RETURN(0);
}
GUEST_IMPORT(NetDll_XNetTsAddrToInAddr) { RETURN(WSAENETDOWN); }

GUEST_IMPORT(NetDll_XnpLogonGetStatus) { static int n = 0; if (n++ < 3) Log("net: XnpLogonGetStatus lr=0x%X", (uint32_t)ctx.lr); RETURN(0); }
GUEST_IMPORT(XNetLogonGetTitleID) { RETURN(0x4156091D); }
GUEST_IMPORT(XNetLogonGetMachineID) { NetInit(); if (ARG(3)) GuestWrite64(ARG(3), 0xFA00000000000000ull | (s_host >= 0 ? VirtualIp() : 1u)); RETURN(0); }
GUEST_IMPORT(NetDll_XNetRandom)   // (caller, buffer, length)
{
    static std::mt19937 rng{ std::random_device{}() };
    for (uint32_t i = 0; i < ARG(5); i++) GuestWrite8(ARG(4) + i, uint8_t(rng()));
    RETURN(0);
}

// QoS. A host publishes its reply data with XNetQosListen; the virtual network announces it to the
// peers (PKT_QOS), and XNetQosLookup answers a probe of a peer with what it last announced (the
// public match search QoS-probes every host it finds before joining).
// XNetQosListen(caller, XNKID*, data, size, bits per second, flags)
GUEST_IMPORT(NetDll_XNetQosListen)
{
    enum { ENABLE = 1, DISABLE = 2, SET_DATA = 4, RELEASE = 16 };
    uint32_t flags = ARG(8);
    NetInit();
    std::lock_guard lock(s_netLock);
    if (flags & SET_DATA) s_qosData.assign(reinterpret_cast<const char*>(g_base + ARG(5)), ARG(5) ? ARG(6) : 0);
    if (flags & ENABLE) s_qosOn = true;
    if (flags & (DISABLE | RELEASE)) s_qosOn = false;
    RETURN(0);
}
// XNetQosLookup(caller, count, XNADDR*[], XNKID*[], XNKEY*[], server count, IN_ADDR[], service ids[],
//               probes, bits per second, flags, event, XNQOS** result)
GUEST_IMPORT(NetDll_XNetQosLookup)
{
    uint32_t cxna = ARG(4), apxna = ARG(5), cina = ARG(8);
    uint32_t hEvent = STACK_ARG(11), out = STACK_ARG(12);
    if (!out) RETURN(10022);
    NetInit();
    uint32_t total = cxna + cina;
    // XNQOS: count, pending, XNQOSINFO[total] {flags, reserved, probes sent u16, probes received u16,
    // data size u16, data*, rtt min u16, rtt median u16, up bps, down bps} (24 bytes each)
    std::vector<std::string> data(total);
    std::vector<bool> reached(total, false);
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(s_netLock);
        for (uint32_t i = 0; i < cxna; i++)
        {
            uint32_t xna = GuestRead32(apxna + i * 4);
            uint32_t ip = xna ? GuestRead32(xna) : 0;
            auto it = s_peers.find(ip);
            if (ip == s_vip) { reached[i] = true; data[i] = s_qosData; }
            else if (it != s_peers.end() && now - it->second.seen < std::chrono::seconds(5))
            {
                reached[i] = true;
                if (now - it->second.qosSeen < std::chrono::seconds(5)) data[i] = it->second.qos;
            }
        }
    }
    uint32_t size = 8 + 24 * std::max(total, 1u);
    for (auto& d : data) size += uint32_t(d.size() + 3) & ~3u;
    uint32_t q = GuestAlloc(size);
    if (!q) RETURN(10055);
    memset(g_base + q, 0, size);
    GuestWrite32(q + 0, total);
    GuestWrite32(q + 4, 0);
    uint32_t dataAt = q + 8 + 24 * std::max(total, 1u);
    for (uint32_t i = 0; i < total; i++)
    {
        uint32_t e = q + 8 + 24 * i;
        enum { COMPLETE = 1, CONTACTED = 2, DISABLED = 4, DATA = 8 };
        if (!reached[i]) { GuestWrite8(e, COMPLETE | DISABLED); continue; }
        GuestWrite8(e, uint8_t(COMPLETE | CONTACTED | (data[i].empty() ? 0 : DATA)));
        GuestWrite16(e + 2, 4);
        GuestWrite16(e + 4, 4);
        GuestWrite16(e + 6, uint16_t(data[i].size()));
        if (!data[i].empty())
        {
            memcpy(g_base + dataAt, data[i].data(), data[i].size());
            GuestWrite32(e + 8, dataAt);
            dataAt += uint32_t(data[i].size() + 3) & ~3u;
        }
        GuestWrite16(e + 12, 20);              // rtt: a LAN-like 20 ms
        GuestWrite16(e + 14, 25);
        GuestWrite32(e + 16, 20000000);
        GuestWrite32(e + 20, 20000000);
    }
    GuestWrite32(out, q);
    if (hEvent) HostSetEventHandle(hEvent);
    static int logs = 0;
    if (logs++ < 20) Log("net: QoS lookup of %u host(s): %u reachable", cxna, uint32_t(std::count(reached.begin(), reached.begin() + cxna, true)));
    RETURN(0);
}
GUEST_IMPORT(NetDll_XNetQosServiceLookup) { RETURN(WSAENETDOWN); }
GUEST_IMPORT(NetDll_XNetQosRelease) { if (ARG(4)) GuestFree(ARG(4)); RETURN(0); }   // (caller, XNQOS*)
// XAuth: tokens for third-party services (Demonware's login sends one). Online mode only.
// XampXAuthStartup(params {buffer, size}): the game hands over a 500 KB buffer; on shutdown it
// frees whatever XampXAuthGetTitleBuffer returns (when XampXAuthShutdown reports 0) - returning
// nothing leaked it on every HTTP restart until the game ran out of memory and relaunched itself.
static uint32_t s_xauthBuffer = 0;
GUEST_IMPORT(XampXAuthStartup)
{
    if (!LiveSignedIn()) RETURN(0x80004005u);
    s_xauthBuffer = ARG(3) ? GuestRead32(ARG(3)) : 0;
    RETURN(0);
}
GUEST_IMPORT(XampXAuthShutdown) { if (ARG(3)) GuestWrite32(ARG(3), 0); RETURN(0); }
GUEST_IMPORT(XampXAuthGetTitleBuffer) { uint32_t b = s_xauthBuffer; s_xauthBuffer = 0; RETURN(b); }
GUEST_IMPORT(XamGetToken) { Log("xauth: XamGetToken(0x%X, 0x%X, 0x%X, 0x%X, 0x%X, 0x%X) lr=0x%X", ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8), (uint32_t)ctx.lr); RETURN(0x80004005u); }
GUEST_IMPORT(XamFreeToken) { RETURN(0); }
