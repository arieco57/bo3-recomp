// ppcdis: disassemble a range of a bo3prep image.bin
//   ppcdis <image.bin> <image_base_hex> <start_hex> <size_hex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <disasm.h>

int main(int argc, char** argv)
{
    if (argc < 5) { fprintf(stderr, "usage: ppcdis <image.bin> <base> <start> <size>\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> img(std::istreambuf_iterator<char>(f), {});
    uint64_t base = strtoull(argv[2], nullptr, 16), start = strtoull(argv[3], nullptr, 16), size = strtoull(argv[4], nullptr, 16);
    for (uint64_t a = start; a < start + size && a - base + 4 <= img.size(); a += 4)
    {
        ppc_insn insn{};
        const uint8_t* p = img.data() + (a - base);
        ppc::Disassemble(p, a, insn);
        printf("%08llX  %02X%02X%02X%02X  %-8s %s\n", (unsigned long long)a, p[0], p[1], p[2], p[3],
            insn.opcode ? insn.opcode->name : ".long", insn.op_str);
    }
}
