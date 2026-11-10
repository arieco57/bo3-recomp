// Internet play without a game server: a rendezvous in the Supabase project (table online_peers,
// tools/supabase_online.sql) plus UDP hole punching on the virtual network's socket (net.cpp).
//
// Every instance in online mode (live=1, signed in to its account) asks a public STUN server for
// the public IP / port of its vnet socket, publishes it (with its LAN address) every 10 s, and reads
// the instances seen in the last 90 s. net.cpp sends its hello beacons to all of them every second:
// when two instances send to each other both NATs open the path (most home routers keep the port
// for every destination), and from then on they are ordinary vnet peers - public match sessions,
// QoS and game traffic go exactly as on a LAN.
#include "account.h"
#include "http.h"
#include "json_mini.h"
#include "log.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace json_mini;

std::string ConfigValue(const char* key);   // bo3recomp.ini (xam.cpp)
bool LiveSignedIn();                         // xam.cpp
const char* ProfileGamertag();               // xam.cpp
uint32_t NetVirtualIp();                     // net.cpp
uint16_t NetHostPort();
bool NetStun(const char* server, uint16_t port, uint32_t& ip, uint16_t& pt);
void NetSetRendezvousPeers(const std::vector<std::pair<uint32_t, uint16_t>>& eps);

namespace
{
std::string IpText(uint32_t ip)
{
    char b[20];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}
uint32_t IpParse(const std::string& s)
{
    in_addr a{};
    return inet_aton(s.c_str(), &a) ? ntohl(a.s_addr) : 0;
}

// the LAN address the default route uses (UDP "connect" sends nothing)
uint32_t LocalIp()
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x08080808);
    uint32_t ip = 0;
    if (connect(s, (sockaddr*)&a, sizeof(a)) == 0)
    {
        socklen_t l = sizeof(a);
        if (getsockname(s, (sockaddr*)&a, &l) == 0) ip = ntohl(a.sin_addr.s_addr);
    }
    close(s);
    return ip;
}

bool PublicAddress(uint32_t& ip, uint16_t& port)
{
    static const std::pair<const char*, uint16_t> servers[] = {
        { "stun.l.google.com", 19302 }, { "stun.cloudflare.com", 3478 }, { "stun1.l.google.com", 19302 } };
    for (auto& [host, p] : servers)
        if (NetStun(host, p, ip, port)) return true;
    return false;
}

std::string IsoTime(time_t t)
{
    char b[32];
    tm g{};
    gmtime_r(&t, &g);
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

void Worker(std::string url, std::string key)
{
    pthread_setname_np(pthread_self(), "rendezvous");
    const bool noLanAddr = getenv("BO3_NET_NOLAN") && *getenv("BO3_NET_NOLAN") == '1';
    uint32_t pubIp = 0;
    uint16_t pubPort = 0;
    auto lastStun = std::chrono::steady_clock::now() - std::chrono::hours(1);
    bool tableWarned = false, stunWarned = false;
    std::set<std::string> known;
    for (;;)
    {
        std::string token = AccountToken();
        if (token.empty()) { std::this_thread::sleep_for(std::chrono::seconds(10)); continue; }
        auto now = std::chrono::steady_clock::now();
        if (!pubIp || now - lastStun > std::chrono::seconds(60))
        {
            uint32_t ip;
            uint16_t port;
            if (PublicAddress(ip, port))
            {
                if (ip != pubIp || port != pubPort) Log("rendezvous: public address %s:%u (vnet port %u)", IpText(ip).c_str(), port, NetHostPort());
                pubIp = ip;
                pubPort = port;
                lastStun = now;
            }
            else if (!stunWarned) { LogWarn("rendezvous: no STUN answer - other players can only reach us on the LAN"); stunWarned = true; }
        }
        std::vector<std::pair<std::string, std::string>> h = {
            { "apikey", key }, { "Authorization", "Bearer " + token }, { "Content-Type", "application/json" } };
        // publish ourselves
        uint32_t lan = LocalIp();
        std::string body = "{\"vip\":" + std::to_string(NetVirtualIp()) + ",\"gamertag\":\"" + JsonEscape(ProfileGamertag()) +
                           "\",\"public_ip\":\"" + (pubIp ? IpText(pubIp) : "") + "\",\"public_port\":" + std::to_string(pubPort) +
                           ",\"local_ip\":\"" + (lan && !noLanAddr ? IpText(lan) : "") + "\",\"local_port\":" + std::to_string(NetHostPort()) + "}";
        auto hp = h;
        hp.push_back({ "Prefer", "resolution=merge-duplicates,return=minimal" });
        HttpResponse r = HttpRequest("POST", url + "/rest/v1/online_peers?on_conflict=owner,vip", hp, body);
        if (r.status == 404 || (r.status >= 400 && r.body.find("online_peers") != std::string::npos))
        {
            if (!tableWarned) LogWarn("rendezvous: table online_peers missing - run tools/supabase_online.sql in the Supabase SQL editor (%d)", r.status);
            tableWarned = true;
            std::this_thread::sleep_for(std::chrono::seconds(30));
            continue;
        }
        if (r.status < 200 || r.status >= 300) LogWarn("rendezvous: publish failed (%d) %s", r.status, r.body.substr(0, 200).c_str());
        // the others seen lately
        r = HttpRequest("GET", url + "/rest/v1/online_peers?select=vip,gamertag,public_ip,public_port,local_ip,local_port&updated_at=gt." +
                                   IsoTime(time(nullptr) - 90), h, "");
        if (r.status == 200)
        {
            std::vector<std::pair<uint32_t, uint16_t>> eps;
            std::set<std::string> seen;
            for (auto& obj : JsonObjects(r.body))
            {
                uint32_t vip = uint32_t(strtoul(JsonField(obj, "vip").c_str(), nullptr, 10));
                if (!vip || vip == NetVirtualIp()) continue;
                uint32_t pip = IpParse(JsonField(obj, "public_ip")), lip = IpParse(JsonField(obj, "local_ip"));
                uint16_t pp = uint16_t(atoi(JsonField(obj, "public_port").c_str())), lp = uint16_t(atoi(JsonField(obj, "local_port").c_str()));
                if (pip && pip == pubIp && pp == pubPort) continue;   // ourselves, an older run (stale row)
                if (pip && pp) eps.push_back({ pip, pp });
                // same public IP: the other one is behind our router - its LAN address works (and
                // the public one may not: many routers do not loop back)
                if (lip && lp && (!pip || pip == pubIp)) eps.push_back({ lip, lp });
                std::string id = JsonField(obj, "gamertag") + " " + IpText(pip) + ":" + std::to_string(pp);
                seen.insert(id);
                if (!known.count(id)) Log("rendezvous: online player \"%s\" at %s:%u (LAN %s:%u)", JsonField(obj, "gamertag").c_str(), IpText(pip).c_str(), pp, IpText(lip).c_str(), lp);
            }
            known = seen;
            NetSetRendezvousPeers(eps);
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}
}

void RendezvousStart()
{
    if (!LiveSignedIn()) return;
    std::string url = ConfigValue("supabase_url"), key = ConfigValue("supabase_key");
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.empty() || key.empty()) { Log("rendezvous: off (no [online] Supabase settings) - internet players cannot find us"); return; }
    std::thread(Worker, url, key).detach();
}
