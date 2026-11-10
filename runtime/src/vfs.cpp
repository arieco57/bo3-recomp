#include "vfs.h"
#include "log.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct Mount { std::string prefix; std::vector<std::string> roots; bool writable; };
std::vector<Mount> s_mounts;
std::string s_userDir;
std::mutex s_cacheLock;
std::map<std::string, std::vector<std::string>> s_dirCache;   // host dir -> entries
std::map<std::string, std::string> s_links;                    // lower-case link -> target

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Finds `name` inside host directory `dir`, ignoring case.
std::optional<std::string> FindCI(const std::string& dir, const std::string& name)
{
    fs::path exact = fs::path(dir) / name;
    std::error_code ec;
    if (fs::exists(exact, ec)) return exact.string();
    std::lock_guard lock(s_cacheLock);
    auto it = s_dirCache.find(dir);
    if (it == s_dirCache.end())
    {
        std::vector<std::string> entries;
        for (auto& e : fs::directory_iterator(dir, ec)) entries.push_back(e.path().filename().string());
        it = s_dirCache.emplace(dir, std::move(entries)).first;
    }
    std::string ln = Lower(name);
    for (auto& e : it->second)
        if (Lower(e) == ln) return (fs::path(dir) / e).string();
    return std::nullopt;
}
}

std::string VfsUserDir() { return s_userDir; }

static std::string StripDosPrefix(std::string p)
{
    std::replace(p.begin(), p.end(), '/', '\\');
    if (p.rfind("\\??\\", 0) == 0) p = p.substr(4);
    while (!p.empty() && p.back() == '\\') p.pop_back();
    return p;
}

void VfsAddLink(const std::string& link, const std::string& target)
{
    std::lock_guard lock(s_cacheLock);
    s_links[Lower(StripDosPrefix(link))] = StripDosPrefix(target);
    Log("vfs: link %s -> %s", link.c_str(), target.c_str());
}

void VfsMount(const std::string& prefix, const std::string& hostDir, bool writable)
{
    std::lock_guard lock(s_cacheLock);
    std::string lp = Lower(prefix);
    std::erase_if(s_mounts, [&](const Mount& m) { return m.prefix == lp; });
    s_mounts.insert(s_mounts.begin(), Mount{ lp, { hostDir }, writable });
}

void VfsUnmount(const std::string& prefix)
{
    std::lock_guard lock(s_cacheLock);
    std::string lp = Lower(prefix);
    std::erase_if(s_mounts, [&](const Mount& m) { return m.prefix == lp; });
}

void VfsRemoveLink(const std::string& link)
{
    std::lock_guard lock(s_cacheLock);
    s_links.erase(Lower(StripDosPrefix(link)));
}

void VfsInit(const std::string& gameDir, const std::string& updateDir)
{
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    s_userDir = xdg ? std::string(xdg) + "/bo3recomp" : std::string(home ? home : ".") + "/.local/share/bo3recomp";
    fs::create_directories(s_userDir + "/cache");
    fs::create_directories(s_userDir + "/hdd");

    std::string game = fs::absolute(gameDir).string();
    std::string update = fs::absolute(updateDir).string();
    // The title update overrides disc files with the same name, so it is searched first.
    s_mounts = {
        { "game:", { update, game }, false },
        { "d:", { update, game }, false },
        { "\\device\\cdrom0", { update, game }, false },
        { "update:", { update }, false },
        { "cache:", { s_userDir + "/cache" }, true },
        { "\\device\\harddisk0\\cache0", { s_userDir + "/cache" }, true },
        { "\\device\\harddisk0\\partition1", { s_userDir + "/hdd" }, true },
        { "hdd:", { s_userDir + "/hdd" }, true },
    };
    if (!fs::exists(game)) LogWarn("vfs: game directory %s does not exist", game.c_str());
    Log("vfs: game=%s update=%s user=%s", game.c_str(), update.c_str(), s_userDir.c_str());
}

std::optional<VfsPath> VfsResolve(const std::string& guestPath, bool forCreate)
{
    std::string p = guestPath;
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string lp = Lower(p);
    // "\??\game:\..." style prefixes
    if (lp.rfind("\\??\\", 0) == 0) { p = p.substr(4); lp = lp.substr(4); }

    std::vector<Mount> mounts;
    {
        std::lock_guard lock(s_cacheLock);
        mounts = s_mounts;
    }
    for (auto& m : mounts)
    {
        if (lp.rfind(m.prefix, 0) != 0) continue;
        std::string rest = p.substr(m.prefix.size());
        std::vector<std::string> parts;
        size_t s = 0;
        while (s <= rest.size())
        {
            size_t e = rest.find('\\', s);
            if (e == std::string::npos) e = rest.size();
            std::string part = rest.substr(s, e - s);
            if (!part.empty() && part != ".") parts.push_back(part);
            s = e + 1;
        }
        for (auto& root : m.roots)
        {
            std::string cur = root;
            bool ok = true;
            for (size_t i = 0; i < parts.size() && ok; i++)
            {
                auto f = FindCI(cur, parts[i]);
                if (f) cur = *f;
                else if (forCreate && m.writable && i + 1 == parts.size()) cur = (fs::path(cur) / parts[i]).string();
                else ok = false;
            }
            if (!ok) continue;
            std::error_code ec;
            VfsPath r;
            r.host = cur;
            r.exists = fs::exists(cur, ec);
            r.isDir = r.exists && fs::is_directory(cur, ec);
            r.writable = m.writable;
            return r;
        }
        if (forCreate && m.writable)
        {
            VfsPath r;
            r.host = m.roots[0] + "/" + rest;
            std::replace(r.host.begin(), r.host.end(), '\\', '/');
            r.writable = true;
            return r;
        }
        return std::nullopt;
    }
    LogWarn("vfs: no mount for '%s'", guestPath.c_str());
    return std::nullopt;
}
