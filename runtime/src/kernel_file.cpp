// xboxkrnl: files and directories on top of the VFS (synchronous I/O).
#include "guest.h"
#include "kobject.h"
#include "stubs.h"
#include "vfs.h"
#include "write_watch.h"

#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{
constexpr uint32_t FILE_ATTRIBUTE_READONLY = 0x01, FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr uint32_t FILE_DIRECTORY_FILE = 0x01, FILE_NON_DIRECTORY_FILE = 0x40;
enum { FILE_SUPERSEDE, FILE_OPEN, FILE_CREATE, FILE_OPEN_IF, FILE_OVERWRITE, FILE_OVERWRITE_IF };
// IO_STATUS_BLOCK.Information for creates
enum { FILE_SUPERSEDED, FILE_OPENED, FILE_CREATED, FILE_OVERWRITTEN, FILE_EXISTS, FILE_DOES_NOT_EXIST };

struct KFile : KObject
{
    KFile() : KObject(KType::File) {}
    ~KFile() override { if (fd >= 0) close(fd); }
    std::string guestPath, hostPath;
    int fd = -1;
    bool isDir = false;
    uint64_t position = 0;
    std::mutex lock;
    // directory enumeration
    std::vector<std::string> entries;
    size_t dirIndex = 0;
    bool dirListed = false;
};

uint64_t ToFiletime(const struct timespec& ts)
{
    return 116444736000000000ull + uint64_t(ts.tv_sec) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
}

void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t info)
{
    if (!iosb) return;
    GuestWrite32(iosb, status);
    GuestWrite32(iosb + 4, info);
}

void SignalEvent(uint32_t handle)
{
    if (!handle) return;
    if (auto e = std::dynamic_pointer_cast<KEvent>(HandleGet(handle)))
    {
        {
            std::lock_guard lock(g_dispatchLock);
            e->signaled = true;
            e->SyncGuest();
        }
        DispatchNotify();
    }
}

std::string AttrPath(uint32_t attrs)
{
    // X_OBJECT_ATTRIBUTES: root_directory, name (ANSI_STRING*), attributes
    std::string name = GuestAnsiString(GuestRead32(attrs + 4));
    uint32_t root = GuestRead32(attrs);
    if (root && root != 0xFFFFFFFD)
        if (auto dir = std::dynamic_pointer_cast<KFile>(HandleGet(root)))
            return dir->guestPath + "\\" + name;
    return name;
}

uint32_t Attributes(const struct stat& st, bool writable)
{
    uint32_t a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!writable && !S_ISDIR(st.st_mode)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

// FILE_NETWORK_OPEN_INFORMATION (0x38)
void WriteNetworkOpenInfo(uint32_t p, const struct stat& st, bool writable)
{
    uint64_t t = ToFiletime(st.st_mtim);
    GuestWrite64(p + 0x00, t);
    GuestWrite64(p + 0x08, ToFiletime(st.st_atim));
    GuestWrite64(p + 0x10, t);
    GuestWrite64(p + 0x18, t);
    GuestWrite64(p + 0x20, S_ISDIR(st.st_mode) ? 0 : (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull);
    GuestWrite64(p + 0x28, S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size));
    GuestWrite32(p + 0x30, Attributes(st, writable));
    GuestWrite32(p + 0x34, 0);
}

uint32_t OpenCommon(PPCContext& ctx, uint32_t handlePtr, uint32_t access, uint32_t attrs, uint32_t iosb,
                    uint32_t disposition, uint32_t options)
{
    std::string path = AttrPath(attrs);
    bool wantsWrite = (access & 0x40000002) != 0 || disposition != FILE_OPEN;   // GENERIC_WRITE / FILE_WRITE_DATA
    auto vp = VfsResolve(path, disposition != FILE_OPEN);
    if (!vp)
    {
        // Missing file in an existing directory is the common case: NAME_NOT_FOUND (what
        // the real kernel reports; PATH_NOT_FOUND would mean the directory is missing).
        if (g_logImports) Log("open \"%s\": not found", path.c_str());
        SetIoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, FILE_DOES_NOT_EXIST);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    bool existed = vp->exists;
    if (!existed && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
    {
        if (g_logImports) Log("open \"%s\": no such file", path.c_str());
        SetIoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, FILE_DOES_NOT_EXIST);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (existed && disposition == FILE_CREATE)
    {
        SetIoStatus(iosb, STATUS_OBJECT_NAME_COLLISION, FILE_EXISTS);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    if (wantsWrite && !vp->writable && !(existed && disposition == FILE_OPEN))
    {
        LogWarn("open \"%s\": write access to read-only mount", path.c_str());
        SetIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
        return STATUS_ACCESS_DENIED;
    }

    auto f = std::make_shared<KFile>();
    f->guestPath = path;
    f->hostPath = vp->host;
    bool dir = existed ? vp->isDir : (options & FILE_DIRECTORY_FILE) != 0;
    if (dir && (options & FILE_NON_DIRECTORY_FILE)) { SetIoStatus(iosb, 0xC00000BA, 0); return 0xC00000BA; }   // STATUS_FILE_IS_A_DIRECTORY
    if (!dir && existed && (options & FILE_DIRECTORY_FILE)) { SetIoStatus(iosb, 0xC0000103, 0); return 0xC0000103; } // STATUS_NOT_A_DIRECTORY

    uint32_t info = existed ? FILE_OPENED : FILE_CREATED;
    if (dir)
    {
        if (!existed) fs::create_directories(vp->host);
        f->isDir = true;
    }
    else
    {
        int flags = O_CLOEXEC;
        if (vp->writable)
        {
            flags |= O_RDWR | O_CREAT;
            if (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE)
            {
                flags |= O_TRUNC;
                if (existed) info = FILE_OVERWRITTEN;
            }
            fs::create_directories(fs::path(vp->host).parent_path());
        }
        else
            flags |= O_RDONLY;
        f->fd = open(vp->host.c_str(), flags, 0644);
        if (f->fd < 0)
        {
            LogWarn("open \"%s\" -> %s failed: %s", path.c_str(), vp->host.c_str(), strerror(errno));
            SetIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
    }
    uint32_t h = HandleCreate(f);
    GuestWrite32(handlePtr, h);
    SetIoStatus(iosb, STATUS_SUCCESS, info);
    if (g_logImports) Log("open \"%s\" -> 0x%X (%s)", path.c_str(), h, f->hostPath.c_str());
    return STATUS_SUCCESS;
}

std::shared_ptr<KFile> FileFromHandle(uint32_t h) { return std::dynamic_pointer_cast<KFile>(HandleGet(h)); }

// byte_offset: nullptr or -2 (FILE_USE_FILE_POINTER_POSITION) = current position
uint64_t ResolveOffset(KFile& f, uint32_t offPtr)
{
    if (!offPtr) return f.position;
    int64_t o = (int64_t)GuestRead64(offPtr);
    return o < 0 ? f.position : uint64_t(o);
}
}

// NtCreateFile(handle*, access, attrs, iosb, alloc_size*, file_attrs, share, disposition, options)
GUEST_IMPORT(NtCreateFile)
{
    RETURN(OpenCommon(ctx, ARG(3), ARG(4), ARG(5), ARG(6), ARG(10), STACK_ARG(8)));
}

// NtOpenFile(handle*, access, attrs, iosb, options)
GUEST_IMPORT(NtOpenFile)
{
    RETURN(OpenCommon(ctx, ARG(3), ARG(4), ARG(5), ARG(6), FILE_OPEN, ARG(7)));
}

// NtReadFile(handle, event, apc, apc_ctx, iosb, buffer, length, byte_offset*)
GUEST_IMPORT(NtReadFile)
{
    auto f = FileFromHandle(ARG(3));
    if (!f || f->fd < 0) RETURN(STATUS_INVALID_HANDLE);
    if (ARG(5)) LogWarn("NtReadFile: APC completion not supported");
    std::lock_guard lock(f->lock);
    uint64_t off = ResolveOffset(*f, ARG(10));
    WatchHostWrite hostWrite(ARG(8), ARG(9));
    ssize_t r = pread(f->fd, g_base + ARG(8), ARG(9), off);
    uint32_t status = r < 0 ? STATUS_UNSUCCESSFUL : (r == 0 && ARG(9) ? STATUS_END_OF_FILE : STATUS_SUCCESS);
    if (r > 0) f->position = off + r;
    SetIoStatus(ARG(7), status, r > 0 ? uint32_t(r) : 0);
    SignalEvent(ARG(4));
    if (g_logImports && f->guestPath.find(".bik") != std::string::npos)
        Log("read %s h=0x%X off=0x%llX len=0x%X -> 0x%zX status 0x%X event 0x%X", f->guestPath.c_str(), ARG(3),
            (unsigned long long)off, ARG(9), r, status, ARG(4));
    RETURN(status);
}

// NtReadFileScatter(handle, event, apc, apc_ctx, iosb, segments (u32 page addresses), length, byte_offset*)
GUEST_IMPORT(NtReadFileScatter)
{
    auto f = FileFromHandle(ARG(3));
    if (!f || f->fd < 0) RETURN(STATUS_INVALID_HANDLE);
    std::lock_guard lock(f->lock);
    uint64_t off = ResolveOffset(*f, ARG(10));
    uint32_t remaining = ARG(9), total = 0;
    for (uint32_t seg = ARG(8); remaining; seg += 8)   // FILE_SEGMENT_ELEMENT is 8 bytes (low word = address)
    {
        uint32_t page = GuestRead32(seg + 4) ? GuestRead32(seg + 4) : GuestRead32(seg);
        uint32_t n = std::min<uint32_t>(remaining, 0x1000);
        WatchHostWrite hostWrite(page, n);
        ssize_t r = pread(f->fd, g_base + page, n, off + total);
        if (r <= 0) break;
        total += uint32_t(r);
        remaining -= uint32_t(r);
        if (uint32_t(r) < n) break;
    }
    f->position = off + total;
    uint32_t status = total == 0 && ARG(9) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    SetIoStatus(ARG(7), status, total);
    SignalEvent(ARG(4));
    RETURN(status);
}

// NtWriteFile(handle, event, apc, apc_ctx, iosb, buffer, length, byte_offset*)
GUEST_IMPORT(NtWriteFile)
{
    auto f = FileFromHandle(ARG(3));
    if (!f || f->fd < 0) RETURN(STATUS_INVALID_HANDLE);
    std::lock_guard lock(f->lock);
    uint64_t off = ResolveOffset(*f, ARG(10));
    ssize_t r = pwrite(f->fd, g_base + ARG(8), ARG(9), off);
    uint32_t status = r < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
    if (r > 0) f->position = off + r;
    SetIoStatus(ARG(7), status, r > 0 ? uint32_t(r) : 0);
    SignalEvent(ARG(4));
    RETURN(status);
}

GUEST_IMPORT(NtFlushBuffersFile)
{
    auto f = FileFromHandle(ARG(3));
    if (f && f->fd >= 0) fsync(f->fd);
    SetIoStatus(ARG(4), STATUS_SUCCESS, 0);
    RETURN(STATUS_SUCCESS);
}

// NtQueryInformationFile(handle, iosb, info, length, class)
GUEST_IMPORT(NtQueryInformationFile)
{
    auto f = FileFromHandle(ARG(3));
    if (!f) RETURN(STATUS_INVALID_HANDLE);
    struct stat st{};
    if (stat(f->hostPath.c_str(), &st) < 0) RETURN(STATUS_UNSUCCESSFUL);
    uint32_t p = ARG(5), cls = ARG(7), len = 0;
    bool writable = f->fd >= 0 && (fcntl(f->fd, F_GETFL) & O_ACCMODE) != O_RDONLY;
    switch (cls)
    {
    case 4:   // FileBasicInformation
        GuestWrite64(p + 0x00, ToFiletime(st.st_mtim));
        GuestWrite64(p + 0x08, ToFiletime(st.st_atim));
        GuestWrite64(p + 0x10, ToFiletime(st.st_mtim));
        GuestWrite64(p + 0x18, ToFiletime(st.st_mtim));
        GuestWrite32(p + 0x20, Attributes(st, writable));
        len = 0x28;
        break;
    case 5:   // FileStandardInformation
        GuestWrite64(p + 0x00, (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull);
        GuestWrite64(p + 0x08, uint64_t(st.st_size));
        GuestWrite32(p + 0x10, 1);
        GuestWrite8(p + 0x14, 0);
        GuestWrite8(p + 0x15, S_ISDIR(st.st_mode) ? 1 : 0);
        len = 0x18;
        break;
    case 14:  // FilePositionInformation
        GuestWrite64(p, f->position);
        len = 8;
        break;
    case 34:  // FileNetworkOpenInformation
        WriteNetworkOpenInfo(p, st, writable);
        len = 0x38;
        break;
    default:
        LogWarn("NtQueryInformationFile: class %u not implemented (%s)", cls, f->guestPath.c_str());
        RETURN(STATUS_NOT_IMPLEMENTED);
    }
    SetIoStatus(ARG(4), STATUS_SUCCESS, len);
    RETURN(STATUS_SUCCESS);
}

// NtSetInformationFile(handle, iosb, info, length, class)
GUEST_IMPORT(NtSetInformationFile)
{
    auto f = FileFromHandle(ARG(3));
    if (!f) RETURN(STATUS_INVALID_HANDLE);
    uint32_t p = ARG(5), cls = ARG(7);
    switch (cls)
    {
    case 14: f->position = GuestRead64(p); break;                                     // position
    case 19: case 20: if (f->fd >= 0 && ftruncate(f->fd, (off_t)GuestRead64(p)) < 0) RETURN(STATUS_ACCESS_DENIED); break; // allocation / EOF
    case 13: if (GuestRead8(p)) { std::error_code ec; fs::remove(f->hostPath, ec); } break; // disposition (delete)
    case 4: break;                                                                     // basic info (times) - ignore
    default: LogWarn("NtSetInformationFile: class %u not implemented", cls); RETURN(STATUS_NOT_IMPLEMENTED);
    }
    SetIoStatus(ARG(4), STATUS_SUCCESS, 0);
    RETURN(STATUS_SUCCESS);
}

// NtQueryFullAttributesFile(attrs, FILE_NETWORK_OPEN_INFORMATION*)
GUEST_IMPORT(NtQueryFullAttributesFile)
{
    std::string path = AttrPath(ARG(3));
    auto vp = VfsResolve(path);
    struct stat st{};
    if (!vp || !vp->exists || stat(vp->host.c_str(), &st) < 0)
    {
        if (g_logImports) Log("NtQueryFullAttributesFile \"%s\": not found", path.c_str());
        RETURN(STATUS_OBJECT_NAME_NOT_FOUND);
    }
    WriteNetworkOpenInfo(ARG(4), st, vp->writable);
    RETURN(STATUS_SUCCESS);
}

// NtQueryVolumeInformationFile(handle, iosb, info, length, class)
GUEST_IMPORT(NtQueryVolumeInformationFile)
{
    uint32_t p = ARG(5), cls = ARG(7), len = 0;
    switch (cls)
    {
    case 1:   // FileFsVolumeInformation: creation_time, serial, label_len, supports_objects, label
        GuestWrite64(p, 0);
        GuestWrite32(p + 8, 0x12345678);
        GuestWrite32(p + 12, 0);
        GuestWrite8(p + 16, 0);
        len = 0x18;
        break;
    case 3:   // FileFsSizeInformation: total units, free units, sectors/unit, bytes/sector
        GuestWrite64(p + 0, 0x1000000);
        GuestWrite64(p + 8, 0x800000);
        GuestWrite32(p + 16, 8);
        GuestWrite32(p + 20, 512);
        len = 0x18;
        break;
    case 5:   // FileFsAttributeInformation: attrs, max name len, fs name len, name ("FATX")
        GuestWrite32(p + 0, 0x2);
        GuestWrite32(p + 4, 255);
        GuestWrite32(p + 8, 8);
        for (int i = 0; i < 4; i++) GuestWrite16(p + 12 + i * 2, "FATX"[i]);
        len = 0x14;
        break;
    default:
        LogWarn("NtQueryVolumeInformationFile: class %u not implemented", cls);
        RETURN(STATUS_NOT_IMPLEMENTED);
    }
    SetIoStatus(ARG(4), STATUS_SUCCESS, len);
    RETURN(STATUS_SUCCESS);
}

// Wildcard match (case-insensitive, * and ?).
static bool Match(const char* pat, const char* s)
{
    if (!*pat) return !*s;
    if (*pat == '*') return Match(pat + 1, s) || (*s && Match(pat, s + 1));
    if (*s && (*pat == '?' || std::tolower((unsigned char)*pat) == std::tolower((unsigned char)*s))) return Match(pat + 1, s + 1);
    return false;
}

// NtQueryDirectoryFile(handle, event, apc, apc_ctx, iosb, FILE_DIRECTORY_INFORMATION*, length,
//                      file_mask (ANSI_STRING*), restart_scan)
GUEST_IMPORT(NtQueryDirectoryFile)
{
    auto f = FileFromHandle(ARG(3));
    if (!f || !f->isDir) RETURN(STATUS_INVALID_HANDLE);
    std::string mask = ARG(10) ? GuestAnsiString(ARG(10)) : "*";
    if (mask.empty()) mask = "*";
    bool restart = STACK_ARG(8) != 0;
    std::lock_guard lock(f->lock);
    if (!f->dirListed || restart)
    {
        f->entries.clear();
        std::error_code ec;
        for (auto& e : fs::directory_iterator(f->hostPath, ec)) f->entries.push_back(e.path().filename().string());
        std::sort(f->entries.begin(), f->entries.end());
        f->dirIndex = 0;
        f->dirListed = true;
    }
    // Return one matching entry per call (games loop until STATUS_NO_MORE_FILES).
    while (f->dirIndex < f->entries.size())
    {
        const std::string& name = f->entries[f->dirIndex++];
        if (!Match(mask.c_str(), name.c_str())) continue;
        struct stat st{};
        if (stat((f->hostPath + "/" + name).c_str(), &st) < 0) continue;
        uint32_t p = ARG(8);
        uint32_t need = 0x40 + uint32_t(name.size());
        if (ARG(9) < need) RETURN(0x80000005);   // STATUS_BUFFER_OVERFLOW
        GuestWrite32(p + 0x00, 0);               // next entry offset
        GuestWrite32(p + 0x04, uint32_t(f->dirIndex));
        GuestWrite64(p + 0x08, ToFiletime(st.st_mtim));
        GuestWrite64(p + 0x10, ToFiletime(st.st_atim));
        GuestWrite64(p + 0x18, ToFiletime(st.st_mtim));
        GuestWrite64(p + 0x20, ToFiletime(st.st_mtim));
        GuestWrite64(p + 0x28, S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size));
        GuestWrite64(p + 0x30, S_ISDIR(st.st_mode) ? 0 : (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull);
        GuestWrite32(p + 0x38, Attributes(st, false));
        GuestWrite32(p + 0x3C, uint32_t(name.size()));
        memcpy(g_base + p + 0x40, name.data(), name.size());
        SetIoStatus(ARG(7), STATUS_SUCCESS, need);
        SignalEvent(ARG(4));
        RETURN(STATUS_SUCCESS);
    }
    SetIoStatus(ARG(7), STATUS_NO_MORE_FILES, 0);
    SignalEvent(ARG(4));
    RETURN(STATUS_NO_MORE_FILES);
}

// Symbolic links: ObCreateSymbolicLink(ANSI_STRING* link, ANSI_STRING* target)
GUEST_IMPORT(ObCreateSymbolicLink)
{
    std::string link = GuestAnsiString(ARG(3)), target = GuestAnsiString(ARG(4));
    VfsAddLink(link, target);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ObDeleteSymbolicLink)
{
    VfsRemoveLink(GuestAnsiString(ARG(3)));
    RETURN(STATUS_SUCCESS);
}
