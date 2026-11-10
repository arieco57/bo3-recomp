// Xenos GPU front end: command processor (PM4), register file, vsync/CP interrupts, DPCs,
// and the Vd* kernel imports.
//
// This is a *null* GPU: it executes the command stream's synchronisation semantics (ring
// buffer read pointer, indirect buffers, waits, memory/fence writes, interrupts, swaps) so
// the engine can run, but it does not render. Draws and state are recorded in the register
// file only. Semantics follow Xenia's command processor.
#include "guest.h"
#include "profiler.h"
#include "kobject.h"
#include "ktime.h"
#include "stubs.h"
#include "thread.h"
#include "gpu/renderer.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <set>
#include <vector>
#include <pthread.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "arch.h"

namespace
{
// GPU register file. Guests access registers through MMIO at 0x7FC80000 (index * 4); here
// that range is plain memory, so we mirror register values there (big-endian) and poll the
// write pointer register.
constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kRegCount = 0x6000;
constexpr uint32_t REG_CP_RB_RPTR = 0x01C4, REG_CP_RB_WPTR = 0x01C5;
constexpr uint32_t REG_VGT_EVENT_INITIATOR = 0x21F9;
constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC, REG_SCRATCH_ADDR = 0x01DD, REG_SCRATCH_REG0 = 0x0578;

enum : uint32_t
{
    PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_DRAW_INDX = 0x22, PM4_VIZ_QUERY = 0x23, PM4_SET_STATE = 0x25,
    PM4_WAIT_FOR_IDLE = 0x26, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B, PM4_SET_CONSTANT = 0x2D,
    PM4_LOAD_CONSTANT_CONTEXT = 0x2E, PM4_LOAD_ALU_CONSTANT = 0x2F, PM4_DRAW_INDX_2 = 0x36,
    PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_INVALIDATE_STATE = 0x3B, PM4_WAIT_REG_MEM = 0x3C, PM4_MEM_WRITE = 0x3D,
    PM4_REG_TO_MEM = 0x3E, PM4_INDIRECT_BUFFER = 0x3F, PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46,
    PM4_ME_INIT = 0x48, PM4_SET_SHADER_BASES = 0x4A, PM4_MEM_WRITE_CNTR = 0x4F, PM4_WAIT_REG_EQ = 0x52,
    PM4_WAIT_REG_GTE = 0x53, PM4_INTERRUPT = 0x54, PM4_SET_CONSTANT2 = 0x55, PM4_SET_SHADER_CONSTANTS = 0x56,
    PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_EXT = 0x59, PM4_EVENT_WRITE_ZPD = 0x5B, PM4_CONTEXT_UPDATE = 0x5E,
    PM4_XE_SWAP = 0x64,   // emitted by our VdSwap (same convention as Xenia)
};

struct Gpu
{
    uint32_t ringBase = 0, ringSizeDwords = 0;   // guest (virtual) address
    uint32_t rptr = 0;                            // dwords
    uint32_t rptrWriteback = 0;                   // guest address (0 = disabled)
    uint32_t interruptCallback = 0, interruptUser = 0;
    uint64_t frames = 0, draws = 0, swapCounter = 0;
    std::atomic<bool> started{ false };
    std::mutex dpcLock;
    std::deque<uint32_t> dpcQueue;
    std::set<uint32_t> dpcQueued;   // KDPC is 0x1C bytes: keep the queued flag host side
    std::deque<uint32_t> pendingInterrupts;   // CP interrupts (source 1)
    std::condition_variable intrCv, intrDoneCv;
    std::mutex intrLock;
    uint64_t interruptsRaised = 0, interruptsHandled = 0;
};
Gpu g;

uint32_t PhysToVirt(uint32_t phys) { return phys >= 0xA0000000 ? phys : 0xA0000000 | (phys & 0x1FFFFFFF); }

uint32_t GpuSwap(uint32_t v, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);   // 8in16
    case 2: return __builtin_bswap32(v);                                  // 8in32
    case 3: return (v << 16) | (v >> 16);                                 // 16in32
    default: return v;
    }
}

// GPU memory writes: host-native store of the swapped value (Xenia semantics).
void GpuWriteMemNow(uint32_t addrWithEndian, uint32_t value)
{
    uint32_t a = PhysToVirt(addrWithEndian & ~3u);
    uint32_t v = GpuSwap(value, addrWithEndian & 3);
    memcpy(g_base + a, &v, 4);
}

// Memory writes by the CP are fences for the guest: they land once the GPU has done the work
// before them (see RendererDeferGuestWrite).
// What waits for the GPU: on Xenos MEM_WRITE / COND_WRITE / REG_TO_MEM / scratch writeback are
// done by the CP when it reaches them; only end-of-pipe events (EVENT_WRITE_SHD) land after
// the preceding rendering. BO3_DEFER_ALL=1: every CP write waits for the GPU (old behaviour).
void GpuWriteMem(uint32_t addrWithEndian, uint32_t value, bool endOfPipe = false)
{
    static const bool deferAll = getenv("BO3_DEFER_ALL") != nullptr;
    uint32_t a = PhysToVirt(addrWithEndian & ~3u), host = GpuSwap(value, addrWithEndian & 3), pending;
    // an older write to the same address still pending must not land after this one
    if (endOfPipe || deferAll || RendererPendingGuestWrite(a, pending)) RendererDeferGuestWrite32(a, host);
    else memcpy(g_base + a, &host, 4);
}

uint32_t GpuReadMem(uint32_t addrWithEndian)
{
    uint32_t a = PhysToVirt(addrWithEndian & ~3u), v;
    memcpy(&v, g_base + a, 4);
    return GpuSwap(v, addrWithEndian & 3);
}

// The MMIO mirror is the source of truth: the CPU writes registers there directly
// (e.g. SCRATCH_ADDR/UMSK during init), the CP through WriteReg.
uint32_t ReadReg(uint32_t index) { return index < kRegCount ? GuestRead32(kMmioBase + index * 4) : 0; }
}

uint32_t GpuReg(uint32_t index) { return ReadReg(index); }
uint64_t g_gpuConstGen = 0;
std::atomic<uint64_t> g_vdSwaps{ 0 };
std::atomic<int> g_vsyncHz{ getenv("BO3_FPS_LIMIT") ? std::clamp(atoi(getenv("BO3_FPS_LIMIT")), 30, 1000) : 60 };   // emulated vsync rate = frame rate limit
void GpuSetFrameLimit(int hz) { g_vsyncHz = std::clamp(hz, 30, 1000); Log("gpu: frame rate limit %d", g_vsyncHz.load()); }
int GpuFrameLimit() { return g_vsyncHz.load(); }
double g_cpIdleMs = 0, g_cpWaitMs = 0, g_cpIntrMs = 0, g_cpIdleTotalMs = 0;   // CP thread time split (stats, renderer logs)
uint64_t g_gpuConstDirty[3] = {};   // 16-register chunks of 0x4000..0x492F changed since the renderer looked
const uint32_t* GpuRegsRaw() { return reinterpret_cast<const uint32_t*>(g_base + kMmioBase); }   // big-endian
float GpuRegF(uint32_t index) { uint32_t v = ReadReg(index); float f; memcpy(&f, &v, 4); return f; }

namespace
{

static uint32_t s_curPacket = 0, s_curHeader = 0, s_lastIbFrom = 0, s_lastIbD0 = 0, s_lastIbD1 = 0;

void WriteReg(uint32_t index, uint32_t value)
{
    if (index >= kRegCount) return;
    if ((index == REG_SCRATCH_UMSK || index == REG_SCRATCH_ADDR) && ReadReg(index) != value)
    {
        static int n = 0;
        if (n++ < 16)
            LogWarn("gpu: reg 0x%X 0x%X -> 0x%X by packet 0x%08X at 0x%08X (last IB from 0x%08X: %08X %08X)", index, ReadReg(index), value,
                    s_curHeader, s_curPacket, s_lastIbFrom, s_lastIbD0, s_lastIbD1);
    }
    // shader constants / fetch / bool / loop registers: let the renderer rebuild its blocks
    if (index >= 0x4000 && index < 0x4928 && ReadReg(index) != value)
    {
        g_gpuConstGen++;
        uint32_t chunk = (index - 0x4000) >> 4;
        g_gpuConstDirty[chunk >> 6] |= 1ull << (chunk & 63);
    }
    GuestWrite32(kMmioBase + index * 4, value);
    // Scratch registers write back to memory when enabled in SCRATCH_UMSK. D3D uses this to
    // unblock its own WAIT_REG_MEM on the writeback area ("reg 0x579 = 1; wait [S+4] == 1").
    if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
    {
        uint32_t n = index - REG_SCRATCH_REG0;
        if (ReadReg(REG_SCRATCH_UMSK) & (1u << n))
        {
            uint32_t a = PhysToVirt(ReadReg(REG_SCRATCH_ADDR) + n * 4);
            GpuWriteMem(a | 2, value);   // endian 8in32: guest memory is big-endian
        }
    }
}

// Constant registers (0x4000..0x4927) from big-endian guest dwords at `src` - the MMIO mirror is
// big-endian too: compared and copied 16 registers at a time, no swapping, no per-register path.
void WriteConstBlock(uint32_t index, const uint8_t* src, uint32_t count)
{
    uint8_t* mirror = g_base + kMmioBase;
    while (count)
    {
        uint32_t n = std::min(count, 16 - (index & 15));
        uint8_t* dst = mirror + index * 4;
        if (!FastEq(dst, src, n * 4))
        {
            memcpy(dst, src, n * 4);
            g_gpuConstGen++;
            uint32_t chunk = (index - 0x4000) >> 4;
            g_gpuConstDirty[chunk >> 6] |= 1ull << (chunk & 63);
        }
        index += n; src += n * 4; count -= n;
    }
}

bool IsConstRange(uint32_t index, uint32_t count) { return index >= 0x4000 && count <= 0x4928 - 0x4000 && index + count <= 0x4928; }

bool Compare(uint32_t func, uint32_t value, uint32_t ref)
{
    switch (func & 7)
    {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
    }
}

// PM4_INTERRUPT carries a CPU mask; the handler clears "its" bit using PCR.current_cpu.
// Delivered synchronously: the command processor does not continue until the handler ran, as on
// hardware. (Async delivery let the handler clear its scratch bit *after* the GPU had already
// written the next frame's value, deadlocking a later WAIT_REG_MEM.)
void RaiseCpInterrupt(uint32_t cpuMask)
{
    std::unique_lock lock(g.intrLock);
    uint64_t ticket = ++g.interruptsRaised;
    g.pendingInterrupts.push_back(cpuMask ? cpuMask : 1);
    g.intrCv.notify_all();
    if (g.interruptCallback)
        g.intrDoneCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return g.interruptsHandled >= ticket; });
}

void ExecuteBuffer(uint32_t addr, uint32_t dwords, int depth);

// Packets ordered with the rendering (waits, memory / fence writes, events, interrupts, swap):
// run by the command processor - with BO3_CP_SPLIT by the recording thread, in stream order.
template <class DF>
void ExecuteSyncOp(uint32_t op, uint32_t count, DF&& D, uint32_t p)
{
    switch (op)
    {
    case PM4_WAIT_REG_MEM:
    {
        uint32_t info = D(0), addr = D(1), ref = D(2), mask = D(3);
        // pending fence writes may be what this (or the guest after it) waits for; at most one
        // submit per BO3_WAIT_SUBMIT_US (default 1000 us) - each is a kernel call
        {
            static const int64_t minUs = getenv("BO3_WAIT_SUBMIT_US") ? atoll(getenv("BO3_WAIT_SUBMIT_US")) : 1000;
            static auto lastSubmit = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::microseconds>(now - lastSubmit).count() >= minUs)
            {
                RendererSubmitPending();
                lastSubmit = now;
            }
        }
        auto waitStart0 = std::chrono::steady_clock::now();
        struct AddWait { std::chrono::steady_clock::time_point t; ~AddWait() { g_cpWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); } } addWait{ waitStart0 };
        auto waitStart = std::chrono::steady_clock::now();
        bool reported = false;
        for (uint32_t spins = 0;; spins++)
        {
            if (!reported && std::chrono::steady_clock::now() - waitStart > std::chrono::milliseconds(5))
            {
                reported = true;
                uint32_t cur = (info & 0x10) ? GpuReadMem(addr) : ReadReg(addr);
                static int n = 0;
                if (n++ < 20)
                    LogWarn("gpu: slow WAIT_REG_MEM %s 0x%X (value 0x%X & mask 0x%X, want func %u ref 0x%X) at pm4 0x%08X",
                            (info & 0x10) ? "mem" : "reg", addr, cur, mask, info & 7, ref, p);
                if (n <= 3)
                {
                    std::string dump;
                    for (int k = -16; k < 0; k++) { char b[12]; snprintf(b, sizeof(b), "%08X ", GuestRead32(p + k * 4)); dump += b; }
                    LogWarn("gpu:   preceding dwords: %s", dump.c_str());
                    LogWarn("gpu:   SCRATCH_UMSK 0x%X ADDR 0x%X REG0 0x%X", ReadReg(REG_SCRATCH_UMSK), ReadReg(REG_SCRATCH_ADDR), ReadReg(REG_SCRATCH_REG0));
                }
            }
            uint32_t v;
            if (info & 0x10)
            {
                uint32_t host;
                static const bool noShortcut = getenv("BO3_DBG_WAIT_GPU") != nullptr;
                if (!noShortcut && RendererPendingGuestWrite(PhysToVirt(addr & ~3u), host))
                {
                    v = GpuSwap(host, addr & 3);
                    static int nlog = 0;
                    if (getenv("BO3_WAIT_LOG") && nlog++ < 60)
                        LogWarn("gpu: wait shortcut addr 0x%08X pending 0x%X mem 0x%X ref 0x%X mask 0x%X func %u spins %u", addr, v, GpuReadMem(addr), ref, mask, info & 7, spins);
                }
                else v = GpuReadMem(addr);
            }
            else v = ReadReg(addr);
            if (Compare(info, v & mask, ref)) break;
            if (spins > 64) std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (spins == 200000) LogWarn("gpu: WAIT_REG_MEM stuck on %s 0x%X (value 0x%X, ref 0x%X, func %u)",
                                         (info & 0x10) ? "mem" : "reg", addr, v & mask, ref, info & 7);
        }
        break;
    }
    case PM4_REG_RMW:
    {
        uint32_t info = D(0), andMask = D(1), orMask = D(2);
        uint32_t reg = info & 0x1FFF;
        uint32_t v = ReadReg(reg);
        if ((info >> 31) & 1) andMask = ReadReg(andMask & 0x1FFF);
        if ((info >> 30) & 1) orMask = ReadReg(orMask & 0x1FFF);
        WriteReg(reg, (v & andMask) | orMask);
        break;
    }
    case PM4_REG_TO_MEM:
        GpuWriteMem(D(1), ReadReg(D(0) & 0x1FFF));
        break;
    case PM4_MEM_WRITE:
        for (uint32_t i = 1; i < count; i++) GpuWriteMem((D(0) & ~3u) + (i - 1) * 4 | (D(0) & 3), D(i));
        break;
    case PM4_COND_WRITE:
    {
        uint32_t info = D(0), poll = D(1), ref = D(2), mask = D(3), waddr = D(4), wdata = D(5);
        uint32_t v = (info & 0x10) ? GpuReadMem(poll) : ReadReg(poll);
        if (Compare(info, v & mask, ref))
        {
            if (info & 0x100) GpuWriteMem(waddr, wdata);
            else WriteReg(waddr, wdata);
        }
        break;
    }
    case PM4_EVENT_WRITE_SHD:
    {
        uint32_t initiator = D(0), addr = D(1), value = D(2);
        WriteReg(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        GpuWriteMem(addr, ((initiator >> 31) & 1) ? uint32_t(g.swapCounter) : value, true);
        break;
    }
    case PM4_EVENT_WRITE_ZPD:
    {
        // Occlusion query begin/end. D3D writes 0xFFFFFEED into ZPass_A/B (offsets 16/20 of the
        // sample-count block at RB_SAMPLE_COUNT_ADDR) on query end and polls until the GPU
        // replaces it. Like Xenia, report a fixed number of passed samples.
        WriteReg(REG_VGT_EVENT_INITIATOR, D(0) & 0x3F);
        uint32_t addr = PhysToVirt(ReadReg(0x2325) & ~3u);
        if (addr > 0xA0000000)
        {
            const uint32_t kFinished = 0xEDFEFFFF;   // 0xFFFFFEED as stored (little-endian struct)
            uint32_t zpassA, zpassB, zfailA, zfailB;
            memcpy(&zfailA, g_base + addr + 8, 4);
            memcpy(&zfailB, g_base + addr + 12, 4);
            memcpy(&zpassA, g_base + addr + 16, 4);
            memcpy(&zpassB, g_base + addr + 20, 4);
            bool end = (zpassA == kFinished && zpassB == kFinished) || (zfailA == kFinished && zfailB == kFinished);
            memset(g_base + addr, 0, 32);
            if (end)
            {
                const uint32_t samples = 1000;   // little-endian, like the hardware writes it
                memcpy(g_base + addr + 16, &samples, 4);
                memcpy(g_base + addr + 0, &samples, 4);
            }
        }
        break;
    }
    case PM4_EVENT_WRITE_EXT:
    {
        // writes 6 u16 screen extents; report the full screen
        uint32_t addr = PhysToVirt(D(1) & ~3u);
        const uint16_t ext[6] = { 0, 1280 / 8, 0, 720 / 8, 0, 1 };
        for (int i = 0; i < 6; i++) GuestWrite16(addr + i * 2, ext[i]);
        break;
    }
    case PM4_EVENT_WRITE:
        WriteReg(REG_VGT_EVENT_INITIATOR, D(0) & 0x3F);
        break;
    case PM4_INTERRUPT:
        {
            auto t0 = std::chrono::steady_clock::now();
            static const bool deferAll = getenv("BO3_DEFER_ALL") != nullptr;
            if (deferAll) RendererWaitPendingWrites();   // the handler reads what the CP wrote before it
            else RendererSubmitPending();
            RaiseCpInterrupt(D(0));
            g_cpIntrMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        break;
    case PM4_XE_SWAP:
        RendererSwap(D(1), D(2), D(3));
        g.frames++;
        g.swapCounter++;
        if (g.frames == 1 || g.frames % 600 == 0)
            Log("gpu: frame %llu (front buffer 0x%08X %ux%u, %llu draws so far)", (unsigned long long)g.frames, D(1),
                D(2), D(3), (unsigned long long)g.draws);
        break;
    default:
        // ME_INIT, WAIT_FOR_IDLE, SET_SHADER_*, CONTEXT_UPDATE, VIZ_QUERY, INVALIDATE_STATE...:
        // no effect here.
        break;
    }
}

// Executes one packet at `p`; returns its size in dwords.
uint32_t ExecutePacket(uint32_t p, int depth)
{
    // Packets in the ring buffer may wrap around its end; indirect buffers are linear.
    const uint32_t ringEnd = depth == 0 ? g.ringBase + g.ringSizeDwords * 4 : 0;
    auto R = [&](uint32_t i) {
        uint32_t a = p + i * 4;
        if (ringEnd && a >= ringEnd) a -= g.ringSizeDwords * 4;
        return GuestRead32(a);
    };
    uint32_t h = GuestRead32(p);
    if (h == 0 || h == 0xCDCDCDCD) return 1;
    s_curPacket = p;
    s_curHeader = h;
    switch (h >> 30)
    {
    case 0:
    {
        uint32_t base = h & 0x7FFF, count = ((h >> 16) & 0x3FFF) + 1;
        bool one = (h >> 15) & 1;
        for (uint32_t i = 0; i < count; i++) WriteReg(one ? base : base + i, R(1 + i));
        return 1 + count;
    }
    case 1:
        WriteReg(h & 0x7FF, R(1));
        WriteReg((h >> 11) & 0x7FF, R(2));
        return 3;
    case 2:
        return 1;
    }
    uint32_t op = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
    auto D = [&](uint32_t i) { return R(1 + i); };
    static uint32_t s_opCount[128];   // diagnostics: type-3 packets per op (logged with the frame count)
    s_opCount[op]++;
    if (op == PM4_XE_SWAP && getenv("BO3_PM4_STATS") && (g.frames + 1) % 120 == 0)
    {
        std::string st;
        for (int k = 0; k < 128; k++) if (s_opCount[k]) { char b[32]; snprintf(b, sizeof(b), " %02X:%.1f", k, s_opCount[k] / 120.0); st += b; }
        Log("gpu: type-3 packets per frame:%s", st.c_str());
        memset(s_opCount, 0, sizeof(s_opCount));
    }
    static const bool trace = getenv("BO3_GPU_TRACE") != nullptr;
    if (trace && op != PM4_SET_CONSTANT && op != PM4_SET_CONSTANT2)
        Log("pm4 %*s@%08X op 0x%02X n=%u: %08X %08X %08X %08X %08X %08X", depth * 2, "", p, op, count, D(0),
            count > 1 ? D(1) : 0, count > 2 ? D(2) : 0, count > 3 ? D(3) : 0, count > 4 ? D(4) : 0, count > 5 ? D(5) : 0);
    switch (op)
    {
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
        s_lastIbFrom = p;
        s_lastIbD0 = D(0);
        s_lastIbD1 = D(1);
        ExecuteBuffer(PhysToVirt(D(0) & ~3u), D(1) & 0xFFFFF, depth + 1);
        break;
    case PM4_DRAW_INDX:
        // d0 = viz query, d1 = initiator, [d2 = index base (phys), d3 = index size] for DMA indices
        g.draws++;
        RendererDraw(D(1), count > 2 ? D(2) : 0, count > 3 ? D(3) : 0, ((D(1) >> 6) & 3) == 0);
        break;
    case PM4_DRAW_INDX_2:
        // d0 = initiator, immediate indices follow (rare): treated as auto-indexed
        g.draws++;
        RendererDraw(D(0), 0, 0, false);
        break;
    case PM4_IM_LOAD:
        // d0 = address | shader type (0 = vertex, 1 = pixel), d1 = start << 16 | size (dwords)
        RendererShaderLoad(D(0) & 3, PhysToVirt(D(0) & ~3u), D(1) & 0xFFFF, nullptr);
        break;
    case PM4_IM_LOAD_IMMEDIATE:
    {
        std::vector<uint32_t> code(D(1) & 0xFFFF);
        for (uint32_t i = 0; i < code.size(); i++) code[i] = D(2 + i);
        RendererShaderLoad(D(0) & 3, 0, uint32_t(code.size()), code.data());
        break;
    }
    case PM4_SET_CONSTANT:
    {
        // d0: index (11 bits) | type << 16; type selects the register block
        uint32_t info = D(0), type = (info >> 16) & 0xFF, index = info & 0x7FF;
        uint32_t base = type == 0 ? 0x4000 : type == 1 ? 0x4800 : type == 2 ? 0x4900 : type == 3 ? 0x4908 : type == 4 ? 0x2000 : UINT32_MAX;
        if (base != UINT32_MAX)
            for (uint32_t i = 1; i < count; i++) WriteReg(base + index + i - 1, D(i));
        break;
    }
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS:
    {
        // d0: absolute register index (16 bits)
        uint32_t index = D(0) & 0xFFFF;
        uint32_t first = p + 8;   // address of D(1)
        if (!AbOld("constblock") && count > 1 && IsConstRange(index, count - 1) && (!ringEnd || first + (count - 1) * 4 <= ringEnd))
            WriteConstBlock(index, g_base + first, count - 1);
        else
            for (uint32_t i = 1; i < count; i++) WriteReg(index + i - 1, D(i));
        break;
    }
    case PM4_LOAD_ALU_CONSTANT:
    {
        // d0: physical address, d1: index | type << 16, d2: size (dwords)
        uint32_t addr = PhysToVirt(D(0) & 0x3FFFFFFF), info = D(1), size = D(2) & 0xFFF;
        uint32_t type = (info >> 16) & 0xFF, index = info & 0x7FF;
        uint32_t base = type == 0 ? 0x4000 : type == 1 ? 0x4800 : type == 2 ? 0x4900 : type == 3 ? 0x4908 : type == 4 ? 0x2000 : UINT32_MAX;
        if (!AbOld("constblock") && base != UINT32_MAX && size && IsConstRange(base + index, size))
            WriteConstBlock(base + index, g_base + addr, size);
        else if (base != UINT32_MAX)
            for (uint32_t i = 0; i < size; i++) WriteReg(base + index + i, GuestRead32(addr + i * 4));
        break;
    }
    default:
        ExecuteSyncOp(op, count, D, p);
        break;
    }
    return 1 + count;
}

void ExecuteBuffer(uint32_t addr, uint32_t dwords, int depth)
{
    if (depth > 8) { LogWarn("gpu: indirect buffer nesting too deep"); return; }
    for (uint32_t i = 0; i < dwords;) i += ExecutePacket(addr + i * 4, depth);
}


// ---------------------------------------------------------------- split command processor
// BO3_CP_SPLIT (default 1): the parser thread ("gpu-pm4") walks the ring and the indirect
// buffers and queues register writes, constants, draws, shader hashes and the ordered packets
// (waits, fence writes, events, interrupts, swaps); the recording thread ("gpu-cp") applies them
// in order and does all the Vulkan work. The parser reads guest memory (indirect buffers,
// constants, microcode) ahead of the recorder, so at a WAIT_REG_MEM it cannot prove satisfied -
// the value is neither in memory nor written earlier in the stream by the CP itself (fences,
// scratch writeback) - it waits until the recorder has executed it: such waits guard memory the
// guest CPU is still writing.
constexpr uint32_t kQWords = 1u << 22, kQMask = kQWords - 1;
enum : uint32_t { Q_REGS = 1, Q_CONST, Q_DRAW, Q_SHADER, Q_PACKET };
struct CmdQueue
{
    uint32_t* buf = nullptr;
    alignas(64) std::atomic<uint64_t> head{ 0 };   // published by the parser
    alignas(64) std::atomic<uint64_t> tail{ 0 };   // executed by the recorder
    alignas(64) std::atomic<uint32_t> sleeping{ 0 };
    uint64_t wpos = 0;                             // parser: written, not yet published
};
CmdQueue q;
bool s_split = false;
double s_parserWaitMs = 0;
uint32_t s_parserSyncs = 0;
std::unordered_map<uint32_t, uint32_t> s_cpWrites;   // parser: guest addr -> host bytes the CP stream writes there
std::unordered_set<uint64_t> s_sentShaders;          // parser: microcode already handed to the recorder
uint64_t s_parseSwaps = 0;
uint32_t s_parseRegs[kRegCount];                     // parser: register values written by the stream
uint8_t s_parseRegValid[kRegCount];                  // (else the mirror, i.e. the guest's own writes)
uint32_t ParserReg(uint32_t index) { return index < kRegCount && s_parseRegValid[index] ? s_parseRegs[index] : ReadReg(index); }
uint32_t s_parseUmsk = UINT32_MAX, s_parseScratchAddr = UINT32_MAX;   // as written in the stream (UINT32_MAX: mirror)

void QPublish()
{
    q.head.store(q.wpos, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (q.sleeping.load(std::memory_order_relaxed)) q.head.notify_one();
}

template <class Pred>
void ParserWait(Pred done)
{
    auto t0 = std::chrono::steady_clock::now();
    for (uint32_t spins = 0; !done(); spins++)
    {
        if (spins < 4000) CpuRelax();
#ifdef __ANDROID__
        else if (spins > 4200) std::this_thread::sleep_for(std::chrono::microseconds(50));   // (yield loops eat a core)
#endif
        else std::this_thread::yield();
    }
    s_parserWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void QReserve(uint32_t n)
{
    if (q.wpos + n - q.tail.load(std::memory_order_acquire) <= kQWords) return;
    QPublish();
    ParserWait([n] { return q.wpos + n - q.tail.load(std::memory_order_acquire) <= kQWords; });
}

inline void QPut(uint32_t v) { q.buf[q.wpos++ & kQMask] = v; }

void ParserNoteReg(uint32_t index, uint32_t value)
{
    if (index == REG_SCRATCH_UMSK) s_parseUmsk = value;
    else if (index == REG_SCRATCH_ADDR) s_parseScratchAddr = value;
    else if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
    {
        uint32_t n = index - REG_SCRATCH_REG0;
        uint32_t umsk = s_parseUmsk != UINT32_MAX ? s_parseUmsk : ReadReg(REG_SCRATCH_UMSK);
        uint32_t base = s_parseScratchAddr != UINT32_MAX ? s_parseScratchAddr : ReadReg(REG_SCRATCH_ADDR);
        if (umsk & (1u << n)) s_cpWrites[PhysToVirt(base + n * 4) & ~3u] = GpuSwap(value, 2);
    }
}

template <class VF>
void QRegs(uint32_t base, bool one, uint32_t count, VF value)
{
    QReserve(2 + count);
    QPut(Q_REGS | (count << 8));
    QPut(base | (one ? 0x80000000u : 0u));
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t v = value(i), idx = one ? base : base + i;
        QPut(v);
        if (idx < kRegCount) { s_parseRegs[idx] = v; s_parseRegValid[idx] = 1; }
        if (idx == REG_SCRATCH_UMSK || idx == REG_SCRATCH_ADDR || (idx >= REG_SCRATCH_REG0 && idx < REG_SCRATCH_REG0 + 8)) ParserNoteReg(idx, v);
    }
}

template <class VF>
void QConsts(uint32_t index, uint32_t count, VF value)
{
    if (!count) return;
    if (!IsConstRange(index, count)) { QRegs(index, false, count, value); return; }
    QReserve(2 + count);
    QPut(Q_CONST | (count << 8));
    QPut(index);
    for (uint32_t i = 0; i < count; i++) QPut(__builtin_bswap32(value(i)));   // the mirror is big-endian
}

void ParseBuffer(uint32_t addr, uint32_t dwords, int depth);

uint32_t ParsePacket(uint32_t p, int depth)
{
    const uint32_t ringEnd = depth == 0 ? g.ringBase + g.ringSizeDwords * 4 : 0;
    auto R = [&](uint32_t i) {
        uint32_t a = p + i * 4;
        if (ringEnd && a >= ringEnd) a -= g.ringSizeDwords * 4;
        return GuestRead32(a);
    };
    uint32_t h = GuestRead32(p);
    if (h == 0 || h == 0xCDCDCDCD) return 1;
    switch (h >> 30)
    {
    case 0:
    {
        uint32_t base = h & 0x7FFF, count = ((h >> 16) & 0x3FFF) + 1;
        QRegs(base, (h >> 15) & 1, count, [&](uint32_t i) { return R(1 + i); });
        QPublish();
        return 1 + count;
    }
    case 1:
        QRegs(h & 0x7FF, false, 1, [&](uint32_t) { return R(1); });
        QRegs((h >> 11) & 0x7FF, false, 1, [&](uint32_t) { return R(2); });
        QPublish();
        return 3;
    case 2:
        return 1;
    }
    uint32_t op = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
    auto D = [&](uint32_t i) { return R(1 + i); };
    if (static const bool hist = getenv("BO3_PM4_HIST") != nullptr; hist)
    {
        // diagnostics: type-3 opcode histogram per frame, every 300 frames
        static uint64_t n[128] = {};
        n[op]++;
        if (op == PM4_XE_SWAP && s_parseSwaps % 300 == 299)
        {
            std::string line;
            char b[32];
            for (int i = 0; i < 128; i++) if (n[i]) { snprintf(b, sizeof(b), " %02X:%.1f", i, n[i] / 300.0); line += b; n[i] = 0; }
            Log("gpu: pm4 ops/frame%s", line.c_str());
        }
    }
    switch (op)
    {
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
        ParseBuffer(PhysToVirt(D(0) & ~3u), D(1) & 0xFFFFF, depth + 1);
        break;
    case PM4_SET_CONSTANT:
    {
        uint32_t info = D(0), type = (info >> 16) & 0xFF, index = info & 0x7FF;
        uint32_t base = type == 0 ? 0x4000 : type == 1 ? 0x4800 : type == 2 ? 0x4900 : type == 3 ? 0x4908 : type == 4 ? 0x2000 : UINT32_MAX;
        if (base != UINT32_MAX) QConsts(base + index, count - 1, [&](uint32_t i) { return D(1 + i); });
        break;
    }
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS:
        QConsts(D(0) & 0xFFFF, count - 1, [&](uint32_t i) { return D(1 + i); });
        break;
    case PM4_LOAD_ALU_CONSTANT:
    {
        uint32_t addr = PhysToVirt(D(0) & 0x3FFFFFFF), info = D(1), size = D(2) & 0xFFF;
        uint32_t type = (info >> 16) & 0xFF, index = info & 0x7FF;
        uint32_t base = type == 0 ? 0x4000 : type == 1 ? 0x4800 : type == 2 ? 0x4900 : type == 3 ? 0x4908 : type == 4 ? 0x2000 : UINT32_MAX;
        if (base != UINT32_MAX) QConsts(base + index, size, [&](uint32_t i) { return GuestRead32(addr + i * 4); });
        break;
    }
    case PM4_DRAW_INDX:
    case PM4_DRAW_INDX_2:
        QReserve(5);
        QPut(Q_DRAW);
        if (op == PM4_DRAW_INDX) { QPut(D(1)); QPut(count > 2 ? D(2) : 0); QPut(count > 3 ? D(3) : 0); QPut(((D(1) >> 6) & 3) == 0); }
        else { QPut(D(0)); QPut(0); QPut(0); QPut(0); }
        break;
    case PM4_IM_LOAD:
    case PM4_IM_LOAD_IMMEDIATE:
    {
        uint32_t type = D(0) & 3, dwords = D(1) & 0xFFFF;
        std::vector<uint32_t> inl;
        if (op == PM4_IM_LOAD_IMMEDIATE) { inl.resize(dwords); for (uint32_t i = 0; i < dwords; i++) inl[i] = D(2 + i); }
        uint32_t addr = op == PM4_IM_LOAD ? PhysToVirt(D(0) & ~3u) : 0;
        const uint32_t* inlp = op == PM4_IM_LOAD_IMMEDIATE ? inl.data() : nullptr;
        uint64_t hs = RendererShaderHash(type, addr, dwords, inlp, nullptr);
        std::vector<uint32_t> code;
        if (s_sentShaders.insert(hs).second) RendererShaderHash(type, addr, dwords, inlp, &code);
        QReserve(4 + uint32_t(code.size()));
        QPut(Q_SHADER | (uint32_t(code.size()) << 8));
        QPut(type); QPut(uint32_t(hs)); QPut(uint32_t(hs >> 32));
        for (uint32_t w : code) QPut(w);
        break;
    }
    case PM4_NOP:
    case PM4_WAIT_FOR_IDLE:
    case PM4_INVALIDATE_STATE:
    case PM4_CONTEXT_UPDATE:
    case PM4_ME_INIT:
    case PM4_SET_SHADER_BASES:
        break;
    default:
    {
        QReserve(3 + count);
        QPut(Q_PACKET | (count << 8));
        QPut(p);
        QPut(op);
        for (uint32_t i = 0; i < count; i++) QPut(D(i));
        // parser-side record of what this stream writes (for its own WAIT_REG_MEM checks)
        if (op == PM4_EVENT_WRITE_SHD)
            s_cpWrites[PhysToVirt(D(1) & ~3u)] = GpuSwap(((D(0) >> 31) & 1) ? uint32_t(s_parseSwaps) : D(2), D(1) & 3);
        else if (op == PM4_MEM_WRITE)
            for (uint32_t i = 1; i < count; i++) s_cpWrites[PhysToVirt((D(0) & ~3u) + (i - 1) * 4)] = GpuSwap(D(i), D(0) & 3);
        else if (op == PM4_COND_WRITE && (D(0) & 0x100)) s_cpWrites.erase(PhysToVirt(D(4) & ~3u));
        else if (op == PM4_REG_TO_MEM) s_cpWrites.erase(PhysToVirt(D(1) & ~3u));
        else if (op == PM4_XE_SWAP) s_parseSwaps++;
        else if (op == PM4_WAIT_REG_MEM)
        {
            uint32_t info = D(0), addr = D(1), ref = D(2), mask = D(3);
            bool ok;
            if (info & 0x10)
            {
                auto it = s_cpWrites.find(PhysToVirt(addr & ~3u));
                uint32_t v = it != s_cpWrites.end() ? GpuSwap(it->second, addr & 3) : GpuReadMem(addr);
                ok = Compare(info, v & mask, ref);
            }
            else ok = Compare(info, ParserReg(addr) & mask, ref);
            static int nlog = 0;
            if (!ok && getenv("BO3_SPLIT_LOG") && nlog++ < 40)
            {
                auto it = s_cpWrites.find(PhysToVirt(addr & ~3u));
                LogWarn("gpu: parser wait %s 0x%08X func %u ref 0x%X mask 0x%X mem 0x%X stream %s 0x%X", (info & 0x10) ? "mem" : "reg", addr, info & 7, ref, mask,
                        (info & 0x10) ? GpuReadMem(addr) : ReadReg(addr), it != s_cpWrites.end() ? "yes" : "no", it != s_cpWrites.end() ? GpuSwap(it->second, addr & 3) : 0);
            }
            if (!ok)
            {
                // not provably satisfied: let the recorder execute it before reading on
                QPublish();
                uint64_t want = q.wpos;
                s_parserSyncs++;
                ParserWait([want] { return q.tail.load(std::memory_order_acquire) >= want; });
            }
        }
        break;
    }
    }
    QPublish();
    if (op == PM4_XE_SWAP && s_parseSwaps % 600 == 0)
    {
        Log("gpu: parser waited %.2f ms/frame for the recorder, %.1f unresolved waits/frame", s_parserWaitMs / 600, s_parserSyncs / 600.0);
        s_parserWaitMs = 0;
        s_parserSyncs = 0;
    }
    return 1 + count;
}

void ParseBuffer(uint32_t addr, uint32_t dwords, int depth)
{
    if (depth > 8) { LogWarn("gpu: indirect buffer nesting too deep"); return; }
    for (uint32_t i = 0; i < dwords;) i += ParsePacket(addr + i * 4, depth);
}

void RecorderThread()
{
    pthread_setname_np(pthread_self(), "gpu-cp");
    ProfilerAttachThisThread();
    if (!getenv("BO3_NO_WINDOW")) RendererInit();
    auto W = [](uint64_t i) { return q.buf[i & kQMask]; };
    std::vector<uint32_t> tmp;
    uint64_t rpos = 0;
    bool idleSubmitted = false;
    for (;;)
    {
        uint64_t head = q.head.load(std::memory_order_acquire);
        if (rpos == head)
        {
            auto ti = std::chrono::steady_clock::now();
            if (!idleSubmitted) { RendererSubmitPending(); idleSubmitted = true; }
            for (uint32_t i = 0; i < 4000 && q.head.load(std::memory_order_acquire) == rpos; i++) CpuRelax();
            static const double stallMs = getenv("BO3_STALL_DUMP") ? atof(getenv("BO3_STALL_DUMP")) : 0;   // diagnostics
            if (stallMs > 0)
            {
                // poll instead of sleeping, and sample every guest thread once the gap is long
                bool dumped = false;
                while (q.head.load(std::memory_order_acquire) == rpos)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    if (!dumped && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ti).count() > stallMs)
                    {
                        dumped = true;
                        ThreadSampleAll("recorder idle");
                    }
                }
            }
            if (q.head.load(std::memory_order_acquire) == rpos)
            {
                q.sleeping.store(1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (q.head.load(std::memory_order_acquire) == rpos) q.head.wait(rpos, std::memory_order_acquire);
                q.sleeping.store(0, std::memory_order_relaxed);
            }
            double idleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ti).count();
            g_cpIdleMs += idleMs;
            g_cpIdleTotalMs += idleMs;
            continue;
        }
        idleSubmitted = false;
        while (rpos < head)
        {
            uint32_t hd = W(rpos), op = hd & 0xFF, n = hd >> 8;
            switch (op)
            {
            case Q_REGS:
            {
                uint32_t b = W(rpos + 1), base = b & 0x7FFFFFFF;
                bool one = b >> 31;
                // plain registers (no constants, no scratch side effects): straight into the mirror
                auto hits = [&](uint32_t lo, uint32_t hi) { return base <= hi && base + n > lo; };   // [lo, hi] inclusive
                bool plain = !one && base + n <= kRegCount && !hits(0x4000, 0x4927) && !hits(REG_SCRATCH_UMSK, REG_SCRATCH_ADDR) &&
                             !hits(REG_SCRATCH_REG0, REG_SCRATCH_REG0 + 7);
                if (plain)
                {
                    uint32_t* mirror = reinterpret_cast<uint32_t*>(g_base + kMmioBase) + base;
                    for (uint32_t i = 0; i < n; i++) mirror[i] = __builtin_bswap32(W(rpos + 2 + i));
                }
                else
                    for (uint32_t i = 0; i < n; i++) WriteReg(one ? base : base + i, W(rpos + 2 + i));
                rpos += 2 + n;
                break;
            }
            case Q_CONST:
            {
                uint32_t index = W(rpos + 1);
                uint64_t first = rpos + 2;
                const uint32_t* src = &q.buf[first & kQMask];
                if ((first & kQMask) + n > kQWords)
                {
                    tmp.resize(n);
                    for (uint32_t i = 0; i < n; i++) tmp[i] = W(first + i);
                    src = tmp.data();
                }
                WriteConstBlock(index, reinterpret_cast<const uint8_t*>(src), n);
                rpos += 2 + n;
                break;
            }
            case Q_DRAW:
                g.draws++;
                RendererDraw(W(rpos + 1), W(rpos + 2), W(rpos + 3), W(rpos + 4) != 0);
                rpos += 5;
                break;
            case Q_SHADER:
            {
                std::vector<uint32_t> code(n);
                for (uint32_t i = 0; i < n; i++) code[i] = W(rpos + 4 + i);
                RendererShaderSet(W(rpos + 1), uint64_t(W(rpos + 2)) | (uint64_t(W(rpos + 3)) << 32), std::move(code));
                rpos += 4 + n;
                break;
            }
            case Q_PACKET:
            {
                uint64_t d0 = rpos + 3;
                ExecuteSyncOp(W(rpos + 2), n, [&](uint32_t i) { return W(d0 + i); }, W(rpos + 1));
                rpos += 3 + n;
                break;
            }
            default:
                Fatal("gpu: bad command queue entry 0x%08X", hd);
            }
            q.tail.store(rpos, std::memory_order_release);
        }
    }
}

void CommandProcessorThread()
{
    s_split = !getenv("BO3_CP_SPLIT") || atoi(getenv("BO3_CP_SPLIT")) != 0;
    if (s_split)
    {
        pthread_setname_np(pthread_self(), "gpu-pm4");
        q.buf = new uint32_t[kQWords];
        std::thread(RecorderThread).detach();
        Log("gpu: command processor split into parser + recording threads");
    }
    else
    {
        pthread_setname_np(pthread_self(), "gpu-cp");
        ProfilerAttachThisThread();
        if (!getenv("BO3_NO_WINDOW")) RendererInit();
    }
    for (;;)
    {
        uint32_t wptr = GuestRead32(kMmioBase + REG_CP_RB_WPTR * 4);
        // the commands written before the guest bumped WPTR must be visible (ARM is weakly ordered)
        std::atomic_thread_fence(std::memory_order_acquire);
        static uint32_t idleSpins = 0;
        if (!g.ringBase || wptr == g.rptr || wptr >= g.ringSizeDwords)
        {
            // Idle: let deferred fence writes complete - but not on every short gap between
            // ring kicks (each submit ends the command buffer): only once idle for ~250 us.
            auto ti = std::chrono::steady_clock::now();
            if (++idleSpins == 1 && !s_split) RendererSubmitPending();   // (split: the recorder does it)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            if (!s_split)
            {
                double idleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ti).count();
                g_cpIdleMs += idleMs;
                g_cpIdleTotalMs += idleMs;
            }
            continue;
        }
        idleSpins = 0;
        while (g.rptr != wptr)
        {
            uint32_t n = s_split ? ParsePacket(g.ringBase + g.rptr * 4, 0) : ExecutePacket(g.ringBase + g.rptr * 4, 0);
            g.rptr = (g.rptr + n) % g.ringSizeDwords;
        }
        WriteReg(REG_CP_RB_RPTR, g.rptr);
        if (g.rptrWriteback) GuestWrite32(g.rptrWriteback, g.rptr);
    }
}

// KDPC (0x1C bytes): +0x00 type, +0x04 list entry, +0x0C routine, +0x10 context, +0x14 arg1, +0x18 arg2
void RunDpcs(GuestThread& t)
{
    for (;;)
    {
        uint32_t dpc;
        {
            std::lock_guard lock(g.dpcLock);
            if (g.dpcQueue.empty()) return;
            dpc = g.dpcQueue.front();
            g.dpcQueue.pop_front();
            g.dpcQueued.erase(dpc);
        }
        GuestCallOnThread(t, GuestRead32(dpc + 0x0C), dpc, GuestRead32(dpc + 0x10), GuestRead32(dpc + 0x14),
                          GuestRead32(dpc + 0x18));
    }
}

// Interrupt thread: vsync (source 0) at 60 Hz plus CP interrupts (source 1), then DPCs.
// The game paces its frames on the vsync interrupt; its simulation runs on real time, so a
// faster "vsync" raises the frame rate without speeding the game up (FPS limit option).
void InterruptThread(GuestThread& t)
{
    auto next = std::chrono::steady_clock::now();
    for (;;)
    {
        const auto period = std::chrono::microseconds(1000000 / std::clamp(g_vsyncHz.load(), 10, 1000));
        next += period;
        std::deque<uint32_t> cp;
        {
            std::unique_lock lock(g.intrLock);
            g.intrCv.wait_until(lock, next, [] { return !g.pendingInterrupts.empty(); });
            cp.swap(g.pendingInterrupts);
        }
        bool vsync = std::chrono::steady_clock::now() >= next;
        if (!vsync) next -= period;   // woke early for a CP interrupt
        else if (std::chrono::steady_clock::now() - next > std::chrono::milliseconds(100)) next = std::chrono::steady_clock::now();   // fell behind
        if (g.interruptCallback)
        {
            for (uint32_t mask : cp)
            {
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                    if (mask & (1u << cpu))
                    {
                        GuestWrite8(t.pcr + 0x10C, uint8_t(cpu));
                        GuestCallOnThread(t, g.interruptCallback, 1, g.interruptUser);
                    }
                std::lock_guard lock(g.intrLock);
                g.interruptsHandled++;
                g.intrDoneCv.notify_all();
            }
            if (vsync)
            {
                GuestWrite8(t.pcr + 0x10C, 2);
                GuestCallOnThread(t, g.interruptCallback, 0, g.interruptUser);
                static uint64_t nv = 0;
                static auto tl = std::chrono::steady_clock::now();
                nv++;
                if (getenv("BO3_VSYNC_LOG") && std::chrono::steady_clock::now() - tl > std::chrono::seconds(2))
                {
                    Log("vsync: %.1f interrupts/s, %.1f VdSwap/s", nv / 2.0, g_vdSwaps.exchange(0) / 2.0);
                    nv = 0;
                    tl = std::chrono::steady_clock::now();
                }
            }
        }
        RunDpcs(t);
    }
}

void StartGpu()
{
    if (g.started.exchange(true)) return;
    // Fixed MMIO read values (byte offsets), as Xenia's GraphicsSystem::ReadRegister reports.
    GuestWrite32(kMmioBase + 0x3C00, 0x08100748);
    GuestWrite32(kMmioBase + 0x3C04, 0x0000200E);
    GuestWrite32(kMmioBase + 0x6530, 0x000002D0);   // scanline
    GuestWrite32(kMmioBase + 0x6544, 0x00000001);   // vblank pending
    GuestWrite32(kMmioBase + 0x6584, 0x050002D0);   // display size 1280x720
    if (const char* e = getenv("BO3_FPS_LIMIT")) GpuSetFrameLimit(atoi(e));
    std::thread(CommandProcessorThread).detach();
    ThreadCreateHost("gpu-interrupt", InterruptThread);
    Log("gpu: null command processor started");
}
}

// ---------------------------------------------------------------------------------------
// Vd* imports

GUEST_IMPORT(VdInitializeEngines) { StartGpu(); RETURN(1); }
GUEST_IMPORT(VdShutdownEngines) { RETURN(0); }
GUEST_IMPORT(VdEnableDisableClockGating) { RETURN(0); }

GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    g.interruptCallback = ARG(3);
    g.interruptUser = ARG(4);
    Log("gpu: interrupt callback 0x%08X (user 0x%08X)", ARG(3), ARG(4));
    RETURN(0);
}

// VdInitializeRingBuffer(physical_address, size_log2): size = 1 << (size_log2 + 3) bytes
GUEST_IMPORT(VdInitializeRingBuffer)
{
    g.ringBase = PhysToVirt(ARG(3));
    g.ringSizeDwords = (1u << (ARG(4) + 3)) / 4;
    g.rptr = 0;
    WriteReg(REG_CP_RB_RPTR, 0);
    WriteReg(REG_CP_RB_WPTR, 0);
    Log("gpu: ring buffer 0x%08X, %u dwords", g.ringBase, g.ringSizeDwords);
    StartGpu();
    RETURN(0);
}

GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    g.rptrWriteback = PhysToVirt(ARG(3));
    RETURN(0);
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { RETURN(0); }

GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    memset(g_base + ARG(3), 0, 0x94);
    GuestWrite32(ARG(3), 0xBEEF0000);
    GuestWrite32(ARG(4), 0xBEEF0001);
    RETURN(0);
}

// VdSwap(buffer, fetch, unk2, unk3, unk4, frontbuffer*, format*, color_space*, width*, height*)
// The caller reserves 64 dwords in the ring for the kernel to fill.
GUEST_IMPORT(VdSwap)
{
    g_vdSwaps++;
    uint32_t buf = ARG(3);
    uint32_t front = ARG(8) ? GuestRead32(ARG(8)) & 0x1FFFFFFF : 0;
    uint32_t widthPtr = STACK_ARG(8), heightPtr = STACK_ARG(9);
    uint32_t w = widthPtr ? GuestRead32(widthPtr) : 1280, h = heightPtr ? GuestRead32(heightPtr) : 720;
    uint32_t o = 0;
    GuestWrite32(buf + o++ * 4, 0xC0000000 | (3u << 16) | (PM4_XE_SWAP << 8));   // type 3, 4 data dwords
    GuestWrite32(buf + o++ * 4, 0x53574150);                                     // 'SWAP'
    GuestWrite32(buf + o++ * 4, front);
    GuestWrite32(buf + o++ * 4, w);
    GuestWrite32(buf + o++ * 4, h);
    while (o < 64) GuestWrite32(buf + o++ * 4, 0x80000000);                      // type 2 NOP
    RETURN(0);
}

GUEST_IMPORT(VdQueryVideoMode)
{
    uint32_t m = ARG(3);
    memset(g_base + m, 0, 0x30);
    GuestWrite32(m + 0x00, 1280);
    GuestWrite32(m + 0x04, 720);
    GuestWrite32(m + 0x0C, 1);
    GuestWrite32(m + 0x10, 1);
    float hz = float(g_vsyncHz.load());   // refresh rate the game sees (= frame rate limit)
    uint32_t bits;
    memcpy(&bits, &hz, 4);
    GuestWrite32(m + 0x14, bits);
    GuestWrite32(m + 0x18, 1);
    GuestWrite32(m + 0x1C, 0x4A);
    GuestWrite32(m + 0x20, 0x01);
    RETURN(0);
}

GUEST_IMPORT(VdQueryVideoFlags) { RETURN(7); }   // widescreen | >=1024 wide | >=720 high

// X_DISPLAY_INFO
GUEST_IMPORT(VdGetCurrentDisplayInformation)
{
    uint32_t p = ARG(3);
    memset(g_base + p, 0, 0x58);
    GuestWrite16(p + 0x00, 1280);
    GuestWrite16(p + 0x02, 720);
    GuestWrite16(p + 0x44, 1280);
    GuestWrite16(p + 0x46, 720);
    float hz = float(g_vsyncHz.load());   // refresh rate the game sees (= frame rate limit)
    uint32_t bits;
    memcpy(&bits, &hz, 4);
    GuestWrite32(p + 0x48, bits);
    GuestWrite16(p + 0x52, 1280);
    RETURN(0);
}

GUEST_IMPORT(VdGetCurrentDisplayGamma)   // (type*, power*)
{
    GuestWrite32(ARG(3), 1);
    float power = 2.22222233f;
    uint32_t bits;
    memcpy(&bits, &power, 4);
    GuestWrite32(ARG(4), bits);
    RETURN(0);
}

GUEST_IMPORT(VdSetDisplayMode) { RETURN(0); }
GUEST_IMPORT(VdSetDisplayModeOverride) { RETURN(0); }
GUEST_IMPORT(VdPersistDisplay) { if (ARG(4)) GuestWrite32(ARG(4), 1); RETURN(0); }
GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { RETURN(1); }
GUEST_IMPORT(VdRetrainEDRAM) { RETURN(0); }
GUEST_IMPORT(VdRetrainEDRAMWorker) { RETURN(0); }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { RETURN(0); }

// VdInitializeScalerCommandBuffer(10 scaler params..., dest): dest is the 11th argument
// (stack, r1+0x64 - verified at the call site). Fill with NOPs, return dword count / 2.
GUEST_IMPORT(VdInitializeScalerCommandBuffer)
{
    uint32_t dest = STACK_ARG(10);
    const uint32_t words = 0x1CC / 4;
    if (dest)
        for (uint32_t i = 0; i < words; i++) GuestWrite32(dest + i * 4, 0x80000000);
    RETURN(words >> 1);
}

// ---------------------------------------------------------------------------------------
// DPCs

GUEST_IMPORT(KeInitializeDpc)   // (dpc, routine, context)
{
    uint32_t d = ARG(3);
    GuestWrite16(d, 0x13);
    GuestWrite32(d + 0x0C, ARG(4));
    GuestWrite32(d + 0x10, ARG(5));
    RETURN(0);
}

GUEST_IMPORT(KeInsertQueueDpc)   // (dpc, arg1, arg2) -> BOOL
{
    uint32_t d = ARG(3);
    std::lock_guard lock(g.dpcLock);
    if (!g.dpcQueued.insert(d).second) RETURN(0);
    GuestWrite32(d + 0x14, ARG(4));
    GuestWrite32(d + 0x18, ARG(5));
    g.dpcQueue.push_back(d);
    RETURN(1);
}
