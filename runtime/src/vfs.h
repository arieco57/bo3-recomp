#pragma once
#include <optional>
#include <string>

// Guest path -> host path. Guest paths look like "game:\\zone\\all\\foo.ff",
// "d:\\...", "\\Device\\Cdrom0\\...", "update:\\..." (title update) or "cache:\\...".
// Lookups are case-insensitive (the Xbox 360 filesystem is), writes go to the writable root.
void VfsInit(const std::string& gameDir, const std::string& updateDir);

struct VfsPath
{
    std::string host;      // resolved host path (may not exist when creating)
    bool exists = false;
    bool isDir = false;
    bool writable = false; // lives under the writable root
};

std::optional<VfsPath> VfsResolve(const std::string& guestPath, bool forCreate = false);
std::string VfsUserDir();  // writable data dir (cache, saves)

// ObCreateSymbolicLink("\\??\\foo:", "\\Device\\Cdrom0"): guest paths starting with the
// link are rewritten to the target before resolution.
void VfsAddLink(const std::string& link, const std::string& target);
void VfsRemoveLink(const std::string& link);

// Dynamic device mounts (XamContentCreateEx): "dlc0:" -> host directory.
void VfsMount(const std::string& prefix, const std::string& hostDir, bool writable);
void VfsUnmount(const std::string& prefix);
