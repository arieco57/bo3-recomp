#pragma once
#include <vector>
// Renderer interface used by the command processor (gpu.cpp).
#include <cstdint>

// Register file access (owned by gpu.cpp).
uint32_t GpuReg(uint32_t index);
float GpuRegF(uint32_t index);
const uint32_t* GpuRegsRaw();
extern uint64_t g_gpuConstDirty[3];   // dirty 16-register chunks of 0x4000..0x492F (renderer clears)     // the register file (guest MMIO mirror, big-endian dwords)
extern uint64_t g_gpuConstGen;    // bumped when a constant/fetch/bool/loop register changes

void RendererInit();                       // on the GPU thread, before the first packet
void RendererShaderLoad(uint32_t type, uint32_t guestAddr, uint32_t dwords, const uint32_t* inlineCode);
// Split form (parser thread / recording thread): hash (optionally with the microcode), then set.
uint64_t RendererShaderHash(uint32_t type, uint32_t guestAddr, uint32_t dwords, const uint32_t* inlineCode, std::vector<uint32_t>* code);
void RendererShaderSet(uint32_t type, uint64_t h, std::vector<uint32_t> code);
void RendererDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSize, bool indexed);
void RendererSwap(uint32_t frontBufferPhys, uint32_t width, uint32_t height);
// Before a CPU-visible fence: finish GPU work that writes guest memory (superseded by deferred writes).
void RendererFlushGuestWrites();
#include <functional>
// A CPU-visible write (fence) that must only land once the GPU work recorded so far is done.
void RendererDeferGuestWrite(std::function<void()> write);
// Same for a 32-bit store of `hostBytes` (as they lie in memory) at guest address `addr`;
// RendererPendingGuestWrite finds the latest such write not yet visible.
void RendererDeferGuestWrite32(uint32_t addr, uint32_t hostBytes);
bool RendererPendingGuestWrite(uint32_t addr, uint32_t& hostBytes);
void RendererWaitPendingWrites();   // submit + wait until all deferred writes are visible
// Submit recorded work so pending deferred writes can complete (CP idle / waiting on memory).
void RendererSubmitPending();

// Internal render resolution multiplier (1 = the console's resolution). See video options.
float RendererScale();
// Request a new internal resolution multiplier (1..4); applied at the next frame boundary.
void RendererSetScale(float scale);
// BO3_AB_TOGGLE=n: flips every n stats windows (120 frames) so a change can be measured against
// the old path in the same run and scene (code under test checks it; logged with the stats).
extern bool g_ab;
// true while variant B runs and the change `name` is under test (BO3_AB_FEATURE=<name>; unset: all)
bool AbOld(const char* name);
extern bool g_computeDispatched;   // set by compute dispatches (graphics push constants must be re-sent)
// Performance overlay: 0 off, 1 FPS, 2 detailed (any thread).
void RendererSetOverlay(int mode);
int RendererOverlay();
// Frame rate limit (30..240): rate of the emulated vsync interrupt the game paces on (gpu.cpp).
void GpuSetFrameLimit(int hz);
int GpuFrameLimit();
