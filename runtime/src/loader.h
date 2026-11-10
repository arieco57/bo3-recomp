#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct LoadedModule
{
    std::string path;
    std::vector<uint8_t> xexFile;   // patched XEX (host copy)
    uint32_t imageBase = 0, imageSize = 0, entryPoint = 0;
    uint32_t stackSize = 0x40000;   // XEX_HEADER_DEFAULT_STACK_SIZE
    uint32_t heapSize = 0;
    // TLS (XEX_HEADER_TLS_INFO)
    uint32_t tlsSlotCount = 0, tlsRawAddress = 0, tlsDataSize = 0, tlsRawSize = 0;
    // Guest-side structures
    uint32_t xexHeaderGuest = 0;    // copy of the XEX header in guest memory
    uint32_t ldrEntryGuest = 0;     // X_LDR_DATA_TABLE_ENTRY
    uint32_t titleId = 0;
};

extern LoadedModule g_module;

void LoadModule(const std::string& patchedXexPath);

// Address of the guest copy of an optional XEX header field (for RtlImageXexHeaderField).
uint32_t XexHeaderFieldGuest(uint32_t key);
