// ZArchive (.zar) reader: the disc image format of the game's copy (format by Exzap, used by Cemu
// and Xenia packs). Port of tools/zar_extract.py, which is the reference for the layout:
//   [compressed data: 64 KiB blocks, each zstd-compressed or stored raw]
//   [offset records: { u64 baseOffset; u16 sizeMinus1[16]; } per 16 blocks]
//   [names table] [file tree: 16-byte entries] [footer: 6 x {u64 offset,u64 size}, u8 sha256[32],
//    u64 totalSize, u32 version, u32 magic]
// All integers are big-endian. Desktop only (zstd is the system library); Android has no .zar input.
#include "zar.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#ifndef __ANDROID__
#include <zstd.h>
#endif

namespace fs = std::filesystem;

namespace
{
constexpr uint32_t kMagic = 0x169F52D6, kVersion = 0x61BF3A01;
constexpr uint64_t kBlock = 64 * 1024;
constexpr uint32_t kPerRec = 16;
constexpr uint64_t kFooterSize = 6 * 16 + 32 + 8 + 4 + 4;   // 144

uint64_t BE64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct Reader
{
    FILE* f = nullptr;
    uint64_t size = 0;
    uint64_t dataOff = 0;
    struct Rec { uint64_t base; uint16_t sizes[kPerRec]; };
    std::vector<Rec> records;
    std::vector<uint8_t> names, tree;
    std::vector<uint8_t> cache;
    int64_t cacheIdx = -1;
    std::vector<uint8_t> raw, out;

    bool ReadAt(uint64_t off, std::vector<uint8_t>& buf, size_t n)
    {
        buf.resize(n);
        return fseeko(f, off_t(off), SEEK_SET) == 0 && (n == 0 || fread(buf.data(), 1, n, f) == n);
    }

    std::string Name(uint32_t off) const
    {
        if (off == 0x7FFFFFFF) return {};
        uint32_t ln;
        if (names[off] & 0x80) { ln = (names[off] & 0x7F) | uint32_t(names[off + 1]) << 7; off += 2; }
        else { ln = names[off]; off += 1; }
        return std::string(reinterpret_cast<const char*>(names.data() + off), ln);
    }

    // one 64 KiB block, decompressed (cached: files are read in order)
    bool Block(uint64_t idx, std::vector<uint8_t>& data)
    {
        if (int64_t(idx) == cacheIdx) { data = cache; return true; }
        const Rec& r = records[idx / kPerRec];
        uint32_t sub = uint32_t(idx % kPerRec);
        uint64_t off = r.base;
        for (uint32_t k = 0; k < sub; k++) off += uint64_t(r.sizes[k]) + 1;
        uint64_t csize = uint64_t(r.sizes[sub]) + 1;
        if (!ReadAt(dataOff + off, raw, size_t(csize))) return false;
        if (csize == kBlock) data = raw;
        else
        {
#ifndef __ANDROID__
            data.resize(kBlock);
            size_t n = ZSTD_decompress(data.data(), kBlock, raw.data(), raw.size());
            if (ZSTD_isError(n)) return false;
            data.resize(n);
#else
            return false;
#endif
        }
        cache = data;
        cacheIdx = int64_t(idx);
        return true;
    }

    bool ReadRange(uint64_t offset, uint64_t n, FILE* o)
    {
        while (n > 0)
        {
            uint64_t idx = offset / kBlock, inner = offset % kBlock;
            if (!Block(idx, out) || inner >= out.size()) return false;
            size_t k = size_t(std::min<uint64_t>(n, out.size() - inner));
            if (fwrite(out.data() + inner, 1, k, o) != k) return false;
            offset += k;
            n -= k;
        }
        return true;
    }
};

struct Walker
{
    Reader& r;
    const std::string& outDir;
    std::function<void(const std::string&, uint64_t)> onFile;
    std::string err;

    bool Walk(uint32_t idx, const std::string& prefix)
    {
        const uint8_t* e = r.tree.data() + size_t(idx) * 16;
        uint32_t tn = BE32(e);
        std::string name = r.Name(tn & 0x7FFFFFFF);
        std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (tn & 0x80000000)   // file
        {
            uint64_t off = (uint64_t(BE16(e + 14)) << 32) | BE32(e + 4);
            uint64_t size = (uint64_t(BE16(e + 12)) << 32) | BE32(e + 8);
            fs::path dst = fs::path(outDir) / path;
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            FILE* o = fopen(dst.c_str(), "wb");
            if (!o) { err = "cannot write " + dst.string(); return false; }
            bool ok = r.ReadRange(off, size, o);
            ok = fclose(o) == 0 && ok;
            if (!ok) { err = "reading " + path; return false; }
            onFile(path, size);
            return true;
        }
        uint32_t start = BE32(e + 4), count = BE32(e + 8);
        if (idx != 0) { std::error_code ec; fs::create_directories(fs::path(outDir) / path, ec); }
        for (uint32_t c = start; c < start + count; c++)
            if (!Walk(c, path)) return false;
        return true;
    }
};
}

bool ZarIsArchive(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t ft[kFooterSize];
    bool ok = fseeko(f, -off_t(kFooterSize), SEEK_END) == 0 && fread(ft, 1, sizeof(ft), f) == sizeof(ft);
    fclose(f);
    return ok && BE32(ft + 140) == kMagic && BE32(ft + 136) == kVersion;
}

bool ZarExtract(const std::string& path, const std::string& outDir, std::string& err, std::function<void(uint64_t, uint64_t)> progress)
{
#ifdef __ANDROID__
    err = "los archivos .zar no se soportan en Android (usá la imagen .iso o instalá desde Linux)";
    return false;
#else
    Reader r;
    r.f = fopen(path.c_str(), "rb");
    if (!r.f) { err = "no se pudo abrir " + path; return false; }
    struct Close { FILE* f; ~Close() { fclose(f); } } closer{ r.f };
    fseeko(r.f, 0, SEEK_END);
    r.size = uint64_t(ftello(r.f));
    if (r.size < kFooterSize) { err = "archivo demasiado chico"; return false; }
    std::vector<uint8_t> ft;
    if (!r.ReadAt(r.size - kFooterSize, ft, kFooterSize)) { err = "no se pudo leer el final del archivo"; return false; }
    if (BE32(ft.data() + 140) != kMagic || BE32(ft.data() + 136) != kVersion) { err = "no es un archivo ZArchive"; return false; }
    if (BE64(ft.data() + 128) != r.size) { err = "el tamaño no coincide con el archivo (¿descarga incompleta?)"; return false; }
    struct Sec { uint64_t off, size; } secs[6];
    for (int i = 0; i < 6; i++) secs[i] = { BE64(ft.data() + i * 16), BE64(ft.data() + i * 16 + 8) };
    r.dataOff = secs[0].off;

    std::vector<uint8_t> recRaw;
    if (!r.ReadAt(secs[1].off, recRaw, size_t(secs[1].size))) { err = "tabla de bloques ilegible"; return false; }
    for (size_t i = 0; i + 40 <= recRaw.size(); i += 40)
    {
        Reader::Rec rec{};
        rec.base = BE64(recRaw.data() + i);
        for (uint32_t k = 0; k < kPerRec; k++) rec.sizes[k] = BE16(recRaw.data() + i + 8 + 2 * k);
        r.records.push_back(rec);
    }
    if (!r.ReadAt(secs[2].off, r.names, size_t(secs[2].size)) || !r.ReadAt(secs[3].off, r.tree, size_t(secs[3].size)))
    { err = "índice del archivo ilegible"; return false; }

    // total size of the files, for the progress
    uint64_t total = 0, done = 0;
    std::function<void(uint32_t)> sum = [&](uint32_t idx) {
        const uint8_t* e = r.tree.data() + size_t(idx) * 16;
        if (BE32(e) & 0x80000000) { total += (uint64_t(BE16(e + 12)) << 32) | BE32(e + 8); return; }
        uint32_t start = BE32(e + 4), count = BE32(e + 8);
        for (uint32_t c = start; c < start + count; c++) sum(c);
    };
    sum(0);

    std::error_code ec;
    fs::create_directories(outDir, ec);
    Walker w{ r, outDir, [&](const std::string&, uint64_t n) { done += n; if (progress) progress(done, total); }, {} };
    if (!w.Walk(0, "")) { err = w.err; return false; }
    return true;
#endif
}
