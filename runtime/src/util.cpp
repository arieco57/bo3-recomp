#include "guest.h"

std::string GuestString(uint32_t addr)
{
    if (!addr) return {};
    return std::string((const char*)(g_base + addr));
}

std::string GuestAnsiString(uint32_t p)
{
    if (!p) return {};
    uint16_t len = GuestRead16(p);
    uint32_t buf = GuestRead32(p + 4);
    if (!buf) return {};
    return std::string((const char*)(g_base + buf), len);
}

std::u16string GuestWString(uint32_t addr)
{
    std::u16string s;
    if (!addr) return s;
    for (uint16_t c; (c = GuestRead16(addr)) != 0; addr += 2) s.push_back(char16_t(c));
    return s;
}

std::string Utf16ToUtf8(const std::u16string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size())
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}
