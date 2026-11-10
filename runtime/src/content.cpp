// XAM content: enumeration and mounting of DLC / saves / title updates from the Xbox 360
// "content" folder layout (Xenia's format, which is what the user's dump uses):
//   content/<owner>/<title id>/<content type>/<file name>/...           package contents
//   content/<owner>/<title id>/Headers/<content type>/<file name>.header XCONTENT_DATA (+license)
// owner = 0000000000000000 for device-wide content (DLC), the profile XUID for saves.
#include "guest.h"
#include "kobject.h"
#include "stubs.h"
#include "vfs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

uint64_t ProfileXuid();   // xam.cpp

namespace
{
constexpr uint32_t kTitleId = 0x4156091D;
constexpr uint32_t kContentDataSize = 0x134;   // XCONTENT_DATA
// Save folder of the local profile: the original one (the user's saves), unless BO3_XUID picks
// another profile for a test instance.
std::string ProfileXuidDir()
{
    if (!getenv("BO3_XUID")) return "E0300000126D662A";
    char b[20];
    snprintf(b, sizeof(b), "%016llX", (unsigned long long)ProfileXuid());
    return b;
}

struct ContentItem
{
    uint8_t data[kContentDataSize] = {};
};

struct KEnumerator : KObject
{
    KEnumerator() : KObject(KType::Other) {}
    std::vector<ContentItem> items;          // content enumerators (XCONTENT_DATA items)
    std::vector<std::string> blobs;          // generic enumerators (XamCreateEnumeratorHandle): raw items
    uint32_t itemSize = kContentDataSize;
    uint32_t extra = 0;                      // guest "private enum structure"
    size_t next = 0;
    uint32_t itemsPerEnumerate = 1;
};

std::string ContentRoot() { return fs::absolute("content").string(); }

std::string OwnerDir(uint32_t userIndex, uint32_t contentType)
{
    // saves (type 1) are per profile, everything else device-wide
    return contentType == 1 && userIndex != 0xFF ? ProfileXuidDir() : std::string("0000000000000000");
}

std::string TypeDir(uint32_t contentType)
{
    char b[16];
    snprintf(b, sizeof(b), "%08X", contentType);
    return b;
}

std::vector<ContentItem> ListContent(uint32_t userIndex, uint32_t contentType)
{
    std::vector<ContentItem> out;
    char tid[16];
    snprintf(tid, sizeof(tid), "%08X", kTitleId);
    std::string titleDir = ContentRoot() + "/" + OwnerDir(userIndex, contentType) + "/" + tid;
    std::string pkgDir = titleDir + "/" + TypeDir(contentType);
    std::string hdrDir = titleDir + "/Headers/" + TypeDir(contentType);
    std::error_code ec;
    for (auto& e : fs::directory_iterator(pkgDir, ec))
    {
        if (!e.is_directory()) continue;
        std::string name = e.path().filename().string();
        ContentItem item;
        std::ifstream h(hdrDir + "/" + name + ".header", std::ios::binary);
        if (h && h.read((char*)item.data, kContentDataSize))
        {
            // header already is an XCONTENT_DATA (device id, type, display name, file name)
        }
        else
        {
            uint32_t dev = __builtin_bswap32(1), type = __builtin_bswap32(contentType);
            memcpy(item.data, &dev, 4);
            memcpy(item.data + 4, &type, 4);
            for (size_t i = 0; i < name.size() && i < 127; i++) { item.data[8 + i * 2] = 0; item.data[9 + i * 2] = uint8_t(name[i]); }
        }
        // file name field (0x108, 42 bytes) must match the folder name
        memset(item.data + 0x108, 0, 42);
        memcpy(item.data + 0x108, name.data(), std::min<size_t>(name.size(), 42));
        out.push_back(item);
    }
    return out;
}

uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0)
{
    if (!overlapped) return result;
    GuestWrite32(overlapped + 0, result);
    GuestWrite32(overlapped + 4, length);
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
}

// XamContentCreateEnumerator(user, device_id, content_type, content_flags, items_per_enumerate,
//                            buffer_size*, handle*)
GUEST_IMPORT(XamContentCreateEnumerator)
{
    uint32_t user = ARG(3), type = ARG(5), perEnum = std::max<uint32_t>(ARG(7), 1);
    auto en = std::make_shared<KEnumerator>();
    en->items = ListContent(user, type);
    en->itemsPerEnumerate = perEnum;
    if (ARG(8)) GuestWrite32(ARG(8), perEnum * kContentDataSize);
    GuestWrite32(ARG(9), HandleCreate(en));
    Log("content: enumerator for type %08X (user %u): %zu items", type, user, en->items.size());
    RETURN(ERROR_SUCCESS);
}

// XamEnumerate(handle, flags, buffer, buffer_size, items_returned*, overlapped)
GUEST_IMPORT(XamEnumerate)
{
    auto en = std::dynamic_pointer_cast<KEnumerator>(HandleGet(ARG(3)));
    uint32_t buffer = ARG(5), size = ARG(6), returnedPtr = ARG(7), overlapped = ARG(8);
    if (!en)
    {
        if (returnedPtr) GuestWrite32(returnedPtr, 0);
        RETURN(CompleteOverlapped(overlapped, ERROR_NO_MORE_FILES));
    }
    uint32_t n = 0;
    if (!en->blobs.empty() || en->itemSize != kContentDataSize)
        while (en->next < en->blobs.size() && n < en->itemsPerEnumerate && (n + 1) * en->itemSize <= size)
        {
            memcpy(g_base + buffer + n * en->itemSize, en->blobs[en->next].data(), std::min<size_t>(en->itemSize, en->blobs[en->next].size()));
            en->next++;
            n++;
        }
    else
        while (en->next < en->items.size() && n < en->itemsPerEnumerate && (n + 1) * kContentDataSize <= size)
        {
            memcpy(g_base + buffer + n * kContentDataSize, en->items[en->next].data, kContentDataSize);
            en->next++;
            n++;
        }
    if (returnedPtr) GuestWrite32(returnedPtr, n);
    if (!n) RETURN(CompleteOverlapped(overlapped, ERROR_NO_MORE_FILES));
    if (overlapped) RETURN(CompleteOverlapped(overlapped, ERROR_SUCCESS, n));
    RETURN(ERROR_SUCCESS);
}

// XamCreateEnumeratorHandle(user, app, open message, close message, extra size, item count, flags,
// handle*): system-app enumerators. Online mode: the title's LSP servers (XLive 0x58039,
// XTitleServerCreateEnumerator) = one XTITLE_SERVER_INFO {IN_ADDR, flags, info[200]} - the
// in-process Demonware (placeholder address, see net.cpp).
bool LiveSignedIn();   // xam.cpp
GUEST_IMPORT(XamCreateEnumeratorHandle)
{
    uint32_t app = ARG(4), open = ARG(5), extraSize = ARG(7), count = ARG(8), handlePtr = ARG(10);
    auto en = std::make_shared<KEnumerator>();
    en->extra = extraSize ? GuestAlloc(extraSize) : 0;
    if (en->extra) memset(g_base + en->extra, 0, extraSize);
    en->itemsPerEnumerate = std::max<uint32_t>(count, 1);
    if (app == 0xFC && open == 0x00058039 && LiveSignedIn())
    {
        en->itemSize = 208;
        std::string item(208, '\0');
        uint32_t ip = 0x0A445701u;   // big-endian IN_ADDR
        item[0] = char(ip >> 24); item[1] = char(ip >> 16); item[2] = char(ip >> 8); item[3] = char(ip);
        const char* info = "demonware";
        memcpy(&item[8], info, strlen(info));
        en->blobs.push_back(item);
    }
    else en->itemSize = 0;
    uint32_t h = HandleCreate(en);
    if (handlePtr) GuestWrite32(handlePtr, h);
    Log("xam: XamCreateEnumeratorHandle(app 0x%X, open 0x%X, extra %u, count %u) -> %zu item(s)", app, open, extraSize, count, en->blobs.size());
    RETURN(ERROR_SUCCESS);
}
GUEST_IMPORT(XamGetPrivateEnumStructureFromHandle)   // (handle, out*)
{
    auto en = std::dynamic_pointer_cast<KEnumerator>(HandleGet(ARG(3)));
    if (!en) RETURN(6);
    if (ARG(4)) GuestWrite32(ARG(4), en->extra);
    RETURN(ERROR_SUCCESS);
}

// XamContentCreateEx(user, root_name, content_data*, flags, disposition*, license_mask*,
//                    cache_size, content_size, overlapped)
GUEST_IMPORT(XamContentCreateEx)
{
    std::string root = GuestString(ARG(4));
    uint32_t data = ARG(5), flags = ARG(6);
    uint32_t type = GuestRead32(data + 4);
    std::string file((const char*)g_base + data + 0x108, strnlen((const char*)g_base + data + 0x108, 42));
    char tid[16];
    snprintf(tid, sizeof(tid), "%08X", kTitleId);
    std::string dir = ContentRoot() + "/" + OwnerDir(ARG(3), type) + "/" + tid + "/" + TypeDir(type) + "/" + file;
    std::error_code ec;
    bool exists = fs::is_directory(dir, ec);
    uint32_t disposition = 1;   // opened
    // flags: 1 = create new, 2 = create always, 3 = open existing, 4 = open always, 5 = truncate
    uint32_t mode = flags & 0xF;
    if (!exists)
    {
        if (mode == 3 || mode == 5)
        {
            uint32_t overlapped = STACK_ARG(8);
            RETURN(CompleteOverlapped(overlapped, ERROR_FILE_NOT_FOUND));
        }
        fs::create_directories(dir, ec);
        disposition = 2;    // created
    }
    else if (mode == 1)
        RETURN(CompleteOverlapped(STACK_ARG(8), ERROR_ALREADY_EXISTS));
    else if (mode == 2 || mode == 5)
    {
        // create always / truncate existing: the package starts out empty (the game then
        // creates its files with FILE_CREATE). Keep a one-time backup of the original data
        // under content/backup/ (outside the enumerated folders).
        std::string rel = fs::relative(dir, ContentRoot(), ec).string();
        std::string backup = ContentRoot() + "/backup/" + rel;
        if (!fs::exists(backup, ec))
        {
            fs::create_directories(backup, ec);
            fs::copy(dir, backup, fs::copy_options::recursive, ec);
            Log("content: backed up %s -> %s", dir.c_str(), backup.c_str());
        }
        for (auto& e : fs::directory_iterator(dir, ec)) fs::remove_all(e.path(), ec);
        disposition = 2;
    }
    VfsMount(root + ":", dir, type == 1 /* saves are writable */);
    if (ARG(7)) GuestWrite32(ARG(7), disposition);
    if (ARG(8)) GuestWrite32(ARG(8), 0xFFFFFFFF);   // license mask: everything
    Log("content: mounted %s: -> %s (type %08X, mode %u, %s)", root.c_str(), dir.c_str(), type, mode,
        disposition == 2 ? "created" : "opened");
    uint32_t overlapped = STACK_ARG(8);
    RETURN(CompleteOverlapped(overlapped, ERROR_SUCCESS));
}

GUEST_IMPORT(XamContentClose)   // (root_name, overlapped)
{
    VfsUnmount(GuestString(ARG(3)) + ":");
    RETURN(CompleteOverlapped(ARG(4), ERROR_SUCCESS));
}

// XamContentGetDeviceState(device_id, overlapped): the HDD is always present.
GUEST_IMPORT(XamContentGetDeviceState) { RETURN(CompleteOverlapped(ARG(4), ERROR_SUCCESS)); }

// XamContentGetDeviceData(device_id, XDEVICE_DATA*): id, type, total, free, name[28]
GUEST_IMPORT(XamContentGetDeviceData)
{
    uint32_t p = ARG(4);
    memset(g_base + p, 0, 0x50);
    GuestWrite32(p + 0x00, ARG(3) ? ARG(3) : 1);
    GuestWrite32(p + 0x04, 1);                       // HDD
    GuestWrite64(p + 0x08, 20ull * 1024 * 1024 * 1024);
    GuestWrite64(p + 0x10, 10ull * 1024 * 1024 * 1024);
    const char* name = "Hard Drive";
    for (int i = 0; name[i]; i++) GuestWrite16(p + 0x18 + i * 2, uint16_t(name[i]));
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserGetDeviceContext) { if (ARG(5)) GuestWrite32(ARG(5), 1); RETURN(ERROR_SUCCESS); }
