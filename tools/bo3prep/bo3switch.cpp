// bo3switch: jump-table detector for XenonRecomp, tolerant of instruction scheduling.
//
//   bo3switch <patched.xex> <out_switch_tables.toml>
//
// XenonAnalyse matches fixed opcode sequences (tuned for Sonic Unleashed). Black Ops III's
// compiler interleaves/reorders the table setup and inserts nops, e.g.
//     cmplwi cr6,r11,N ; bgt cr6,default
//     lis r12,T@ha ; rlwinm r0,r11,1,0,30 ; addi r12,r12,T@l ; lhzx r0,r12,r0
//     lis r12,B@ha ; addi r12,r12,B@l ; nop ; add r12,r12,r0 ; mtctr r12 ; bctr
// so instead we do a small forward symbolic evaluation of the registers between the bounds
// check and the `mtctr/bctr`, and accept a table only if every target lands inside the same
// function (according to .pdata).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <image.h>
#include <file.h>

static uint32_t be32(const void* p)
{
    auto b = (const uint8_t*)p;
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

enum class VK { Unknown, Const, Index, Load, Target };

struct Val
{
    VK k = VK::Unknown;
    uint32_t c = 0;      // Const value / Target base
    uint32_t scale = 1;  // Index scale
    uint32_t lo = 0;     // Index: constant offset added after the bounds check (values lo..lo+n-1)
    int src = -1;        // Index/Load/Target: register the raw index originally came from
    uint32_t width = 0;  // Load width (1,2,4)
    bool sgn = false;    // Load sign-extended (lhax)
    uint32_t table = 0;  // Load table address
    uint32_t shift = 0;  // Load shift (applied after load)
    bool absolute = false;
};

struct Func { uint32_t begin, size; };

struct Ctx
{
    Image img;
    std::vector<Func> funcs;
    const Section* text = nullptr;

    const Func* owner(uint32_t a) const
    {
        size_t lo = 0, hi = funcs.size();
        while (lo < hi)
        {
            size_t m = (lo + hi) / 2;
            if (funcs[m].begin <= a) lo = m + 1; else hi = m;
        }
        if (lo == 0) return nullptr;
        const Func& f = funcs[lo - 1];
        return (a < f.begin + f.size) ? &f : nullptr;
    }

    std::optional<uint32_t> word(uint32_t a) const
    {
        auto p = img.Find(a);
        if (!p) return std::nullopt;
        return be32(p);
    }
};

static bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Which GPR an instruction writes (−1 = none or unknown-but-harmless). Conservative.
static int gprWritten(uint32_t i)
{
    uint32_t op = i >> 26;
    uint32_t rD = (i >> 21) & 31, rA = (i >> 16) & 31;
    switch (op)
    {
    case 7: case 8: case 12: case 13: case 14: case 15: // mulli subfic addic addic. addi addis
        return rD;
    case 24: case 25: case 26: case 27: case 28: case 29: // ori oris xori xoris andi. andis.
    case 20: case 21: case 23: // rlwimi rlwinm rlwnm
    case 30: // rld*
        return rA;
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43: case 58: // loads
        return rD;
    case 36: case 37: case 38: case 39: case 44: case 45: case 46: case 47: case 62: // stores
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: // fp loads/stores
    case 10: case 11: // cmpli cmpi
    case 16: case 18: case 19: case 17: // branches / cr ops / sc
    case 59: case 63: case 4: case 3: case 2: // fp / vmx / traps
        return -1;
    case 31:
    {
        uint32_t xo = (i >> 1) & 0x3FF;
        switch (xo)
        {
        case 0: case 32: case 4: case 68: // cmp cmpl tw td
        case 151: case 183: case 407: case 439: case 215: case 247: case 149: case 181: // stores
        case 662: case 918: case 150: case 214: case 135: case 167: case 231: case 487: case 199: // misc stores/vmx
        case 663: case 695: case 727: case 759: case 983: // fp stores
        case 467: case 144: // mtspr mtcrf
        case 86: case 54: case 278: case 246: case 1014: case 470: case 598: case 854: // dcb*, sync, eieio
        case 535: case 567: case 599: case 631: // fp loads (write FPR)
        case 103: case 359: case 7: case 39: case 71: case 6: case 38: // vmx loads
            return -1;
        case 444: case 28: case 60: case 124: case 284: case 316: case 412: case 476: // or and andc nor eqv xor orc nand
        case 24: case 27: case 536: case 539: case 792: case 794: case 824: case 826: case 413: // shifts
        case 26: case 58: case 922: case 954: case 986: // cntlz ext*
            return rA;
        default:
            return rD; // add, subf, mul, div, loads x-form, mfspr, ...
        }
    }
    }
    return -1;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: bo3switch <patched.xex> <out_switch.toml> [out_functions.toml]\n"); return 1; }
    auto file = LoadFile(argv[1]);
    Ctx cx{ Image::ParseImage(file.data(), file.size()) };

    for (auto& s : cx.img.sections)
    {
        if (startsWith(s.name, ".text")) cx.text = &s;
        if (startsWith(s.name, ".pdata"))
            for (uint32_t off = 0; off + 8 <= s.size; off += 8)
            {
                uint32_t b = be32(s.data + off), fl = be32(s.data + off + 4);
                if (b) cx.funcs.push_back({ b, ((fl >> 8) & 0x3FFFFF) * 4 });
            }
    }
    std::sort(cx.funcs.begin(), cx.funcs.end(), [](auto& a, auto& b) { return a.begin < b.begin; });

    // Known function entry points: .pdata begins + every `bl` target. Used to bound leaf functions
    // (no .pdata) that contain a jump table, which XenonRecomp's analyser would cut at the bctr.
    std::set<uint32_t> starts;
    for (auto& f : cx.funcs) starts.insert(f.begin);
    for (auto& s : cx.img.sections)
    {
        if (!(s.flags & SectionFlags_Code)) continue;
        for (uint32_t off = 0; off + 4 <= s.size; off += 4)
        {
            uint32_t w = be32(s.data + off);
            if ((w >> 26) == 18 && (w & 3) == 1)
            {
                int32_t li = w & 0x03FFFFFC; if (li & 0x02000000) li -= 0x04000000;
                starts.insert(s.base + off + li);
            }
        }
    }
    std::map<uint32_t, uint32_t> leafFuncs; // begin -> size
    std::string leafNotes;

    std::string out = "# Generated by bo3switch (scheduling-tolerant jump table detector)\n\n";
    size_t nSites = 0, nTables = 0, nRejected = 0;
    std::string rejected;

    for (auto& sec : cx.img.sections)
    {
        if (!(sec.flags & SectionFlags_Code)) continue;
        for (uint32_t off = 4; off + 4 <= sec.size; off += 4)
        {
            uint32_t ins = be32(sec.data + off);
            uint32_t prev = be32(sec.data + off - 4);
            if (ins != 0x4E800420 || (prev & 0xFC1FFFFF) != 0x7C0903A6) continue; // bctr preceded by mtctr
            nSites++;
            uint32_t bctr = sec.base + off;

            // Find bounds check: conditional branch (bgt/bge on a cmplwi) within 160 instrs back (same block).
            // zlib inflate has its check 84 instructions before the bctr.
            int64_t brAt = -1, cmpAt = -1;
            uint32_t idxReg = 0, bound = 0, count = 0, defLabel = 0;
            for (int k = 2; k <= 160 && off >= 4u * k; k++)
            {
                uint32_t a = bctr - 4 * k;
                uint32_t w = *cx.word(a);
                uint32_t op = w >> 26;
                if (op == 18 || w == 0x4E800020 || w == 0x4E800420) break; // b / blr / bctr: different block
                bool isBc = op == 16, isBclr = (op == 19 && ((w >> 1) & 0x3FF) == 16);
                if (brAt < 0 && (isBc || isBclr))
                {
                    uint32_t BO = (w >> 21) & 31, BI = (w >> 16) & 31;
                    uint32_t bit = BI & 3; // 0 lt,1 gt,2 eq
                    bool ifTrue = (BO & 0x1C) == 0x0C, ifFalse = (BO & 0x1C) == 0x04;
                    int kind = 0; // 1: count=N+1, 2: count=N
                    if (bit == 1 && ifTrue) kind = 1;        // bgt  -> default if idx > N
                    else if (bit == 0 && ifFalse) kind = 2;  // bge  -> default if idx >= N
                    if (!kind) continue; // some other branch, keep looking
                    brAt = a;
                    if (isBc) { int32_t bd = (int16_t)(w & 0xFFFC); defLabel = a + bd; }
                    uint32_t cr = BI >> 2;
                    for (int j = 1; j <= 40; j++)
                    {
                        uint32_t c = *cx.word(a - 4 * j);
                        if ((c >> 26) == 10 && ((c >> 23) & 7) == cr && ((c >> 21) & 1) == 0)
                        {
                            cmpAt = a - 4 * j;
                            idxReg = (c >> 16) & 31;
                            bound = c & 0xFFFF;
                            count = kind == 1 ? bound + 1 : bound;
                            break;
                        }
                        if ((c >> 26) == 18 || c == 0x4E800020) break;
                    }
                    break;
                }
            }
            auto reject = [&](const char* why)
            {
                nRejected++;
                char buf[160];
                snprintf(buf, sizeof(buf), "#   0x%08X  %s\n", bctr, why);
                rejected += buf;
            };
            bool seedAfter = false; // seed applies after the instruction at cmpAt (clrlwi) vs before (cmplwi)
            {
                // An index bounded by a mask in the same basic block, e.g. `clrlwi r11,r10,30` (Bink decoder).
                // Used when there is no compare, or when it is closer to the bctr than the compare found.
                for (int k = 2; k <= 24 && off >= 4u * k; k++)
                {
                    uint32_t a = bctr - 4 * k;
                    if (cmpAt >= 0 && (int64_t)a <= brAt) break;
                    uint32_t w = *cx.word(a);
                    if ((w >> 26) == 18 || w == 0x4E800020 || w == 0x4E800420 || (w >> 26) == 16) break;
                    uint32_t SH = (w >> 11) & 31, MB = (w >> 6) & 31, ME = (w >> 1) & 31;
                    if ((w >> 26) == 21 && SH == 0 && ME == 31 && MB >= 26)
                    {
                        cmpAt = a; idxReg = (w >> 16) & 31; count = 1u << (32 - MB);
                        seedAfter = true; defLabel = 0;
                        break;
                    }
                }
            }
            // No bounds check in this block (the table block is a branch target and the check sits in
            // the predecessor): seed every register as a candidate index at the block start and let
            // the table arithmetic tell which one it is; the entry count is then probed below.
            bool unbounded = false;
            if (cmpAt < 0)
            {
                uint32_t a = bctr - 8;
                for (int k = 2; k <= 24 && a > sec.base; k++, a -= 4)
                {
                    uint32_t w = *cx.word(a - 4);
                    if ((w >> 26) == 18 || w == 0x4E800020 || w == 0x4E800420 || (w >> 26) == 16) break;
                }
                cmpAt = a;
                unbounded = true;
                count = 1024;
                defLabel = 0;
            }
            if (count == 0 || count > 4096) { reject("implausible case count"); continue; }

            // Forward symbolic evaluation from 24 instrs before the compare to the bctr.
            Val R[32];
            // Instruction that defined the index register just before the compare: compilers sometimes
            // re-materialise it after the bounds check (e.g. `addi r11,r22,-1` twice).
            uint32_t idxDef = 0;
            for (int j = 1; j <= 12 && !unbounded; j++)
            {
                uint32_t c = *cx.word((uint32_t)cmpAt - 4 * j);
                if (gprWritten(c) == (int)idxReg) { idxDef = c; break; }
            }
            uint32_t start = (uint32_t)cmpAt >= sec.base + 96 ? (uint32_t)cmpAt - 96 : sec.base;
            // Copy tracking before the compare so `mr r31,r4 ; cmplwi r4,N` marks r31 as the index too.
            uint32_t ver[32] = {}, nextVer = 1;
            for (uint32_t a = start; a < bctr; a += 4)
            {
                uint32_t w = *cx.word(a);
                if (a == (uint32_t)cmpAt && unbounded)
                {
                    for (int r = 0; r < 32; r++) { R[r] = Val{ VK::Index }; R[r].src = r; }
                    // fall through: the instruction at the block start still executes
                }
                else if (a == (uint32_t)cmpAt)
                {
                    if (seedAfter) { R[idxReg] = Val{ VK::Index }; R[idxReg].src = idxReg; continue; }
                    uint32_t v = ver[idxReg];
                    for (int r = 0; r < 32; r++)
                        if (r == (int)idxReg || (v && ver[r] == v)) { R[r] = Val{ VK::Index }; R[r].src = idxReg; }
                    continue;
                }
                if (a < (uint32_t)cmpAt)
                {
                    int d = gprWritten(w);
                    bool isMr = ((w >> 26) == 31 && ((w >> 1) & 0x3FF) == 444 && ((w >> 21) & 31) == ((w >> 11) & 31))
                             || ((w >> 26) == 24 && (w & 0xFFFF) == 0);
                    if (d >= 0)
                    {
                        uint32_t src = (w >> 21) & 31;
                        if (isMr) { if (!ver[src]) ver[src] = nextVer++; ver[d] = ver[src]; }
                        else ver[d] = 0;
                    }
                }
                if (a > (uint32_t)cmpAt && idxDef && w == idxDef && gprWritten(w) == (int)idxReg)
                {
                    uint32_t srcA = (w >> 16) & 31;
                    if (srcA != idxReg) continue; // same recomputation from an unchanged source: still the index
                }
                uint32_t op = w >> 26, rD = (w >> 21) & 31, rA = (w >> 16) & 31, rB = (w >> 11) & 31;
                if (a < (uint32_t)cmpAt && (op == 18 || w == 0x4E800020)) { for (auto& r : R) r = {}; continue; }
                Val nv{}; int dst = gprWritten(w);
                if (op == 15) { nv = { VK::Const }; nv.c = (rA ? (R[rA].k == VK::Const ? R[rA].c : 0) : 0) + ((w & 0xFFFF) << 16); if (rA && R[rA].k != VK::Const) nv = {}; }
                else if (op == 14)
                {
                    int32_t s = (int16_t)(w & 0xFFFF);
                    if (rA == 0) { nv = { VK::Const }; nv.c = (uint32_t)s; }
                    else if (R[rA].k == VK::Const) { nv = { VK::Const }; nv.c = R[rA].c + s; }
                    else if (R[rA].k == VK::Index && R[rA].scale == 1 && s > 0 && s < 64) { nv = R[rA]; nv.lo += s; }
                }
                else if (op == 24 && (w & 0xFFFF) == 0) { nv = R[rD]; } // ori rA,rS,0 (incl. nop) = copy
                else if (op == 24 && R[rD].k == VK::Const) { nv = { VK::Const }; nv.c = R[rD].c | (w & 0xFFFF); }
                else if (op == 21)
                {
                    uint32_t SH = (w >> 11) & 31, MB = (w >> 6) & 31, ME = (w >> 1) & 31;
                    const Val& s = R[rD]; // rS field is bits 21-25
                    bool slwi = MB == 0 && ME == 31 - SH;
                    bool clr = SH == 0 && ME == 31; // clrlwi (zero-extend): keeps value if small
                    if (s.k == VK::Index && slwi) { nv = s; nv.scale = s.scale << SH; }
                    else if (s.k == VK::Index && clr) nv = s;
                    else if (s.k == VK::Load && slwi && s.shift == 0) { nv = s; nv.shift = SH; }
                    else if (s.k == VK::Load && clr) nv = s;
                }
                else if (op == 31)
                {
                    uint32_t xo = (w >> 1) & 0x3FF;
                    uint32_t width = xo == 23 ? 4 : (xo == 279 || xo == 343) ? 2 : xo == 87 ? 1 : 0;
                    if (width)
                    {
                        const Val *t = &R[rA], *x = &R[rB];
                        if (t->k != VK::Const) std::swap(t, x);
                        if (rA != 0 && t->k == VK::Const && x->k == VK::Index && x->scale == width)
                        {
                            nv = { VK::Load }; nv.width = width; nv.table = t->c; nv.sgn = xo == 343; nv.src = x->src;
                            nv.absolute = width == 4;
                        }
                    }
                    else if (xo == 266) // add
                    {
                        const Val *b = &R[rA], *l = &R[rB];
                        if (b->k != VK::Const) std::swap(b, l);
                        if (b->k == VK::Const && l->k == VK::Load) { nv = *l; nv.k = VK::Target; nv.c = b->c; nv.absolute = false; }
                    }
                    else if (xo == 444 && rD == rB) { nv = R[rD]; } // mr rA,rS
                }
                if (dst >= 0) R[dst] = nv;
            }
            uint32_t ctrReg = (prev >> 21) & 31;
            Val t = R[ctrReg];
            if (t.k == VK::Load && t.absolute && t.shift == 0) { t.k = VK::Target; t.c = 0; }
            if (t.k != VK::Target) { reject(unbounded ? "indirect call / vtable / tail call (no jump table pattern)" : "could not resolve table arithmetic"); continue; }

            // Register holding the raw index at bctr.
            int rIdx = -1;
            if (unbounded)
            {
                // the register that fed the table must still hold the raw index
                if (t.src >= 0 && R[t.src].k == VK::Index && R[t.src].src == t.src && R[t.src].scale == 1 && R[t.src].lo == 0)
                    rIdx = t.src;
                if (rIdx < 0) { reject("unbounded table: index register not preserved"); continue; }
            }
            else if (R[idxReg].k == VK::Index && R[idxReg].scale == 1) rIdx = idxReg;
            else for (int r = 0; r < 32; r++) if (R[r].k == VK::Index && R[r].scale == 1) { rIdx = r; break; }
            if (rIdx < 0) { reject("index register clobbered before bctr"); continue; }

            const Func* fn = cx.owner(bctr);
            std::vector<uint32_t> labels;
            bool ok = true;
            uint32_t lo = R[rIdx].lo;
            for (uint32_t i = lo; i < lo + count && ok; i++)
            {
                auto p = (const uint8_t*)cx.img.Find(t.table + i * t.width);
                if (!p) { ok = false; break; }
                uint32_t e = t.width == 4 ? be32(p) : t.width == 2 ? ((p[0] << 8) | p[1]) : p[0];
                if (t.sgn && t.width == 2) e = (uint32_t)(int32_t)(int16_t)e;
                uint32_t lab = t.c + (e << t.shift);
                if (lab & 3) ok = false;
                if (fn ? (lab < fn->begin || lab >= fn->begin + fn->size)
                       : (!cx.text || lab < bctr - 0x8000 || lab > bctr + 0x8000))
                    ok = false;
                if (!ok && unbounded) { ok = labels.size() >= 2; break; }   // end of the table
                labels.push_back(lab);
            }
            if (!ok) { reject(unbounded ? "unbounded table: no valid entries" : "targets outside owning function"); continue; }
            // Values below `lo` can't occur; XenonRecomp indexes labels from 0, so pad with a valid label.
            if (lo) labels.insert(labels.begin(), lo, labels.front());
            if (!defLabel) defLabel = labels.front();

            if (!fn)
            {
                // Leaf function without .pdata: compute [begin, end) explicitly.
                auto it = starts.upper_bound((uint32_t)cmpAt);
                if (it == starts.begin()) { reject("leaf function start not found"); continue; }
                uint32_t begin = *std::prev(it);
                // Functions only reached through pointers are neither .pdata entries nor `bl`
                // targets: if alignment padding (0x00000000, never a valid instruction) lies
                // between the known start and the switch, the function begins after it.
                for (uint32_t a = (uint32_t)cmpAt; a > begin; a -= 4)
                {
                    auto wo = cx.word(a - 4);
                    if (wo && *wo == 0) { begin = a; break; }
                }
                uint32_t far = std::max(defLabel, bctr);
                for (auto l : labels) far = std::max(far, l);
                uint32_t a = far, end = 0;
                for (; a < far + 0x4000; a += 4)
                {
                    auto wo = cx.word(a); if (!wo) break;
                    uint32_t w = *wo, op = w >> 26;
                    if (op == 16) { int32_t bd = (int16_t)(w & 0xFFFC); if (a + bd > far) far = a + bd; continue; }
                    if (op == 18 && (w & 3) == 0)
                    {
                        int32_t li = w & 0x03FFFFFC; if (li & 0x02000000) li -= 0x04000000;
                        uint32_t tgt = a + li;
                        if (tgt > a && tgt < a + 0x1000 && !starts.count(tgt)) { far = std::max(far, tgt); continue; }
                    }
                    bool term = w == 0x4E800020 || w == 0x4E800420 || (op == 18 && (w & 1) == 0);
                    if (term && a >= far) { end = a + 4; break; }
                }
                // Never run into the next known .pdata function.
                auto nx = std::upper_bound(cx.funcs.begin(), cx.funcs.end(), begin, [](uint32_t v, const Func& f) { return v < f.begin; });
                if (nx != cx.funcs.end() && end > nx->begin) end = nx->begin;
                if (!end || end <= bctr) { reject("leaf function end not found"); continue; }
                auto [li, inserted] = leafFuncs.emplace(begin, end - begin);
                if (!inserted) li->second = std::max(li->second, end - begin);
            }

            char buf[128];
            snprintf(buf, sizeof(buf), "[[switch]]\nbase = 0x%X\nr = %d\ndefault = 0x%X\nlabels = [\n", bctr, rIdx, defLabel);
            out += buf;
            for (auto l : labels) { snprintf(buf, sizeof(buf), "    0x%X,\n", l); out += buf; }
            out += "]\n\n";
            nTables++;
        }
    }

    out += "# ---- bctr sites NOT treated as switch tables ----\n" + rejected;
    std::ofstream(argv[2]).write(out.data(), out.size());

    if (argc > 3)
    {
        std::string f = "# Generated by bo3switch: explicit bounds for leaf functions (no .pdata) containing jump tables\nfunctions = [\n";
        for (auto& [b, sz] : leafFuncs)
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "    { address = 0x%X, size = 0x%X },\n", b, sz);
            f += buf;
        }
        f += "]\n";
        std::ofstream(argv[3]).write(f.data(), f.size());
        printf("leaf functions with explicit bounds: %zu\n", leafFuncs.size());
    }
    printf("bctr sites: %zu, switch tables: %zu, other/unresolved: %zu, .pdata funcs: %zu\n",
        nSites, nTables, nRejected, cx.funcs.size());
}
