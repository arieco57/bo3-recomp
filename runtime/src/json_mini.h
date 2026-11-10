// Tiny JSON helpers for the Supabase replies (flat objects, no nested escapes needed).
#pragma once
#include <string>
#include <vector>

namespace json_mini
{
inline std::string JsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

// "key":"value" of a flat JSON object (no escapes in the values we use)
inline std::string JsonField(const std::string& obj, const std::string& key)
{
    std::string k = "\"" + key + "\"";
    size_t p = obj.find(k);
    if (p == std::string::npos) return {};
    p = obj.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = obj.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos) return {};
    if (obj[p] != '"')
    {
        size_t e = obj.find_first_of(",}", p);
        return obj.substr(p, e - p);
    }
    size_t e = p + 1;
    while (e < obj.size() && obj[e] != '"') { if (obj[e] == '\\') e++; e++; }
    std::string v = obj.substr(p + 1, e - p - 1), o;
    for (size_t i = 0; i < v.size(); i++) { if (v[i] == '\\' && i + 1 < v.size()) i++; o += v[i]; }
    return o;
}

// objects of a top-level JSON array
inline std::vector<std::string> JsonObjects(const std::string& arr)
{
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    bool str = false;
    for (size_t i = 0; i < arr.size(); i++)
    {
        char c = arr[i];
        if (str) { if (c == '\\') i++; else if (c == '"') str = false; continue; }
        if (c == '"') str = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}') { if (--depth == 0) out.push_back(arr.substr(start, i - start + 1)); }
    }
    return out;
}

}
