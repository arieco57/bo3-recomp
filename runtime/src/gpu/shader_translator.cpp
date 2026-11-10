// Xenos microcode -> GLSL translator.
//
// Instruction encodings come from Xenia's ucode.h (vendored); semantics follow Xenia's
// shader translators and XenosRecomp. Structure of the generated code:
//
//   void xmain() { int pc = 0; while (true) { switch (pc) { case 0: ... case N: ... } return; } }
//
// Each control-flow instruction is one case; jumps set pc and `continue`, the end of the
// shader returns. main() initialises registers (vertex index / interpolants), calls xmain(),
// then writes the outputs (position transform to host NDC, colours, depth).
#include "shader_translator.h"

#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <map>

#include "xenia/gpu/ucode.h"

using namespace xe::gpu;
using namespace xe::gpu::ucode;

namespace
{
const char kSwz[] = "xyzw";

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

struct Translator
{
    bool ps;
    const uint32_t* code;
    uint32_t dwords;
    std::string out;
    TranslatedShader result;
    // last full vertex fetch (for mini fetches)
    uint32_t lastVfetchConst = 0, lastVfetchSrc = 0, lastVfetchSrcSwz = 0, lastVfetchStride = 0;
    bool lastVfetchRounded = false;

    void L(const std::string& s) { out += "        "; out += s; out += '\n'; }

    // ---------------------------------------------------------------- operands

    // Registers used: the array is sized to them, and only relative addressing needs the full
    // dynamically indexed file (which drivers keep in scratch memory - very slow).
    mutable uint32_t maxTemp = 0;
    mutable bool relativeTemps = false;

    std::string TempReg(uint32_t reg, bool relative) const
    {
        if (relative) { relativeTemps = true; return Fmt("r[clamp(%u + aL, 0, 63)]", reg); }
        maxTemp = std::max(maxTemp, reg);
        return Fmt("r[%u]", reg);
    }

    mutable uint64_t constChunks = 0;   // groups of 4 float constants read
    uint64_t fetchVec4 = 0;             // uvec4s of fetchc[] read (bit i = fetch dwords 4i..4i+3)
    void NoteFetchDwords(uint32_t first, uint32_t count)
    {
        for (uint32_t d = first; d < first + count && d < 192; d++) fetchVec4 |= 1ull << (d >> 2);
    }
    mutable bool relativeConsts = false;

    std::string ConstReg(uint32_t reg, bool addressed, bool a0Relative) const
    {
        if (!addressed) { constChunks |= 1ull << ((reg >> 2) & 63); return Fmt("c[%u]", reg); }
        relativeConsts = true;
        return Fmt("c[clamp(int(%u) + %s, 0, 255)]", reg, a0Relative ? "a0" : "aL");
    }

    // memexport: the constants an eA write reads hold the stream (base address, count, format).
    void NoteStreamConstants(const AluInstruction& in)
    {
        for (uint32_t i = 1; i <= 3; i++)
        {
            if (in.src_is_temp(i) || in.src_const_is_addressed(i)) continue;
            uint8_t reg = uint8_t(in.src_reg(i));
            bool known = false;
            for (uint32_t k = 0; k < result.memexportConstCount; k++) known |= result.memexportConsts[k] == reg;
            if (!known && result.memexportConstCount < 4) result.memexportConsts[result.memexportConstCount++] = reg;
        }
    }

    // Full 4-component vector operand of source i (1..3).
    std::string VecOperand(const AluInstruction& in, uint32_t i)
    {
        uint32_t reg = in.src_reg(i);
        std::string base;
        bool abs;
        if (in.src_is_temp(i))
        {
            base = TempReg(AluInstruction::src_temp_reg(reg), AluInstruction::is_src_temp_relative(reg));
            abs = AluInstruction::is_src_temp_value_absolute(reg);
        }
        else
        {
            base = ConstReg(reg, in.src_const_is_addressed(i), in.is_const_address_register_relative());
            abs = in.abs_constants();
        }
        uint32_t swz = in.src_swizzle(i);
        std::string s = base + ".";
        for (uint32_t c = 0; c < 4; c++) s += kSwz[AluInstruction::GetSwizzledComponentIndex(swz, c)];
        if (abs) s = "abs(" + s + ")";
        if (in.src_negate(i)) s = "(-" + s + ")";
        return s;
    }

    // Scalar operand from src3: which = 0 -> component 3 of the swizzle ("a"), 1 -> component 0 ("b").
    std::string ScalarOperand(const AluInstruction& in, uint32_t which)
    {
        uint32_t reg = in.src_reg(3);
        std::string base;
        bool abs;
        if (in.src_is_temp(3))
        {
            base = TempReg(AluInstruction::src_temp_reg(reg), AluInstruction::is_src_temp_relative(reg));
            abs = AluInstruction::is_src_temp_value_absolute(reg);
        }
        else
        {
            base = ConstReg(reg, in.src_const_is_addressed(3), in.is_const_address_register_relative());
            abs = in.abs_constants();
        }
        uint32_t swz = in.src_swizzle(3);
        uint32_t comp = which == 0 ? AluInstruction::GetSwizzledComponentIndex(swz, 3) : AluInstruction::GetSwizzledComponentIndex(swz, 0);
        std::string s = base + "." + kSwz[comp];
        if (abs) s = "abs(" + s + ")";
        if (in.src_negate(3)) s = "(-" + s + ")";
        return s;
    }

    // MULSC/ADDSC/SUBSC: constant operand (src3 as constant) and a temp register encoded
    // in the remaining bits.
    std::string ScalarConstOperand(const AluInstruction& in)
    {
        uint32_t swz = in.src_swizzle(3);
        std::string s = ConstReg(in.src_reg(3), in.src_const_is_addressed(3), in.is_const_address_register_relative()) +
                        "." + kSwz[AluInstruction::GetSwizzledComponentIndex(swz, 3)];
        if (in.abs_constants()) s = "abs(" + s + ")";
        if (in.src_negate(3)) s = "(-" + s + ")";
        return s;
    }
    std::string ScalarTempOperand(const AluInstruction& in)
    {
        uint32_t swz = in.src_swizzle(3);
        std::string s = TempReg(in.scalar_const_reg_op_src_temp_reg(), false) + "." + kSwz[AluInstruction::GetSwizzledComponentIndex(swz, 0)];
        if (in.src_negate(3)) s = "(-" + s + ")";
        return s;
    }

    // ---------------------------------------------------------------- ALU

    void Alu(const AluInstruction& in)
    {
        if (in.is_predicated()) L(Fmt("if (%sp0) {", in.predicate_condition() ? "" : "!"));

        // Vector op (evaluated into vres before anything is written).
        auto A = [&] { return VecOperand(in, 1); };
        auto B = [&] { return VecOperand(in, 2); };
        auto C = [&] { return VecOperand(in, 3); };
        std::string v;
        bool vectorUsed = in.vector_write_mask() != 0 || in.is_export();
        switch (in.vector_opcode())
        {
        case AluVectorOpcode::kAdd: v = A() + " + " + B(); break;
        case AluVectorOpcode::kMul: v = "xmul(" + A() + ", " + B() + ")"; break;
        case AluVectorOpcode::kMax: v = "max(" + A() + ", " + B() + ")"; break;
        case AluVectorOpcode::kMin: v = "min(" + A() + ", " + B() + ")"; break;
        case AluVectorOpcode::kSeq: v = "vec4(equal(" + A() + ", " + B() + "))"; break;
        case AluVectorOpcode::kSgt: v = "vec4(greaterThan(" + A() + ", " + B() + "))"; break;
        case AluVectorOpcode::kSge: v = "vec4(greaterThanEqual(" + A() + ", " + B() + "))"; break;
        case AluVectorOpcode::kSne: v = "vec4(notEqual(" + A() + ", " + B() + "))"; break;
        case AluVectorOpcode::kFrc: v = "fract(" + A() + ")"; break;
        case AluVectorOpcode::kTrunc: v = "trunc(" + A() + ")"; break;
        case AluVectorOpcode::kFloor: v = "floor(" + A() + ")"; break;
        case AluVectorOpcode::kMad: v = "xmul(" + A() + ", " + B() + ") + " + C(); break;
        case AluVectorOpcode::kCndEq: v = "mix(" + C() + ", " + B() + ", equal(" + A() + ", vec4(0.0)))"; break;
        case AluVectorOpcode::kCndGe: v = "mix(" + C() + ", " + B() + ", greaterThanEqual(" + A() + ", vec4(0.0)))"; break;
        case AluVectorOpcode::kCndGt: v = "mix(" + C() + ", " + B() + ", greaterThan(" + A() + ", vec4(0.0)))"; break;
        case AluVectorOpcode::kDp4: v = "vec4(xdot4(" + A() + ", " + B() + "))"; break;
        case AluVectorOpcode::kDp3: v = "vec4(xdot4(vec4((" + A() + ").xyz, 0.0), vec4((" + B() + ").xyz, 0.0)))"; break;
        case AluVectorOpcode::kDp2Add: v = "vec4(xdot4(vec4((" + A() + ").xy, 0.0, 0.0), vec4((" + B() + ").xy, 0.0, 0.0)) + (" + C() + ").x)"; break;
        case AluVectorOpcode::kCube: v = "xcube(" + A() + ", " + B() + ")"; break;
        case AluVectorOpcode::kMax4: { std::string a = A(); v = "vec4(max(max((" + a + ").x, (" + a + ").y), max((" + a + ").z, (" + a + ").w)))"; break; }
        case AluVectorOpcode::kSetpEqPush:
        case AluVectorOpcode::kSetpNePush:
        case AluVectorOpcode::kSetpGtPush:
        case AluVectorOpcode::kSetpGePush:
        {
            const char* cmp = in.vector_opcode() == AluVectorOpcode::kSetpEqPush ? "==" :
                              in.vector_opcode() == AluVectorOpcode::kSetpNePush ? "!=" :
                              in.vector_opcode() == AluVectorOpcode::kSetpGtPush ? ">" : ">=";
            L("vA = " + A() + "; vB = " + B() + ";");
            L(Fmt("p0n = vA.w == 0.0 && vB.w %s 0.0;", cmp));
            v = Fmt("vec4((vA.x == 0.0 && vB.x %s 0.0) ? 0.0 : vA.x + 1.0)", cmp);
            break;
        }
        case AluVectorOpcode::kKillEq:
        case AluVectorOpcode::kKillGt:
        case AluVectorOpcode::kKillGe:
        case AluVectorOpcode::kKillNe:
        {
            const char* f = in.vector_opcode() == AluVectorOpcode::kKillEq ? "equal" :
                            in.vector_opcode() == AluVectorOpcode::kKillGt ? "greaterThan" :
                            in.vector_opcode() == AluVectorOpcode::kKillGe ? "greaterThanEqual" : "notEqual";
            L(Fmt("killv = any(%s(%s, %s));", f, A().c_str(), B().c_str()));
            v = "vec4(killv ? 1.0 : 0.0)";
            result.kills = true;
            vectorUsed = true;
            break;
        }
        case AluVectorOpcode::kDst:
        {
            L("vA = " + A() + "; vB = " + B() + ";");
            v = "vec4(1.0, xmul1(vA.y, vB.y), vA.z, vB.w)";
            break;
        }
        case AluVectorOpcode::kMaxA:
        {
            L("vA = " + A() + "; vB = " + B() + ";");
            L("a0n = int(clamp(floor(vA.w + 0.5), -256.0, 255.0));");
            v = "max(vA, vB)";
            break;
        }
        default: v = "vec4(0.0)"; break;
        }
        bool setsP0FromVector = in.vector_opcode() >= AluVectorOpcode::kSetpEqPush && in.vector_opcode() <= AluVectorOpcode::kSetpGePush;
        if (in.vector_clamp()) v = "clamp(" + v + ", 0.0, 1.0)";
        if (vectorUsed) L("vres = " + v + ";");

        // Scalar op.
        AluScalarOpcode sop = in.scalar_opcode();
        auto sa = [&] { return ScalarOperand(in, 0); };
        auto sb = [&] { return ScalarOperand(in, 1); };
        std::string s;
        bool setsP0FromScalar = false, scalarKill = false, setsA0FromScalar = false;
        switch (sop)
        {
        case AluScalarOpcode::kAdds: s = sa() + " + " + sb(); break;
        case AluScalarOpcode::kAddsPrev: s = sa() + " + ps"; break;
        case AluScalarOpcode::kMuls: s = "xmul1(" + sa() + ", " + sb() + ")"; break;
        case AluScalarOpcode::kMulsPrev: s = "xmul1(" + sa() + ", ps)"; break;
        case AluScalarOpcode::kMulsPrev2:
            L("sA = " + sa() + "; sB = " + sb() + ";");
            s = "(ps == -3.402823466e38 || isinf(ps) || isnan(ps) || isinf(sB) || isnan(sB) || sB <= 0.0) ? -3.402823466e38 : xmul1(sA, ps)";
            break;
        case AluScalarOpcode::kMaxs: s = "max(" + sa() + ", " + sb() + ")"; break;
        case AluScalarOpcode::kMins: s = "min(" + sa() + ", " + sb() + ")"; break;
        case AluScalarOpcode::kSeqs: s = "(" + sa() + " == 0.0) ? 1.0 : 0.0"; break;
        case AluScalarOpcode::kSgts: s = "(" + sa() + " > 0.0) ? 1.0 : 0.0"; break;
        case AluScalarOpcode::kSges: s = "(" + sa() + " >= 0.0) ? 1.0 : 0.0"; break;
        case AluScalarOpcode::kSnes: s = "(" + sa() + " != 0.0) ? 1.0 : 0.0"; break;
        case AluScalarOpcode::kFrcs: s = "fract(" + sa() + ")"; break;
        case AluScalarOpcode::kTruncs: s = "trunc(" + sa() + ")"; break;
        case AluScalarOpcode::kFloors: s = "floor(" + sa() + ")"; break;
        case AluScalarOpcode::kExp: s = "exp2(" + sa() + ")"; break;
        case AluScalarOpcode::kLogc: s = "xlogc(" + sa() + ")"; break;
        case AluScalarOpcode::kLog: s = "log2(" + sa() + ")"; break;
        case AluScalarOpcode::kRcpc: s = "xrcpc(" + sa() + ")"; break;
        case AluScalarOpcode::kRcpf: s = "xrcpf(" + sa() + ")"; break;
        case AluScalarOpcode::kRcp: s = "1.0 / " + sa(); break;
        case AluScalarOpcode::kRsqc: s = "xrsqc(" + sa() + ")"; break;
        case AluScalarOpcode::kRsqf: s = "xrsqf(" + sa() + ")"; break;
        case AluScalarOpcode::kRsq: s = "inversesqrt(" + sa() + ")"; break;
        case AluScalarOpcode::kMaxAs:
            L("sA = " + sa() + "; sB = " + sb() + ";");
            setsA0FromScalar = true;
            L("a0n = int(clamp(floor(sA + 0.5), -256.0, 255.0));");
            s = "max(sA, sB)";
            break;
        case AluScalarOpcode::kMaxAsf:
            L("sA = " + sa() + "; sB = " + sb() + ";");
            setsA0FromScalar = true;
            L("a0n = int(clamp(floor(sA), -256.0, 255.0));");
            s = "max(sA, sB)";
            break;
        case AluScalarOpcode::kSubs: s = sa() + " - " + sb(); break;
        case AluScalarOpcode::kSubsPrev: s = sa() + " - ps"; break;
        case AluScalarOpcode::kSetpEq: L("sA = " + sa() + ";"); L("p0n = sA == 0.0;"); s = "p0n ? 0.0 : 1.0"; setsP0FromScalar = true; break;
        case AluScalarOpcode::kSetpNe: L("sA = " + sa() + ";"); L("p0n = sA != 0.0;"); s = "p0n ? 0.0 : 1.0"; setsP0FromScalar = true; break;
        case AluScalarOpcode::kSetpGt: L("sA = " + sa() + ";"); L("p0n = sA > 0.0;"); s = "p0n ? 0.0 : 1.0"; setsP0FromScalar = true; break;
        case AluScalarOpcode::kSetpGe: L("sA = " + sa() + ";"); L("p0n = sA >= 0.0;"); s = "p0n ? 0.0 : 1.0"; setsP0FromScalar = true; break;
        case AluScalarOpcode::kSetpInv: L("sA = " + sa() + ";"); L("p0n = sA == 1.0;"); s = "p0n ? 0.0 : (sA == 0.0 ? 1.0 : sA)"; setsP0FromScalar = true; break;
        case AluScalarOpcode::kSetpPop: L("sA = " + sa() + ";"); L("p0n = sA - 1.0 <= 0.0;"); s = "p0n ? 0.0 : sA - 1.0"; setsP0FromScalar = true; break;
        case AluScalarOpcode::kSetpClr: L("p0n = false;"); s = "3.402823466e38"; setsP0FromScalar = true; break;
        case AluScalarOpcode::kSetpRstr: L("sA = " + sa() + ";"); L("p0n = sA == 0.0;"); s = "p0n ? 0.0 : sA"; setsP0FromScalar = true; break;
        case AluScalarOpcode::kKillsEq: s = "(" + sa() + " == 0.0) ? 1.0 : 0.0"; scalarKill = true; break;
        case AluScalarOpcode::kKillsGt: s = "(" + sa() + " > 0.0) ? 1.0 : 0.0"; scalarKill = true; break;
        case AluScalarOpcode::kKillsGe: s = "(" + sa() + " >= 0.0) ? 1.0 : 0.0"; scalarKill = true; break;
        case AluScalarOpcode::kKillsNe: s = "(" + sa() + " != 0.0) ? 1.0 : 0.0"; scalarKill = true; break;
        case AluScalarOpcode::kKillsOne: s = "(" + sa() + " == 1.0) ? 1.0 : 0.0"; scalarKill = true; break;
        case AluScalarOpcode::kSqrt: s = "sqrt(" + sa() + ")"; break;
        case AluScalarOpcode::kMulsc0:
        case AluScalarOpcode::kMulsc1: s = "xmul1(" + ScalarConstOperand(in) + ", " + ScalarTempOperand(in) + ")"; break;
        case AluScalarOpcode::kAddsc0:
        case AluScalarOpcode::kAddsc1: s = ScalarConstOperand(in) + " + " + ScalarTempOperand(in); break;
        case AluScalarOpcode::kSubsc0:
        case AluScalarOpcode::kSubsc1: s = ScalarConstOperand(in) + " - " + ScalarTempOperand(in); break;
        case AluScalarOpcode::kSin: s = "sin(" + sa() + ")"; break;
        case AluScalarOpcode::kCos: s = "cos(" + sa() + ")"; break;
        case AluScalarOpcode::kRetainPrev: s = "ps"; break;
        default: s = "0.0"; break;
        }
        if (in.scalar_clamp()) s = "clamp(" + s + ", 0.0, 1.0)";
        L("sres = " + s + ";");

        // Commit: predicate / address register / previous scalar.
        if (setsP0FromVector || setsP0FromScalar) L("p0 = p0n;");
        if (in.vector_opcode() == AluVectorOpcode::kMaxA || setsA0FromScalar) L("a0 = a0n;");
        L("ps = sres;");

        // Writes.
        uint32_t vmask = in.GetVectorOpResultWriteMask(), smask = in.GetScalarOpResultWriteMask();
        uint32_t zeroMask = in.GetConstant0WriteMask(), oneMask = in.GetConstant1WriteMask();
        std::string dst;
        if (in.is_export())
        {
            uint32_t e = in.vector_dest();
            if (ps)
            {
                if (e <= 3) { dst = Fmt("oC[%u]", e); result.colorExports |= 1u << e; }
                else if (e == 61) { dst = "oDepth"; result.writesDepth = true; }
                else if (e == 32) { dst = "eA"; NoteStreamConstants(in); }
                else if (e >= 33 && e <= 37) { result.memexports++; dst = Fmt("eM[%u]", e - 33); }
                else dst = "dummy";
            }
            else
            {
                if (e <= 15) dst = Fmt("oInterp[%u]", e);
                else if (e == 62) dst = "oPos";
                else if (e == 63) dst = "oPointSize";
                else if (e == 32) { dst = "eA"; NoteStreamConstants(in); }
                else if (e >= 33 && e <= 37) { result.memexports++; dst = Fmt("eM[%u]", e - 33); }
                else dst = "dummy";
            }
        }
        auto write = [&](const std::string& target, uint32_t mask, const std::string& value) {
            for (uint32_t c = 0; c < 4; c++)
                if (mask & (1u << c)) L(Fmt("%s.%c = %s;", target.c_str(), kSwz[c], value.c_str()));
        };
        auto writeVec = [&](const std::string& target, uint32_t mask) {
            std::string m;
            for (uint32_t c = 0; c < 4; c++) if (mask & (1u << c)) m += kSwz[c];
            if (!m.empty()) L(Fmt("%s.%s = vres.%s;", target.c_str(), m.c_str(), m.c_str()));
        };
        if (in.is_export())
        {
            writeVec(dst, vmask);
            write(dst, smask, "sres");
            write(dst, zeroMask, "0.0");
            write(dst, oneMask, "1.0");
            // Memory export: store the element now (eA describes the stream).
            uint32_t e = in.vector_dest();
            if (e >= 33 && e <= 37) L(Fmt("memExport(%uu, eM[%u]);", e - 33, e - 33));
        }
        else
        {
            if (vmask) writeVec(TempReg(in.vector_dest(), in.is_vector_dest_relative()), vmask);
            if (smask) write(TempReg(in.scalar_dest(), in.is_scalar_dest_relative()), smask, "sres");
        }

        if (in.vector_opcode() >= AluVectorOpcode::kKillEq && in.vector_opcode() <= AluVectorOpcode::kKillNe)
            L(ps ? "if (killv) discard;" : "");
        if (scalarKill)
        {
            result.kills = true;
            if (ps) L("if (sres != 0.0) discard;");
        }
        if (in.is_predicated()) L("}");
    }

    // ---------------------------------------------------------------- fetch

    // Writes `value` (a vec4 expression) to r[dst] through a fetch destination swizzle.
    void FetchDest(uint32_t dst, bool rel, uint32_t dstSwz, const std::string& value)
    {
        L("fres = " + value + ";");
        std::string reg = TempReg(dst, rel);
        for (uint32_t c = 0; c < 4; c++)
        {
            uint32_t sel = (dstSwz >> (c * 3)) & 7;
            if (sel <= 3) L(Fmt("%s.%c = fres.%c;", reg.c_str(), kSwz[c], kSwz[sel]));
            else if (sel == 4) L(Fmt("%s.%c = 0.0;", reg.c_str(), kSwz[c]));
            else if (sel == 5) L(Fmt("%s.%c = 1.0;", reg.c_str(), kSwz[c]));
        }
    }

    void VertexFetch(const VertexFetchInstruction& in)
    {
        if (in.is_predicated()) L(Fmt("if (%sp0) {", in.predicate_condition() ? "" : "!"));
        uint32_t fc = in.fetch_constant_index(), src = in.src(), srcSwz = in.src_swizzle();
        bool rounded = in.is_index_rounded();
        // mini fetches reuse the previous full fetch's constant, index and stride (their own
        // stride field is 0)
        uint32_t stride = in.stride();
        if (in.is_mini_fetch()) { fc = lastVfetchConst; src = lastVfetchSrc; srcSwz = lastVfetchSrcSwz; rounded = lastVfetchRounded; stride = lastVfetchStride; }
        else { lastVfetchConst = fc; lastVfetchSrc = src; lastVfetchSrcSwz = srcSwz; lastVfetchRounded = rounded; lastVfetchStride = stride; }
        NoteFetchDwords(fc * 2, 2);
        // The full fetch computes the index once; mini fetches reuse that value (not the register,
        // which an earlier fetch of the group may already have overwritten - e.g. skinning:
        // "vfetch r0, r0.w" followed by "vfetch_mini r6").
        std::string idx = "vfIndex";
        if (!in.is_mini_fetch())
        {
            std::string reg = Fmt("%s.%c", TempReg(src, in.is_src_relative()).c_str(), kSwz[srcSwz & 3]);
            L(Fmt("vfIndex = %s;", (rounded ? "int(floor(" + reg + " + 0.5))" : "int(floor(" + reg + "))").c_str()));
        }
        uint32_t fmt = uint32_t(in.data_format());
        float expScale = std::ldexp(1.0f, in.exp_adjust());
        std::string value = Fmt("vfetch(%u, %s, %u, %d, %u, %d, %d, %d, %.9g)", fc, idx.c_str(), stride, in.offset(),
                                fmt, in.is_signed() ? 1 : 0, in.is_normalized() ? 1 : 0,
                                in.signed_rf_mode() == xenos::SignedRepeatingFractionMode::kNoZero ? 1 : 0, expScale);
        FetchDest(in.dest(), in.is_dest_relative(), in.dest_swizzle(), value);
        if (in.is_predicated()) L("}");
    }

    void TextureFetch(const TextureFetchInstruction& in)
    {
        FetchOpcode op = in.opcode();
        if (in.is_predicated()) L(Fmt("if (%sp0) {", in.predicate_condition() ? "" : "!"));
        uint32_t t = in.fetch_constant_index();
        NoteFetchDwords(t * 6, 6);
        std::string srcReg = TempReg(in.src(), in.is_src_relative());
        uint32_t sw = in.src_swizzle();
        auto comp = [&](uint32_t i) { return Fmt("%s.%c", srcReg.c_str(), kSwz[(sw >> (i * 2)) & 3]); };
        switch (op)
        {
        case FetchOpcode::kTextureFetch:
        {
            result.texturesUsed |= 1u << t;
            auto dim = in.dimension();
            result.textureDim[t] = uint8_t(dim);
            // Filter overrides of the instruction (kUseFetchConst = 3 keeps the constant's).
            if (in.has_mag_filter()) result.texMag[t] = uint8_t(in.mag_filter());
            if (in.has_min_filter()) result.texMin[t] = uint8_t(in.min_filter());
            std::string lod = in.use_register_lod() ? "tlod" : ps ? "-1000.0" : "0.0";
            float bias = in.lod_bias();
            std::string value;
            bool denorm = in.unnormalized_coordinates();
            switch (dim)
            {
            case xenos::FetchOpDimension::k1D:
                value = Fmt("tfetch2D(%u, vec2(%s, 0.5), %d, %s, %.6g)", t, comp(0).c_str(), denorm ? 1 : 0, lod.c_str(), bias);
                break;
            case xenos::FetchOpDimension::k2D:
                value = Fmt("tfetch2D(%u, vec2(%s, %s), %d, %s, %.6g)", t, comp(0).c_str(), comp(1).c_str(), denorm ? 1 : 0, lod.c_str(), bias);
                break;
            case xenos::FetchOpDimension::k3DOrStacked:
                value = Fmt("tfetch3D(%u, vec3(%s, %s, %s), %d, %s, %.6g)", t, comp(0).c_str(), comp(1).c_str(), comp(2).c_str(), denorm ? 1 : 0, lod.c_str(), bias);
                break;
            case xenos::FetchOpDimension::kCube:
                value = Fmt("tfetchCube(%u, vec3(%s, %s, %s), %s, %.6g)", t, comp(0).c_str(), comp(1).c_str(), comp(2).c_str(), lod.c_str(), bias);
                break;
            }
            FetchDest(in.dest(), in.is_dest_relative(), in.dest_swizzle(), value);
            break;
        }
        case FetchOpcode::kSetTextureLod:
            L("tlod = " + comp(0) + ";");
            break;
        case FetchOpcode::kGetTextureComputedLod:
            FetchDest(in.dest(), in.is_dest_relative(), in.dest_swizzle(), "vec4(0.0)");
            break;
        case FetchOpcode::kGetTextureWeights:
            FetchDest(in.dest(), in.is_dest_relative(), in.dest_swizzle(), Fmt("vec4(fract(vec2(%s, %s) * tsize(%u) - 0.5), 0.0, 0.0)", comp(0).c_str(), comp(1).c_str(), t));
            break;
        default:
            // gradients / border colour: not needed for a first pass
            break;
        }
        if (in.is_predicated()) L("}");
    }

    // ---------------------------------------------------------------- control flow

    void ExecBody(uint32_t address, uint32_t count, uint32_t sequence)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t* ins = code + (address + i) * 3;
            if (size_t(address + i) * 3 + 3 > dwords) { result.error = "exec past end of shader"; return; }
            bool fetch = (sequence >> (i * 2)) & 1;
            if (fetch)
            {
                auto op = FetchOpcode(ins[0] & 0x1F);
                if (op == FetchOpcode::kVertexFetch) VertexFetch(*reinterpret_cast<const VertexFetchInstruction*>(ins));
                else TextureFetch(*reinterpret_cast<const TextureFetchInstruction*>(ins));
            }
            else
                Alu(*reinterpret_cast<const AluInstruction*>(ins));
        }
    }

    std::string BoolTest(uint32_t boolAddress, bool condition)
    {
        return Fmt("(((boolc[%u][%u] >> %u) & 1u) %s 0u)", boolAddress / 128, (boolAddress / 32) & 3, boolAddress & 31, condition ? "!=" : "==");
    }

    void Run()
    {
        // Count control-flow instructions: they end where the first exec block starts.
        uint32_t cfEnd = dwords;
        std::vector<ControlFlowInstruction> cf;
        for (uint32_t i = 0; i * 3 + 3 <= cfEnd; i++)
        {
            ControlFlowInstruction pair[2];
            UnpackControlFlowInstructions(code + i * 3, pair);
            for (auto& c : pair)
            {
                cf.push_back(c);
                if (IsControlFlowOpcodeExec(c.opcode()))
                {
                    uint32_t a = c.exec.address();
                    if (c.exec.count() && a * 3 < cfEnd) cfEnd = a * 3;
                }
            }
        }

        // Straight-line shaders (only exec / conditional exec blocks - most of them): emit the
        // blocks in order. The pc state machine below is only needed for loops, calls and
        // jumps; drivers optimize it much worse (BO3_SHADER_STATE_MACHINE=1 forces it).
        bool structured = getenv("BO3_SHADER_STATE_MACHINE") == nullptr;
        for (auto& c : cf)
        {
            auto op = c.opcode();
            if (op == ControlFlowOpcode::kLoopStart || op == ControlFlowOpcode::kLoopEnd || op == ControlFlowOpcode::kCondCall ||
                op == ControlFlowOpcode::kReturn || op == ControlFlowOpcode::kCondJmp)
                structured = false;
            if (op == ControlFlowOpcode::kExecEnd) break;   // the rest is unreachable
        }
        if (structured)
        {
            out += "void xmain() {\n";
            for (auto& c : cf)
            {
                bool end = false;
                switch (c.opcode())
                {
                case ControlFlowOpcode::kExec:
                case ControlFlowOpcode::kExecEnd:
                    ExecBody(c.exec.address(), c.exec.count(), c.exec.sequence());
                    end = c.opcode() == ControlFlowOpcode::kExecEnd;
                    break;
                case ControlFlowOpcode::kCondExec:
                case ControlFlowOpcode::kCondExecEnd:
                case ControlFlowOpcode::kCondExecPredClean:
                case ControlFlowOpcode::kCondExecPredCleanEnd:
                    L("if " + BoolTest(c.cond_exec.bool_address(), c.cond_exec.condition()) + " {");
                    ExecBody(c.cond_exec.address(), c.cond_exec.count(), c.cond_exec.sequence());
                    L("}");
                    end = DoesControlFlowOpcodeEndShader(c.opcode());
                    break;
                case ControlFlowOpcode::kCondExecPred:
                case ControlFlowOpcode::kCondExecPredEnd:
                    L(Fmt("if (p0 == %s) {", c.cond_exec_pred.condition() ? "true" : "false"));
                    ExecBody(c.cond_exec_pred.address(), c.cond_exec_pred.count(), c.cond_exec_pred.sequence());
                    L("}");
                    end = DoesControlFlowOpcodeEndShader(c.opcode());
                    break;
                default:
                    break;
                }
                if (end) break;
            }
            out += "}\n";
            return;
        }

        out += "void xmain() {\n";
        out += "    int pc = 0;\n";
        out += "    int loopDepth = 0; int loopCount[4]; int loopStep[4]; int aLStack[4];\n";
        out += "    int callDepth = 0; int callStack[4];\n";
        out += "    while (true) {\n";
        out += "    switch (pc) {\n";
        for (uint32_t i = 0; i < cf.size(); i++)
        {
            const auto& c = cf[i];
            out += Fmt("    case %u:\n", i);
            switch (c.opcode())
            {
            case ControlFlowOpcode::kExec:
            case ControlFlowOpcode::kExecEnd:
                ExecBody(c.exec.address(), c.exec.count(), c.exec.sequence());
                if (c.opcode() == ControlFlowOpcode::kExecEnd) L("return;");
                break;
            case ControlFlowOpcode::kCondExec:
            case ControlFlowOpcode::kCondExecEnd:
            case ControlFlowOpcode::kCondExecPredClean:
            case ControlFlowOpcode::kCondExecPredCleanEnd:
                L("if " + BoolTest(c.cond_exec.bool_address(), c.cond_exec.condition()) + " {");
                ExecBody(c.cond_exec.address(), c.cond_exec.count(), c.cond_exec.sequence());
                L("}");
                if (DoesControlFlowOpcodeEndShader(c.opcode())) L("return;");
                break;
            case ControlFlowOpcode::kCondExecPred:
            case ControlFlowOpcode::kCondExecPredEnd:
                L(Fmt("if (p0 == %s) {", c.cond_exec_pred.condition() ? "true" : "false"));
                ExecBody(c.cond_exec_pred.address(), c.cond_exec_pred.count(), c.cond_exec_pred.sequence());
                L("}");
                if (DoesControlFlowOpcodeEndShader(c.opcode())) L("return;");
                break;
            case ControlFlowOpcode::kLoopStart:
            {
                uint32_t id = c.loop_start.loop_id();
                L(Fmt("{ uint lc = loopc[%u][%u];", id / 4, id % 4));
                L("  if (loopDepth < 4) { aLStack[loopDepth] = aL; loopCount[loopDepth] = int(lc & 0xFFu);");
                L("    loopStep[loopDepth] = bitfieldExtract(int(lc), 16, 8);");
                L("    aL = int((lc >> 8) & 0xFFu);");
                L("    loopDepth++; }");
                L(Fmt("  if ((lc & 0xFFu) == 0u) { loopDepth--; aL = aLStack[loopDepth]; pc = %u; continue; } }", c.loop_start.address()));
                break;
            }
            case ControlFlowOpcode::kLoopEnd:
            {
                std::string brk = c.loop_end.is_predicated_break() ? Fmt("p0 == %s", c.loop_end.condition() ? "true" : "false") : "false";
                L("if (loopDepth > 0) { int d = loopDepth - 1; loopCount[d]--; aL += loopStep[d];");
                L(Fmt("  if (loopCount[d] > 0 && !(%s)) { pc = %u; continue; }", brk.c_str(), c.loop_end.address()));
                L("  loopDepth--; aL = aLStack[loopDepth]; }");
                break;
            }
            case ControlFlowOpcode::kCondCall:
            {
                std::string cond = c.cond_call.is_unconditional() ? "true" :
                                   c.cond_call.is_predicated() ? Fmt("p0 == %s", c.cond_call.condition() ? "true" : "false") :
                                   BoolTest(c.cond_call.bool_address(), c.cond_call.condition());
                L(Fmt("if (%s && callDepth < 4) { callStack[callDepth++] = %u; pc = %u; continue; }", cond.c_str(), i + 1, c.cond_call.address()));
                break;
            }
            case ControlFlowOpcode::kReturn:
                L("if (callDepth > 0) { pc = callStack[--callDepth]; continue; }");
                L("return;");
                break;
            case ControlFlowOpcode::kCondJmp:
            {
                std::string cond = c.cond_jmp.is_unconditional() ? "true" :
                                   c.cond_jmp.is_predicated() ? Fmt("p0 == %s", c.cond_jmp.condition() ? "true" : "false") :
                                   BoolTest(c.cond_jmp.bool_address(), c.cond_jmp.condition());
                L(Fmt("if (%s) { pc = %u; continue; }", cond.c_str(), c.cond_jmp.address()));
                break;
            }
            default:
                break;   // nop, alloc, mark vs fetch done
            }
        }
        out += "    default: break;\n";
        out += "    }\n";
        out += "    return;\n";
        out += "    }\n";
        out += "}\n";
    }
};

const char* kCommon = R"(#version 460
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

// Guest physical memory (512 MiB). Every index is masked to it: Xenos addresses are 29-bit
// physical, and a garbage fetch constant must not fault the host GPU.
layout(set = 0, binding = 0, std430) MEM_QUALIFIER buffer GuestMemory { uint mem[]; };
layout(set = 0, binding = BINDING_STATE, std140) uniform StageState {
    vec4 c[256];
    uvec4 fetchc[48];
    uvec4 boolc[2];
    uvec4 loopc[8];
};
// per-draw values: push constants (vertex stage at 0, pixel stage at 64)
layout(push_constant) uniform DrawConsts {
    layout(offset = PUSH_OFFSET) vec4 ndcScale;
    vec4 ndcOffset;
    uvec4 drawInfo;
    uvec4 psInfo;
};
layout(set = 1, binding = 0) uniform sampler2D tex2D[32];
layout(set = 1, binding = 1) uniform sampler3D tex3D[32];
layout(set = 1, binding = 2) uniform samplerCube texCube[32];

vec4 r[NUM_TEMPS];
int a0 = 0, aL = 0, a0n = 0, vfIndex = 0;
bool p0 = false, p0n = false, killv = false;
float ps = 0.0, sres = 0.0, sA = 0.0, sB = 0.0, tlod = 0.0;
vec4 vres = vec4(0.0), vA = vec4(0.0), vB = vec4(0.0), fres = vec4(0.0), dummy = vec4(0.0);

uint fetchDword(uint i) { return fetchc[i >> 2][i & 3u]; }

// ---- memory export (Xenia's semantics). eA holds integers in float mantissas (2^23 bias):
// x = base address in dwords (bits 30-31 = 01), y = base index, z = format, w = element count.
vec4 eA = vec4(0.0);
vec4 eM[5];
uint mxSwap(uint v, uint e) {
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (e == 2u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}
uint mxPack(float v, bool sgn, bool integer, uint bits) {
    float m = float((1u << bits) - 1u), h = float((1u << (bits - 1u)) - 1u);
    if (sgn) {
        float f = integer ? clamp(v, -h - 1.0, h) : clamp(v, -1.0, 1.0) * h;
        return uint(int(f + (f < 0.0 ? -0.5 : 0.5))) & ((1u << bits) - 1u);
    }
    float f = integer ? clamp(v, 0.0, m) : clamp(v, 0.0, 1.0) * m;
    return uint(f + 0.5);
}
void memExport(uint k, vec4 v) {
#ifdef MEMEXPORT
    uvec4 a = floatBitsToUint(eA);
    if ((a.x >> 30) != 1u || (a.y >> 23) != 0x96u || (a.z >> 23) != 0x96u || (a.w >> 23) != 0x96u) return;
    uint index = (a.y & 0x7FFFFFu) + k;
    if (index >= (a.w & 0x7FFFFFu)) return;
    uint fmt = (a.z >> 8) & 63u, endian = a.z & 7u;
    bool sgn = ((a.z >> 16) & 1u) != 0u, integer = ((a.z >> 17) & 1u) != 0u;
    if (((a.z >> 19) & 1u) != 0u) v = v.zyxw;
    uvec4 w = uvec4(0u);
    uint n = 1u;
    switch (fmt) {
    case 6u: w.x = mxPack(v.x, sgn, integer, 8u) | (mxPack(v.y, sgn, integer, 8u) << 8) | (mxPack(v.z, sgn, integer, 8u) << 16) | (mxPack(v.w, sgn, integer, 8u) << 24); break;
    case 7u: w.x = mxPack(v.x, sgn, integer, 10u) | (mxPack(v.y, sgn, integer, 10u) << 10) | (mxPack(v.z, sgn, integer, 10u) << 20) | (mxPack(v.w, sgn, integer, 2u) << 30); break;
    case 25u: w.x = mxPack(v.x, sgn, integer, 16u) | (mxPack(v.y, sgn, integer, 16u) << 16); break;
    case 26u: w.x = mxPack(v.x, sgn, integer, 16u) | (mxPack(v.y, sgn, integer, 16u) << 16);
              w.y = mxPack(v.z, sgn, integer, 16u) | (mxPack(v.w, sgn, integer, 16u) << 16); n = 2u; break;
    case 31u: w.x = packHalf2x16(v.xy); break;
    case 32u: w.x = packHalf2x16(v.xy); w.y = packHalf2x16(v.zw); n = 2u; break;
    case 36u: w.x = floatBitsToUint(v.x); break;
    case 37u: w.xy = floatBitsToUint(v.xy); n = 2u; break;
    case 38u: w = floatBitsToUint(v); n = 4u; break;
    default: return;   // 8/16-bit elements would need sub-dword stores
    }
    // 8-in-64 / 8-in-128: reorder dwords, then 8-in-32
    if (endian == 4u) { w = w.yxwz; endian = 2u; }
    else if (endian == 5u) { w = w.wzyx; endian = 2u; }
    uint addr = ((a.x & 0x3FFFFFFFu) + index * n);   // in dwords
    for (uint i = 0u; i < n; i++) mem[(addr + i) & 0x7FFFFFFu] = mxSwap(w[i], endian);
#endif
}

// Xenos "legacy" multiply: 0 * anything (including inf/nan) = 0.
float xmul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 xmul(vec4 a, vec4 b) {
    return mix(a * b, vec4(0.0), bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0)))));
}
float xdot4(vec4 a, vec4 b) { vec4 m = xmul(a, b); return (m.x + m.y) + (m.z + m.w); }
float xlogc(float a) { float l = log2(a); return isinf(l) && l < 0.0 ? -3.402823466e38 : l; }
float xrcpc(float a) { float v = 1.0 / a; return isinf(v) ? (v > 0.0 ? 3.402823466e38 : -3.402823466e38) : v; }
float xrcpf(float a) { float v = 1.0 / a; return isinf(v) ? 0.0 : v; }
float xrsqc(float a) { float v = inversesqrt(a); return isinf(v) ? 3.402823466e38 : v; }
float xrsqf(float a) { float v = inversesqrt(a); return isinf(v) ? 0.0 : v; }

// CUBE: src0 = (z, z, x, y) and src1 = (y, x, z, z) in the usual usage; result = (tc, sc, 2*ma, face).
vec4 xcube(vec4 a, vec4 b) {
    // D3D asm: cube rD, rS.zzxy, rS.yxzz -> direction (x, y, z) = (a.z, a.w, a.x)
    vec3 v = vec3(a.z, a.w, a.x);
    vec3 av = abs(v);
    float ma, sc, tc, face;
    if (av.z >= av.x && av.z >= av.y) { ma = v.z; face = v.z < 0.0 ? 5.0 : 4.0; sc = v.z < 0.0 ? -v.x : v.x; tc = -v.y; }
    else if (av.y >= av.x) { ma = v.y; face = v.y < 0.0 ? 3.0 : 2.0; sc = v.x; tc = v.y < 0.0 ? -v.z : v.z; }
    else { ma = v.x; face = v.x < 0.0 ? 1.0 : 0.0; sc = v.x < 0.0 ? v.z : -v.z; tc = -v.y; }
    return vec4(tc, sc, 2.0 * abs(ma), face);
}

uint swapE(uint v, uint e) {
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (e == 2u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}

float unpackComp(uint v, uint off, uint width, int isSigned, int normalized, int noZero) {
    float f;
    if (isSigned != 0) {
        int iv = bitfieldExtract(int(v), int(off), int(width));
        f = float(iv);
        if (normalized != 0) {
            float s = float((1u << (width - 1u)) - 1u) + (noZero != 0 ? 0.5 : 0.0);
            f = noZero != 0 ? (f + 0.5) / s : max(f / s, -1.0);
        }
    } else {
        f = float(bitfieldExtract(v, int(off), int(width)));
        if (normalized != 0) f /= float((1u << width) - 1u);
    }
    return f;
}

vec4 vfetch(uint fc, int index, uint stride, int offset, uint fmt, int isSigned, int normalized, int noZero, float expScale) {
    uint d0 = fetchDword(fc * 2u), d1 = fetchDword(fc * 2u + 1u);
    uint base = d0 >> 2;
    uint e = d1 & 3u;
    uint a = base + uint(index) * stride + uint(offset);
    vec4 v = vec4(0.0, 0.0, 0.0, 1.0);
    uint w0 = swapE(mem[(a) & 0x7FFFFFFu], e);
    switch (fmt) {
    case 6u:  v = vec4(unpackComp(w0, 0u, 8u, isSigned, normalized, noZero), unpackComp(w0, 8u, 8u, isSigned, normalized, noZero),
                       unpackComp(w0, 16u, 8u, isSigned, normalized, noZero), unpackComp(w0, 24u, 8u, isSigned, normalized, noZero)); break;
    case 7u:  v = vec4(unpackComp(w0, 0u, 10u, isSigned, normalized, noZero), unpackComp(w0, 10u, 10u, isSigned, normalized, noZero),
                       unpackComp(w0, 20u, 10u, isSigned, normalized, noZero), unpackComp(w0, 30u, 2u, isSigned, normalized, noZero)); break;
    case 16u: v = vec4(unpackComp(w0, 0u, 11u, isSigned, normalized, noZero), unpackComp(w0, 11u, 11u, isSigned, normalized, noZero),
                       unpackComp(w0, 22u, 10u, isSigned, normalized, noZero), 1.0); break;
    case 17u: v = vec4(unpackComp(w0, 0u, 10u, isSigned, normalized, noZero), unpackComp(w0, 10u, 11u, isSigned, normalized, noZero),
                       unpackComp(w0, 21u, 11u, isSigned, normalized, noZero), 1.0); break;
    case 25u: v = vec4(unpackComp(w0, 0u, 16u, isSigned, normalized, noZero), unpackComp(w0, 16u, 16u, isSigned, normalized, noZero), 0.0, 1.0); break;
    case 26u: { uint w1 = swapE(mem[(a + 1u) & 0x7FFFFFFu], e);
                v = vec4(unpackComp(w0, 0u, 16u, isSigned, normalized, noZero), unpackComp(w0, 16u, 16u, isSigned, normalized, noZero),
                         unpackComp(w1, 0u, 16u, isSigned, normalized, noZero), unpackComp(w1, 16u, 16u, isSigned, normalized, noZero)); break; }
    case 31u: v = vec4(unpackHalf2x16(w0), 0.0, 1.0); break;
    case 32u: { uint w1 = swapE(mem[(a + 1u) & 0x7FFFFFFu], e); v = vec4(unpackHalf2x16(w0), unpackHalf2x16(w1)); break; }
    case 33u: v = vec4(isSigned != 0 ? float(int(w0)) : float(w0), 0.0, 0.0, 1.0); break;
    case 34u: { uint w1 = swapE(mem[(a + 1u) & 0x7FFFFFFu], e);
                v = isSigned != 0 ? vec4(float(int(w0)), float(int(w1)), 0.0, 1.0) : vec4(float(w0), float(w1), 0.0, 1.0); break; }
    case 35u: { uvec4 w = uvec4(w0, swapE(mem[(a + 1u) & 0x7FFFFFFu], e), swapE(mem[(a + 2u) & 0x7FFFFFFu], e), swapE(mem[(a + 3u) & 0x7FFFFFFu], e));
                v = isSigned != 0 ? vec4(ivec4(w)) : vec4(w); break; }
    case 36u: v = vec4(uintBitsToFloat(w0), 0.0, 0.0, 1.0); break;
    case 37u: v = vec4(uintBitsToFloat(w0), uintBitsToFloat(swapE(mem[(a + 1u) & 0x7FFFFFFu], e)), 0.0, 1.0); break;
    case 38u: v = vec4(uintBitsToFloat(w0), uintBitsToFloat(swapE(mem[(a + 1u) & 0x7FFFFFFu], e)), uintBitsToFloat(swapE(mem[(a + 2u) & 0x7FFFFFFu], e)), uintBitsToFloat(swapE(mem[(a + 3u) & 0x7FFFFFFu], e))); break;
    case 57u: v = vec4(uintBitsToFloat(w0), uintBitsToFloat(swapE(mem[(a + 1u) & 0x7FFFFFFu], e)), uintBitsToFloat(swapE(mem[(a + 2u) & 0x7FFFFFFu], e)), 1.0); break;
    }
    if (fmt == 33u || fmt == 34u || fmt == 35u) { if (normalized != 0) v *= isSigned != 0 ? 1.0 / 2147483647.0 : 1.0 / 4294967295.0; }
    return v * expScale;
}

// Texture size from the fetch constant (dword 2) - the host image may be bigger (scaled resolves).
uint fetchDword(uint t, uint d) { uint i = t * 6u + d; return fetchc[i >> 2][i & 3u]; }
vec2 tsize(uint t) { uint s = fetchDword(t, 2u); return vec2(float((s & 0x1FFFu) + 1u), float(((s >> 13) & 0x1FFFu) + 1u)); }
vec4 tfetch2D(uint t, vec2 uv, int denorm, float lod, float bias) {
    if (denorm != 0) uv /= tsize(t);
    return lod < -999.0 ? TEX2D_IMPLICIT : textureLod(tex2D[t], uv, lod + bias);
}
vec4 tfetch3D(uint t, vec3 uvw, int denorm, float lod, float bias) {
    if (denorm != 0) uvw /= vec3(textureSize(tex3D[t], 0));
    return lod < -999.0 ? TEX3D_IMPLICIT : textureLod(tex3D[t], uvw, lod + bias);
}
// Xenos cube coordinates come from the CUBE instruction: (tc, sc, 2*ma, face) with sc/tc
// already divided-ready; reconstruct a direction for the host cube map.
vec4 tfetchCube(uint t, vec3 coord, float lod, float bias) {
    // D3D passes sc/tc as sc / (2|ma|) + 1.5, i.e. in [1, 2] (Xenia: * 2 - 3)
    vec2 st = coord.xy * 2.0 - 3.0;
    float face = coord.z;
    vec3 d;
    int f = int(face + 0.5);
    if (f == 0) d = vec3(1.0, -st.y, -st.x);
    else if (f == 1) d = vec3(-1.0, -st.y, st.x);
    else if (f == 2) d = vec3(st.x, 1.0, st.y);
    else if (f == 3) d = vec3(st.x, -1.0, -st.y);
    else if (f == 4) d = vec3(st.x, -st.y, 1.0);
    else d = vec3(-st.x, -st.y, -1.0);
    return lod < -999.0 ? TEXCUBE_IMPLICIT : textureLod(texCube[t], d, lod + bias);
}
)";
}

TranslatedShader TranslateShader(bool pixel, const uint32_t* code, uint32_t dwords)
{
    Translator t{ pixel, code, dwords };
    t.result.pixel = pixel;
    t.Run();
    std::string head = kCommon;
    auto replaceAll = [&](const std::string& a, const std::string& b) {
        for (size_t p; (p = head.find(a)) != std::string::npos;) head.replace(p, a.size(), b);
    };
    replaceAll("BINDING_STATE", pixel ? "2" : "1");
    replaceAll("PUSH_OFFSET", pixel ? "64" : "0");
    replaceAll("MEM_QUALIFIER", t.result.memexports ? "" : "readonly");
    if (t.result.memexports) head.insert(head.find('\n') + 1, "#define MEMEXPORT 1\n");
    // Pixel shaders receive the 16 interpolators in r0..r15.
    const uint32_t numTemps = t.relativeTemps ? 64u : std::max(t.maxTemp + 1, pixel ? 16u : 1u);
    replaceAll("NUM_TEMPS", std::to_string(numTemps));
    // Implicit-LOD sampling only exists in fragment shaders.
    replaceAll("TEX2D_IMPLICIT", pixel ? "texture(tex2D[t], uv, bias)" : "textureLod(tex2D[t], uv, bias)");
    replaceAll("TEX3D_IMPLICIT", pixel ? "texture(tex3D[t], uvw, bias)" : "textureLod(tex3D[t], uvw, bias)");
    replaceAll("TEXCUBE_IMPLICIT", pixel ? "texture(texCube[t], d, bias)" : "textureLod(texCube[t], d, bias)");

    std::string io;
    std::string mainFn;
    if (!pixel)
    {
        io = "layout(location = 0) out vec4 oInterp[16];\n"
             "vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\nvec4 oPointSize = vec4(0.0);\n";
        mainFn = R"(
// The index buffer address comes as the draw's first instance (it changes with almost every
// draw; in the push constants it re-sent them every time).
uint fetchIndex(uint i) {
    uint base = uint(gl_InstanceIndex), flags = drawInfo.y, e = drawInfo.z;
    if ((flags & 1u) == 0u) return i;
    if ((flags & 2u) != 0u) return swapE(mem[((base >> 2) + i) & 0x7FFFFFFu], e);
    uint byteAddr = base + i * 2u;
    uint w = swapE(mem[(byteAddr >> 2) & 0x7FFFFFFu], e);
    return (byteAddr & 2u) != 0u ? (w >> 16) : (w & 0xFFFFu);
}

void main() {
    for (int i = 0; i < NUM_TEMPS; i++) r[i] = vec4(0.0);
    for (int i = 0; i < 16; i++) oInterp[i] = vec4(0.0);
    uint index = fetchIndex(uint(gl_VertexIndex)) + drawInfo.w;
    r[0].x = float(index);
    xmain();
    gl_Position = vec4(oPos.xy * ndcScale.xy + oPos.ww * ndcOffset.xy, oPos.z * ndcScale.z + oPos.w * ndcOffset.z, oPos.w);
    gl_PointSize = 1.0;
}
)";
    }
    else
    {
        io = "layout(location = 0) in vec4 iInterp[16];\n"
             "layout(location = 0) out vec4 oC[4];\n"
             "float oDepthV = 0.0;\n"
             "vec4 oDepth = vec4(0.0);\n";
        // param_gen register (psInfo.x) selected with constant indices only
        std::string paramGen;
        for (uint32_t i = 0; i < 16; i++)
            paramGen += Fmt("    if (psInfo.x == %uu) r[%u] = pg;\n", i, i);
        mainFn = std::string(R"(
void main() {
    for (int i = 16; i < NUM_TEMPS; i++) r[i] = vec4(0.0);
    for (int i = 0; i < 16; i++) r[i] = iInterp[i];
    // host sample grid: MSAA (x, y) and the resolution scale; pg is the guest pixel position
    vec2 ss = vec2(float(max(psInfo.w & 15u, 1u)), float(max((psInfo.w >> 4) & 15u, 1u)));
    float rs = float(max((psInfo.w >> 8) & 255u, 1u)) / 6.0;   // resolution scale (x6 encoded: 2/3, 1.5x)
    vec4 pg = vec4((floor(gl_FragCoord.xy / ss) + 0.5) / rs, 0.0, gl_FrontFacing ? 0.0 : 1.0);
)") + paramGen + R"(    for (int i = 0; i < 4; i++) oC[i] = vec4(0.0);
    xmain();
    // Alpha test (RB_COLORCONTROL): psInfo.y = enable(8) | func, psInfo.z = reference (float bits)
    if ((psInfo.y & 8u) != 0u) {
        float a = oC[0].a, ref = uintBitsToFloat(psInfo.z);
        uint fn = psInfo.y & 7u;
        bool pass = fn == 0u ? false : fn == 1u ? a < ref : fn == 2u ? a == ref : fn == 3u ? a <= ref :
                    fn == 4u ? a > ref : fn == 5u ? a != ref : fn == 6u ? a >= ref : true;
        if (!pass) discard;
    }
)" + std::string(t.result.writesDepth ? "    gl_FragDepth = oDepth.x;\n" : "") + "}\n";
    }

    for (size_t p; (p = mainFn.find("NUM_TEMPS")) != std::string::npos;) mainFn.replace(p, 9, std::to_string(numTemps));
    t.result.constChunks = t.relativeConsts ? ~0ull : t.constChunks;
    t.result.fetchVec4 = t.fetchVec4;
    t.result.glsl = head + io + t.out + mainFn;
    if (!t.result.error.empty()) return t.result;
    std::string err;
    if (!CompileGlsl(pixel, t.result.glsl, t.result.spirv, err)) t.result.error = err;
    return t.result;
}

const char* ShaderTranslatorBuildId() { return __DATE__ " " __TIME__; }
