// XMA hardware decoder emulation (the Xbox 360 audio chip that turns XMA2 packets into PCM).
//
// Guest side (XAudio library in the game):
//   * XMACreateContext returns a 64-byte context in an array whose physical address the game
//     reads from register ContextArrayAddress (MMIO 0x7FEA1800); the context index is
//     (physical address - array base) / 64.
//   * The game fills the context (input buffers of 2048-byte packets, output ring buffer of
//     256-byte blocks of 16-bit big-endian PCM, ...) and writes bit masks to the Kick / Lock /
//     Clear registers (0x7FEA1940 / 1A40 / 1A80 + 4 * (index / 32)).
// Host side:
//   * The register page is mapped read-only; a guest store faults, the SIGSEGV handler decodes
//     the x86 store, applies the register write through a writable alias and skips the
//     instruction (no write is lost, unlike polling).
//   * A worker thread decodes kicked contexts with FFmpeg's stock "xma2" decoder (whole
//     packets, following each packet's skip count), converts to s16 BE and fills the guest's
//     output ring; only decoder-owned context fields are written back.
// Mostly following Xenia (src/xenia/apu, BSD). BO3_XMA_LOG=1 logs context activity.
#include "guest.h"
#include "memory.h"
#include "stubs.h"
#include "xma.h"
#include "arch.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

#include <semaphore.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
constexpr uint32_t kContextCount = 320;
constexpr uint32_t kContextSize = 64;
constexpr uint32_t kRegPage = 0x7FEA1000;   // guest page holding the XMA registers 0x400-0x7FF
constexpr uint32_t kRegBase = 0x7FEA0000;
constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kPacketBits = kPacketBytes * 8;
constexpr uint32_t kBlockBytes = 256;
constexpr int kSampleRates[4] = { 24000, 32000, 44100, 48000 };

enum : uint32_t
{
    RegContextArrayAddress = 0x600,
    RegKick = 0x650,
    RegLock = 0x690,
    RegClear = 0x6A0,
};

struct Context
{
    std::atomic<bool> allocated{ false }, kicked{ false };
    std::atomic_flag busy = ATOMIC_FLAG_INIT;   // worker <-> register writes (signal handler)
    bool needsReset = false;
    AVCodecContext* av = nullptr;
    int avRate = 0, avChannels = 0;
    bool fedSinceDrain = false;
    std::vector<uint8_t> pcm;   // decoded, not yet written (s16 BE, interleaved)

    void Lock() { while (busy.test_and_set(std::memory_order_acquire)) { } }
    void Unlock() { busy.clear(std::memory_order_release); }
};

Context s_ctx[kContextCount];
uint32_t s_arrayPhys = 0;          // physical address of the context array
uint8_t* s_regsRW = nullptr;       // writable alias of the register page
sem_t s_work;
std::mutex s_allocLock;
bool s_log = false;
AVPacket* s_packet = nullptr;
AVFrame* s_frame = nullptr;

uint8_t* Phys(uint32_t phys) { return g_base + 0xA0000000u + (phys & 0x1FFFFFFF); }
uint8_t* ContextData(uint32_t i) { return Phys(s_arrayPhys + i * kContextSize); }
uint32_t ReadDword(uint32_t i, int d) { uint32_t v; memcpy(&v, ContextData(i) + d * 4, 4); return __builtin_bswap32(v); }
void WriteDword(uint32_t i, int d, uint32_t v) { v = __builtin_bswap32(v); memcpy(ContextData(i) + d * 4, &v, 4); }
uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

void SetReg(uint32_t reg, uint32_t value) { memcpy(s_regsRW + reg * 4 - (kRegPage - kRegBase), &value, 4); }

// Context data (dwords after byte swap; bit 0 = lowest):
//  d0: ib0 packet count 0-11, loop count 12-19, ib0 valid 20, ib1 valid 21, out block count 22-26, out write offset 27-31
//  d1: ib1 packet count 0-11, ..., out padding 24-26, sample rate 27-28, stereo 29, output valid 31
//  d2: input read offset (bits) 0-25      d3: loop start 0-25      d4: loop end 0-25, current buffer 31
//  d5/d6: input buffer 0/1 (physical)     d7: output buffer (physical)     d9: output read offset 0-4
void ClearContextData(uint32_t i)
{
    uint32_t d0 = ReadDword(i, 0), d1 = ReadDword(i, 1), d2 = ReadDword(i, 2), d9 = ReadDword(i, 9);
    d0 &= ~((1u << 20) | (1u << 21) | (31u << 27));   // inputs invalid, write offset 0
    d1 &= ~(1u << 31);                                // output invalid
    d2 = (d2 & ~0x3FFFFFFu) | 32;                     // read offset: first frame of packet 0
    d9 &= ~31u;                                       // output read offset 0
    WriteDword(i, 0, d0); WriteDword(i, 1, d1); WriteDword(i, 2, d2); WriteDword(i, 9, d9);
}

// Register write from the guest (signal handler context: no allocation, no locks other than
// the per-context spin flag the worker holds briefly).
void WriteRegister(uint32_t reg, uint32_t value)
{
    SetReg(reg, value);
    auto forEach = [&](uint32_t group, auto&& fn) {
        for (uint32_t bits = value; bits; bits &= bits - 1)
        {
            uint32_t id = group * 32 + __builtin_ctz(bits);
            if (id < kContextCount) fn(s_ctx[id], id);
        }
    };
    if (reg >= RegKick && reg < RegKick + 10)
    {
        forEach(reg - RegKick, [](Context& c, uint32_t) { c.kicked.store(true, std::memory_order_release); });
        sem_post(&s_work);
    }
    else if (reg >= RegLock && reg < RegLock + 10)
        forEach(reg - RegLock, [](Context& c, uint32_t) {
            c.kicked.store(false);
            c.Lock();   // wait for the worker to leave it
            c.Unlock();
        });
    else if (reg >= RegClear && reg < RegClear + 10)
        forEach(reg - RegClear, [](Context& c, uint32_t id) {
            c.Lock();
            ClearContextData(id);
            c.needsReset = true;
            c.Unlock();
        });
}

// ------------------------------------------------------------------ decoding

bool EnsureDecoder(Context& c, int rate, int channels)
{
    if (c.av && c.avRate == rate && c.avChannels == channels) return true;
    if (c.av) avcodec_free_context(&c.av);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!codec) return false;
    c.av = avcodec_alloc_context3(codec);
    av_channel_layout_default(&c.av->ch_layout, channels);
    c.av->sample_rate = rate;
    c.av->block_align = kPacketBytes;
    // XMA2WAVEFORMATEX-sized extradata: 1 stream, no channel mask
    c.av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    c.av->extradata_size = 34;
    c.av->extradata[0] = 1;
    if (avcodec_open2(c.av, codec, nullptr) < 0) { avcodec_free_context(&c.av); return false; }
    c.avRate = rate;
    c.avChannels = channels;
    c.fedSinceDrain = false;
    c.pcm.clear();
    return true;
}

void AppendFrame(Context& c, const AVFrame* f)
{
    static const char* dumpDir = getenv("BO3_XMA_DUMP");   // diagnostics: raw decoder output per context
    if (dumpDir && f->format == AV_SAMPLE_FMT_FLTP)
    {
        static FILE* files[16] = {};
        uint32_t id = uint32_t(&c - s_ctx);
        if (id < 16)
        {
            if (!files[id]) { char p[512]; snprintf(p, sizeof(p), "%s/ctx%02u_%dch_%d.raw", dumpDir, id, c.avChannels, c.avRate); files[id] = fopen(p, "wb"); }
            for (int s = 0; s < f->nb_samples; s++)
                for (int k = 0; k < c.avChannels; k++) fwrite(reinterpret_cast<const float*>(f->extended_data[std::min(k, f->ch_layout.nb_channels - 1)]) + s, 4, 1, files[id]);
            fflush(files[id]);
        }
    }
    static bool once = false;
    if (!once) { once = true; Log("xma: decoder output format %d, %d samples, %d channels, %d Hz", f->format, f->nb_samples, f->ch_layout.nb_channels, f->sample_rate); }
    int ch = std::min(f->ch_layout.nb_channels, 2);
    size_t start = c.pcm.size();
    c.pcm.resize(start + size_t(f->nb_samples) * c.avChannels * 2);
    uint8_t* out = c.pcm.data() + start;
    for (int s = 0; s < f->nb_samples; s++)
        for (int k = 0; k < c.avChannels; k++)
        {
            float v = reinterpret_cast<const float*>(f->extended_data[std::min(k, ch - 1)])[s];
            int16_t x = int16_t(std::lrint(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
            *out++ = uint8_t(uint16_t(x) >> 8);
            *out++ = uint8_t(x);
        }
}

void Receive(Context& c)
{
    while (avcodec_receive_frame(c.av, s_frame) == 0)
    {
        AppendFrame(c, s_frame);
        av_frame_unref(s_frame);
    }
}

void FeedPacket(Context& c, const uint8_t* packet)
{
    static uint8_t buf[kPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE];
    memcpy(buf, packet, kPacketBytes);
    memset(buf + kPacketBytes, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    s_packet->data = buf;
    s_packet->size = kPacketBytes;
    if (avcodec_send_packet(c.av, s_packet) == 0) c.fedSinceDrain = true;
    Receive(c);
}

void Drain(Context& c)
{
    avcodec_send_packet(c.av, nullptr);
    Receive(c);
    avcodec_flush_buffers(c.av);
    c.fedSinceDrain = false;
}

void Work(uint32_t id)
{
    Context& c = s_ctx[id];
    c.Lock();
    if (!c.allocated)
    {
        c.Unlock();
        return;
    }
    if (c.needsReset)
    {
        c.needsReset = false;
        if (c.av) avcodec_flush_buffers(c.av);
        c.fedSinceDrain = false;
        c.pcm.clear();
    }
    uint32_t d0 = ReadDword(id, 0), d1 = ReadDword(id, 1), d2 = ReadDword(id, 2), d3 = ReadDword(id, 3), d4 = ReadDword(id, 4);
    uint32_t inPtr[2] = { ReadDword(id, 5), ReadDword(id, 6) }, outPtr = ReadDword(id, 7), d9 = ReadDword(id, 9);
    if (!Bits(d1, 31, 1) || !outPtr)   // output buffer not valid
    {
        c.Unlock();
        return;
    }
    const uint32_t capacity = Bits(d0, 22, 5), readBlock = Bits(d9, 0, 5);
    uint32_t writeBlock = Bits(d0, 27, 5);
    const int channels = Bits(d1, 29, 1) ? 2 : 1;
    if (!capacity || !EnsureDecoder(c, kSampleRates[Bits(d1, 27, 2)], channels))
    {
        c.Unlock();
        return;
    }
    uint32_t valid[2] = { Bits(d0, 20, 1), Bits(d0, 21, 1) }, count[2] = { Bits(d0, 0, 12), Bits(d1, 0, 12) };
    uint32_t cur = Bits(d4, 31, 1), readOffset = Bits(d2, 0, 26), loopCount = Bits(d0, 12, 8);
    const uint32_t loopStart = Bits(d3, 0, 26), loopEnd = Bits(d4, 0, 26);
    bool invalidated[2] = { false, false }, curChanged = false, loopChanged = false;

    uint32_t freeBlocks = readBlock > writeBlock ? readBlock - writeBlock : capacity - writeBlock + readBlock;
    uint32_t written = 0;
    int packetsFed = 0;
    while (freeBlocks > 0)
    {
        // decode until a whole output block is available
        while (c.pcm.size() < kBlockBytes && packetsFed < 64)
        {
            if (!valid[cur])
            {
                if (!valid[cur ^ 1]) break;
                cur ^= 1;
                curChanged = true;
                readOffset = 32;
            }
            uint32_t packet = readOffset / kPacketBits;
            bool pastLoopEnd = loopCount && loopEnd && readOffset >= loopEnd;
            if (packet >= count[cur] || pastLoopEnd)
            {
                if (loopCount)   // loop back (packet granularity); 255 = forever
                {
                    if (loopCount != 255) { loopCount--; loopChanged = true; }
                    readOffset = loopStart ? loopStart : 32;
                    if (readOffset / kPacketBits >= count[cur]) readOffset = 32;
                    continue;
                }
                valid[cur] = 0;   // buffer consumed: hand it back, continue with the other one
                invalidated[cur] = true;
                cur ^= 1;
                curChanged = true;
                readOffset = 32;
                continue;
            }
            const uint8_t* p = Phys(inPtr[cur] + packet * kPacketBytes);
            FeedPacket(c, p);
            packetsFed++;
            // header byte 3: packets to skip (other streams' packets); 0xFF = this packet starts no
            // frame, the next one simply follows
            uint32_t next = packet + 1 + (p[3] == 0xFF ? 0 : p[3]);
            if (s_log)
            {
                static int n = 0;
                if (n++ < 40)
                    Log("xma: ctx %u packet %u/%u: frames %u frame-offset %u metadata %u skip %u | %02X %02X %02X %02X %02X %02X %02X %02X", id,
                        packet, count[cur], p[0] >> 2, (((p[0] & 3) << 13) | (p[1] << 5) | (p[2] >> 3)) + 32, p[2] & 7, p[3], p[0], p[1], p[2],
                        p[3], p[4], p[5], p[6], p[7]);
            }
            readOffset = next * kPacketBits + 32;
        }
        if (c.pcm.size() < kBlockBytes && !valid[0] && !valid[1] && c.fedSinceDrain)
        {
            Drain(c);   // end of the sound: get the decoder's buffered tail
            if (!c.pcm.empty() && c.pcm.size() < kBlockBytes) c.pcm.resize(kBlockBytes, 0);
        }
        if (c.pcm.size() < kBlockBytes) break;
        memcpy(Phys(outPtr + writeBlock * kBlockBytes), c.pcm.data(), kBlockBytes);
        c.pcm.erase(c.pcm.begin(), c.pcm.begin() + kBlockBytes);
        writeBlock = (writeBlock + 1) % capacity;
        freeBlocks--;
        written++;
    }

    // write back decoder-owned fields onto the current guest values (the game may have changed
    // others meanwhile)
    d0 = ReadDword(id, 0);
    d1 = ReadDword(id, 1);
    d2 = ReadDword(id, 2);
    d4 = ReadDword(id, 4);
    if (invalidated[0]) d0 &= ~(1u << 20);
    if (invalidated[1]) d0 &= ~(1u << 21);
    d0 = (d0 & ~(31u << 27)) | (writeBlock << 27);
    if (loopChanged) d0 = (d0 & ~(0xFFu << 12)) | (loopCount << 12);
    if (written && freeBlocks == 0) d1 &= ~(1u << 31);   // output ring full
    d2 = (d2 & ~0x3FFFFFFu) | (readOffset & 0x3FFFFFF);
    if (curChanged) d4 = (d4 & ~(1u << 31)) | (cur << 31);
    WriteDword(id, 0, d0);
    WriteDword(id, 1, d1);
    WriteDword(id, 2, d2);
    WriteDword(id, 4, d4);
    c.Unlock();
    if (s_log && (written || packetsFed))
        Log("xma: ctx %u: %d packets -> %u blocks (ring %u/%u, buffers %u%u, %d Hz %dch)", id, packetsFed, written, writeBlock,
            capacity, valid[0], valid[1], c.avRate, channels);
}

void Worker()
{
    pthread_setname_np(pthread_self(), "xma-decoder");
    for (;;)
    {
        sem_wait(&s_work);
        for (uint32_t i = 0; i < kContextCount; i++)
            if (s_ctx[i].kicked.exchange(false, std::memory_order_acq_rel)) Work(i);
    }
}

// ------------------------------------------------------------------ store decoding
#if defined(__aarch64__)
// ARM64: STR/STUR of a W/X register (unsigned offset, unscaled, pre/post-indexed, register
// offset). value = the low 32 bits stored; pre/post-indexed forms write the base back.
bool DecodeStore(uint32_t inst, ucontext_t* uc, uint32_t& value)
{
    if (((inst >> 27) & 7) != 7 || ((inst >> 26) & 1) != 0 || ((inst >> 22) & 3) != 0) return false;   // integer store
    uint32_t size = inst >> 30, rt = inst & 31, rn = (inst >> 5) & 31;
    if (size != 2) return false;   // 32-bit register writes only
    uint32_t cls = (inst >> 24) & 3;
    auto reg = [&](uint32_t r) -> decltype(uc->uc_mcontext.sp)& { return r == 31 ? uc->uc_mcontext.sp : uc->uc_mcontext.regs[r]; };
    value = rt == 31 ? 0 : uint32_t(uc->uc_mcontext.regs[rt]);   // (Rt 31 is the zero register)
    if (cls == 1) return true;                                    // unsigned offset
    if (cls != 0) return false;
    if ((inst >> 21) & 1) return ((inst >> 10) & 3) == 2;         // register offset
    uint32_t mode = (inst >> 10) & 3;                             // 0 unscaled, 1 post, 3 pre, 2 unprivileged
    if (mode == 1 || mode == 3)
    {
        int64_t imm = int64_t(int32_t(inst << 11) >> 23);         // imm9, sign-extended
        reg(rn) += uint64_t(imm);
    }
    return true;
}
#else
// ------------------------------------------------------------------ x86 store decoding

// Value stored by the faulting instruction (a 32-bit store to the register page) and its
// length. Handles mov r/m32,r32 (89), mov r/m32,imm32 (C7 /0) and movbe m32,r32 (0F 38 F1)
// with REX prefixes and any ModRM/SIB/displacement form.
bool DecodeStore(const uint8_t* ip, const ucontext_t* uc, uint32_t& value, int& length)
{
    static const int kRegs[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                   REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
    const uint8_t* p = ip;
    uint8_t rex = 0;
    if ((*p & 0xF0) == 0x40) rex = *p++;
    int kind;   // 0 = mov reg, 1 = mov imm, 2 = movbe
    if (*p == 0x89) { kind = 0; p++; }
    else if (*p == 0xC7) { kind = 1; p++; }
    else if (p[0] == 0x0F && p[1] == 0x38 && p[2] == 0xF1) { kind = 2; p += 3; }
    else return false;
    uint8_t modrm = *p++;
    int mod = modrm >> 6, reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0), rm = modrm & 7;
    if (mod == 3) return false;
    if (rm == 4)
    {
        uint8_t sib = *p++;
        if (mod == 0 && (sib & 7) == 5) p += 4;
    }
    else if (mod == 0 && rm == 5) p += 4;
    if (mod == 1) p += 1;
    else if (mod == 2) p += 4;
    if (kind == 1)
    {
        memcpy(&value, p, 4);
        p += 4;
    }
    else
    {
        value = uint32_t(uc->uc_mcontext.gregs[kRegs[reg]]);
        if (kind == 2) value = __builtin_bswap32(value);
    }
    length = int(p - ip);
    return true;
}
#endif
}

// ------------------------------------------------------------------ API

void XmaInit()
{
    s_log = getenv("BO3_XMA_LOG") != nullptr;
    av_log_set_level(AV_LOG_QUIET);
    s_packet = av_packet_alloc();
    s_frame = av_frame_alloc();
    sem_init(&s_work, 0, 0);
    s_arrayPhys = g_physicalHeap.Alloc(kContextCount * kContextSize, 256) & 0x1FFFFFFF;

    // register page: guest view read-only, host alias writable
    int fd = HostMemfd("bo3-xma-regs");
    if (fd < 0 || ftruncate(fd, 0x1000) != 0) Fatal("xma: memfd failed");
    if (mmap(g_base + kRegPage, 0x1000, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) Fatal("xma: register page map failed");
    s_regsRW = static_cast<uint8_t*>(mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    // the game reads it with lwbrx: store the value in host (little-endian) order
    SetReg(RegContextArrayAddress, s_arrayPhys);
    std::thread(Worker).detach();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    Log("xma: %u contexts at physical 0x%08X, FFmpeg %s decoder %s", kContextCount, s_arrayPhys, avcodec_version() ? "libavcodec" : "?",
        codec ? "found" : "MISSING (no XMA audio)");
}

bool XmaHandleFault(void* addr, void* ucontext)
{
    uintptr_t a = uintptr_t(addr);
    if (!s_regsRW || a < uintptr_t(g_base + kRegPage) || a >= uintptr_t(g_base + kRegPage + 0x1000)) return false;
    auto* uc = static_cast<ucontext_t*>(ucontext);
    uint32_t value;
#if defined(__aarch64__)
    uint32_t inst;
    memcpy(&inst, reinterpret_cast<const void*>(HostPc(uc)), 4);
    if (!DecodeStore(inst, uc, value)) return false;
    int length = 4;
#else
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(HostPc(uc));
    int length;
    if (!DecodeStore(ip, uc, value, length)) return false;
#endif
    uint32_t reg = uint32_t(a - uintptr_t(g_base) - kRegBase) / 4;
    WriteRegister(reg, value);
    HostPc(uc) += length;
    return true;
}

// XMACreateContext(context**): a free context of the array (virtual address in the physical view)
GUEST_IMPORT(XMACreateContext)
{
    std::lock_guard lock(s_allocLock);
    for (uint32_t i = 0; i < kContextCount; i++)
        if (!s_ctx[i].allocated)
        {
            Context& c = s_ctx[i];
            c.Lock();
            memset(ContextData(i), 0, kContextSize);
            c.allocated = true;
            c.kicked = false;
            c.needsReset = true;
            c.Unlock();
            GuestWrite32(ARG(3), 0xA0000000u | (s_arrayPhys + i * kContextSize));
            RETURN(0);
        }
    GuestWrite32(ARG(3), 0);
    RETURN(0x80004005u);
}

GUEST_IMPORT(XMAReleaseContext)
{
    uint32_t phys = ARG(3) & 0x1FFFFFFF;
    if (phys >= s_arrayPhys && phys < s_arrayPhys + kContextCount * kContextSize)
    {
        Context& c = s_ctx[(phys - s_arrayPhys) / kContextSize];
        c.Lock();
        c.allocated = false;
        c.kicked = false;
        c.needsReset = true;
        memset(ContextData((phys - s_arrayPhys) / kContextSize), 0, kContextSize);
        c.Unlock();
    }
    RETURN(0);
}
