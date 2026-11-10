// Offline check of the Xenos shader translator against dumped microcode
// (BO3_DUMP_SHADERS=dir build/bo3_mp). Usage: shader_test <file.bin>... [-v]
#include "gpu/shader_translator.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv)
{
    bool verbose = false;
    int ok = 0, fail = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-v")) { verbose = true; continue; }
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<char> raw(std::istreambuf_iterator<char>(f), {});
        std::vector<uint32_t> code(raw.size() / 4);
        memcpy(code.data(), raw.data(), code.size() * 4);
        bool pixel = strstr(argv[i], "/ps_") || !strncmp(argv[i], "ps_", 3);
        auto t = TranslateShader(pixel, code.data(), uint32_t(code.size()));
        if (t.error.empty()) ok++; else fail++;
        printf("%s %s: %zu dwords -> %zu GLSL bytes, %zu SPIR-V words, tex 0x%X, exports 0x%X%s%s\n",
               t.error.empty() ? "OK  " : "FAIL", argv[i], code.size(), t.glsl.size(), t.spirv.size(), t.texturesUsed,
               t.colorExports, t.kills ? " kill" : "", t.memexports ? " MEMEXPORT" : "");
        if (!t.error.empty()) printf("%s\n", t.error.c_str());
        if (verbose || !t.error.empty())
        {
            std::string out = std::string(argv[i]) + ".glsl";
            std::ofstream(out) << t.glsl;
        }
    }
    printf("%d ok, %d failed\n", ok, fail);
    return fail ? 1 : 0;
}
