#include "loader.h"
#include "guest.h"
#include "kernel_vars.h"

#include <fstream>
#include <map>
#include <unordered_map>

#include <image.h>
#include <xex.h>

LoadedModule g_module;

namespace
{
struct ExportInfo { const char* name; bool variable; };

#define XE_EXPORT(lib, ord, name, kind) { ord, { #name, kind == kVariable } }
constexpr int kFunction = 0, kVariable = 1;
const std::unordered_map<uint32_t, ExportInfo> kKernelExports = {
#include "xbox/xboxkrnl_table.inc"
};
const std::unordered_map<uint32_t, ExportInfo> kXamExports = {
#include "xbox/xam_table.inc"
};
#undef XE_EXPORT

uint32_t be32(const void* p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }

const uint8_t* OptHeader(uint32_t key)
{
    return (const uint8_t*)getOptHeaderPtr(g_module.xexFile.data(), key);
}
}

uint32_t XexHeaderFieldGuest(uint32_t key)
{
    const auto* hdr = (const Xex2Header*)g_module.xexFile.data();
    const auto* opts = (const Xex2OptHeader*)(hdr + 1);
    for (uint32_t i = 0; i < hdr->headerCount; i++)
    {
        if (opts[i].key != key) continue;
        switch (key & 0xFF)
        {
        case 0x00: return opts[i].value;                                    // the value itself
        case 0x01: return g_module.xexHeaderGuest + uint32_t((const uint8_t*)&opts[i].value - g_module.xexFile.data());
        default:   return g_module.xexHeaderGuest + opts[i].offset;          // pointer to the data
        }
    }
    return 0;
}

void LoadModule(const std::string& path)
{
    g_module.path = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) Fatal("cannot open %s", path.c_str());
    g_module.xexFile.assign(std::istreambuf_iterator<char>(f), {});

    // Decrypt/decompress via XenonUtils (same loader the recompiler used).
    Image img = Image::ParseImage(g_module.xexFile.data(), g_module.xexFile.size());
    if (!img.data) Fatal("cannot parse XEX %s", path.c_str());
    g_module.imageBase = uint32_t(img.base);
    g_module.imageSize = img.size;
    g_module.entryPoint = uint32_t(img.entry_point);
    if (g_module.imageBase != PPC_IMAGE_BASE || g_module.imageSize != PPC_IMAGE_SIZE)
        Fatal("XEX image (0x%X/0x%X) does not match the recompiled code (0x%llX/0x%llX) - wrong executable?",
              g_module.imageBase, g_module.imageSize, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
    memcpy(g_base + img.base, img.data.get(), img.size);
    Log("loader: image 0x%08X..0x%08X, entry 0x%08X", g_module.imageBase, g_module.imageBase + g_module.imageSize, g_module.entryPoint);

    // Optional headers we care about.
    if (auto p = OptHeader(XEX_HEADER_DEFAULT_STACK_SIZE)) g_module.stackSize = be32(p);
    if (auto p = OptHeader(XEX_HEADER_DEFAULT_HEAP_SIZE)) g_module.heapSize = be32(p);
    if (auto p = OptHeader(XEX_HEADER_TLS_INFO))
    {
        g_module.tlsSlotCount = be32(p);
        g_module.tlsRawAddress = be32(p + 4);
        g_module.tlsDataSize = be32(p + 8);
        g_module.tlsRawSize = be32(p + 12);
    }
    if (auto p = OptHeader(XEX_HEADER_EXECUTION_INFO)) g_module.titleId = be32(p + 0xC);
    Log("loader: title %08X, stack 0x%X, TLS slots %u data 0x%X (raw 0x%X @ 0x%08X)", g_module.titleId,
        g_module.stackSize, g_module.tlsSlotCount, g_module.tlsDataSize, g_module.tlsRawSize, g_module.tlsRawAddress);

    // XEX header copy + loader data table entry in guest memory.
    const auto* hdr = (const Xex2Header*)g_module.xexFile.data();
    uint32_t headerSize = hdr->headerSize;
    g_module.xexHeaderGuest = GuestAlloc(headerSize);
    memcpy(g_base + g_module.xexHeaderGuest, g_module.xexFile.data(), headerSize);

    uint32_t ldr = GuestAlloc(0x100);
    g_module.ldrEntryGuest = ldr;
    GuestWrite32(ldr + 0x00, ldr); GuestWrite32(ldr + 0x04, ldr);   // empty list links
    GuestWrite32(ldr + 0x18, g_module.imageBase);                   // dll_base
    GuestWrite32(ldr + 0x1C, g_module.imageBase);                   // image_base
    GuestWrite32(ldr + 0x20, g_module.imageSize);
    GuestWrite32(ldr + 0x3C, g_module.entryPoint);
    GuestWrite32(ldr + 0x58, g_module.xexHeaderGuest);              // xex_header_base

    // Imports. XenonUtils already replaced function thunks (type 1) with nop/blr and left the
    // type-0 records byte-swapped to host order, so we resolve those here.
    auto* imports = (const Xex2ImportHeader*)OptHeader(XEX_HEADER_IMPORT_LIBRARIES);
    if (!imports) return;
    std::vector<std::string> libs;
    const char* strTab = (const char*)(imports + 1);
    for (uint32_t i = 0, off = 0; i < imports->numImports; i++)
    {
        libs.emplace_back(strTab + off);
        off += ((libs.back().size() + 1) + 3) & ~3u;
    }

    auto* lib = (const Xex2ImportLibrary*)((const char*)imports + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
    size_t nVar = 0, nIat = 0;
    for (size_t li = 0; li < libs.size(); li++)
    {
        const bool isXam = libs[li] == "xam.xex";
        const auto& table = isXam ? kXamExports : kKernelExports;
        const auto* desc = (const Xex2ImportDescriptor*)(lib + 1);

        // XenonUtils overwrote type-1 thunks with code (nop,nop,nop,blr), losing their ordinal.
        // Function imports come as [IAT record (type 0), thunk (type 1)] descriptor pairs, so the
        // thunk of a type-0 function record is the following descriptor (checked below).
        for (uint32_t i = 0; i < lib->numberOfImports; i++)
        {
            uint32_t slot = desc[i].firstThunk;
            uint32_t raw;
            memcpy(&raw, g_base + slot, 4);
            if (raw == 0x00000060) continue;   // a function thunk (code)
            uint32_t ordinal = raw & 0xFFFF;
            uint32_t type = raw >> 24;
            if (type != 0) { LogWarn("import %s #%u: unexpected record type %u", libs[li].c_str(), ordinal, type); continue; }
            auto ex = table.find(ordinal);
            const char* name = ex != table.end() ? ex->second.name : "?";
            if (ex != table.end() && !ex->second.variable)
            {
                // IAT entry of a function: point it at the thunk (the next descriptor for this ordinal).
                uint32_t thunk = 0;
                if (i + 1 < lib->numberOfImports)
                {
                    uint32_t nslot = desc[i + 1].firstThunk, nraw;
                    memcpy(&nraw, g_base + nslot, 4);
                    if (nraw == 0x00000060) thunk = nslot;
                }
                if (!thunk) LogWarn("import %s %s: IAT record without following thunk", libs[li].c_str(), name);
                GuestWrite32(slot, thunk);
                nIat++;
                continue;
            }
            uint32_t var = KernelVariable(libs[li].c_str(), name, ordinal);
            GuestWrite32(slot, var);
            nVar++;
        }
        lib = (const Xex2ImportLibrary*)((const char*)(lib + 1) + lib->numberOfImports * sizeof(Xex2ImportDescriptor));
    }
    Log("loader: %zu kernel variables, %zu function IAT entries resolved", nVar, nIat);
}
