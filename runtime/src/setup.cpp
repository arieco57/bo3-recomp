// First-run installation from the player's own copy of the game: everything the runtime needs is
// produced here from the original files, nothing of the game is shipped.
//
//   disc image (*.iso: Redump XGD2 / XGD3, or a plain XISO)  -> extracted/game/
//   Title Update package (STFS "LIVE", the console's TU file) -> content/0000000000000000/4156091D/000B0000/tu00000001_00000000/
//   DLC packages (STFS, optional)                             -> content/0000000000000000/4156091D/00000002/<package>/
//   default.xex / default_zm.xex + the TU's .xexp (XexPatcher) -> private/<mp|zm>/default_patched.xex
//
// The recompiled code matches one exact version of the game (disc + Title Update 1), so the
// results are checked by SHA-1. Inputs are looked for in the data folder, its "instalar" folder
// and, on Android, /sdcard/BO3Recomp (all files access, asked for when needed). A folder copied
// from Xenia's content layout or an already extracted disc also works: what exists is kept.
#include "setup.h"
#include "zar.h"
#include "log.h"
#include "ui.h"

#include <xex_patcher.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace
{
const char* kGameDir = "extracted/game";
const char* kTuDir = "content/0000000000000000/4156091D/000B0000/tu00000001_00000000";
const char* kDlcDir = "content/0000000000000000/4156091D/00000002";
const char* kHdrDir = "content/0000000000000000/4156091D/Headers";
constexpr uint32_t kTitleId = 0x4156091D;

struct Exe { const char* kind; const char* xex; const char* xexp; const char* xexSha; const char* xexpSha; const char* patchedSha; };
const Exe kExes[] = {
    { "mp", "default.xex", "default.xexp", "3fa9af387164d1343729db373f4604f9bec51e00", "474125f5dd21295d87ca3741cf2bbef6d382582e",
      "ab1a201ef739e714727064adadb7ffa2842394c3" },
    { "zm", "default_zm.xex", "default_zm.xexp", "9d9b70ecfa613cc7c9d7b453cc26677fb8333703", "e7ebdbc48812fdff30484db29dd8a69a8c2291a4",
      "1ee13582229fe02f0b2dbd1e6fba71dacbca28fd" },
};

// ------------------------------------------------------------------------------ SHA-1
struct Sha1
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t buf[64];
    size_t n = 0;
    uint64_t total = 0;
    static uint32_t Rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
    void Block(const uint8_t* p)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void Add(const void* data, size_t len)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += len;
        while (len)
        {
            size_t k = std::min(len, 64 - n);
            memcpy(buf + n, p, k);
            n += k; p += k; len -= k;
            if (n == 64) { Block(buf); n = 0; }
        }
    }
    std::string Hex()
    {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        Add(&pad, 1);
        uint8_t z = 0;
        while (n != 56) Add(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        Add(len, 8);
        char out[41];
        for (int i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

std::string FileSha1(const std::string& path)
{
    FILE* f = FopenInput(path.c_str(), "rb");
    if (!f) return {};
    Sha1 s;
    std::vector<uint8_t> b(1 << 20);
    size_t k;
    while ((k = fread(b.data(), 1, b.size(), f)) > 0) s.Add(b.data(), k);
    fclose(f);
    return s.Hex();
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = FopenInput(path.c_str(), "rb");
    if (!f) return false;
    fseeko(f, 0, SEEK_END);
    off_t n = ftello(f);
    fseeko(f, 0, SEEK_SET);
    out.resize(size_t(n));
    bool ok = n == 0 || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool WriteFileBytes(const std::string& path, const uint8_t* data, size_t n)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
    return ok;
}

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t LE24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
uint32_t LE32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// Copies `size` bytes at `offset` of `in` to the file `dst` (created with its folders).
bool CopyRange(FILE* in, uint64_t offset, uint64_t size, const std::string& dst)
{
    std::error_code ec;
    fs::create_directories(fs::path(dst).parent_path(), ec);
    FILE* out = fopen((dst + ".part").c_str(), "wb");
    if (!out) return false;
    std::vector<uint8_t> b(4 << 20);
    bool ok = fseeko(in, off_t(offset), SEEK_SET) == 0;
    while (ok && size)
    {
        size_t k = size_t(std::min<uint64_t>(size, b.size()));
        ok = fread(b.data(), 1, k, in) == k && fwrite(b.data(), 1, k, out) == k;
        size -= k;
    }
    ok = fclose(out) == 0 && ok;
    if (ok) ok = rename((dst + ".part").c_str(), dst.c_str()) == 0;
    return ok;
}

// ------------------------------------------------------------------------------ disc images
// Xbox 360 game discs: XDVDFS ("MICROSOFT*XBOX*MEDIA" at sector 32 of the game partition), which
// starts at 0 (plain XISO), 0xFD90000 (XGD2), 0x2080000 (XGD3) or 0x18300000 (XGD1).
bool DiscPartition(FILE* f, uint64_t& base)
{
    static const char kMagic[] = "MICROSOFT*XBOX*MEDIA";
    for (uint64_t off : { 0ull, 0xFD90000ull, 0x2080000ull, 0x18300000ull })
    {
        char m[20];
        if (fseeko(f, off_t(off + 0x10000), SEEK_SET) == 0 && fread(m, 1, 20, f) == 20 && memcmp(m, kMagic, 20) == 0) { base = off; return true; }
    }
    return false;
}

struct DiscFile { std::string path; uint64_t offset, size; };

bool DiscList(FILE* f, uint64_t base, std::vector<DiscFile>& files)
{
    uint8_t vd[8];
    if (fseeko(f, off_t(base + 0x10000 + 20), SEEK_SET) != 0 || fread(vd, 1, 8, f) != 8) return false;
    std::function<bool(uint32_t, uint32_t, const std::string&, int)> walk = [&](uint32_t sector, uint32_t size, const std::string& prefix, int depth) {
        if (depth > 32 || size > (64u << 20)) return false;
        std::vector<uint8_t> d(size);
        if (fseeko(f, off_t(base + uint64_t(sector) * 2048), SEEK_SET) != 0 || fread(d.data(), 1, size, f) != size) return false;
        std::vector<uint32_t> stack = { 0 }, seen;
        while (!stack.empty())
        {
            uint32_t o = stack.back();
            stack.pop_back();
            if (o + 14 > size || std::find(seen.begin(), seen.end(), o) != seen.end()) continue;
            seen.push_back(o);
            const uint8_t* e = d.data() + o;
            uint16_t l = uint16_t(e[0] | e[1] << 8), r = uint16_t(e[2] | e[3] << 8);
            if (l == 0xFFFF && r == 0xFFFF) continue;
            uint32_t s = LE32(e + 4), sz = LE32(e + 8);
            uint8_t attr = e[12], nl = e[13];
            if (o + 14 + nl > size) continue;
            std::string name(reinterpret_cast<const char*>(e + 14), nl);
            if (l) stack.push_back(uint32_t(l) * 4);
            if (r) stack.push_back(uint32_t(r) * 4);
            if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) continue;
            if (attr & 0x10) { if (sz && !walk(s, sz, prefix + name + "/", depth + 1)) return false; }
            else files.push_back({ prefix + name, base + uint64_t(s) * 2048, sz });
        }
        return true;
    };
    return walk(LE32(vd), LE32(vd + 4), "", 0);
}

// ------------------------------------------------------------------------------ STFS packages
struct Stfs
{
    FILE* f = nullptr;
    uint32_t contentType = 0, titleId = 0;
    std::u16string displayName;
    uint64_t first = 0;
    int shift = 0;
    uint32_t ftBlock = 0, ftCount = 0;

    bool Open(const std::string& path)
    {
        f = FopenInput(path.c_str(), "rb");
        if (!f) return false;
        uint8_t h[0x500];
        if (fread(h, 1, sizeof(h), f) != sizeof(h)) return false;
        if (memcmp(h, "LIVE", 4) && memcmp(h, "PIRS", 4) && memcmp(h, "CON ", 4)) return false;
        uint32_t headerSize = BE32(h + 0x340);
        contentType = BE32(h + 0x344);
        titleId = BE32(h + 0x360);
        for (int i = 0; i < 128; i++) { char16_t c = char16_t(h[0x411 + i * 2] << 8 | h[0x412 + i * 2]); if (!c) break; displayName += c; }
        const uint8_t* vd = h + 0x379;
        shift = (vd[2] & 1) ? 0 : 1;   // read-only packages keep one hash table per level, others two
        ftCount = uint32_t(vd[3] | vd[4] << 8);
        ftBlock = LE24(vd + 5);
        first = (uint64_t(headerSize) + 0xFFF) & ~uint64_t(0xFFF);
        return true;
    }
    ~Stfs() { if (f) fclose(f); }
    // data block -> physical block (hash tables are interleaved: one per 170 blocks, per 170^2...)
    uint64_t Phys(uint32_t b) const
    {
        uint64_t r = b + ((uint64_t(b) + 0xAA) / 0xAA << shift);
        if (b >= 0xAA)
        {
            r += (uint64_t(b) + 0x70E4) / 0x70E4 << shift;
            if (b >= 0x70E4) r += (uint64_t(b) + 0x4AF768) / 0x4AF768 << shift;
        }
        return r;
    }
    uint64_t Offset(uint32_t b) const { return first + Phys(b) * 0x1000; }
    // level-0 hash entry of block b (its "next block" link for fragmented files)
    uint32_t Next(uint32_t b)
    {
        uint64_t t;
        if (b < 0xAA) t = 0;
        else
        {
            t = uint64_t(b / 0xAA) * 0xAB + ((uint64_t(b / 0x70E4) + 1) << shift);
            if (b >= 0x70E4) t += uint64_t(1) << shift;
        }
        uint8_t e[0x18];
        if (fseeko(f, off_t(first + t * 0x1000 + uint64_t(b % 0xAA) * 0x18), SEEK_SET) != 0 || fread(e, 1, 0x18, f) != 0x18) return 0xFFFFFF;
        return uint32_t(e[0x15]) << 16 | uint32_t(e[0x16]) << 8 | e[0x17];
    }

    bool Extract(const std::string& dst, std::string& err)
    {
        struct Ent { std::string name; uint8_t flags; uint32_t start; int16_t parent; uint32_t size; };
        std::vector<Ent> ents;
        for (uint32_t i = 0; i < ftCount; i++)
        {
            uint8_t blk[0x1000];
            if (fseeko(f, off_t(Offset(ftBlock + i)), SEEK_SET) != 0 || fread(blk, 1, sizeof(blk), f) != sizeof(blk)) { err = "file table"; return false; }
            for (int j = 0; j < 0x1000; j += 0x40)
            {
                const uint8_t* e = blk + j;
                uint8_t nl = e[0x28] & 0x3F;
                if (!nl) continue;
                ents.push_back({ std::string(reinterpret_cast<const char*>(e), nl), e[0x28], LE24(e + 0x2F), int16_t(e[0x32] << 8 | e[0x33]), BE32(e + 0x34) });
            }
        }
        auto full = [&](size_t i) {
            std::string p;
            for (int k = 0, cur = int(i); cur >= 0 && cur < int(ents.size()) && k < 64; k++, cur = ents[size_t(cur)].parent)
                p = ents[size_t(cur)].name + (p.empty() ? "" : "/" + p);
            return p;
        };
        for (size_t i = 0; i < ents.size(); i++)
        {
            const Ent& e = ents[i];
            std::string path = dst + "/" + full(i);
            if (e.name.find("..") != std::string::npos) continue;
            std::error_code ec;
            if (e.flags & 0x80) { fs::create_directories(path, ec); continue; }
            fs::create_directories(fs::path(path).parent_path(), ec);
            FILE* out = fopen((path + ".part").c_str(), "wb");
            if (!out) { err = "cannot write " + path; return false; }
            uint32_t b = e.start;
            uint64_t left = e.size;
            std::vector<uint8_t> buf(0x1000);
            bool ok = true;
            while (ok && left)
            {
                ok = fseeko(f, off_t(Offset(b)), SEEK_SET) == 0 && fread(buf.data(), 1, 0x1000, f) == 0x1000;
                size_t k = size_t(std::min<uint64_t>(left, 0x1000));
                ok = ok && fwrite(buf.data(), 1, k, out) == k;
                left -= k;
                b = (e.flags & 0x40) ? b + 1 : Next(b);
            }
            ok = fclose(out) == 0 && ok;
            if (!ok || rename((path + ".part").c_str(), path.c_str()) != 0) { err = "reading " + e.name; return false; }
        }
        return true;
    }
};

// Xenia-style header (XCONTENT_DATA: device, type, display name, file name, title) so the content
// enumeration shows the package's real name
void WriteContentHeader(const std::string& typeDir, const std::string& name, uint32_t type, const std::u16string& display)
{
    uint8_t h[0x14C] = {};
    h[3] = 1;
    h[4] = uint8_t(type >> 24); h[5] = uint8_t(type >> 16); h[6] = uint8_t(type >> 8); h[7] = uint8_t(type);
    for (size_t i = 0; i < display.size() && i < 127; i++) { h[8 + i * 2] = uint8_t(display[i] >> 8); h[9 + i * 2] = uint8_t(display[i]); }
    memcpy(h + 0x108, name.data(), std::min<size_t>(name.size(), 42));
    h[0x140] = 0x41; h[0x141] = 0x56; h[0x142] = 0x09; h[0x143] = 0x1D;
    if (type == 2) h[0x148] = 1;
    WriteFileBytes(std::string(kHdrDir) + "/" + typeDir + "/" + name + ".header", h, sizeof(h));
}

// ------------------------------------------------------------------------------ the steps
bool Exists(const std::string& p) { std::error_code ec; return fs::exists(p, ec); }

struct Inputs { std::vector<std::string> discs, updates, dlcs; };

std::vector<std::string> InputDirs()
{
    std::vector<std::string> d = { ".", "instalar" };
#ifdef __ANDROID__
    d.push_back("/sdcard/BO3Recomp");
    d.push_back("/sdcard/Download");
#endif
    return d;
}

// Puts one file in the list of its kind (disc image, Title Update, DLC); other files are ignored.
// Already extracted content folders (Xenia's layout) work too: a Title Update folder (it has
// default.xexp), DLC folders (console names: 40 hex digits + "41"), or a parent of those
// (000B0000, 00000002, 4156091D...) - looked through a few levels down.
const char* kCp1 = "A92CACF8DF6D9DB1C32970035A6B8B63B09C2F1641";
const char* kCp2 = "4FD64D1A99FF002F54CEE0FE922808E1D2D21A5341";
bool IsContentName(const std::string& n)
{
    if (n.size() != 42) return false;
    for (char c : n) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

void Classify(Inputs& in, const std::string& p);
void ClassifyDir(Inputs& in, const fs::path& d, int depth)
{
    std::error_code ec;
    if (fs::exists(d / "default.xexp", ec) || fs::exists(d / "default_zm.xexp", ec)) { in.updates.push_back(d.string()); return; }
    if (IsContentName(d.filename().string())) { in.dlcs.push_back(d.string()); return; }
    if (depth <= 0) return;
    for (auto& e : fs::directory_iterator(d, ec))
    {
        if (e.is_directory(ec)) ClassifyDir(in, e.path(), depth - 1);
        else if (e.is_regular_file(ec)) Classify(in, e.path().string());   // packages (STFS) or a disc image inside
    }
}

bool IsDir(const std::string& p) { uint64_t size; bool dir; return InputStat(p, size, dir) && dir; }

// display name of a DLC input (STFS header, or the known name of a Compatibility Pack folder)
std::string DlcDisplay(const std::string& p)
{
    if (IsDir(p))
    {
        std::string n = fs::path(p).filename().string();
        return n == kCp1 ? "Compatibility Pack 1" : n == kCp2 ? "Compatibility Pack 2" : n;
    }
    Stfs s;
    std::string name;
    if (s.Open(p)) for (char16_t c : s.displayName) if (c < 128) name += char(c);
    return name;
}

// a folder input: moved when it is our own copy (the installer screen copies picked folders into
// "instalar-carpetas" of the data folder), copied otherwise (the player's folders stay as they are)
bool PlaceDir(const std::string& src, const std::string& dst, std::string& err)
{
    std::error_code ec;
    fs::remove_all(dst + ".part", ec);
    fs::create_directories(fs::path(dst).parent_path(), ec);
    std::string rel = fs::relative(src, fs::current_path(), ec).string();
    if (!ec && rel.rfind("instalar-carpetas", 0) == 0)
    {
        fs::rename(src, dst, ec);
        if (!ec) return true;
    }
    fs::copy(src, dst + ".part", fs::copy_options::recursive, ec);
    if (ec) { err = ec.message(); fs::remove_all(dst + ".part", ec); return false; }
    fs::rename(dst + ".part", dst, ec);
    if (ec) { err = ec.message(); return false; }
    return true;
}

void Classify(Inputs& in, const std::string& p)
{
    uint64_t size;
    bool dir;
    if (!InputStat(p, size, dir)) return;
    if (dir) { ClassifyDir(in, p, 3); return; }
    if (size < 0x1000) return;
    FILE* f = FopenInput(p.c_str(), "rb");
    if (!f) return;
    uint64_t base;
    bool big = size > (64ull << 20);
    bool disc = big && (DiscPartition(f, base) || ZarIsArchive(p));
    fclose(f);
    if (disc) { in.discs.push_back(p); return; }
    Stfs s;
    if (!s.Open(p) || s.titleId != kTitleId) return;
    if (s.contentType == 0xB0000) in.updates.push_back(p);
    else if (s.contentType == 2) in.dlcs.push_back(p);
}

Inputs FindInputs()
{
    Inputs in;
    for (auto& dir : InputDirs())
    {
        std::error_code ec;
        // folders too in the player's input folders (Xenia's content/ tree: content/0000000000000000/
        // 4156091D/00000002/<DLC> is 4 levels down), not in the data folder (its own installed
        // content) nor in Download
        const bool dirs = dir != "." && dir.find("Download") == std::string::npos;
        for (auto& e : fs::directory_iterator(dir, ec))
        {
            if (e.is_regular_file(ec)) Classify(in, e.path().string());
            else if (dirs && e.is_directory(ec) && e.path().filename() != "instalar-carpetas") ClassifyDir(in, e.path(), 4);
        }
    }
    return in;
}

bool GameOk() { return Exists(std::string(kGameDir) + "/default.xex") && Exists(std::string(kGameDir) + "/default_zm.xex"); }
// the supported Title Update in place (by SHA-1: a wrong one left by an earlier attempt counts as missing)
bool TuOk()
{
    for (auto& x : kExes)
        if (!Exists(std::string(kTuDir) + "/" + x.xexp) || FileSha1(std::string(kTuDir) + "/" + x.xexp) != x.xexpSha) return false;
    return true;
}
bool PatchedOk() { for (auto& x : kExes) if (!Exists(std::string("private/") + x.kind + "/default_patched.xex")) return false; return true; }

const char* kHowTo =
    "Necesitás tu copia original de Call of Duty: Black Ops III para Xbox 360:\n\n"
    "1. La imagen del disco (.iso).\n"
    "2. El Title Update del juego (el archivo TU de la consola, en\n"
    "   Content/0000000000000000/4156091D/000B0000/).\n"
    "3. Los Compatibility Pack 1 y 2 (gratis en Xbox Live) y, si querés, el resto de los DLC\n"
    "   (Content/0000000000000000/4156091D/00000002/).\n\n"
    "Copialos a la carpeta del juego o a su carpeta \"instalar\""
#ifdef __ANDROID__
    ", o a la carpeta BO3Recomp de la memoria del teléfono"
#endif
    " y volvé a abrir el juego. La instalación es automática.";

// packages in the list whose display name contains `needle`
int CountDlcNamed(const Inputs& in, const std::string& needle)
{
    int n = 0;
    for (auto& p : in.dlcs)
        if (DlcDisplay(p).find(needle) != std::string::npos) n++;
    return n;
}

// Android installer screen (InstallerActivity -> setup_jni.cpp): status/progress go to it, no dialogs
SetupReport g_report = nullptr;
// display names of explicitly chosen files (they arrive as /proc/self/fd/N paths)
std::map<std::string, std::string> g_names;
std::string NameOf(const std::string& p) { auto it = g_names.find(p); return it != g_names.end() ? it->second : fs::path(p).filename().string(); }

void Report(int pct, const std::string& text)
{
    static int last = -1;
    if (pct / 5 != last / 5 || pct < 0) Log("setup: %s (%d%%)", text.c_str(), pct);
    last = pct;
    if (g_report) g_report(text.c_str(), pct);
}

// BO3_SETUP_YES=1: no dialogs (automated tests); the messages still go to the log
void Tell(const std::string& text)
{
    if (g_report) { Log("setup: %s", text.c_str()); g_report(text.c_str(), -1); return; }
    static const bool quiet = getenv("BO3_SETUP_YES") && *getenv("BO3_SETUP_YES") == '1';
    if (quiet) { Log("setup: %s", text.c_str()); return; }
    UiMessage("BO3 Recomp - instalación", text);
}

bool Fail(const std::string& what)
{
    LogWarn("setup: %s", what.c_str());
    Tell(what);
    return false;
}

#ifdef __ANDROID__
bool AndroidStorageAccess(bool ask);   // below (JNI)
#endif
}

#ifndef __ANDROID__
// ------------------------------------------------------------------------------ step by step
// When the game's files are not where the installer looks, the first run of the AppImage (or of
// the binary) asks for them one by one with the desktop's file dialogs (zenity, or kdialog).
bool HaveTool(const char* name) { return system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0; }
const char* DialogTool()
{
    static const char* tool = HaveTool("zenity") ? "zenity" : HaveTool("kdialog") ? "kdialog" : nullptr;
    return tool;
}
bool IsZenity() { return std::string(DialogTool() ? DialogTool() : "") == "zenity"; }

// stdout of a shell command and its exit status
std::string Shell(const std::string& cmd, int& status)
{
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { status = -1; return out; }
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), p)) > 0) out.append(b, n);
    status = pclose(p);
    return out;
}

// file dialog: the chosen paths (false = cancelled). patterns like "*.zar *.iso".
bool WizardPick(const std::string& title, const std::string& filterName, const std::string& patterns, bool multiple, std::vector<std::string>& out)
{
    int st = 0;
    std::string cmd;
    if (IsZenity())
        cmd = "zenity --file-selection --title='" + title + "' --file-filter='" + filterName + " | " + patterns + "'" +
              (multiple ? " --multiple --separator='|'" : "");
    else
        cmd = std::string("kdialog --title '") + title + "' --getopenfilename \"$HOME\" '" + patterns + "'" +
              (multiple ? " --multiple --separate-output" : "");
    std::string res = Shell(cmd, st);
    if (st != 0) return false;
    while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) res.pop_back();
    if (res.empty()) return false;
    for (char& c : res) if (c == '\n') c = '|';
    size_t pos = 0;
    while (pos <= res.size())
    {
        size_t e = res.find('|', pos);
        if (e == std::string::npos) e = res.size();
        if (e > pos) out.push_back(res.substr(pos, e - pos));
        pos = e + 1;
    }
    return !out.empty();
}

// question with two buttons (true = the first one)
bool WizardAsk(const std::string& text, const std::string& yes, const std::string& no)
{
    int st = 0;
    if (IsZenity())
        Shell("zenity --question --title='BO3 Recomp - instalación' --text='" + text + "' --ok-label='" + yes + "' --cancel-label='" + no + "'", st);
    else
        Shell("kdialog --title 'BO3 Recomp - instalación' --yesno '" + text + "'", st);
    return st == 0;
}

// Asks for what is missing, one step at a time. False = the player cancelled.
bool RunWizard(Inputs& in)
{
    if (!DialogTool()) return false;
    std::vector<std::string> picked;
    if (!WizardAsk("Vas a instalar Call of Duty: Black Ops III (Xbox 360) desde tu copia. "
                   "Te voy a pedir, paso a paso:\n\n1. La imagen del disco (.zar o .iso)\n2. El Title Update del juego\n"
                   "3. Los Compatibility Pack 1 y 2 (gratis, obligatorios) y los DLC que tengas\n\nEl proceso tarda unos minutos.",
                   "Empezar", "Cancelar"))
        return false;
    if (!GameOk() && in.discs.empty())
    {
        if (!WizardPick("Paso 1 de 3: la imagen del disco", "Disco de Xbox 360", "*.zar *.iso", false, picked)) return false;
        for (auto& p : picked) Classify(in, p);
        picked.clear();
    }
    if (!TuOk() && in.updates.empty())
    {
        if (!WizardPick("Paso 2 de 3: el Title Update (archivo de Content/0000000000000000/4156091D/000B0000/)", "Title Update", "*", false, picked))
            return false;
        for (auto& p : picked) Classify(in, p);
        picked.clear();
    }
    if (CountDlcNamed(in, "Compatibility Pack") < 2)
    {
        if (WizardAsk("Paso 3 de 3: elegí los Compatibility Pack 1 y 2 y, si querés, los demás DLC (archivos de Content/0000000000000000/4156091D/00000002/).",
                      "Elegir", "Omitir") &&
            WizardPick("Paso 3 de 3: DLC (Compatibility Pack 1 y 2 y los demás)", "DLC (STFS)", "*", true, picked))
            for (auto& p : picked) Classify(in, p);
    }
    return true;
}
#endif

bool Install(Inputs& in);

bool SetupGameData()
{
    if (GameOk() && TuOk() && PatchedOk()) return true;
#ifdef __ANDROID__
    // the inputs are usually in /sdcard/BO3Recomp: reading it needs "all files access"
    if (!AndroidStorageAccess(false))
    {
        std::error_code ec;
        Inputs local = FindInputs();
        if ((!GameOk() && local.discs.empty()) || (!TuOk() && local.updates.empty()))
        {
            UiMessage("BO3 Recomp - instalación",
                      "Para instalar el juego desde tu copia (carpeta BO3Recomp de la memoria del teléfono) hace falta el permiso "
                      "\"Acceso a todos los archivos\". Activalo en la pantalla que se abre y volvé al juego.");
            AndroidStorageAccess(true);
        }
    }
    {
        std::error_code ec;
        fs::create_directories("/sdcard/BO3Recomp", ec);
    }
#endif
    Inputs in = FindInputs();
    Log("setup: %zu disc image(s), %zu title update(s), %zu DLC package(s) found", in.discs.size(), in.updates.size(), in.dlcs.size());
#ifndef __ANDROID__
    static const bool quiet = getenv("BO3_SETUP_YES") && *getenv("BO3_SETUP_YES") == '1';
    bool cpInstalled = Exists(std::string(kDlcDir) + "/A92CACF8DF6D9DB1C32970035A6B8B63B09C2F1641") &&
                       Exists(std::string(kDlcDir) + "/4FD64D1A99FF002F54CEE0FE922808E1D2D21A5341");
    bool needWizard = (!GameOk() && in.discs.empty()) || (!TuOk() && in.updates.empty()) ||
                      (!cpInstalled && CountDlcNamed(in, "Compatibility Pack") < 2);
    if (needWizard && !quiet)
    {
        Log("setup: asking for the missing files");
        RunWizard(in);
        Log("setup: %zu disc image(s), %zu title update(s), %zu DLC package(s) after the wizard", in.discs.size(), in.updates.size(), in.dlcs.size());
    }
#endif
    return Install(in);
}

// The installation itself, from the inputs found or chosen.
bool Install(Inputs& in)
{
    if (!GameOk() && in.discs.empty()) return Fail(std::string("No se encontró la imagen del disco (.zar o .iso).\n\n") + kHowTo);
    if (!TuOk() && in.updates.empty()) return Fail(std::string("No se encontró el Title Update del juego.\n\n") + kHowTo);

    std::string what;
    if (!GameOk()) what += "\n- disco: " + NameOf(in.discs[0]);
    if (!TuOk()) what += "\n- Title Update: " + NameOf(in.updates[0]);
    if (!in.dlcs.empty()) what += "\n- " + std::to_string(in.dlcs.size()) + " DLC";
    if (g_report) Report(0, "Instalando desde tu copia:" + what);
    else Tell("Se va a instalar el juego desde tu copia:" + what +
                  "\n\nTarda unos minutos (en el teléfono, hasta 15-20). La pantalla queda quieta mientras tanto: no cierres la app."
                  "\nHacen falta unos 8 GB libres; después podés borrar los archivos originales.");

    // 1. the disc
    bool discNow = false;
    if (!GameOk() && ZarIsArchive(in.discs[0]))
    {
        discNow = true;
        std::string tmp = std::string(kGameDir) + ".part", err;
        std::error_code ec;
        fs::remove_all(tmp, ec);
        Log("setup: extracting the .zar disc image %s", in.discs[0].c_str());
        int lastPct = -1;
        bool ok = ZarExtract(in.discs[0], tmp, err, [&](uint64_t done, uint64_t total) {
            int pct = total ? int(done * 100 / total) : 100;
            if (pct != lastPct) { lastPct = pct; Report(pct * 80 / 100, "Extrayendo el disco: " + std::to_string(pct) + "%"); }
        });
        if (!ok) { fs::remove_all(tmp, ec); return Fail("No se pudo extraer la imagen del disco (.zar): " + err + "\n\nSi dice \"Disk quota\" o \"No space\", falta espacio libre: hacen falta unos 8 GB.");  }
        fs::remove_all(kGameDir, ec);
        fs::rename(tmp, kGameDir, ec);
        if (ec) return Fail("No se pudo terminar la extracción del disco: " + ec.message());
    }
    else if (!GameOk())
    {
        discNow = true;
        FILE* f = FopenInput(in.discs[0].c_str(), "rb");
        uint64_t base;
        std::vector<DiscFile> files;
        if (!f || !DiscPartition(f, base) || !DiscList(f, base, files)) { if (f) fclose(f); return Fail("No se pudo leer la imagen del disco."); }
        std::string tmp = std::string(kGameDir) + ".part";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        uint64_t total = 0, done = 0;
        for (auto& df : files) total += df.size;
        Log("setup: extracting %zu files (%.2f GB) from %s", files.size(), double(total) / 1e9, in.discs[0].c_str());
        int lastPct = -1;
        for (auto& df : files)
        {
            if (df.path.find("..") != std::string::npos) continue;
            if (!CopyRange(f, df.offset, df.size, tmp + "/" + df.path)) { fclose(f); return Fail("Error extrayendo " + df.path + " (¿espacio libre?)."); }
            done += df.size;
            int pct = total ? int(done * 100 / total) : 100;
            if (pct != lastPct) { lastPct = pct; Report(pct * 80 / 100, "Extrayendo el disco: " + std::to_string(pct) + "%"); }
        }
        fclose(f);
        fs::create_directories(fs::path(kGameDir).parent_path(), ec);
        fs::rename(tmp, kGameDir, ec);
        if (ec) return Fail("No se pudo terminar la extracción del disco: " + ec.message());
    }
    for (auto& x : kExes)
        if (FileSha1(std::string(kGameDir) + "/" + x.xex) != x.xexSha)
        {
            std::error_code ec;
            if (discNow) fs::remove_all(kGameDir, ec);   // just extracted from a wrong image: do not keep 8 GB of it
            return Fail(std::string("El disco no es la versión soportada (") + x.xex + " distinto). Hace falta Call of Duty: "
                        "Black Ops III de Xbox 360 (la versión usada para el port)." + (discNow ? "" : " Borrá la carpeta extracted para instalar otra imagen."));
        }

    // 2. the title update. One already in place that is not the supported version (an earlier
    // attempt with another TU) is replaced, or it would block every later attempt.
    Report(82, "Instalando el Title Update");
    if (!TuOk() && fs::exists(kTuDir))
    {
        std::error_code ec;
        fs::remove_all(kTuDir, ec);
        Log("setup: the installed title update is not the supported one - replaced");
    }
    if (!TuOk() && IsDir(in.updates[0]))
    {
        std::string err;
        if (!PlaceDir(in.updates[0], kTuDir, err)) return Fail("No se pudo copiar la carpeta del Title Update: " + err);
        WriteContentHeader("000B0000", "tu00000001_00000000", 0xB0000, u"Title Update 1");
        Log("setup: title update installed (folder)");
    }
    else if (!TuOk())
    {
        Stfs s;
        std::string err, tmp = std::string(kTuDir) + ".part";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        if (!s.Open(in.updates[0]) || !s.Extract(tmp, err)) return Fail("No se pudo leer el Title Update: " + err);
        fs::rename(tmp, kTuDir, ec);
        if (ec) return Fail("No se pudo instalar el Title Update: " + ec.message());
        WriteContentHeader("000B0000", "tu00000001_00000000", 0xB0000, s.displayName);
        Log("setup: title update installed");
    }
    for (auto& x : kExes)
        if (FileSha1(std::string(kTuDir) + "/" + x.xexp) != x.xexpSha)
        {
            std::error_code ec;
            fs::remove_all(kTuDir, ec);   // not kept: the next attempt installs the chosen one
            return Fail(std::string("El Title Update no es la versión soportada (") + x.xexp + " distinto). Hace falta el Title Update "
                        "\"TU 8\" de Xbox 360: un archivo tu00000001_00000000 de 116.740.096 bytes (SHA-1 "
                        "5a0dae7c141baba68cb3bde8a93cd3ee4c7300f6).");
        }

    // 3. DLC (optional). The free Compatibility Packs get their console folder names whatever the
    // file is called (a file chosen on the phone arrives without its original name).
    Report(86, "Instalando los paquetes de contenido");
    for (auto& p : in.dlcs)
    {
        if (IsDir(p))
        {
            std::string name = fs::path(p).filename().string(), dst = std::string(kDlcDir) + "/" + name, err, disp = DlcDisplay(p);
            if (Exists(dst)) continue;
            if (!PlaceDir(p, dst, err)) { LogWarn("setup: DLC folder %s skipped: %s", name.c_str(), err.c_str()); continue; }
            std::u16string d16(disp.begin(), disp.end());
            WriteContentHeader("00000002", name, 2, d16);
            Log("setup: DLC %s installed (folder)", name.c_str());
            continue;
        }
        Stfs s;
        std::string err;
        std::error_code ec;
        if (!s.Open(p)) { LogWarn("setup: DLC %s unreadable", NameOf(p).c_str()); continue; }
        std::string disp;
        for (char16_t c : s.displayName) if (c < 128) disp += char(c);
        std::string name = NameOf(p);
        if (disp.find("Compatibility Pack 1") != std::string::npos) name = "A92CACF8DF6D9DB1C32970035A6B8B63B09C2F1641";
        else if (disp.find("Compatibility Pack 2") != std::string::npos) name = "4FD64D1A99FF002F54CEE0FE922808E1D2D21A5341";
        std::string dst = std::string(kDlcDir) + "/" + name;
        if (Exists(dst)) continue;
        if (!s.Extract(dst + ".part", err)) { LogWarn("setup: DLC %s skipped: %s", name.c_str(), err.c_str()); fs::remove_all(dst + ".part", ec); continue; }
        fs::rename(dst + ".part", dst, ec);
        WriteContentHeader("00000002", name, 2, s.displayName);
        Log("setup: DLC %s installed", name.c_str());
    }

    // the free Compatibility Packs are required content: without them the game stops at "you need to
    // download new required content" and goes back to the title screen
    static const struct { const char* folder; const char* name; } kRequired[] = {
        { "A92CACF8DF6D9DB1C32970035A6B8B63B09C2F1641", "Compatibility Pack 1" },
        { "4FD64D1A99FF002F54CEE0FE922808E1D2D21A5341", "Compatibility Pack 2" } };
    std::string missing;
    for (auto& r : kRequired)
        if (!Exists(std::string(kDlcDir) + "/" + r.folder)) missing += std::string("\n- ") + r.name + " (" + r.folder + ")";
    if (!missing.empty())
        return Fail("Faltan los paquetes de compatibilidad (gratis en Xbox Live, el juego no arranca sin ellos):" + missing +
                    "\n\nCopialos desde Content/0000000000000000/4156091D/00000002/ de tu consola, junto al resto, y volvé a abrir el juego.");

    // 4. the executables with the title update applied
    Report(92, "Aplicando el Title Update a los ejecutables");
    for (auto& x : kExes)
    {
        std::string out = std::string("private/") + x.kind + "/default_patched.xex";
        if (Exists(out) && FileSha1(out) == x.patchedSha) continue;
        std::vector<uint8_t> xex, patch, patched;
        if (!ReadFileBytes(std::string(kGameDir) + "/" + x.xex, xex) || !ReadFileBytes(std::string(kTuDir) + "/" + x.xexp, patch))
            return Fail(std::string("No se pudo leer ") + x.xex);
        auto r = XexPatcher::apply(xex.data(), xex.size(), patch.data(), patch.size(), patched, false);
        if (r != XexPatcher::Result::Success) return Fail(std::string("No se pudo aplicar el Title Update a ") + x.xex + " (" + std::to_string(int(r)) + ").");
        if (!WriteFileBytes(out, patched.data(), patched.size())) return Fail("No se pudo escribir " + out);
        if (FileSha1(out) != x.patchedSha) return Fail(std::string("El ejecutable resultante no coincide (") + x.kind + ").");
        Log("setup: %s ready", out.c_str());
    }
    Report(100, "Instalación terminada");
    if (!g_report) Tell("Instalación terminada. Ya podés borrar la imagen del disco y los paquetes originales si querés.");
    return true;
}

bool SetupInstalled()
{
    return GameOk() && TuOk() && PatchedOk() && Exists(std::string(kDlcDir) + "/A92CACF8DF6D9DB1C32970035A6B8B63B09C2F1641") &&
           Exists(std::string(kDlcDir) + "/4FD64D1A99FF002F54CEE0FE922808E1D2D21A5341");
}

SetupProbe SetupProbePath(const std::string& path)
{
    Inputs in;
    Classify(in, path);
    SetupProbe r;
    r.discs = int(in.discs.size());
    r.updates = int(in.updates.size());
    r.dlcs = int(in.dlcs.size());
    for (auto& p : in.dlcs)
    {
        std::string n = DlcDisplay(p);
        if (n.find("Compatibility Pack 1") != std::string::npos) r.cp1 = true;
        if (n.find("Compatibility Pack 2") != std::string::npos) r.cp2 = true;
    }
    return r;
}

void SetupInstalledParts(bool out[4])
{
    out[0] = GameOk();
    out[1] = TuOk();
    out[2] = Exists(std::string(kDlcDir) + "/" + kCp1);
    out[3] = Exists(std::string(kDlcDir) + "/" + kCp2);
}

bool SetupLocalComplete()
{
    bool part[4];
    SetupInstalledParts(part);
    if (part[0] && part[1] && part[2] && part[3]) return true;
    Inputs in = FindInputs();
    return (part[0] || !in.discs.empty()) && (part[1] || !in.updates.empty()) &&
           (part[2] || CountDlcNamed(in, "Compatibility Pack 1") > 0) && (part[3] || CountDlcNamed(in, "Compatibility Pack 2") > 0);
}

// Android installer screen: the four files the player chose (disc image, Title Update, Compatibility
// Pack 1 and 2), each { path, display name }; the current directory is the app's data folder.
bool SetupGameDataFrom(const std::vector<std::pair<std::string, std::string>>& files, SetupReport report)
{
    g_report = report;
    g_names.clear();
    Inputs in;
    for (auto& f : files)
    {
        g_names[f.first] = f.second;
        size_t before = in.discs.size() + in.updates.size() + in.dlcs.size();
        Classify(in, f.first);
        if (in.discs.size() + in.updates.size() + in.dlcs.size() == before)
        {
            bool ok = Fail("\"" + f.second + "\" no es un archivo de Black Ops III para Xbox 360 (ni imagen de disco, ni Title Update, ni paquete de contenido).");
            g_report = nullptr;
            return ok;
        }
    }
    Log("setup: chosen: %zu disc image(s), %zu title update(s), %zu DLC package(s)", in.discs.size(), in.updates.size(), in.dlcs.size());
    bool ok;
    if (in.discs.empty() && !GameOk()) ok = Fail("Falta la imagen del disco (.iso o .zar).");
    else if (in.updates.empty() && !TuOk()) ok = Fail("Falta el Title Update.");
    else if (!Exists(std::string(kDlcDir) + "/A92CACF8DF6D9DB1C32970035A6B8B63B09C2F1641") && CountDlcNamed(in, "Compatibility Pack 1") < 1)
        ok = Fail("Falta el Compatibility Pack 1 (o el archivo elegido no es ese paquete).");
    else if (!Exists(std::string(kDlcDir) + "/4FD64D1A99FF002F54CEE0FE922808E1D2D21A5341") && CountDlcNamed(in, "Compatibility Pack 2") < 1)
        ok = Fail("Falta el Compatibility Pack 2 (o el archivo elegido no es ese paquete).");
    else ok = Install(in);
    g_report = nullptr;
    return ok;
}

#ifdef __ANDROID__
#include <jni.h>
#include <SDL3/SDL_system.h>
namespace
{
// com.bo3recomp.Storage.hasAccess() / request(): MANAGE_EXTERNAL_STORAGE (Android 11+)
bool AndroidStorageAccess(bool ask)
{
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    if (!env) return false;
    jclass cls = env->FindClass("com/bo3recomp/Storage");
    if (!cls) { env->ExceptionClear(); return false; }
    jmethodID has = env->GetStaticMethodID(cls, "hasAccess", "()Z");
    jmethodID req = env->GetStaticMethodID(cls, "request", "()V");
    bool ok = has && env->CallStaticBooleanMethod(cls, has);
    if (!ok && ask && req)
    {
        env->CallStaticVoidMethod(cls, req);
        for (int i = 0; i < 600 && !ok; i++)   // up to 5 minutes in the settings screen
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ok = env->CallStaticBooleanMethod(cls, has);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return ok;
}
}
#endif
