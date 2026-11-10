// Demonware emulation (online mode): shared state of the signed-in session.
#pragma once
#include <cstdint>
#include <string>

struct DwSession
{
    bool authenticated = false;
    uint32_t titleId = 0, ivSeed = 0;
    uint64_t userId = 0;
    std::string userName;
    uint8_t sessionKey[24] = {};
};

DwSession DwCurrentSession();
