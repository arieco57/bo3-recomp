// ZArchive (.zar) disc images (zar.cpp): detection and extraction.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

bool ZarIsArchive(const std::string& path);   // footer magic/version check (cheap)
// Extracts every file of the archive into outDir. progress(done, total) in bytes.
bool ZarExtract(const std::string& path, const std::string& outDir, std::string& err,
                std::function<void(uint64_t, uint64_t)> progress = nullptr);
