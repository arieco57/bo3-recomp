// bo3prep: prepare an Xbox 360 XEX for XenonRecomp.
//
//   bo3prep <in.xex> <in.xexp|-> <out_dir>
//
// - applies the title-update delta patch (.xexp), writes <out_dir>/default_patched.xex
// - loads (decrypts + decompresses) the image and dumps it to <out_dir>/image.bin
// - writes <out_dir>/info.txt  : base, size, entry point, sections
// - writes <out_dir>/imports.txt: kernel/xam import thunks (what the runtime must implement)
// - writes <out_dir>/recomp_addresses.toml: register save/restore helpers found by byte pattern
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <image.h>
#include <xex.h>
#include <xex_patcher.h>

static std::vector<uint8_t> loadFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

struct Pattern
{
    const char* key;
    std::vector<uint32_t> words; // consecutive big-endian instruction words
};

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: bo3prep <in.xex> <in.xexp|-> <out_dir>\n");
        return 1;
    }
    std::string outDir = argv[3];

    auto xex = loadFile(argv[1]);
    if (xex.empty()) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

    if (strcmp(argv[2], "-") != 0)
    {
        auto patch = loadFile(argv[2]);
        if (patch.empty()) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        std::vector<uint8_t> out;
        auto r = XexPatcher::apply(xex.data(), xex.size(), patch.data(), patch.size(), out, false);
        if (r != XexPatcher::Result::Success)
        {
            fprintf(stderr, "patch failed (XexPatcher::Result %d)\n", int(r));
            return 2;
        }
        xex.swap(out);
        std::ofstream(outDir + "/default_patched.xex", std::ios::binary)
            .write((const char*)xex.data(), xex.size());
        printf("patched XEX written (%zu bytes)\n", xex.size());
    }

    Image img = Image::ParseImage(xex.data(), xex.size());
    if (!img.data) { fprintf(stderr, "image load failed\n"); return 3; }

    std::ofstream(outDir + "/image.bin", std::ios::binary).write((const char*)img.data.get(), img.size);

    FILE* info = fopen((outDir + "/info.txt").c_str(), "w");
    fprintf(info, "base        0x%08zX\nsize        0x%08X\nentry_point 0x%08zX\n\nsections:\n", img.base, img.size, img.entry_point);
    for (auto& s : img.sections)
    {
        // PE section names are 8 bytes, not necessarily NUL-terminated: keep printable chars only.
        std::string nm;
        for (size_t i = 0; i < s.name.size() && i < 8 && s.name[i]; i++)
            nm += (s.name[i] > 0x20 && s.name[i] < 0x7F) ? s.name[i] : '_';
        fprintf(info, "  %-10s 0x%08zX  size 0x%08X  %s\n", nm.c_str(), s.base, s.size,
            (s.flags & SectionFlags_Code) ? "CODE" : "DATA");
    }
    fclose(info);

    FILE* imp = fopen((outDir + "/imports.txt").c_str(), "w");
    size_t nimp = 0;
    for (auto& sym : img.symbols)
        if (sym.type == Symbol_Function) { fprintf(imp, "0x%08zX  %s\n", sym.address, sym.name.c_str()); nimp++; }
    fclose(imp);

    // Two-word signatures to avoid false positives from inlined prologues/epilogues.
    const Pattern pats[] = {
        { "restgprlr_14_address", { 0xE9C1FF68, 0xE9E1FF70 } }, // ld  r14,-0x98(r1); ld  r15,-0x90(r1)
        { "savegprlr_14_address", { 0xF9C1FF68, 0xF9E1FF70 } }, // std r14,-0x98(r1); std r15,-0x90(r1)
        { "restfpr_14_address",   { 0xC9CCFF70, 0xC9ECFF78 } }, // lfd f14,-0x90(r12); lfd f15,-0x88(r12)
        { "savefpr_14_address",   { 0xD9CCFF70, 0xD9ECFF78 } }, // stfd f14,-0x90(r12); stfd f15,-0x88(r12)
        { "restvmx_14_address",   { 0x3960FEE0, 0x7DCB60CE } }, // li r11,-0x120; lvx v14,r11,r12
        { "savevmx_14_address",   { 0x3960FEE0, 0x7DCB61CE } }, // li r11,-0x120; stvx v14,r11,r12
        { "restvmx_64_address",   { 0x3960FC00, 0x100B60CB } }, // li r11,-0x400; lvx128 v64,r11,r12
        { "savevmx_64_address",   { 0x3960FC00, 0x100B61CB } }, // li r11,-0x400; stvx128 v64,r11,r12
    };

    std::map<std::string, std::vector<size_t>> hits;
    for (auto& s : img.sections)
    {
        if (!(s.flags & SectionFlags_Code)) continue;
        for (uint32_t off = 0; off + 8 <= s.size; off += 4)
        {
            const uint8_t* p = s.data + off;
            for (auto& pat : pats)
            {
                bool ok = true;
                for (size_t i = 0; i < pat.words.size() && ok; i++)
                    ok = (off + 4 * (i + 1) <= s.size) && be32(p + 4 * i) == pat.words[i];
                if (ok) hits[pat.key].push_back(s.base + off);
            }
        }
    }

    FILE* t = fopen((outDir + "/recomp_addresses.toml").c_str(), "w");
    fprintf(t, "# Auto-detected by bo3prep (first match). Review any key with multiple candidates.\n");
    for (auto& pat : pats)
    {
        auto& v = hits[pat.key];
        if (v.empty())
            fprintf(t, "# %s = NOT FOUND\n", pat.key);
        else
        {
            fprintf(t, "%s = 0x%08zX", pat.key, v[0]);
            if (v.size() > 1)
            {
                fprintf(t, "  # %zu candidates:", v.size());
                for (auto a : v) fprintf(t, " 0x%08zX", a);
            }
            fprintf(t, "\n");
        }
        printf("%-22s %zu match(es)%s\n", pat.key, v.size(), v.empty() ? "  <-- MISSING" : "");
    }
    fclose(t);

    printf("base 0x%08zX size 0x%08X entry 0x%08zX, %zu sections, %zu import thunks\n",
        img.base, img.size, img.entry_point, img.sections.size(), nimp);
    return 0;
}
