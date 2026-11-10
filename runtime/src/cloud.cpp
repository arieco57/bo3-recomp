// Sync model: every file under content/<profile>/4156091D is one row (owner = the Supabase user,
// path, data = base64, hash). A local state file remembers the hash of each path at the last
// sync, so each side can tell who changed what without trusting clocks:
//   local unchanged since the sync, remote different -> download
//   remote unchanged since the sync, local different -> upload
//   both changed, or no state yet (a new device) -> the cloud wins (local copy kept in content/backup/)
// While the game runs, changed files are uploaded every 20 s and on CloudFlush().
#include "cloud.h"
#include "http.h"
#include "log.h"
#include "json_mini.h"
#include "account.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

std::string ConfigValue(const char* key);   // bo3recomp.ini (xam.cpp)
namespace fs = std::filesystem;
using namespace json_mini;

namespace
{
struct Config { std::string url, key; bool ok = false; };
std::mutex s_lock;
Config s_cfg;
std::map<std::string, std::string> s_synced;   // path -> hash at the last sync
bool s_running = false;

std::string ProfileDir()
{
    const char* x = getenv("BO3_XUID");
    return std::string("content/") + (x ? x : "E0300000126D662A");
}
std::string Root() { return ProfileDir() + "/4156091D"; }
std::string StatePath() { return ProfileDir() + "/.cloud_sync"; }

std::string Hash(const std::string& data)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data) { h ^= c; h *= 1099511628211ull; }
    char b[20];
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return b;
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

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
std::string UnB64(const std::string& in)
{
    int T[256];
    for (int& t : T) t = -1;
    for (int i = 0; i < 64; i++) T[uint8_t(kB64[i])] = i;
    std::string out;
    uint32_t v = 0;
    int bits = 0;
    for (unsigned char c : in)
    {
        if (T[c] < 0) continue;
        v = (v << 6) | uint32_t(T[c]);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += char((v >> bits) & 0xFF); }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> Headers(bool json)
{
    std::vector<std::pair<std::string, std::string>> h = { { "apikey", s_cfg.key } };
    std::string t = AccountToken();
    if (!t.empty()) h.push_back({ "Authorization", "Bearer " + t });
    if (json) h.push_back({ "Content-Type", "application/json" });
    return h;
}

bool Login() { return !AccountToken().empty(); }

void LoadState()
{
    std::ifstream f(StatePath());
    std::string path, hash;
    while (f >> hash && std::getline(f >> std::ws, path)) s_synced[path] = hash;
}
void SaveState()
{
    std::ofstream f(StatePath(), std::ios::trunc);
    for (auto& [p, h] : s_synced) f << h << " " << p << "\n";
}

// path (relative to the profile folder) -> contents
std::map<std::string, std::string> LocalFiles()
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (!fs::exists(Root(), ec)) return out;
    for (auto it = fs::recursive_directory_iterator(Root(), ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec))
            out[fs::relative(it->path(), ProfileDir(), ec).generic_string()] = ReadFile(it->path());
    return out;
}

bool Upload(const std::map<std::string, std::string>& files)
{
    if (files.empty()) return true;
    std::string body = "[";
    for (auto& [p, d] : files)
        body += std::string(body.size() > 1 ? "," : "") + "{\"path\":\"" + JsonEscape(p) + "\",\"hash\":\"" + Hash(d) + "\",\"data\":\"" + B64(d) + "\"}";
    body += "]";
    auto h = Headers(true);
    h.push_back({ "Prefer", "resolution=merge-duplicates,return=minimal" });
    HttpResponse r = HttpRequest("POST", s_cfg.url + "/rest/v1/save_files?on_conflict=owner,path", h, body, 15000);
    if (r.status < 200 || r.status >= 300) { LogWarn("cloud: upload failed (%d) %s %s", r.status, r.error.c_str(), r.body.substr(0, 300).c_str()); return false; }
    for (auto& [p, d] : files) s_synced[p] = Hash(d);
    SaveState();
    Log("cloud: uploaded %zu file(s)", files.size());
    return true;
}

void Backup(const std::string& path)
{
    std::error_code ec;
    static const std::string stamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    fs::path dst = fs::path("content/backup") / ("cloud-" + stamp) / path;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(fs::path(ProfileDir()) / path, dst, fs::copy_options::overwrite_existing, ec);
}

void UploadChanges()
{
    if (!Login()) return;
    std::map<std::string, std::string> changed;
    for (auto& [p, d] : LocalFiles())
        if (s_synced[p] != Hash(d)) changed[p] = d;
    Upload(changed);
}

void Worker()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::lock_guard g(s_lock);
        UploadChanges();
    }
}
}

void CloudStartupSync()
{
    std::lock_guard g(s_lock);
    s_cfg.url = ConfigValue("supabase_url");
    s_cfg.key = ConfigValue("supabase_key");
    while (!s_cfg.url.empty() && s_cfg.url.back() == '/') s_cfg.url.pop_back();
    if (s_cfg.url.empty() || s_cfg.key.empty()) { Log("cloud: off (no [online] settings)"); return; }
    if (!AccountInit(s_cfg.url, s_cfg.key)) { Log("cloud: off (no online account)"); return; }
    s_cfg.ok = true;
    LoadState();
    if (!Login()) return;
    HttpResponse r = HttpRequest("GET", s_cfg.url + "/rest/v1/save_files?select=path,hash,data", Headers(false), "", 15000);
    if (r.status != 200) { LogWarn("cloud: download failed (%d) %s %s", r.status, r.error.c_str(), r.body.substr(0, 300).c_str()); return; }
    auto local = LocalFiles();
    std::map<std::string, std::string> up;
    int down = 0;
    std::map<std::string, bool> remoteSeen;
    for (auto& obj : JsonObjects(r.body))
    {
        std::string path = JsonField(obj, "path"), rh = JsonField(obj, "hash");
        if (path.empty() || path.find("..") != std::string::npos) continue;
        remoteSeen[path] = true;
        auto it = local.find(path);
        std::string lh = it == local.end() ? "" : Hash(it->second), sh = s_synced.count(path) ? s_synced[path] : "";
        if (lh == rh) { s_synced[path] = rh; continue; }
        if (!lh.empty() && sh == rh) { up[path] = it->second; continue; }   // only the local copy changed
        // remote changed (or both / first sync on this device): the cloud wins, local kept
        if (!lh.empty()) Backup(path);
        std::string data = UnB64(JsonField(obj, "data"));
        std::error_code ec;
        fs::path dst = fs::path(ProfileDir()) / path;
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream(dst, std::ios::binary | std::ios::trunc).write(data.data(), std::streamsize(data.size()));
        s_synced[path] = Hash(data);
        down++;
    }
    for (auto& [p, d] : local) if (!remoteSeen.count(p)) up[p] = d;   // not in the cloud yet
    SaveState();
    Log("cloud: %s - %d file(s) downloaded, %zu to upload", AccountUsername().c_str(), down, up.size());
    Upload(up);
    if (!s_running) { s_running = true; std::thread(Worker).detach(); }
}

void CloudFlush()
{
    if (!s_cfg.ok) return;
    std::lock_guard g(s_lock);
    UploadChanges();
}
