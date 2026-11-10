// Host renderer: turns Xenos draws into Vulkan work. All calls come from the GPU thread.
//
// Model (simplified "host render targets", like Xenia's RTV/DSV path):
//   * every EDRAM render-target configuration (base tile, pitch, format) is a Vulkan image;
//   * draws render into the bound images with dynamic rendering;
//   * a resolve (draw with RB_MODECONTROL = copy) copies a rectangle of a render target into a
//     "resolved texture" keyed by its guest physical destination, optionally clearing the RT;
//   * a swap presents the resolved texture at the front-buffer address.
// Shaders are translated from Xenos microcode (shader_translator.cpp); vertex and index data
// are read in the shader directly from guest memory (imported as a storage buffer).
#include "renderer.h"
#include "shader_translator.h"
#include "texture_cache.h"
#include "overlay.h"
#include "../platform.h"
#include "vk.h"
#include "../log.h"
#include "../memory.h"
#include "../write_watch.h"

#include "xenia/gpu/registers.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace xe::gpu;

bool CompileGlslStage(int stage, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& error);

namespace
{
constexpr uint32_t kFrames = 2;
constexpr uint32_t kUniformRingSize = 64 * 1024 * 1024;
constexpr uint32_t kIndexRingSize = 16 * 1024 * 1024;   // host-endian copies of the frame's index buffers
constexpr uint32_t kRtHeight = 2048;

template <class T> T Reg() { T v; v.value = GpuReg(T::register_index); return v; }

const uint32_t kColorInfoRegs[4] = { XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO, XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO };
const uint32_t kBlendRegs[4] = { XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1, XE_GPU_REG_RB_BLENDCONTROL2, XE_GPU_REG_RB_BLENDCONTROL3 };

struct Frame
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;   // cmd buffers of this frame (a new one after each mid-frame submit)
    uint32_t cmdIndex = 0;
    VkCommandBuffer presentCmd = VK_NULL_HANDLE;   // swapchain blit only (waits for acquire)
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkBufferObj uniforms;
    uint32_t uniformOffset = 0;
    VkBufferObj indexRing;
    uint32_t indexOffset = 0;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    bool recording = false;
    // GPU timing (BO3_GPU_TIMING): 2 timestamps per draw
    VkQueryPool queries = VK_NULL_HANDLE;
    uint32_t queryCount = 0;
    // BO3_GPU_TIMING: serialized timestamps after each operation; the delta to the previous
    // mark is that operation's cost.
    struct Mark { std::string label; };
    std::vector<Mark> marks;
    bool timingPending = false;
    // GPU busy time: a timestamp at the start and the end of every command buffer of the frame
    VkQueryPool busyQ = VK_NULL_HANDLE;
    uint32_t busyN = 0;
};

struct ShaderEntry
{
    TranslatedShader t;
    VkShaderModule module = VK_NULL_HANDLE;
    std::atomic<int> state{ 0 };   // 0 compiling (background), 1 ready, 2 failed
    bool ok() const { return state.load(std::memory_order_acquire) == 1; }
};

struct HostRT
{
    uint32_t base = 0, pitch = 0, format = 0, height = 0;   // pitch/height: host image (sample space x scale)
    bool depth = false;
    VkImageObj img;
};

struct Resolved
{
    uint32_t phys = 0, width = 0, height = 0;
    float scale = 1.0f;
    VkImageObj img;
};

struct R
{
    bool ready = false;
    Frame frames[kFrames];
    std::vector<VkSemaphore> presentReady;
    uint32_t frameIndex = 0;
    uint64_t swapCount = 0, drawCount = 0, resolveCount = 0, skipped = 0, vertCount = 0;
    uint64_t skip[12] = {};
    uint64_t passCount = 0;   // reasons a draw was dropped (see RendererDraw)

    std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> shaders;
    uint64_t shadersPendingSkips = 0;
    uint64_t curVS = 0, curPS = 0;

    VkDescriptorSetLayout setLayout0 = VK_NULL_HANDLE, setLayout1 = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool staticPool = VK_NULL_HANDLE;
    VkShaderModule rectGs = VK_NULL_HANDLE, quadGs = VK_NULL_HANDLE;
    // resolve -> guest memory (compute)
    VkDescriptorSetLayout resolveSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout resolveLayout = VK_NULL_HANDLE;
    VkPipeline resolvePipe = VK_NULL_HANDLE;
    std::map<uint32_t, Resolved> resolvedDepth;   // depth resolves by guest physical address
    uint64_t memexportDraws = 0;
    // GPU work that writes guest memory (resolve-to-memory, memexport): recorded in the open
    // command buffer / submitted but maybe not finished. See RendererFlushGuestWrites.
    bool guestWritesRecorded = false, guestWritesInFlight = false;
    uint64_t blockPushes = 0;   // constant blocks uploaded (stats)
    uint64_t stateChanges[32] = {};   // per-draw state commands recorded (stats, BO3_STATE_STATS)
    bool midFrameRestart = false;   // the frame's command buffer was submitted early (flush)
    // CPU-visible fence writes waiting for the GPU work recorded before them
    std::vector<std::function<void()>> deferred;
    std::vector<std::pair<uint32_t, uint32_t>> deferredWrites;   // (guest address, host bytes) of the 32-bit ones
    uint64_t guestWriteFlushes = 0;
    double shaderMs = 0, pipelineMs = 0, worstFrameMs = 0;   // stats per log interval
    double gpuBusyMs = 0, ovGpuMs = 0, presentMs = 0;   // (ov*: overlay window accumulators)
    uint64_t barriers = 0;
    uint64_t submits = 0, fenceBatches = 0, interruptWaits = 0;
    uint32_t drawsSinceSubmit = 0;
    struct PipeEntry { std::atomic<VkPipeline> p{ VK_NULL_HANDLE }; std::atomic<int> state{ 0 }; };   // 0 compiling, 1 ready, 2 failed
    // keyed by the PipelineDesc bytes, looked up without building a string (transparent hash)
    struct BytesHash { using is_transparent = void; size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); } };
    std::unordered_map<std::string, std::unique_ptr<PipeEntry>, BytesHash, std::equal_to<>> pipelines;
    uint64_t pipelinePendingSkips = 0;

    std::map<uint64_t, HostRT> rts;
    struct RtMemo { uint64_t key = ~0ull; HostRT* rt = nullptr; } rtMemo[4];   // last lookups (map nodes are stable; reset with rts)
    std::map<uint32_t, Resolved> resolved;   // by guest physical address

    bool pendingShot = false;
    uint32_t shotW = 0, shotH = 0;
    VkBufferObj* shotBuf = nullptr;
    std::string shotPath;

    bool inPass = false;
    VkImage passColor[4] = {};
    VkImage passDepth = VK_NULL_HANDLE;
    uint32_t passWidth = 0, passHeight = 0, passMsaa = 0;
};
R r;

Frame& Cur() { return r.frames[r.frameIndex]; }
static int g_idxBound = -1;   // index type bound in the current command buffer (-1: none)
void BusyMark(Frame& f, bool end)
{
    if (!f.busyQ || f.busyN >= 512) return;
    vkCmdWriteTimestamp(f.cmd, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.busyQ, f.busyN++);
}
void SubmitDeferredBatch(const VkSubmitInfo* si = nullptr);
void CompletionThread();
void SubmitThread();
void QueueSync();
void SubmitRecorded();
static uint64_t g_frameSerial = 0;   // incremented when a frame starts recording
// Internal resolution multiplier: EDRAM render targets and resolve images are S times the
// guest size in both dimensions (applied between frames; see RendererSetScale).
static float g_scale = 1.0f;                        // 1, 1.5, 2, 3
static std::atomic<uint32_t> g_pendingScale{ 0 };   // requested scale x 6 (0 = none)
static int R(float v) { return int(std::lround(v)); }

// Surfaces narrower than 256 samples (colour-grading LUTs, bloom chains, small effect buffers)
// stay at 1x: no gain from resolution, and position-based math in them breaks when scaled.
float SurfaceScale(uint32_t surfacePitch, uint32_t msaa)
{
    uint32_t samplePitch = surfacePitch * (msaa == 2 ? 2u : 1u);
    return samplePitch >= 256 ? g_scale : 1.0f;
}

// Host sample grid per guest pixel: EDRAM sample space (MSAA samples are separate host
// pixels - surfaces of different MSAA modes alias like in EDRAM) times the surface's
// resolution scale. Pixel shading still runs once per (scaled) pixel through the fragment
// shading rate (see RendererDraw), like real MSAA.
// Without fragment shading rate (e.g. Adreno 6xx) sample space shades every sample: 2x MSAA
// surfaces (BO3's main scene) then keep one sample per pixel - half the pixel work, no MSAA
// edges. 4x stays in full sample space: D3D clears 1x/2x depth through a 4x surface at half
// pitch, which then still covers every row (a superset for collapsed 2x surfaces, harmless).
// BO3_MSAA_FULL=1 keeps all samples.
static bool MsaaRowCollapsed()
{
    static const bool c = getenv("BO3_MSAA_COLLAPSE") || (!g_vk.hasVrs && !getenv("BO3_MSAA_FULL"));
    return c;
}
static uint32_t MsaaRows(uint32_t msaa) { return msaa >= 2 || (msaa == 1 && !MsaaRowCollapsed()) ? 2u : 1u; }

void HostGrid(uint32_t msaa, uint32_t surfacePitch, float& gx, float& gy)
{
    float mx = msaa == 2 ? 2.0f : 1.0f, my = float(MsaaRows(msaa)), S = SurfaceScale(surfacePitch, msaa);
    gx = mx * S;
    gy = my * S;
}

static int TimingMode() { static const int m = getenv("BO3_GPU_TIMING") ? atoi(getenv("BO3_GPU_TIMING")) : 0; return m; }
void TimingMarkRaw(const char* label);
void TimingMark(const char* label)
{
    if (TimingMode() == 2) return;   // per-pass mode: only EndPass / resolves mark
    TimingMarkRaw(label);
}
void TimingMarkRaw(const char* label)
{
    Frame& f = Cur();
    if (!f.queries || !f.recording || f.queryCount >= 8192) return;
    vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.queries, f.queryCount++);
    f.marks.push_back({ label });
}

uint64_t Hash(const uint32_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

void BeginFrame()
{
    Frame& f = Cur();
    if (f.recording) return;
    VK_CHECK(vkWaitForFences(g_vk.device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetCommandPool(g_vk.device, f.pool, 0));
    VK_CHECK(vkResetDescriptorPool(g_vk.device, f.descPool, 0));
    f.cmdIndex = 0;
    f.cmd = f.cmds[0];
    TextureCacheBeginFrame(r.frameIndex, !r.midFrameRestart);
    r.midFrameRestart = false;
    f.uniformOffset = 0;
    f.indexOffset = 0;
    g_frameSerial++;
    static const bool timing = getenv("BO3_GPU_TIMING") != nullptr;
    if (timing)
    {
        if (!f.queries)
        {
            VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 8192;
            VK_CHECK(vkCreateQueryPool(g_vk.device, &qi, nullptr, &f.queries));
        }
        if (f.timingPending && f.queryCount > 1)
        {
            std::vector<uint64_t> ts(f.queryCount);
            vkGetQueryPoolResults(g_vk.device, f.queries, 0, f.queryCount, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            double period = g_vk.props.limits.timestampPeriod * 1e-6;   // ms per tick
            static uint64_t frames = 0;
            static std::map<std::string, std::pair<double, uint64_t>> acc;
            static double accTotal = 0;
            for (size_t i = 1; i < f.queryCount && i < f.marks.size(); i++)
            {
                double ms = (ts[i] - ts[i - 1]) * period;
                auto& e = acc[f.marks[i].label];
                e.first += ms;
                e.second++;
                accTotal += ms;
            }
            if (++frames % 60 == 0)
            {
                std::vector<std::pair<double, std::string>> v;
                for (auto& [k, e] : acc) v.push_back({ e.first, k });
                std::sort(v.rbegin(), v.rend());
                Log("gpu timing: %.2f ms/frame over 60 frames", accTotal / 60);
                for (size_t k = 0; k < v.size() && k < 8; k++)
                    Log("  %7.2f ms/frame  %5.1f/frame  %s", v[k].first / 60, double(acc[v[k].second].second) / 60, v[k].second.c_str());
                acc.clear();
                accTotal = 0;
            }
        }
        f.queryCount = 0;
        f.marks.clear();
        f.timingPending = false;
    }
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!f.busyQ)
    {
        VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 512;
        VK_CHECK(vkCreateQueryPool(g_vk.device, &qi, nullptr, &f.busyQ));
    }
    else if (f.busyN >= 2)
    {
        uint64_t ts[512];
        if (vkGetQueryPoolResults(g_vk.device, f.busyQ, 0, f.busyN & ~1u, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (uint32_t i = 0; i + 1 < (f.busyN & ~1u); i += 2)
            {
                double ms = double(ts[i + 1] - ts[i]) * g_vk.props.limits.timestampPeriod * 1e-6;
                r.gpuBusyMs += ms;
                r.ovGpuMs += ms;
            }
    }
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    g_idxBound = -1;
    vkCmdResetQueryPool(f.cmd, f.busyQ, 0, 512);
    f.busyN = 0;
    BusyMark(f, false);
    if (f.queries) vkCmdResetQueryPool(f.cmd, f.queries, 0, 8192);
    f.recording = true;
    TimingMark("frame start");
}

void EndPass()
{
    if (!r.inPass) return;
    vkCmdEndRendering(Cur().cmd);
    r.inPass = false;
    if (TimingMode() == 2)
    {
        char label[96];
        snprintf(label, sizeof(label), "pass %ux%u m%u c%s d%s", r.passWidth, r.passHeight, r.passMsaa, r.passColor[0] ? "1" : "0", r.passDepth ? "1" : "0");
        TimingMarkRaw(label);
    }
}

void CreatePresentSemaphores()
{
    for (auto s : r.presentReady) vkDestroySemaphore(g_vk.device, s, nullptr);
    r.presentReady.resize(g_vk.swapImages.size());
    VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (auto& s : r.presentReady) VK_CHECK(vkCreateSemaphore(g_vk.device, &si, nullptr, &s));
}

VkShaderModule MakeModule(const std::vector<uint32_t>& spirv)
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(g_vk.device, &ci, nullptr, &m));
    return m;
}

// Geometry shaders for primitives Vulkan lacks. Rect list: 3 vertices of a rectangle; the 4th
// mirrors the right-angle corner (the vertex opposite the longest edge).
const char* kRectGs = R"(#version 460
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec4 iInterp[][16];
layout(location = 0) out vec4 oInterp[16];
void emitV(int i) { gl_Position = gl_in[i].gl_Position; for (int k = 0; k < 16; k++) oInterp[k] = iInterp[i][k]; EmitVertex(); }
void main() {
    vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w, p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w,
         p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;
    float d01 = distance(p0, p1), d12 = distance(p1, p2), d20 = distance(p2, p0);
    int c, a, b;
    if (d01 >= d12 && d01 >= d20) { c = 2; a = 0; b = 1; }
    else if (d12 >= d01 && d12 >= d20) { c = 0; a = 1; b = 2; }
    else { c = 1; a = 2; b = 0; }
    // strip c, a, b, d: both triangles share the a-b diagonal
    emitV(c); emitV(a); emitV(b);
    gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;
    for (int k = 0; k < 16; k++) oInterp[k] = iInterp[a][k] + iInterp[b][k] - iInterp[c][k];
    EmitVertex();
    EndPrimitive();
}
)";
const char* kQuadGs = R"(#version 460
layout(lines_adjacency) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec4 iInterp[][16];
layout(location = 0) out vec4 oInterp[16];
void emitV(int i) { gl_Position = gl_in[i].gl_Position; for (int k = 0; k < 16; k++) oInterp[k] = iInterp[i][k]; EmitVertex(); }
void main() { emitV(0); emitV(1); emitV(3); emitV(2); EndPrimitive(); }
)";

VkShaderModule CompileGs(const char* src)
{
    std::vector<uint32_t> spv;
    std::string err;
    if (!CompileGlslStage(2, src, spv, err)) Fatal("geometry shader: %s", err.c_str());
    return MakeModule(spv);
}

void CreateLayouts()
{
    VkDescriptorSetLayoutBinding b0[3] = {};
    b0[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr };
    b0[1] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr };
    b0[2] = { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr };
    VkDescriptorSetLayoutCreateInfo l0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    l0.bindingCount = 3;
    l0.pBindings = b0;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.device, &l0, nullptr, &r.setLayout0));

    VkDescriptorSetLayoutBinding b1[3] = {};
    for (uint32_t i = 0; i < 3; i++) b1[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr };
    VkDescriptorSetLayoutCreateInfo l1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    l1.bindingCount = 3;
    l1.pBindings = b1;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.device, &l1, nullptr, &r.setLayout1));

    VkDescriptorSetLayout sets[2] = { r.setLayout0, r.setLayout1 };
    VkPushConstantRange drawPc{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 /* 2 x DrawConsts */ };
    VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 2;
    pl.pSetLayouts = sets;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &drawPc;
    VK_CHECK(vkCreatePipelineLayout(g_vk.device, &pl, nullptr, &r.pipelineLayout));

    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrames }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * kFrames } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = kFrames;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(g_vk.device, &pci, nullptr, &r.staticPool));
    for (auto& f : r.frames)
    {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r.staticPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r.setLayout0;
        VK_CHECK(vkAllocateDescriptorSets(g_vk.device, &ai, &f.set0));
        VkDescriptorBufferInfo mem{ g_vk.guestBuffer, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo ubo{ f.uniforms.buffer, 0, kStageStateSize };
        VkWriteDescriptorSet w[3] = {};
        for (uint32_t i = 0; i < 3; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = f.set0; w[i].dstBinding = i; w[i].descriptorCount = 1; }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &mem;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[1].pBufferInfo = &ubo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[2].pBufferInfo = &ubo;
        vkUpdateDescriptorSets(g_vk.device, 3, w, 0, nullptr);
    }
}

// ---------------------------------------------------------------- render targets

VkFormat ColorFormat(uint32_t f)
{
    switch (xenos::ColorRenderTargetFormat(f))
    {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16: return VK_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16: return VK_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

// EDRAM render targets are host images of the sample grid (x scale). Their height grows on
// demand (the rows a surface really uses depend on predicated tiling / EDRAM wrap-around and
// cannot be derived from its base): `rows` is what the caller is about to touch.
uint32_t MaxRtRows()
{
    static const uint32_t cap = getenv("BO3_RT_ROWS") ? uint32_t(atoi(getenv("BO3_RT_ROWS"))) : kRtHeight;   // diagnostics
    return std::min<uint32_t>(uint32_t(R(float(cap) * g_scale)), 16384);
}

void EnsureRTHeight(HostRT& rt, uint32_t rows)
{
    rows = std::min<uint32_t>(rows, MaxRtRows());
    if (rt.img.image && rt.img.height >= rows) return;
    uint32_t h = std::min<uint32_t>(std::max<uint32_t>((rows + 63) & ~63u, rt.img.image ? rt.img.height + rt.img.height / 4 : 0), MaxRtRows());
    VkFormat vf = rt.depth ? VK_FORMAT_D24_UNORM_S8_UINT : ColorFormat(rt.format);
    uint32_t w = std::clamp<uint32_t>(rt.pitch, 1, 16384);
    VkImageUsageFlags common = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageObj img = rt.depth ? VkCreateImage(w, h, vf, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | common)
                              : VkCreateImage(w, h, vf, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | common);
    EndPass();
    VkCommandBuffer cmd = Cur().cmd;
    VkTransition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange range{ img.aspect, 0, 1, 0, 1 };
    if (rt.depth) { VkClearDepthStencilValue v{ 1.0f, 0 }; vkCmdClearDepthStencilImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range); }
    else { VkClearColorValue v{}; vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range); }
    if (rt.img.image)
    {
        VkTransition(cmd, rt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageCopy rc{};
        rc.srcSubresource = { rt.img.aspect, 0, 0, 1 };
        rc.dstSubresource = rc.srcSubresource;
        rc.extent = { w, rt.img.height, 1 };
        vkCmdCopyImage(cmd, rt.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
        TextureCacheForgetImage(rt.img.image);
        TextureCacheDeferDestroy(rt.img);
        Log("renderer: %s RT base %u pitch %u grown to %u rows", rt.depth ? "depth" : "color", rt.base, rt.pitch, h);
    }
    rt.img = img;
    rt.height = h;
}

HostRT& GetRT(uint32_t base, uint32_t pitch, uint32_t format, bool depth, uint32_t rows)
{
    // EDRAM aliasing: formats with the same host representation (e.g. 2_10_10_10 and
    // 2_10_10_10_AS_10_10_10_10) share one image, like they share the EDRAM tiles.
    uint32_t hostFmt = depth ? 0u : uint32_t(ColorFormat(format));
    uint64_t key = (uint64_t(base) << 44) | (uint64_t(pitch) << 30) | (uint64_t(hostFmt) << 1) | (depth ? 1 : 0);
    for (auto& m : r.rtMemo)
        if (m.key == key) { EnsureRTHeight(*m.rt, rows); return *m.rt; }
    auto it = r.rts.find(key);
    if (it != r.rts.end())
    {
        static uint32_t next = 0;
        r.rtMemo[next++ & 3] = { key, &it->second };
        EnsureRTHeight(it->second, rows);
        return it->second;
    }
    HostRT rt;
    rt.base = base; rt.pitch = pitch; rt.format = format; rt.depth = depth;
    auto& ref = r.rts.emplace(key, rt).first->second;
    EnsureRTHeight(ref, std::max<uint32_t>(rows, 64));
    Log("renderer: new %s RT base %u pitch %u format %u (%ux%u)", depth ? "depth" : "color", base, pitch, format, ref.img.width, ref.img.height);
    return ref;
}

// ---------------------------------------------------------------- pipelines

VkBlendFactor BlendFactor(xenos::BlendFactor f)
{
    switch (f)
    {
    case xenos::BlendFactor::kZero: return VK_BLEND_FACTOR_ZERO;
    case xenos::BlendFactor::kOne: return VK_BLEND_FACTOR_ONE;
    case xenos::BlendFactor::kSrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case xenos::BlendFactor::kDstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case xenos::BlendFactor::kConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case xenos::BlendFactor::kOneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case xenos::BlendFactor::kConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case xenos::BlendFactor::kOneMinusConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case xenos::BlendFactor::kSrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(xenos::BlendOp o)
{
    switch (o)
    {
    case xenos::BlendOp::kSubtract: return VK_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin: return VK_BLEND_OP_MIN;
    case xenos::BlendOp::kMax: return VK_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

struct PipelineDesc
{
    uint64_t vs, ps;
    int32_t gs;                   // 0 none, 1 rect, 2 quad
    uint32_t topologyClass;       // VkPrimitiveTopology of the draw (point/line/tri/adjacency)
    VkFormat color[4];
    uint32_t colorCount;
    VkFormat depth;
    uint32_t blend[4], writeMask[4];
    uint32_t discard;             // memexport-only draw: rasterizer discard, no attachments
};

// Pipelines are compiled by background threads (~50 ms each when the driver's cache misses -
// a new shader, a new map, or a translator update); draws needing one that is not ready are
// skipped meanwhile. BO3_SYNC_SHADERS=1 compiles inline.
std::atomic<uint64_t> g_pipelineUs{ 0 };
std::mutex s_pipeLock;
std::condition_variable s_pipeCv;
std::deque<std::function<void()>> s_pipeJobs;
std::atomic<int> s_pipeJobsPending{ 0 };

void PipelineWorker()
{
    pthread_setname_np(pthread_self(), "pipeline-build");
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(s_pipeLock);
            s_pipeCv.wait(lock, [] { return !s_pipeJobs.empty(); });
            job = std::move(s_pipeJobs.front());
            s_pipeJobs.pop_front();
        }
        job();
        s_pipeJobsPending--;
        // persist new pipelines every few seconds (the app may be killed at any time on Android)
        static std::atomic<int64_t> lastSave{ 0 };
        static std::mutex saveLock;
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - lastSave.load() >= 5 && saveLock.try_lock())
        {
            lastSave = now;
            VkSavePipelineCache();
            saveLock.unlock();
        }
    }
}

VkPipeline BuildPipeline(const PipelineDesc& d, VkShaderModule vsm, VkShaderModule psm);

VkPipeline GetPipeline(const PipelineDesc& d)
{
    // consecutive draws mostly use the same pipeline
    static PipelineDesc lastDesc;
    static VkPipeline lastPipe = VK_NULL_HANDLE;
    if (lastPipe && FastEq(&lastDesc, &d, sizeof(d))) return lastPipe;
    std::string_view key((const char*)&d, sizeof(d));
    auto it = r.pipelines.find(key);
    if (it == r.pipelines.end())
    {
        auto* e = r.pipelines.emplace(std::string(key), std::make_unique<R::PipeEntry>()).first->second.get();
        VkShaderModule vsm = r.shaders[d.vs]->module, psm = r.shaders[d.ps]->module;
        auto job = [e, d, vsm, psm] {
            VkPipeline p = BuildPipeline(d, vsm, psm);
            e->p.store(p);
            e->state.store(p ? 1 : 2, std::memory_order_release);
        };
        static const bool sync = getenv("BO3_SYNC_SHADERS") != nullptr;
        if (sync) job();
        else
        {
            static std::once_flag started;
            std::call_once(started, [] { for (int i = 0; i < 3; i++) std::thread(PipelineWorker).detach(); });
            s_pipeJobsPending++;
            {
                std::lock_guard lock(s_pipeLock);
                s_pipeJobs.push_back(job);
            }
            s_pipeCv.notify_one();
        }
        it = r.pipelines.find(key);
    }
    R::PipeEntry& e = *it->second;
    if (e.state.load(std::memory_order_acquire) != 1)
    {
        if (e.state.load() == 0) r.pipelinePendingSkips++;
        return VK_NULL_HANDLE;
    }
    lastDesc = d;
    lastPipe = e.p.load();
    return lastPipe;
}

VkPipeline BuildPipeline(const PipelineDesc& d, VkShaderModule vsm, VkShaderModule psm)
{
    VkPipelineShaderStageCreateInfo st[3] = {};
    uint32_t nst = 0;
    st[nst++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main", nullptr };
    if (d.gs) st[nst++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT, d.gs == 1 ? r.rectGs : r.quadGs, "main", nullptr };
    st[nst++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psm, "main", nullptr };

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VkPrimitiveTopology(d.topologyClass);
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.rasterizerDiscardEnable = d.discard ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = VK_TRUE;
    rs.depthBiasEnable = VK_TRUE;   // values are dynamic (0 when the guest has polygon offset off)
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState att[4] = {};
    for (uint32_t i = 0; i < d.colorCount; i++)
    {
        reg::RB_BLENDCONTROL b;
        b.value = d.blend[i];
        att[i].colorWriteMask = d.writeMask[i];
        bool opaque = b.color_srcblend == xenos::BlendFactor::kOne && b.color_destblend == xenos::BlendFactor::kZero &&
                      b.color_comb_fcn == xenos::BlendOp::kAdd && b.alpha_srcblend == xenos::BlendFactor::kOne &&
                      b.alpha_destblend == xenos::BlendFactor::kZero && b.alpha_comb_fcn == xenos::BlendOp::kAdd;
        att[i].blendEnable = !opaque && d.color[i] != VK_FORMAT_UNDEFINED;
        att[i].srcColorBlendFactor = BlendFactor(b.color_srcblend);
        att[i].dstColorBlendFactor = BlendFactor(b.color_destblend);
        att[i].colorBlendOp = BlendOp(b.color_comb_fcn);
        att[i].srcAlphaBlendFactor = BlendFactor(b.alpha_srcblend);
        att[i].dstAlphaBlendFactor = BlendFactor(b.alpha_destblend);
        att[i].alphaBlendOp = BlendOp(b.alpha_comb_fcn);
    }
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = d.colorCount;
    cb.pAttachments = att;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                             VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                             VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
                             VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR /* last: only with VRS */ };
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = uint32_t(std::size(dyn)) - (g_vk.hasVrs ? 0 : 1);
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = d.colorCount;
    ri.pColorAttachmentFormats = d.color;
    ri.depthAttachmentFormat = d.depth;
    ri.stencilAttachmentFormat = d.depth;

    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    gp.stageCount = nst;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = r.pipelineLayout;
    VkPipeline p = VK_NULL_HANDLE;
    auto t0 = std::chrono::steady_clock::now();
    VkResult res = vkCreateGraphicsPipelines(g_vk.device, g_vk.pipelineCache, 1, &gp, nullptr, &p);
    g_pipelineUs += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    if (res != VK_SUCCESS) { LogError("renderer: pipeline creation failed (%d)", int(res)); p = VK_NULL_HANDLE; }
    return p;
}

// ---------------------------------------------------------------- uniforms

struct DrawConsts
{
    float ndcScale[4], ndcOffset[4];
    uint32_t drawInfo[4], psInfo[4];
};
static_assert(2 * sizeof(DrawConsts) == 128);
struct StageBlock
{
    float c[256][4];
    uint32_t fetch[192];
    uint32_t boolc[8];
    uint32_t loopc[32];
};

static_assert(sizeof(StageBlock) == kStageStateSize);

// Consecutive draws usually share their constants: reuse the previous block of the same stage
// (compared against a host copy - the ring itself is write-combined memory).

// Only the float constant groups in `chunks` (bit i = c[4i..4i+3]) and the fetch constant
// uvec4s in `fetch` are written - the rest of the slot is never read by the draws using it;
// bool / loop constants always are.
void CopyMasked(uint8_t* dst, const uint8_t* src, uint64_t mask, uint32_t unit)
{
    while (mask)
    {
        uint32_t first = uint32_t(__builtin_ctzll(mask));
        uint64_t run = mask >> first;
        uint32_t n = run == ~0ull >> first ? 64 - first : uint32_t(__builtin_ctzll(~run));
        memcpy(dst + first * unit, src + first * unit, n * unit);
        mask = n + first >= 64 ? 0 : mask & ~(((1ull << n) - 1) << first);
    }
}

uint32_t PushStageBlock(const StageBlock& b, uint64_t chunks, uint64_t fetch)
{
    Frame& f = Cur();
    uint32_t align = uint32_t(g_vk.props.limits.minUniformBufferOffsetAlignment);
    uint32_t off = (f.uniformOffset + align - 1) & ~(align - 1);
    if (off + sizeof(b) > kUniformRingSize)
    {
        static int n = 0;
        if (n++ % 1000 == 0) LogWarn("renderer: uniform ring full (%d times)", n);
        return UINT32_MAX;
    }
    uint8_t* dst = (uint8_t*)f.uniforms.mapped + off;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(&b);
    CopyMasked(dst, src, chunks, 64);
    CopyMasked(dst + offsetof(StageBlock, fetch), src + offsetof(StageBlock, fetch), fetch & ((1ull << 48) - 1), 16);
    memcpy(dst + offsetof(StageBlock, boolc), src + offsetof(StageBlock, boolc), sizeof(b) - offsetof(StageBlock, boolc));
    f.uniformOffset = off + uint32_t(sizeof(b));
    r.blockPushes++;
    return off;
}

void FillCommon(StageBlock& b, uint32_t constBase)
{
    // The register file lives in the guest MMIO mirror (big-endian; the CPU may write it too).
    const uint32_t* mmio = GpuRegsRaw();
    auto copy = [&](void* dst, uint32_t reg, uint32_t n) {
        uint32_t* d = static_cast<uint32_t*>(dst);
        for (uint32_t i = 0; i < n; i++) d[i] = __builtin_bswap32(mmio[reg + i]);
    };
    copy(b.c, 0x4000 + constBase * 4, 1024);
    copy(b.fetch, 0x4800, 192);
    copy(b.boolc, 0x4900, 8);
    copy(b.loopc, 0x4908, 32);
}

ShaderEntry* GetShader(uint64_t h)
{
    auto it = r.shaders.find(h);
    if (it == r.shaders.end()) return nullptr;
    if (!it->second->ok()) { if (it->second->state.load() == 0) r.shadersPendingSkips++; return nullptr; }
    return it->second.get();
}

// Clears a rectangle of an attachment with a tiny rendering pass.
void ClearRect(HostRT& rt, const VkClearRect& rect, const VkClearValue& v)
{
    VkCommandBuffer cmd = Cur().cmd;
    VkRenderingAttachmentInfo a{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkImageLayout layout = rt.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkTransition(cmd, rt.img, layout);
    a.imageView = rt.img.view;
    a.imageLayout = layout;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { rt.img.width, rt.img.height } };
    ri.layerCount = 1;
    if (rt.depth) { ri.pDepthAttachment = &a; ri.pStencilAttachment = &a; }
    else { ri.colorAttachmentCount = 1; ri.pColorAttachments = &a; }
    vkCmdBeginRendering(cmd, &ri);
    VkClearAttachment ca{ rt.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, v };
    vkCmdClearAttachments(cmd, 1, &ca, 1, &rect);
    vkCmdEndRendering(cmd);
}

// ---------------------------------------------------------------- resolve

// BO3_FRAME_LOG + BO3_FRAME_DUMP=dir: every resolve of the logged frame is written to
// dir/NN_<addr>.ppm (HDR formats clamped to [0,1]) with per-channel means in the log.
struct PendingDump { VkBufferObj buf; uint32_t w, h; VkFormat fmt; std::string path; };
static std::vector<PendingDump> s_dumps;

void DumpImage(VkCommandBuffer cmd, VkImageObj& img, uint32_t w, uint32_t h, const char* tag)
{
    static const char* dir = getenv("BO3_FRAME_DUMP");
    if (!dir || !g_frameLog || s_dumps.size() >= 40) return;   // (disk space)
    PendingDump d;
    d.w = std::min(w, img.width);
    d.h = std::min(h, img.height);
    d.fmt = img.format;
    d.buf = VkCreateBuffer(uint64_t(d.w) * d.h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    char path[512];
    snprintf(path, sizeof(path), "%s/%02zu_%s.ppm", dir, s_dumps.size(), tag);
    d.path = path;
    VkImageLayout old = img.layout;
    VkTransition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bic{};
    bool depth = (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    bic.imageSubresource = { depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1 };
    bic.imageExtent = { d.w, d.h, 1 };
    vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.buf.buffer, 1, &bic);
    if (old != VK_IMAGE_LAYOUT_UNDEFINED) VkTransition(cmd, img, old);
    s_dumps.push_back(std::move(d));
}

static float HalfToFloat(uint16_t h)
{
    uint32_t sign = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
    if (e == 0) return (sign ? -1.0f : 1.0f) * float(m) * (1.0f / 16777216.0f);
    else if (e == 31) bits = sign | 0x7F800000 | (m << 13);
    else bits = sign | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

void WriteDumps()
{
    if (s_dumps.empty()) return;
    QueueSync();
    vkQueueWaitIdle(g_vk.queue);
    for (auto& d : s_dumps)
    {
        std::vector<uint8_t> rgb(size_t(d.w) * d.h * 3);
        double sum[4] = {}, mx[4] = {};
        const uint8_t* p = (const uint8_t*)d.buf.mapped;
        for (uint32_t i = 0; i < d.w * d.h; i++)
        {
            float v[4] = { 0, 0, 0, 1 };
            switch (d.fmt)
            {
            case VK_FORMAT_R8G8B8A8_UNORM: for (int k = 0; k < 4; k++) v[k] = p[i * 4 + k] / 255.0f; break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { uint32_t w; memcpy(&w, p + i * 4, 4); v[0] = (w & 1023) / 1023.0f; v[1] = ((w >> 10) & 1023) / 1023.0f; v[2] = ((w >> 20) & 1023) / 1023.0f; v[3] = (w >> 30) / 3.0f; break; }
            case VK_FORMAT_R16G16B16A16_SFLOAT: for (int k = 0; k < 4; k++) { uint16_t hh; memcpy(&hh, p + i * 8 + k * 2, 2); v[k] = HalfToFloat(hh); } break;
            case VK_FORMAT_R16G16B16A16_SNORM: for (int k = 0; k < 4; k++) { int16_t hh; memcpy(&hh, p + i * 8 + k * 2, 2); v[k] = hh / 32767.0f; } break;
            case VK_FORMAT_R16G16_SFLOAT: for (int k = 0; k < 2; k++) { uint16_t hh; memcpy(&hh, p + i * 4 + k * 2, 2); v[k] = HalfToFloat(hh); } break;
            case VK_FORMAT_R32_SFLOAT: memcpy(&v[0], p + i * 4, 4); v[1] = v[2] = v[0]; break;
            case VK_FORMAT_D24_UNORM_S8_UINT: { uint32_t w; memcpy(&w, p + i * 4, 4); v[0] = v[1] = v[2] = (w & 0xFFFFFF) / 16777215.0f; break; }
            default: break;
            }
            for (int k = 0; k < 4; k++) { sum[k] += v[k]; mx[k] = std::max(mx[k], double(v[k])); }
            for (int k = 0; k < 3; k++) rgb[i * 3 + k] = uint8_t(std::clamp(v[k], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        double n = double(d.w) * d.h;
        Log("frame: dump %s %ux%u fmt %d mean %.3f %.3f %.3f %.3f max %.2f %.2f %.2f %.2f", d.path.c_str(), d.w, d.h, int(d.fmt),
            sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, mx[0], mx[1], mx[2], mx[3]);
        if (FILE* f = fopen(d.path.c_str(), "wb"))
        {
            fprintf(f, "P6\n%u %u\n255\n", d.w, d.h);
            fwrite(rgb.data(), 1, rgb.size(), f);
            fclose(f);
        }
        vkDestroyBuffer(g_vk.device, d.buf.buffer, nullptr);
        vkFreeMemory(g_vk.device, d.buf.memory, nullptr);
    }
    s_dumps.clear();
}

// Writes a resolved rectangle into guest memory like the hardware: destination format and
// number format, exponent bias (fixed-point formats), R/B swap, endian, 2D tiling.
const char* kResolveCs = R"(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, std430) buffer GuestMemory { uint mem[]; };
layout(set = 0, binding = 1) uniform sampler2D src;
layout(push_constant) uniform P { ivec4 rect; uvec4 info; } pc;

int tiled2D(int x, int y, uint pitchAligned, int log2bpb) {
    int outer = ((y >> 5) * int(pitchAligned >> 5) + (x >> 5)) << 6;
    int inner = (((y >> 1) & 7) << 3) | (x & 7);
    int oib = (outer | inner) << log2bpb;
    int bank = (y >> 4) & 1;
    int pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 15) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
uint swapE(uint v, uint e) {
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (e == 2u || e == 4u || e == 5u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}
uint packN(float v, uint number, uint bits) {
    float m = float((1u << bits) - 1u);
    if (number == 0u) return uint(clamp(v, 0.0, 1.0) * m + 0.5);                  // unorm
    if (number == 1u) {                                                            // snorm
        float h = float((1u << (bits - 1u)) - 1u);
        return uint(int(round(clamp(v, -1.0, 1.0) * h))) & ((1u << bits) - 1u);
    }
    if (number == 3u) {                                                            // sint
        float h = float((1u << (bits - 1u)) - 1u);
        return uint(int(round(clamp(v, -h - 1.0, h)))) & ((1u << bits) - 1u);
    }
    return uint(clamp(round(v), 0.0, m));                                          // uint
}
void main() {
    ivec2 g = ivec2(gl_GlobalInvocationID.xy);
    if (g.x >= pc.rect.z || g.y >= pc.rect.w) return;
    int x = pc.rect.x + g.x, y = pc.rect.y + g.y;
    // host texels per guest pixel, x12 fixed point (fractional with 2/3 or 1.5x resolution scales)
    vec2 fs = vec2(float((pc.info.w >> 8) & 255u), float((pc.info.w >> 16) & 255u)) / 12.0;
    ivec2 a = ivec2(floor(vec2(x, y) * fs)), b = max(a + 1, ivec2(floor(vec2(x + 1, y + 1) * fs)));
    int ssx = b.x - a.x, ssy = b.y - a.y;
    vec4 v = vec4(0.0);
    for (int j = 0; j < ssy; j++)
        for (int i = 0; i < ssx; i++) v += texelFetch(src, a + ivec2(i, j), 0);
    v /= float(ssx * ssy);
    uint fmt = pc.info.z & 0xFFu, number = (pc.info.z >> 8) & 7u, endian = (pc.info.z >> 12) & 7u;
    bool isFloat = fmt == 30u || fmt == 31u || fmt == 32u || fmt == 36u || fmt == 37u || fmt == 38u;
    if (!isFloat) v *= exp2(float(bitfieldExtract(int(pc.info.w), 0, 8)));
    if (((pc.info.z >> 16) & 1u) != 0u) v = v.bgra;
    uint w0 = 0u, w1 = 0u;
    int log2bpb = 2;
    if (fmt == 6u) w0 = packN(v.r, number, 8u) | (packN(v.g, number, 8u) << 8) | (packN(v.b, number, 8u) << 16) | (packN(v.a, number, 8u) << 24);
    else if (fmt == 7u) w0 = packN(v.r, number, 10u) | (packN(v.g, number, 10u) << 10) | (packN(v.b, number, 10u) << 20) | (packN(v.a, number, 2u) << 30);
    else if (fmt == 25u) w0 = packN(v.r, number, 16u) | (packN(v.g, number, 16u) << 16);
    else if (fmt == 26u) { w0 = packN(v.r, number, 16u) | (packN(v.g, number, 16u) << 16); w1 = packN(v.b, number, 16u) | (packN(v.a, number, 16u) << 16); log2bpb = 3; }
    else if (fmt == 31u) w0 = packHalf2x16(v.rg);
    else if (fmt == 32u) { w0 = packHalf2x16(v.rg); w1 = packHalf2x16(v.ba); log2bpb = 3; }
    else if (fmt == 36u) w0 = floatBitsToUint(v.r);
    else if (fmt == 37u) { w0 = floatBitsToUint(v.r); w1 = floatBitsToUint(v.g); log2bpb = 3; }
    else return;
    uint addr = pc.info.x + uint(tiled2D(x, y, pc.info.y, log2bpb));
    if (log2bpb == 3) {
        if (endian == 4u) { uint t = w0; w0 = w1; w1 = t; }
        mem[(addr >> 2) & 0x7FFFFFFu] = swapE(w0, endian);
        mem[((addr >> 2) + 1u) & 0x7FFFFFFu] = swapE(w1, endian);
    } else {
        mem[(addr >> 2) & 0x7FFFFFFu] = swapE(w0, endian);
    }
}
)";

void CreateResolvePipeline()
{
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.device, &li, nullptr, &r.resolveSetLayout));
    VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &r.resolveSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(g_vk.device, &pl, nullptr, &r.resolveLayout));
    std::vector<uint32_t> spv;
    std::string err;
    if (!CompileGlslStage(3, kResolveCs, spv, err)) Fatal("resolve compute shader: %s", err.c_str());
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, MakeModule(spv), "main", nullptr };
    ci.layout = r.resolveLayout;
    VK_CHECK(vkCreateComputePipelines(g_vk.device, g_vk.pipelineCache, 1, &ci, nullptr, &r.resolvePipe));
}

void MemoryBarrierAll(VkCommandBuffer cmd)
{
    g_vkBarrierCount++;
    VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

// Recorded GPU write to guest memory: the texture cache's write tracking learns it now (a check
// before the GPU ran still reads the old data) and again when the submission completes.
void GpuWroteGuest(uint32_t phys, uint32_t size)
{
    WatchMarkDirty(phys, size);
    RendererDeferGuestWrite([phys, size] { WatchMarkDirty(phys, size); });
}

// Resolve of a colour RT rectangle into guest memory (outside a rendering pass).
void ResolveToMemory(VkCommandBuffer cmd, HostRT& src, int x0, int y0, int x1, int y1, uint32_t destBase, uint32_t destPitch,
                     const reg::RB_COPY_DEST_INFO& di, float ssx, float ssy)
{
    static VkSampler pointSampler = VK_NULL_HANDLE;
    if (!pointSampler)
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        VK_CHECK(vkCreateSampler(g_vk.device, &si, nullptr, &pointSampler));
    }
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = Cur().descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r.resolveSetLayout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) return;
    VkTransition(cmd, src.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorBufferInfo mem{ g_vk.guestBuffer, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo img{ pointSampler, src.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w[2] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &mem, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &img, nullptr, nullptr };
    vkUpdateDescriptorSets(g_vk.device, 2, w, 0, nullptr);
    struct { int32_t rect[4]; uint32_t info[4]; } pc;
    pc.rect[0] = x0; pc.rect[1] = y0; pc.rect[2] = x1 - x0; pc.rect[3] = y1 - y0;
    pc.info[0] = destBase;
    pc.info[1] = (destPitch + 31) & ~31u;
    pc.info[2] = uint32_t(di.copy_dest_format) | (uint32_t(di.copy_dest_number) << 8) | (uint32_t(di.copy_dest_endian) << 12) |
                 (uint32_t(di.copy_dest_swap) << 16);
    pc.info[3] = (uint32_t(int32_t(di.copy_dest_exp_bias)) & 0xFFu) | (uint32_t(R(ssx * 12)) << 8) | (uint32_t(R(ssy * 12)) << 16);
    MemoryBarrierAll(cmd);
    r.guestWritesRecorded = true;
    // (generous: tiled rows of 32, up to 16 bytes per pixel)
    GpuWroteGuest(destBase, pc.info[1] * ((uint32_t(y1) + 31) & ~31u) * 16);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.resolvePipe);
    g_computeDispatched = true;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.resolveLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, r.resolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (pc.rect[2] + 7) / 8, (pc.rect[3] + 7) / 8, 1);
    MemoryBarrierAll(cmd);
}

void Resolve()
{
    auto copy = Reg<reg::RB_COPY_CONTROL>();
    auto surface = Reg<reg::RB_SURFACE_INFO>();
    uint32_t destBase = GpuReg(XE_GPU_REG_RB_COPY_DEST_BASE) & 0x1FFFFFFF;
    auto destPitch = Reg<reg::RB_COPY_DEST_PITCH>();
    // Rectangle: bounding box of the 3 rect-list vertices in vertex fetch constant 0.
    uint32_t d0 = GpuReg(0x4800), d1 = GpuReg(0x4801);
    uint32_t vaddr = 0xA0000000 | ((d0 & ~3u) & 0x1FFFFFFF);
    uint32_t endian = d1 & 3;
    float v[6];
    for (int i = 0; i < 6; i++)
    {
        uint32_t w;
        memcpy(&w, g_base + vaddr + i * 4, 4);
        switch (endian) { case 1: w = ((w & 0x00FF00FF) << 8) | ((w & 0xFF00FF00) >> 8); break; case 2: w = __builtin_bswap32(w); break; case 3: w = (w << 16) | (w >> 16); break; }
        memcpy(&v[i], &w, 4);
    }
    int x0 = int(std::floor(std::min({ v[0], v[2], v[4] }) + 0.5f)), y0 = int(std::floor(std::min({ v[1], v[3], v[5] }) + 0.5f));
    int x1 = int(std::floor(std::max({ v[0], v[2], v[4] }) + 0.5f)), y1 = int(std::floor(std::max({ v[1], v[3], v[5] }) + 0.5f));
    if (Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
    {
        auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
        x0 += wo.window_x_offset; x1 += wo.window_x_offset; y0 += wo.window_y_offset; y1 += wo.window_y_offset;
    }
    // rect is in pixels; the source RT is stored in sample space (see RendererDraw)
    const uint32_t msaa = uint32_t(surface.msaa_samples);
    const float S = SurfaceScale(surface.surface_pitch, msaa);
    float ssx, ssy;   // guest pixel -> host sample grid
    HostGrid(uint32_t(msaa), surface.surface_pitch, ssx, ssy);
    uint32_t pitch = surface.surface_pitch;
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min<int>(x1, int(pitch)); y1 = std::min<int>(y1, int(float(kRtHeight) * S / ssy));
    if (x1 <= x0 || y1 <= y0 || !pitch) return;
    const uint32_t samplePitch = uint32_t(R(float(pitch) * ssx));
    // Copies a pixel rect of the (sample-space) source to the same pixel rect of `dst`,
    // averaging (colour) or picking (depth) the samples.
    auto copyRect = [&](VkCommandBuffer cmd, HostRT& from, VkImageObj& dst, bool depth) {
        VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        if (ssx == 1 && ssy == 1)
        {
            VkImageCopy rc{};
            rc.srcSubresource = { aspect, 0, 0, 1 };
            rc.dstSubresource = rc.srcSubresource;
            rc.srcOffset = { x0, y0, 0 };
            rc.dstOffset = { x0, y0, 0 };
            rc.extent = { uint32_t(x1 - x0), uint32_t(y1 - y0), 1 };
            vkCmdCopyImage(cmd, from.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
        }
        else
        {
            VkImageBlit b{};
            b.srcSubresource = { aspect, 0, 0, 1 };
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[0] = { R(x0 * ssx), R(y0 * ssy), 0 };
            b.srcOffsets[1] = { R(x1 * ssx), std::min<int>(R(y1 * ssy), int(from.img.height)), 1 };
            b.dstOffsets[0] = { R(x0 * S), R(y0 * S), 0 };
            b.dstOffsets[1] = { R(x1 * S), std::min<int>(R(y1 * S), int(dst.height)), 1 };
            vkCmdBlitImage(cmd, from.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                           depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        }
    };
    r.resolveCount++;

    bool depthSrc = copy.copy_src_select >= 4;
    if (g_frameLog)
    {
        auto di = Reg<reg::RB_COPY_DEST_INFO>();
        reg::RB_COLOR_INFO ci;
        ci.value = GpuReg(kColorInfoRegs[copy.copy_src_select & 3]);
        Log("frame: RESOLVE msaa %u src %s base %u fmt %u pitch %u rect %d,%d-%d,%d -> 0x%08X dest fmt %u number %u exp_bias %d endian %u array %u "
            "pitch %u h %u cmd %u clear c%u d%u",
            uint32_t(surface.msaa_samples), depthSrc ? "depth" : "color", depthSrc ? Reg<reg::RB_DEPTH_INFO>().depth_base : ci.color_base,
            depthSrc ? uint32_t(Reg<reg::RB_DEPTH_INFO>().depth_format) : uint32_t(ci.color_format), pitch, x0, y0, x1, y1, destBase,
            uint32_t(di.copy_dest_format), uint32_t(di.copy_dest_number), int(di.copy_dest_exp_bias), uint32_t(di.copy_dest_endian),
            uint32_t(di.copy_dest_array), uint32_t(destPitch.copy_dest_pitch), uint32_t(destPitch.copy_dest_height),
            uint32_t(copy.copy_command), uint32_t(copy.color_clear_enable), uint32_t(copy.depth_clear_enable));
        Log("frame:   dest swap %u", uint32_t(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_swap));
    }
    HostRT* src;
    if (depthSrc)
    {
        auto di = Reg<reg::RB_DEPTH_INFO>();
        src = &GetRT(di.depth_base, samplePitch, uint32_t(di.depth_format), true, uint32_t(R(y1 * ssy)));
    }
    else
    {
        reg::RB_COLOR_INFO ci;
        ci.value = GpuReg(kColorInfoRegs[copy.copy_src_select]);
        src = &GetRT(ci.color_base, samplePitch, uint32_t(ci.color_format), false, uint32_t(R(y1 * ssy)));
    }

    EndPass();
    VkCommandBuffer cmd = Cur().cmd;
    if (copy.copy_command != xenos::CopyCommand::kNull && destBase && depthSrc)
    {
        // Depth resolve: keep a depth image for 24_8 texture fetches at the destination.
        uint32_t dw = uint32_t(std::ceil(std::max<uint32_t>(destPitch.copy_dest_pitch, uint32_t(x1)) * S)), dh = uint32_t(std::ceil(std::max<uint32_t>(destPitch.copy_dest_height, uint32_t(y1)) * S));
        auto& res = r.resolvedDepth[destBase];
        if (!res.img.image || res.img.width < dw || res.img.height < dh)
        {
            if (res.img.image) { TextureCacheForgetImage(res.img.image); TextureCacheDeferDestroy(res.img); }
            res.img = VkCreateImage(dw, dh, src->img.format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            res.phys = destBase; res.width = dw; res.height = dh;
            Log("renderer: depth resolve target 0x%08X %ux%u", destBase, dw, dh);
        }
        VkTransition(cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkTransition(cmd, res.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyRect(cmd, *src, res.img, true);
        VkTransition(cmd, res.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        {
            char tag[64];
            snprintf(tag, sizeof(tag), "%08X_depth", destBase);
            DumpImage(cmd, res.img, uint32_t(x1), uint32_t(y1), tag);
        }
        res.scale = S;
        TextureCacheRegisterDepthResolve(destBase, &res.img, S);
        TimingMark("resolve depth copy");
        if (TimingMode() == 2) TimingMarkRaw("resolve depth");
    }
    if (copy.copy_command != xenos::CopyCommand::kNull && destBase && !depthSrc)
    {
        static const bool noResolveMem = getenv("BO3_NO_RESOLVE_MEM") != nullptr;   // diagnostics
        // Only resolves read back through memory are written there (each one forces a GPU
        // sync before the next CPU-visible fence). BO3_RESOLVE_MEM_ALL=1 writes all of them.
        static const bool allMem = getenv("BO3_RESOLVE_MEM_ALL") != nullptr;
        if (!noResolveMem && (allMem || TextureCacheResolveNeedsMemory(destBase)))
            ResolveToMemory(cmd, *src, x0, y0, x1, y1, destBase, destPitch.copy_dest_pitch, Reg<reg::RB_COPY_DEST_INFO>(), ssx, ssy);
        TimingMark("resolve to memory");
        uint32_t dw = uint32_t(std::ceil(std::max<uint32_t>(destPitch.copy_dest_pitch, uint32_t(x1)) * S)), dh = uint32_t(std::ceil(std::max<uint32_t>(destPitch.copy_dest_height, uint32_t(y1)) * S));
        auto& res = r.resolved[destBase];
        if (!res.img.image || res.img.width < dw || res.img.height < dh || res.img.format != src->img.format)
        {
            if (res.img.image) { TextureCacheForgetImage(res.img.image); TextureCacheDeferDestroy(res.img); }
            res.img = VkCreateImage(dw, dh, src->img.format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            res.phys = destBase;
            res.width = dw;
            res.height = dh;
            Log("renderer: resolve target 0x%08X %ux%u", destBase, dw, dh);
        }
        VkTransition(cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkTransition(cmd, res.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyRect(cmd, *src, res.img, false);
        VkTransition(cmd, res.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        {
            char tag[64];
            snprintf(tag, sizeof(tag), "%08X_fmt%u", destBase, src->format);
            DumpImage(cmd, res.img, uint32_t(x1), uint32_t(y1), tag);
        }
        res.scale = S;
        TextureCacheRegisterResolve(destBase, &res.img, uint32_t(res.width), uint32_t(res.height), Reg<reg::RB_COPY_DEST_INFO>().copy_dest_swap, S);
        TimingMark("resolve copy");
        if (TimingMode() == 2) TimingMarkRaw("resolve color");
    }
    VkClearRect cr{ { { R(x0 * ssx), R(y0 * ssy) }, { uint32_t(R(x1 * ssx) - R(x0 * ssx)), uint32_t(R(y1 * ssy) - R(y0 * ssy)) } }, 0, 1 };
    auto clampRect = [&](HostRT& rt) {
        VkClearRect c2 = cr;
        int maxH = int(rt.img.height) - c2.rect.offset.y;
        if (maxH <= 0) c2.rect.extent.height = 0;
        else c2.rect.extent.height = std::min<uint32_t>(c2.rect.extent.height, uint32_t(maxH));
        return c2;
    };
    if (copy.color_clear_enable)
    {
        reg::RB_COLOR_INFO ci;
        ci.value = GpuReg(kColorInfoRegs[copy.copy_src_select & 3]);
        HostRT& rt = GetRT(ci.color_base, samplePitch, uint32_t(ci.color_format), false, uint32_t(R(y1 * ssy)));
        uint32_t c = GpuReg(XE_GPU_REG_RB_COLOR_CLEAR);
        VkClearValue v{};
        v.color = { { (c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, (c >> 24) / 255.0f } };
        VkClearRect c2 = clampRect(rt);
        if (c2.rect.extent.height) ClearRect(rt, c2, v);
        TimingMark("resolve color clear");
    }
    if (copy.depth_clear_enable)
    {
        auto di = Reg<reg::RB_DEPTH_INFO>();
        HostRT& rt = GetRT(di.depth_base, samplePitch, uint32_t(di.depth_format), true, uint32_t(R(y1 * ssy)));
        uint32_t dc = GpuReg(XE_GPU_REG_RB_DEPTH_CLEAR);
        VkClearValue v{};
        v.depthStencil = { float(dc >> 8) / float(0xFFFFFF), dc & 0xFF };
        VkClearRect c2 = clampRect(rt);
        if (c2.rect.extent.height) ClearRect(rt, c2, v);
        TimingMark("resolve depth clear");
    }
}
}

// ======================================================================= API

namespace { void ShaderCacheInit(); void ShaderWorker(); }

void RendererInit()
{
    VkInit();
    for (auto& f : r.frames)
    {
        VkCommandPoolCreateInfo pi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.queueFamilyIndex = g_vk.queueFamily;
        VK_CHECK(vkCreateCommandPool(g_vk.device, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(g_vk.device, &ai, &f.cmd));
        f.cmds.push_back(f.cmd);
        VK_CHECK(vkAllocateCommandBuffers(g_vk.device, &ai, &f.presentCmd));
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(g_vk.device, &fi, nullptr, &f.fence));
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(g_vk.device, &si, nullptr, &f.acquired));
        f.uniforms = VkCreateBuffer(kUniformRingSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f.indexRing = VkCreateBuffer(kIndexRingSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 96 * 8192 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024 } };
        VkDescriptorPoolCreateInfo dp{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dp.maxSets = 8192;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = ps;
        VK_CHECK(vkCreateDescriptorPool(g_vk.device, &dp, nullptr, &f.descPool));
    }
    CreatePresentSemaphores();
    CreateLayouts();
    r.rectGs = CompileGs(kRectGs);
    r.quadGs = CompileGs(kQuadGs);
    CreateResolvePipeline();
    std::thread(CompletionThread).detach();
    std::thread(SubmitThread).detach();
    ShaderCacheInit();
    if (const char* e = getenv("BO3_RENDER_SCALE")) g_scale = float(std::lround(std::clamp(float(atof(e)), 0.5f, 4.0f) * 6)) / 6.0f;
    Log("renderer: internal scale x%g", g_scale);
    for (int i = 0; i < 4; i++) std::thread(ShaderWorker).detach();
    TextureCacheInit(r.setLayout1);
    r.ready = true;
    Log("renderer: ready");
}

// ---------------------------------------------------------------- shader cache + async compile
// Translating a Xenos shader and compiling the GLSL takes ~10 ms; doing it on the command
// processor thread when a new shader shows up made the game hitch (over a second when a map
// starts). Compiled shaders are kept on disk (shadercache/, invalidated when the translator is
// rebuilt) and new ones are compiled by background threads; until then draws using them are
// skipped (BO3_SYNC_SHADERS=1 compiles inline instead).
namespace
{
const char* kShaderCacheDir = "shadercache";
struct ShaderCacheHeader
{
    uint32_t magic, pixel, texturesUsed;
    uint8_t dim[32], mag[32], min[32];
    uint32_t colorExports, writesDepth, kills, memexports, spirvWords;
    uint8_t memexportConsts[4], memexportConstCount, pad[3];
    uint64_t constChunks, fetchVec4;
};

std::string ShaderCachePath(uint64_t h)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%016llx.spv", kShaderCacheDir, (unsigned long long)h);
    return path;
}

void ShaderCacheInit()
{
    std::string id = std::string(ShaderTranslatorBuildId()) + " / " + std::to_string(sizeof(ShaderCacheHeader));
    std::string versionPath = std::string(kShaderCacheDir) + "/version";
    char buf[128] = {};
    if (FILE* f = fopen(versionPath.c_str(), "r")) { size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f); }
    if (id != buf)
    {
        std::error_code ec;
        std::filesystem::remove_all(kShaderCacheDir, ec);
        std::filesystem::create_directories(kShaderCacheDir, ec);
        if (FILE* f = fopen(versionPath.c_str(), "w")) { fputs(id.c_str(), f); fclose(f); }
        Log("renderer: shader cache reset (translator %s)", ShaderTranslatorBuildId());
    }
}

bool ShaderCacheLoad(uint64_t h, TranslatedShader& t)
{
    FILE* f = fopen(ShaderCachePath(h).c_str(), "rb");
    if (!f) return false;
    ShaderCacheHeader hd;
    bool ok = fread(&hd, sizeof(hd), 1, f) == 1 && hd.magic == 0x42335348 && hd.spirvWords > 0 && hd.spirvWords < (16u << 20);
    if (ok)
    {
        t.spirv.resize(hd.spirvWords);
        ok = fread(t.spirv.data(), 4, hd.spirvWords, f) == hd.spirvWords;
        t.pixel = hd.pixel; t.texturesUsed = hd.texturesUsed;
        memcpy(t.textureDim, hd.dim, 32); memcpy(t.texMag, hd.mag, 32); memcpy(t.texMin, hd.min, 32);
        t.colorExports = hd.colorExports; t.writesDepth = hd.writesDepth; t.kills = hd.kills; t.memexports = hd.memexports;
        memcpy(t.memexportConsts, hd.memexportConsts, 4); t.memexportConstCount = hd.memexportConstCount;
        t.constChunks = hd.constChunks; t.fetchVec4 = hd.fetchVec4;
    }
    fclose(f);
    return ok;
}

void ShaderCacheStore(uint64_t h, const TranslatedShader& t)
{
    ShaderCacheHeader hd{};
    hd.magic = 0x42335348; hd.pixel = t.pixel; hd.texturesUsed = t.texturesUsed;
    memcpy(hd.dim, t.textureDim, 32); memcpy(hd.mag, t.texMag, 32); memcpy(hd.min, t.texMin, 32);
    hd.colorExports = t.colorExports; hd.writesDepth = t.writesDepth; hd.kills = t.kills; hd.memexports = t.memexports;
    memcpy(hd.memexportConsts, t.memexportConsts, 4); hd.memexportConstCount = t.memexportConstCount;
    hd.constChunks = t.constChunks; hd.fetchVec4 = t.fetchVec4;
    hd.spirvWords = uint32_t(t.spirv.size());
    std::string path = ShaderCachePath(h), tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(&hd, sizeof(hd), 1, f) == 1 && fwrite(t.spirv.data(), 4, t.spirv.size(), f) == t.spirv.size();
    fclose(f);
    if (ok) std::rename(tmp.c_str(), path.c_str()); else std::remove(tmp.c_str());
}

// Translate + compile + module (any thread).
void BuildShader(uint64_t h, uint32_t type, ShaderEntry& e, std::vector<uint32_t> code)
{
    auto t0 = std::chrono::steady_clock::now();
    e.t = TranslateShader(type != 0, code.data(), uint32_t(code.size()));
    if (!e.t.error.empty())
    {
        LogError("renderer: %s shader %016llx failed: %s", type ? "pixel" : "vertex", (unsigned long long)h, e.t.error.c_str());
        e.state.store(2, std::memory_order_release);
        return;
    }
    ShaderCacheStore(h, e.t);
    e.t.glsl.clear();
    e.t.glsl.shrink_to_fit();
    e.module = MakeModule(e.t.spirv);
    e.state.store(1, std::memory_order_release);
    static std::atomic<uint64_t> totalUs{ 0 };
    totalUs += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    (void)totalUs;
}

struct ShaderJob { uint64_t h; uint32_t type; ShaderEntry* e; std::vector<uint32_t> code; };
std::mutex s_jobLock;
std::condition_variable s_jobCv;
std::deque<ShaderJob> s_jobs;
std::atomic<int> s_jobsPending{ 0 };

void ShaderWorker()
{
    pthread_setname_np(pthread_self(), "shader-compile");
    for (;;)
    {
        ShaderJob j;
        {
            std::unique_lock lock(s_jobLock);
            s_jobCv.wait(lock, [] { return !s_jobs.empty(); });
            j = std::move(s_jobs.front());
            s_jobs.pop_front();
        }
        BuildShader(j.h, j.type, *j.e, std::move(j.code));
        s_jobsPending--;
    }
}
}

// Hash of a shader's microcode (one thread only: the command processor, or its parser).
// D3D reloads the microcode for almost every draw: the hash of a shader in physical memory
// stays valid while the write tracking reports no store into it. `code` (optional) gets the
// microcode.
uint64_t RendererShaderHash(uint32_t type, uint32_t guestAddr, uint32_t dwords, const uint32_t* inlineCode, std::vector<uint32_t>* code)
{
    auto read = [&](std::vector<uint32_t>& out) {
        out.resize(dwords);
        for (uint32_t i = 0; i < dwords; i++) out[i] = inlineCode ? inlineCode[i] : GuestRead32(guestAddr + i * 4);
    };
    struct ShaderMemo { uint32_t stamp = 0; uint64_t h = 0; };
    static std::unordered_map<uint64_t, ShaderMemo> memo;
    ShaderMemo* m = nullptr;
    if (!inlineCode && WatchEnabled() && guestAddr >= 0xA0000000u && guestAddr < 0xE0000000u)
    {
        m = &memo[uint64_t(guestAddr) | (uint64_t(dwords) << 32) | (uint64_t(type) << 62)];
        if (WatchCheckAndArm(guestAddr & 0x1FFFFFFFu, dwords * 4, m->stamp) && m->h)
        {
            if (code) read(*code);
            return m->h;
        }
    }
    std::vector<uint32_t> local;
    read(local);
    uint64_t h = Hash(local.data(), local.size()) ^ (uint64_t(type) << 63);
    if (m) m->h = h;
    if (code) *code = std::move(local);
    return h;
}

void RendererShaderLoad(uint32_t type, uint32_t guestAddr, uint32_t dwords, const uint32_t* inlineCode)
{
    uint64_t h = RendererShaderHash(type, guestAddr, dwords, inlineCode, nullptr);
    if (r.shaders.count(h)) { if (type == 0) r.curVS = h; else r.curPS = h; return; }
    std::vector<uint32_t> code;
    RendererShaderHash(type, guestAddr, dwords, inlineCode, &code);
    RendererShaderSet(type, h, std::move(code));
}

// Make shader `h` current; translate it from `code` if it is new.
void RendererShaderSet(uint32_t type, uint64_t h, std::vector<uint32_t> code)
{
    if (type == 0) r.curVS = h; else r.curPS = h;
    if (r.shaders.count(h) || code.empty()) return;

    static const char* dumpDir = getenv("BO3_DUMP_SHADERS");
    if (dumpDir)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%016llx.bin", dumpDir, type == 0 ? "vs" : "ps", (unsigned long long)h);
        if (FILE* f = fopen(path, "wb")) { fwrite(code.data(), 4, code.size(), f); fclose(f); }
    }
    auto t0 = std::chrono::steady_clock::now();
    struct AddMs { std::chrono::steady_clock::time_point t; ~AddMs() { r.shaderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); } } addMs{ t0 };
    ShaderEntry* e = r.shaders.emplace(h, std::make_unique<ShaderEntry>()).first->second.get();
    if (!r.ready) { e->state = 2; return; }
    static const bool noCache = getenv("BO3_NO_SHADER_CACHE") != nullptr;
    if (!noCache && ShaderCacheLoad(h, e->t))
    {
        e->module = MakeModule(e->t.spirv);
        e->state.store(1, std::memory_order_release);
        return;
    }
    static const bool sync = getenv("BO3_SYNC_SHADERS") != nullptr;
    if (sync) { BuildShader(h, type, *e, std::move(code)); return; }
    s_jobsPending++;
    {
        std::lock_guard lock(s_jobLock);
        s_jobs.push_back({ h, type, e, std::move(code) });
    }
    s_jobCv.notify_one();
}

void RendererDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSize, bool indexed)
{
    if (!r.ready) return;
    BeginFrame();
    reg::VGT_DRAW_INITIATOR di;
    di.value = initiator;
    auto mode = Reg<reg::RB_MODECONTROL>().edram_mode;
    if (mode == xenos::EdramMode::kCopy) { Resolve(); return; }
    if (mode != xenos::EdramMode::kColorDepth && mode != xenos::EdramMode::kDepthOnly) { r.skip[0]++; return; }

    uint32_t count = di.num_indices;
    if (!count) { r.skip[1]++; return; }
    VkPrimitiveTopology topo;
    int gs = 0;
    switch (di.prim_type)
    {
    case xenos::PrimitiveType::kPointList: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case xenos::PrimitiveType::kLineList: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleList: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case xenos::PrimitiveType::kTriangleFan: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case xenos::PrimitiveType::kTriangleStrip: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case xenos::PrimitiveType::kRectangleList: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; gs = 1; break;
    case xenos::PrimitiveType::kQuadList: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; gs = 2; break;
    default:
        if (r.skipped++ < 10) LogWarn("renderer: unsupported primitive type %u", uint32_t(di.prim_type));
        r.skip[2]++;
        return;
    }
    ShaderEntry* vs = GetShader(r.curVS);
    ShaderEntry* ps = GetShader(r.curPS);
    if (!vs || !ps) { r.skip[3]++; return; }

    auto surface = Reg<reg::RB_SURFACE_INFO>();
    uint32_t colorMask = GpuReg(XE_GPU_REG_RB_COLOR_MASK);
    auto depthCtl = Reg<reg::RB_DEPTHCONTROL>();
    // MSAA surfaces live in sample space (EDRAM stores samples): 2x doubles the height, 4x
    // both dimensions. Host RTs are single-sampled images of that sample grid, so a 4x clear
    // at half pitch and a 2x scene at full pitch hit the same image, like on hardware.
    const uint32_t msaa = uint32_t(surface.msaa_samples);
    float ssx, ssy;   // guest pixel -> host sample grid
    HostGrid(msaa, surface.surface_pitch, ssx, ssy);
    const float surfScale = SurfaceScale(surface.surface_pitch, msaa);
    const uint32_t msx = msaa == 2 ? 2 : 1, msy = MsaaRows(msaa);   // host pixels per scaled pixel (MSAA samples)
    uint32_t pitch = uint32_t(R(float(surface.surface_pitch) * ssx));
    if (!pitch) { r.skip[4]++; return; }
    // Rows this draw can touch: the viewport bottom, limited by the scissor (both in guest
    // pixels, with the window offset) - the targets grow to that.
    uint32_t needRows;
    {
        auto vte0 = Reg<reg::PA_CL_VTE_CNTL>();
        auto sc0 = Reg<reg::PA_SU_SC_MODE_CNTL>();
        float woy = sc0.vtx_window_offset_enable ? float(Reg<reg::PA_SC_WINDOW_OFFSET>().window_y_offset) : 0.0f;
        float vpBottom = 1e9f;
        if (vte0.vport_y_scale_ena && vte0.vport_y_offset_ena)
            vpBottom = GpuRegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET) + std::fabs(GpuRegF(XE_GPU_REG_PA_CL_VPORT_YSCALE)) + woy;
        auto stl0 = Reg<reg::PA_SC_WINDOW_SCISSOR_TL>();
        auto sbr0 = Reg<reg::PA_SC_WINDOW_SCISSOR_BR>();
        float scBottom = float(sbr0.br_y) + (stl0.window_offset_disable ? 0.0f : float(Reg<reg::PA_SC_WINDOW_OFFSET>().window_y_offset));
        float bottom = std::min(vpBottom, scBottom);
        if (!(bottom > 0.0f)) bottom = 1.0f;
        needRows = uint32_t(R((std::min(bottom, float(kRtHeight)) + 1.0f) * ssy));
    }
    PipelineDesc pd;
    memset(&pd, 0, sizeof(pd));   // compared bytewise (padding included)
    pd.vs = r.curVS;
    pd.ps = r.curPS;
    pd.gs = gs;
    pd.topologyClass = topo;
    HostRT* colors[4] = {};
    if (mode == xenos::EdramMode::kColorDepth)
        for (uint32_t i = 0; i < 4; i++)
        {
            uint32_t m = (colorMask >> (i * 4)) & 0xF;
            if (!m || !(ps->t.colorExports & (1u << i))) continue;
            reg::RB_COLOR_INFO ci;
            ci.value = GpuReg(kColorInfoRegs[i]);
            colors[i] = &GetRT(ci.color_base, pitch, uint32_t(ci.color_format), false, needRows);
            pd.color[i] = colors[i]->img.format;
            pd.blend[i] = GpuReg(kBlendRegs[i]);
            pd.writeMask[i] = m;
            pd.colorCount = i + 1;
        }
    HostRT* depth = nullptr;
    if (depthCtl.z_enable || depthCtl.stencil_enable)
    {
        auto dinfo = Reg<reg::RB_DEPTH_INFO>();
        depth = &GetRT(dinfo.depth_base, pitch, uint32_t(dinfo.depth_format), true, needRows);
        pd.depth = depth->img.format;
    }
    static const bool noMemexport = getenv("BO3_NO_MEMEXPORT") != nullptr;   // diagnostics
    if (!pd.colorCount && !depth && vs->t.memexports && !noMemexport)
        pd.discard = 1;   // the vertex shader only writes memory
    else if (!pd.colorCount && !depth)
    {
        if (r.skip[5]++ < 8)
            LogWarn("renderer: draw without targets: mode %u colorMask 0x%04X psExports 0x%X depthctl 0x%08X colorinfo0 0x%08X prim %u count %u",
                    uint32_t(mode), colorMask, ps->t.colorExports, depthCtl.value, GpuReg(XE_GPU_REG_RB_COLOR_INFO), uint32_t(di.prim_type), count);
        return;
    }
    VkPipeline pipe = GetPipeline(pd);
    if (!pipe) { r.skip[6]++; return; }

    Frame& f = Cur();
    VkCommandBuffer cmd = f.cmd;

    // Textures first: binding them may need layout transitions, which cannot happen inside
    // a rendering pass.
    VkDescriptorSet set1 = TextureCacheBuildSet(cmd, f.descPool, vs->t, ps->t, [] { EndPass(); });
    if (!set1) { r.skip[7]++; return; }

    VkImage wantColor[4] = {};
    for (uint32_t i = 0; i < 4; i++) wantColor[i] = colors[i] ? colors[i]->img.image : VK_NULL_HANDLE;
    VkImage wantDepth = depth ? depth->img.image : VK_NULL_HANDLE;
    if (!r.inPass || !FastEq(wantColor, r.passColor, sizeof(wantColor)) || wantDepth != r.passDepth)
    {
        EndPass();
        VkRenderingAttachmentInfo ca[4] = {};
        uint32_t w = UINT32_MAX, h = kRtHeight * 16;
        for (uint32_t i = 0; i < pd.colorCount; i++)
        {
            ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            if (!colors[i]) continue;
            VkTransition(cmd, colors[i]->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            ca[i].imageView = colors[i]->img.view;
            ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            w = std::min(w, colors[i]->img.width);
            h = std::min(h, colors[i]->img.height);
        }
        VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        if (depth)
        {
            VkTransition(cmd, depth->img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            da.imageView = depth->img.view;
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            w = std::min(w, depth->img.width);
            h = std::min(h, depth->img.height);
        }
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        if (w == UINT32_MAX) { w = std::max(pitch, 1u); h = uint32_t(R(kRtHeight * g_scale)); }   // no attachments (memexport)
        ri.renderArea = { { 0, 0 }, { w, h } };
        ri.layerCount = 1;
        ri.colorAttachmentCount = pd.colorCount;
        ri.pColorAttachments = ca;
        ri.pDepthAttachment = depth ? &da : nullptr;
        ri.pStencilAttachment = depth ? &da : nullptr;
        vkCmdBeginRendering(cmd, &ri);
        r.passCount++;
        r.inPass = true;
        memcpy(r.passColor, wantColor, sizeof(wantColor));
        r.passDepth = wantDepth;
        r.passWidth = w;
        r.passHeight = h;
        r.passMsaa = msaa;
    }

    // Uniforms: the big constant blocks only change when the CP writes constant registers;
    // the per-draw values go in push constants.
    static StageBlock vb, pb;
    static uint64_t blocksGen = ~0ull, blocksFrame = ~0ull;
    static uint32_t vOffCached = UINT32_MAX, pOffCached = UINT32_MAX;
    DrawConsts vdc{}, pdc{};
    auto vte = Reg<reg::PA_CL_VTE_CNTL>();
    float W = float(r.passWidth), H = float(r.passHeight);
    float xs = vte.vport_x_scale_ena ? GpuRegF(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
    float xo = vte.vport_x_offset_ena ? GpuRegF(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
    float ys = vte.vport_y_scale_ena ? GpuRegF(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
    float yo = vte.vport_y_offset_ena ? GpuRegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
    float zs = vte.vport_z_scale_ena ? GpuRegF(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
    float zo = vte.vport_z_offset_ena ? GpuRegF(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
    auto scMode = Reg<reg::PA_SU_SC_MODE_CNTL>();
    float wx = 0, wy = 0;
    if (scMode.vtx_window_offset_enable)
    {
        auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
        wx = float(wo.window_x_offset);
        wy = float(wo.window_y_offset);
    }
    float ndcS[4] = { xs * 2.0f * ssx / W, ys * 2.0f * ssy / H, zs, 0 };
    float ndcO[4] = { (xo + wx) * 2.0f * ssx / W - 1.0f, (yo + wy) * 2.0f * ssy / H - 1.0f, zo, 0 };
    memcpy(vdc.ndcScale, ndcS, 16);
    memcpy(vdc.ndcOffset, ndcO, 16);
    memcpy(pdc.ndcScale, ndcS, 16);
    memcpy(pdc.ndcOffset, ndcO, 16);
    // Indexed draws: the indices are copied (byte-swapped) into a host index buffer and drawn
    // with vkCmdDrawIndexed, so the GPU's post-transform cache reuses shaded vertices (BO3
    // meshes reuse each vertex ~2-3 times). The vertex shader's own index fetch from guest
    // memory remains for geometry-shader primitives and when the ring is full.
    int64_t hostFirstIndex = -1;
    const bool idx32 = di.index_size == xenos::IndexFormat::kInt32;
    // Default on Android only: there the GPU is the limit; desktops are bound by this (CP)
    // thread, which the copy slows down (185 -> 165 fps measured). BO3_HOST_INDEX=1 / BO3_GUEST_INDEX=1.
#ifdef __ANDROID__
    static const bool hostIndex = !getenv("BO3_GUEST_INDEX");
#else
    static const bool hostIndex = getenv("BO3_HOST_INDEX") != nullptr;
#endif
    if (indexed && gs == 0 && hostIndex)
    {
        Frame& fr = Cur();
        const uint32_t isz = idx32 ? 4u : 2u, e = indexSize >> 30;
        uint32_t off0 = (fr.indexOffset + 3u) & ~3u;
        if (fr.indexRing.mapped && off0 + count * isz <= kIndexRingSize)
        {
            const uint32_t phys = indexBase & 0x1FFFFFFF;
            const uint8_t* src = g_base + 0xA0000000ull;
            auto swapE = [](uint32_t v, uint32_t en) {
                if (en == 1) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
                if (en == 2) return __builtin_bswap32(v);
                if (en == 3) return (v << 16) | (v >> 16);
                return v;
            };
            auto word = [&](uint32_t byteAddr) { uint32_t w; memcpy(&w, src + ((byteAddr & ~3u) & 0x1FFFFFFFu), 4); return swapE(w, e); };
            uint8_t* dst = (uint8_t*)fr.indexRing.mapped + off0;
            if (idx32)
            {
                uint32_t* d = (uint32_t*)dst;
                if (e == 2 && !(phys & 3))
                {
                    const uint32_t* sp = (const uint32_t*)(src + phys);
                    for (uint32_t i = 0; i < count; i++) d[i] = __builtin_bswap32(sp[i]);
                }
                else
                    for (uint32_t i = 0; i < count; i++) d[i] = word(phys + i * 4);
            }
            else
            {
                uint16_t* d = (uint16_t*)dst;
                if (e == 1 && !(phys & 1))
                {
                    const uint16_t* sp = (const uint16_t*)(src + phys);   // 8-in-16: plain 16-bit byte swap
                    for (uint32_t i = 0; i < count; i++) d[i] = __builtin_bswap16(sp[i]);
                }
                else
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint32_t a = phys + i * 2, w = word(a);
                        d[i] = uint16_t((a & 2u) ? (w >> 16) : (w & 0xFFFFu));
                    }
            }
            fr.indexOffset = off0 + count * isz;
            hostFirstIndex = off0 / isz;
        }
    }
    vdc.drawInfo[0] = 0;   // (index base: vkCmdDraw firstInstance)
    vdc.drawInfo[1] = hostFirstIndex >= 0 ? 0u : (indexed ? 1u : 0u) | (idx32 ? 2u : 0u);
    vdc.drawInfo[2] = indexSize >> 30;
    vdc.drawInfo[3] = hostFirstIndex >= 0 ? 0u : GpuReg(XE_GPU_REG_VGT_INDX_OFFSET);
    auto prog = Reg<reg::SQ_PROGRAM_CNTL>();
    auto misc = Reg<reg::SQ_CONTEXT_MISC>();
    auto cc = Reg<reg::RB_COLORCONTROL>();
    pdc.psInfo[0] = prog.param_gen ? misc.param_gen_pos : 0xFF;
    pdc.psInfo[1] = cc.alpha_test_enable ? (8u | uint32_t(cc.alpha_func)) : 0u;
    pdc.psInfo[2] = GpuReg(XE_GPU_REG_RB_ALPHA_REF);
    pdc.psInfo[3] = msx | (msy << 4) | (uint32_t(R(surfScale * 6)) << 8);   // (scale x 6 in bits 8..15)   // MSAA sample grid + resolution scale (param_gen position is in guest pixels)
    memcpy(vdc.psInfo, pdc.psInfo, 16);
    memcpy(pdc.drawInfo, vdc.drawInfo, 16);
    // Constant blocks: refresh only the 16-register chunks the CP changed (all of them when a
    // new command buffer starts - the uniform ring is per frame). A stage's block is uploaded
    // again only when a constant group its shader reads (or a fetch/bool/loop constant) changed,
    // and only those groups are written.
    static const bool noConstCache = getenv("BO3_DBG_NO_CONSTCACHE") != nullptr;
    static uint64_t vValid = 0, pValid = 0;   // float constant groups current in the cached blocks
    static uint64_t vFetch = 0, pFetch = 0;   // fetch constant uvec4s current in them
    static bool vMeta = false, pMeta = false; // bool / loop constants current in them
    {
        bool full = noConstCache || blocksFrame != g_frameSerial || vOffCached == UINT32_MAX || pOffCached == UINT32_MAX;
        const uint32_t* mmio = GpuRegsRaw();
        if (full)
        {
            FillCommon(vb, 0);
            FillCommon(pb, 256);
            vValid = pValid = 0;
            vFetch = pFetch = 0;
            vMeta = pMeta = false;
        }
        else if (g_gpuConstDirty[0] | g_gpuConstDirty[1] | g_gpuConstDirty[2])
        {
            for (uint32_t w = 0; w < 3; w++)
                for (uint64_t bits = g_gpuConstDirty[w]; bits; bits &= bits - 1)
                {
                    uint32_t chunk = w * 64 + uint32_t(__builtin_ctzll(bits));
                    uint32_t reg = 0x4000 + chunk * 16;
                    auto copy16 = [&](uint32_t* dst, uint32_t n) { for (uint32_t i = 0; i < n; i++) dst[i] = __builtin_bswap32(mmio[reg + i]); };
                    if (reg < 0x4400) { copy16(reinterpret_cast<uint32_t*>(vb.c) + (reg - 0x4000), 16); vValid &= ~(1ull << ((reg - 0x4000) >> 4)); }
                    else if (reg < 0x4800) { copy16(reinterpret_cast<uint32_t*>(pb.c) + (reg - 0x4400), 16); pValid &= ~(1ull << ((reg - 0x4400) >> 4)); }
                    else if (reg < 0x48C0)
                    {
                        copy16(vb.fetch + (reg - 0x4800), 16);
                        memcpy(pb.fetch + (reg - 0x4800), vb.fetch + (reg - 0x4800), 64);
                        uint64_t stale = 0xFull << ((reg - 0x4800) >> 2);   // 16 registers = 4 uvec4s
                        vFetch &= ~stale; pFetch &= ~stale;
                    }
                    else if (reg >= 0x4900 && reg < 0x4930)
                    {
                        // bool (0x4900..0x4907) + loop (0x4908..0x4927) constants
                        for (uint32_t i = 0; i < 16; i++)
                        {
                            uint32_t rr = reg + i;
                            uint32_t v = __builtin_bswap32(mmio[rr]);
                            if (rr < 0x4908) vb.boolc[rr - 0x4900] = pb.boolc[rr - 0x4900] = v;
                            else if (rr < 0x4928) vb.loopc[rr - 0x4908] = pb.loopc[rr - 0x4908] = v;
                        }
                        vMeta = pMeta = false;
                    }
                }
        }
        g_gpuConstDirty[0] = g_gpuConstDirty[1] = g_gpuConstDirty[2] = 0;
        const bool oldMask = AbOld("constmask");
        uint64_t vNeed = oldMask ? ~0ull : vs->t.constChunks, pNeed = oldMask ? ~0ull : ps->t.constChunks;
        uint64_t vfNeed = oldMask ? ~0ull : vs->t.fetchVec4, pfNeed = oldMask ? ~0ull : ps->t.fetchVec4;
        if (!vMeta || (vNeed & ~vValid) || (vfNeed & ~vFetch))
        {
            vOffCached = PushStageBlock(vb, vNeed, vfNeed);
            bool ok = vOffCached != UINT32_MAX;
            vValid = ok ? vNeed : 0; vFetch = ok ? vfNeed : 0; vMeta = ok;
        }
        if (!pMeta || (pNeed & ~pValid) || (pfNeed & ~pFetch))
        {
            pOffCached = PushStageBlock(pb, pNeed, pfNeed);
            bool ok = pOffCached != UINT32_MAX;
            pValid = ok ? pNeed : 0; pFetch = ok ? pfNeed : 0; pMeta = ok;
        }
        blocksGen = g_gpuConstGen;
        blocksFrame = g_frameSerial;
    }
    uint32_t vOff = vOffCached, pOff = pOffCached;
    if (vOff == UINT32_MAX || pOff == UINT32_MAX) { r.skip[8]++; return; }

    TimingMark("draw setup (texture uploads, transitions, pass begin)");
    // Bound state / dynamic state persist within a command buffer: only record what changed.
    struct DynState
    {
        VkPipeline pipe; VkDescriptorSet set0, set1; uint32_t dyn[2];
        DrawConsts pcs[2];
        VkViewport vp; VkRect2D sc; float bc[4];
        uint32_t cull, front, topo, dtest, dwrite, dcmp, stest;
        float bias[2];
        uint32_t sop[4], smask, swmask, sref;
        uint32_t vrs;
    };
    static DynState last;
    static VkCommandBuffer lastCmd = VK_NULL_HANDLE;
    static const bool noDynCache = getenv("BO3_NO_DYNCACHE") != nullptr;   // diagnostics
    if (lastCmd != cmd || noDynCache || g_computeDispatched) { memset(&last, 0xFF, sizeof(last)); lastCmd = cmd; g_computeDispatched = false; }
    DynState cur;   // (every field is assigned below)
    cur.pipe = pipe;
    cur.vrs = 0;
    if (cur.pipe != last.pipe) r.stateChanges[0]++, vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    {
        cur.pcs[0] = vdc; cur.pcs[1] = pdc;
        if (g_frameLog) Log("frame:   push ndcScale %g %g ndcOffset %g %g psInfo %u %u %u %u", cur.pcs[0].ndcScale[0], cur.pcs[0].ndcScale[1], cur.pcs[0].ndcOffset[0], cur.pcs[0].ndcOffset[1], cur.pcs[1].psInfo[0], cur.pcs[1].psInfo[1], cur.pcs[1].psInfo[2], cur.pcs[1].psInfo[3]);
        if (!FastEq(cur.pcs, last.pcs, sizeof(cur.pcs)) || cur.pipe != last.pipe)
            r.stateChanges[1]++, vkCmdPushConstants(cmd, r.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(cur.pcs), cur.pcs);
    }
    cur.set0 = f.set0; cur.dyn[0] = vOff; cur.dyn[1] = pOff; cur.set1 = set1;
    if (cur.set0 != last.set0 || cur.dyn[0] != last.dyn[0] || cur.dyn[1] != last.dyn[1])
        r.stateChanges[2]++, vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipelineLayout, 0, 1, &f.set0, 2, cur.dyn);
    if (cur.set1 != last.set1 || cur.set0 != last.set0)
        r.stateChanges[3]++, vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipelineLayout, 1, 1, &set1, 0, nullptr);

    cur.vp = { 0, 0, W, H, 0.0f, 1.0f };
    if (memcmp(&cur.vp, &last.vp, sizeof(cur.vp))) r.stateChanges[4]++, vkCmdSetViewport(cmd, 0, 1, &cur.vp);
    auto stl = Reg<reg::PA_SC_WINDOW_SCISSOR_TL>();
    auto sbr = Reg<reg::PA_SC_WINDOW_SCISSOR_BR>();
    int sx0 = stl.tl_x, sy0 = stl.tl_y, sx1 = sbr.br_x, sy1 = sbr.br_y;
    if (!stl.window_offset_disable)
    {
        auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
        sx0 += wo.window_x_offset; sx1 += wo.window_x_offset; sy0 += wo.window_y_offset; sy1 += wo.window_y_offset;
    }
    sx0 = R(sx0 * ssx); sx1 = R(sx1 * ssx); sy0 = R(sy0 * ssy); sy1 = R(sy1 * ssy);
    sx0 = std::clamp(sx0, 0, int(W)); sx1 = std::clamp(sx1, 0, int(W));
    sy0 = std::clamp(sy0, 0, int(H)); sy1 = std::clamp(sy1, 0, int(H));
    if (sx1 <= sx0 || sy1 <= sy0) { r.skip[9]++; last = cur; last.sc = { { -1, -1 }, { 0, 0 } }; memset(&last.bc, 0xFF, sizeof(DynState) - offsetof(DynState, bc)); return; }
    cur.sc = { { sx0, sy0 }, { uint32_t(sx1 - sx0), uint32_t(sy1 - sy0) } };
    // diagnostics: 1x1 scissor on big targets - vertex work stays, pixel work goes (cost split)
    if (static const bool tiny = getenv("BO3_DBG_TINY_SCISSOR") != nullptr; tiny && W >= 256) cur.sc.extent = { 1, 1 };
    if (memcmp(&cur.sc, &last.sc, sizeof(cur.sc))) r.stateChanges[5]++, vkCmdSetScissor(cmd, 0, 1, &cur.sc);
    cur.bc[0] = GpuRegF(XE_GPU_REG_RB_BLEND_RED); cur.bc[1] = GpuRegF(XE_GPU_REG_RB_BLEND_GREEN);
    cur.bc[2] = GpuRegF(XE_GPU_REG_RB_BLEND_BLUE); cur.bc[3] = GpuRegF(XE_GPU_REG_RB_BLEND_ALPHA);
    if (memcmp(cur.bc, last.bc, sizeof(cur.bc))) r.stateChanges[6]++, vkCmdSetBlendConstants(cmd, cur.bc);
    VkCullModeFlags cull = (scMode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (scMode.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    if (gs) cull = VK_CULL_MODE_NONE;
    cur.cull = cull;
    if (cur.cull != last.cull) r.stateChanges[7]++, vkCmdSetCullMode(cmd, cull);
    cur.front = scMode.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (cur.front != last.front) r.stateChanges[8]++, vkCmdSetFrontFace(cmd, VkFrontFace(cur.front));
    cur.topo = topo;
    if (cur.topo != last.topo) r.stateChanges[9]++, vkCmdSetPrimitiveTopology(cmd, topo);
    cur.dtest = depth && depthCtl.z_enable;
    if (cur.dtest != last.dtest) r.stateChanges[10]++, vkCmdSetDepthTestEnable(cmd, cur.dtest);
    cur.dwrite = depth && depthCtl.z_enable && depthCtl.z_write_enable;
    if (cur.dwrite != last.dwrite) r.stateChanges[11]++, vkCmdSetDepthWriteEnable(cmd, cur.dwrite);
    cur.dcmp = uint32_t(depthCtl.zfunc);
    if (cur.dcmp != last.dcmp) r.stateChanges[12]++, vkCmdSetDepthCompareOp(cmd, VkCompareOp(cur.dcmp));
    cur.stest = depth && depthCtl.stencil_enable;
    if (cur.stest != last.stest) r.stateChanges[13]++, vkCmdSetStencilTestEnable(cmd, cur.stest);
    {
        // Polygon offset (shadow maps): Xenos constant offset is a depth fraction (host D24:
        // x 2^24), the slope scale is in 1/16 subpixel units (Xenia's conversion).
        float scale = 0, offset = 0;
        if (!gs && scMode.poly_offset_front_enable) { scale = GpuRegF(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE); offset = GpuRegF(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET); }
        else if (!gs && scMode.poly_offset_back_enable) { scale = GpuRegF(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE); offset = GpuRegF(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET); }
        cur.bias[0] = offset * 16777216.0f; cur.bias[1] = scale * (1.0f / 16.0f);
        if (memcmp(cur.bias, last.bias, sizeof(cur.bias))) r.stateChanges[14]++, vkCmdSetDepthBias(cmd, cur.bias[0], 0.0f, cur.bias[1]);
        if (g_frameLog && (scale != 0 || offset != 0)) Log("frame:   poly offset scale %g offset %g", scale, offset);
    }
    auto srm = Reg<reg::RB_STENCILREFMASK>();
    cur.sop[0] = uint32_t(depthCtl.stencilfail); cur.sop[1] = uint32_t(depthCtl.stencilzpass);
    cur.sop[2] = uint32_t(depthCtl.stencilzfail); cur.sop[3] = uint32_t(depthCtl.stencilfunc);
    if (memcmp(cur.sop, last.sop, sizeof(cur.sop)))
        r.stateChanges[15]++, vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VkStencilOp(cur.sop[0]), VkStencilOp(cur.sop[1]), VkStencilOp(cur.sop[2]), VkCompareOp(cur.sop[3]));
    cur.smask = srm.stencilmask; cur.swmask = srm.stencilwritemask; cur.sref = srm.stencilref;
    if (cur.smask != last.smask) r.stateChanges[16]++, vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, cur.smask);
    if (cur.swmask != last.swmask) r.stateChanges[17]++, vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, cur.swmask);
    if (cur.sref != last.sref) r.stateChanges[18]++, vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, cur.sref);
    if (g_vk.hasVrs)
    {
        // MSAA surfaces live in sample space (one host pixel per sample): shade once per guest
        // (scaled) pixel like real MSAA - depth/stencil/coverage stay per sample.
        cur.vrs = msx | (msy << 8);
        if (cur.vrs != last.vrs)
        {
            VkExtent2D ext{ msx, msy };
            VkFragmentShadingRateCombinerOpKHR ops[2] = { VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR };
            r.stateChanges[19]++, g_vk.cmdSetFragmentShadingRate(cmd, &ext, ops);
        }
    }
    last = cur;
    if (hostFirstIndex >= 0)
    {
        VkIndexType it = idx32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        if (g_idxBound != int(it))
        {
            vkCmdBindIndexBuffer(cmd, Cur().indexRing.buffer, 0, it);
            g_idxBound = int(it);
        }
        vkCmdDrawIndexed(cmd, count, 1, uint32_t(hostFirstIndex), int32_t(GpuReg(XE_GPU_REG_VGT_INDX_OFFSET)), 0);
    }
    else
        vkCmdDraw(cmd, count, 1, 0, indexed ? (indexBase & 0x1FFFFFFF) : 0);
    if (vs->t.memexports || ps->t.memexports)
    {
        // The texture cache's write tracking only sees CPU stores: report the exported range
        // (stream constant: base address >> 2, element count; up to 16 bytes per element).
        auto streams = [](const TranslatedShader& t, uint32_t constBase) {
            uint32_t found = 0;
            for (uint32_t k = 0; k < t.memexportConstCount; k++)
            {
                uint32_t reg = 0x4000 + (constBase + t.memexportConsts[k]) * 4;
                uint32_t d0 = GpuReg(reg), d1 = GpuReg(reg + 1), d3 = GpuReg(reg + 3);
                if ((d0 >> 30) != 1 || d1 != 0x4B000000u) continue;
                GpuWroteGuest((d0 & 0x3FFFFFFFu) << 2, (d3 & 0x7FFFFFu) * 16);
                found++;
            }
            return found;
        };
        uint32_t found = streams(vs->t, 0) + streams(ps->t, 256);
        if (!found)
        {
            static int n = 0;
            if (n++ < 10)
                LogWarn("renderer: memexport draw without a known stream constant (vs %016llx, %u candidates, ps %016llx, %u)",
                        (unsigned long long)r.curVS, vs->t.memexportConstCount, (unsigned long long)r.curPS, ps->t.memexportConstCount);
        }
    }
    if (vs->t.memexports)
    {
        // later draws/fetches may read what this one exported
        EndPass();
        MemoryBarrierAll(cmd);
        r.memexportDraws++;
        r.guestWritesRecorded = true;
        static const bool mxLog = getenv("BO3_MEMEXPORT_LOG") != nullptr;
        if (mxLog && r.memexportDraws < 400)
        {
            uint32_t c0[4], c255[4];
            for (int k = 0; k < 4; k++) { c0[k] = GpuReg(0x4000 + k); c255[k] = GpuReg(0x4000 + 255 * 4 + k); }
            Log("memexport draw vs %016llx n %u prim %u: c0 %08X %08X %08X %08X (base 0x%08X idx %u fmt %u endian %u count %u) c255 %g %g %g %g",
                (unsigned long long)r.curVS, count, uint32_t(di.prim_type), c0[0], c0[1], c0[2], c0[3], (c0[0] & 0x3FFFFFFF) << 2,
                c0[1] & 0x7FFFFF, (c0[2] >> 8) & 63, c0[2] & 7, c0[3] & 0x7FFFFF, GpuRegF(0x4000 + 1020), GpuRegF(0x4000 + 1021),
                GpuRegF(0x4000 + 1022), GpuRegF(0x4000 + 1023));
        }
    }
    static uint32_t frameDraw = 0;
    static uint64_t frameDrawFrame = ~0ull;
    if (frameDrawFrame != r.swapCount) { frameDrawFrame = r.swapCount; frameDraw = 0; }
    frameDraw++;
    if (g_frameLog)
    {
        // BO3_FRAME_DUMP_DRAWS=n,m,...: dump the bound colour (or depth) target after those draws
        static const std::vector<uint32_t> dumpDraws = [] {
            std::vector<uint32_t> v;
            const char* e = getenv("BO3_FRAME_DUMP_DRAWS");
            for (const char* p = e ? e : ""; *p;)
            {
                char* end;
                v.push_back(uint32_t(strtoul(p, &end, 10)));
                p = *end ? end + 1 : end;
            }
            return v;
        }();
        // BO3_FRAME_DUMP_BASE=n: also dump after every draw into the colour RT at EDRAM base n
        static const int dumpBase = getenv("BO3_FRAME_DUMP_BASE") ? atoi(getenv("BO3_FRAME_DUMP_BASE")) : -1;
        if (std::find(dumpDraws.begin(), dumpDraws.end(), frameDraw) != dumpDraws.end() ||
            (dumpBase >= 0 && colors[0] && int(colors[0]->base) == dumpBase))
        {
            EndPass();
            char tag[64];
            snprintf(tag, sizeof(tag), "draw%05u", frameDraw);
            HostRT* t = colors[0] ? colors[0] : depth;
            if (t) DumpImage(cmd, t->img, t->img.width, 1440, tag);
        }
        Log("frame:   #%u colorcontrol 0x%08X a2c %u alphatest %u", frameDraw, cc.value, uint32_t(cc.alpha_to_mask_enable), uint32_t(cc.alpha_test_enable));
        auto blend0 = GpuReg(kBlendRegs[0]);
        Log("frame:   msaa %u depthctl 0x%08X zfunc %u vte 0x%08X clip 0x%08X sc_mode 0x%08X vp scale %g %g %g off %g %g %g", uint32_t(surface.msaa_samples), depthCtl.value, uint32_t(depthCtl.zfunc), vte.value, GpuReg(XE_GPU_REG_PA_CL_CLIP_CNTL),
            scMode.value, xs, ys, zs, xo, yo, zo);
        Log("frame: draw vs %016llx ps %016llx prim %u n %u | color %s | depth %s ztest %u zwrite %u | blend0 0x%08X mask 0x%04X | pass %ux%u",
            (unsigned long long)r.curVS, (unsigned long long)r.curPS, uint32_t(di.prim_type), count,
            [&] { static char b[160]; b[0] = 0; for (int i = 0; i < 4; i++) if (colors[i]) { char t[40]; snprintf(t, 40, "[%d] base %u fmt %u p %u ", i, colors[i]->base, colors[i]->format, colors[i]->pitch); strcat(b, t); } return b; }(),
            depth ? (std::to_string(depth->base) + " fmt " + std::to_string(depth->format)).c_str() : "-", uint32_t(depthCtl.z_enable),
            uint32_t(depthCtl.z_write_enable), blend0, colorMask, r.passWidth, r.passHeight);
    }
    if (f.queries)
    {
        char label[96];
        snprintf(label, sizeof(label), "draw vs %016llx ps %016llx %ux%u", (unsigned long long)r.curVS, (unsigned long long)r.curPS,
                 r.passWidth, r.passHeight);
        TimingMark(label);
    }
    r.drawCount++;
    r.vertCount += count;
    // Hand the GPU work in chunks while recording, so it runs in parallel with the command
    // processor instead of waiting for a whole frame to be recorded (BO3_SUBMIT_EVERY=n draws).
    static const uint32_t submitEvery = getenv("BO3_SUBMIT_EVERY") ? uint32_t(atoi(getenv("BO3_SUBMIT_EVERY"))) : 0;
    if (submitEvery && ++r.drawsSinceSubmit >= submitEvery)
    {
        SubmitRecorded();
    }
}

// ---------------------------------------------------------------- performance overlay
std::atomic<int> g_overlayMode{ 0 };   // 0 off, 1 FPS, 2 detailed
extern double g_cpIdleTotalMs;
void UpdateOverlay();

void UpdateOverlayNow()
{
    using clk = std::chrono::steady_clock;
    static clk::time_point winStart = clk::now(), lastFrame = clk::now();
    static uint32_t frames = 0;
    static double worst = 0;
    static uint64_t draws0 = 0, resolves0 = 0;
    static double idle0 = 0;
    auto now = clk::now();
    worst = std::max(worst, std::chrono::duration<double, std::milli>(now - lastFrame).count());
    lastFrame = now;
    frames++;
    double win = std::chrono::duration<double>(now - winStart).count();
    if (win < 0.5) return;
    int mode = g_overlayMode.load();
    double fps = frames / win, avgMs = win * 1000.0 / frames;
    double gpu = r.ovGpuMs / frames;
    double cpBusy = std::clamp(1.0 - (g_cpIdleTotalMs - idle0) / (win * 1000.0), 0.0, 1.0);
    std::vector<std::string> lines;
    char b[96];
    if (mode == 1)
    {
        snprintf(b, sizeof(b), "FPS %.0f", fps);
        lines.push_back(b);
    }
    else if (mode == 2)
    {
        snprintf(b, sizeof(b), "FPS %.1f / %d  %.1f MS  MAX %.1f MS", fps, GpuFrameLimit(), avgMs, worst); lines.push_back(b);
        snprintf(b, sizeof(b), "GPU %.1f MS/FRAME (%.0f%%)", gpu, std::min(100.0, gpu / avgMs * 100.0)); lines.push_back(b);
        snprintf(b, sizeof(b), "CPU COMANDOS GPU %.0f%%", cpBusy * 100.0); lines.push_back(b);
        snprintf(b, sizeof(b), "DRAWS %.0f  RESOLVES %.0f", double(r.drawCount - draws0) / frames, double(r.resolveCount - resolves0) / frames); lines.push_back(b);
        snprintf(b, sizeof(b), "RES %dx%d (x%g)  PANTALLA %ux%u", R(1280 * g_scale), R(720 * g_scale), g_scale, g_vk.swapExtent.width, g_vk.swapExtent.height); lines.push_back(b);
    }
    OverlaySetText(lines);
    winStart = now;
    frames = 0;
    worst = 0;
    r.ovGpuMs = 0;
    draws0 = r.drawCount;
    resolves0 = r.resolveCount;
    idle0 = g_cpIdleTotalMs;
}

void UpdateOverlay() { UpdateOverlayNow(); }
void RendererSetOverlay(int mode) { g_overlayMode = std::clamp(mode, 0, 2); }
int RendererOverlay() { return g_overlayMode.load(); }

void RendererSwap(uint32_t frontBufferPhys, uint32_t width, uint32_t height)
{
    if (!r.ready) return;
    BeginFrame();
    EndPass();
    Frame& f = Cur();

    // 1) Submit the frame's rendering right away: it must not wait for the swapchain.
    auto it = r.resolved.find(frontBufferPhys & 0x1FFFFFFF);
    Resolved* res = it != r.resolved.end() ? &it->second : nullptr;
    if (res) VkTransition(f.cmd, res->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uint32_t sw = 0, sh = 0;
    if (res)
    {
        sw = std::min(width ? uint32_t(R(width * res->scale)) : res->width, res->width);
        sh = std::min(height ? uint32_t(R(height * res->scale)) : res->height, res->height);
        // BO3_SCREENSHOT=path.ppm, BO3_SCREENSHOT_FRAME=n[,n...] (several frames: path_<n>.ppm)
        static const char* shotPath = getenv("BO3_SCREENSHOT");
        static const std::vector<uint64_t> shotFrames = [] {
            std::vector<uint64_t> v;
            const char* e = getenv("BO3_SCREENSHOT_FRAME");
            for (const char* p = e ? e : "600"; *p;)
            {
                char* end;
                v.push_back(strtoull(p, &end, 10));
                p = *end ? end + 1 : end;
            }
            return v;
        }();
        // BO3_SCREENSHOT_AT=<seconds>,<count>: <count> consecutive frames from that time (diagnostics)
        static const char* shotAt = getenv("BO3_SCREENSHOT_AT");
        static double atT = shotAt ? atof(shotAt) : -1;
        static int atN = shotAt && strchr(shotAt, ',') ? atoi(strchr(shotAt, ',') + 1) : 0;
        static const auto t0s = std::chrono::steady_clock::now();
        bool atNow = shotPath && atT >= 0 && atN > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0s).count() >= atT;
        if (atNow) atN--;
        bool shotNow = shotPath && (atNow || std::find(shotFrames.begin(), shotFrames.end(), r.swapCount) != shotFrames.end());
        // BO3_SHOT_FILE=<request file>: when it appears, a screenshot is saved to the path it holds (testing)
        static const char* shotReq = getenv("BO3_SHOT_FILE");
        std::string reqPath;
        if (shotReq && r.swapCount % 15 == 0)
            if (FILE* qf = fopen(shotReq, "r"))
            {
                char buf[512] = {};
                fgets(buf, sizeof(buf), qf);
                fclose(qf);
                remove(shotReq);
                buf[strcspn(buf, "\r\n")] = 0;
                if (buf[0]) { reqPath = buf; shotNow = true; }
            }
        if (shotNow && res->img.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            if (g_overlayMode.load() || PlatformTouchUi())   // normally drawn on the swapchain image only
            {
                UpdateOverlay();
                VkTransition(f.cmd, res->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                if (g_overlayMode.load()) OverlayRecord(f.cmd, res->img.image, { sw, sh });
                if (PlatformTouchUi()) OverlayButtonRecord(f.cmd, res->img.image, { sw, sh });
                VkTransition(f.cmd, res->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            }
            static VkBufferObj rb = VkCreateBuffer(uint64_t(4096) * 4096 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            VkBufferImageCopy bic{};
            bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            bic.imageExtent = { sw, sh, 1 };
            vkCmdCopyImageToBuffer(f.cmd, res->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &bic);
            r.pendingShot = true;
            r.shotW = sw; r.shotH = sh; r.shotBuf = &rb;
            r.shotPath = !reqPath.empty() ? reqPath : shotPath;
            if (reqPath.empty() && (shotFrames.size() > 1 || atT >= 0))
            {
                size_t dot = r.shotPath.rfind('.');
                std::string suffix = "_" + std::to_string(r.swapCount);
                r.shotPath.insert(dot == std::string::npos ? r.shotPath.size() : dot, suffix);
            }
        }
    }
    else if (r.swapCount % 600 == 0)
        LogWarn("renderer: front buffer 0x%08X was never resolved (%zu resolve targets)", frontBufferPhys, r.resolved.size());
    BusyMark(f, true);
    VK_CHECK(vkEndCommandBuffer(f.cmd));
    f.recording = false;
    f.timingPending = f.queryCount > 0;
    {
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        SubmitDeferredBatch(&si);
    }
    if (r.guestWritesRecorded) { r.guestWritesRecorded = false; r.guestWritesInFlight = true; }

    // 2) Acquire + blit + present (only this waits for the compositor).
    uint32_t imageIndex = 0;
    auto ta = std::chrono::steady_clock::now();
    VkResult ar = vkAcquireNextImageKHR(g_vk.device, g_vk.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex);
    r.presentMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ta).count();
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.presentCmd, &bi));
    if (ar == VK_ERROR_OUT_OF_DATE_KHR)
    {
        VK_CHECK(vkEndCommandBuffer(f.presentCmd));
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.presentCmd;
        QueueSync();
        VK_CHECK(vkResetFences(g_vk.device, 1, &f.fence));
        VK_CHECK(vkQueueSubmit(g_vk.queue, 1, &si, f.fence));
        VkRecreateSwapchain();
        CreatePresentSemaphores();
        r.frameIndex = (r.frameIndex + 1) % kFrames;
        return;
    }
    VkImage img = g_vk.swapImages[imageIndex];
    VkCommandBuffer pc = f.presentCmd;
    VkTransitionRaw(pc, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(pc, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (res)
    {
        float sx = float(g_vk.swapExtent.width) / sw, sy = float(g_vk.swapExtent.height) / sh, sc = std::min(sx, sy);
        int dw = int(sw * sc), dh = int(sh * sc), dx = (int(g_vk.swapExtent.width) - dw) / 2, dy = (int(g_vk.swapExtent.height) - dh) / 2;
        VkImageBlit b{};
        b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        b.srcOffsets[1] = { int(sw), int(sh), 1 };
        b.dstSubresource = b.srcSubresource;
        b.dstOffsets[0] = { dx, dy, 0 };
        b.dstOffsets[1] = { dx + dw, dy + dh, 1 };
        vkCmdBlitImage(pc, res->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    UpdateOverlay();
    if (g_overlayMode.load()) OverlayRecord(pc, img, g_vk.swapExtent);
    if (PlatformTouchUi()) OverlayButtonRecord(pc, img, g_vk.swapExtent);
    VkTransitionRaw(pc, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VK_CHECK(vkEndCommandBuffer(pc));
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &pc;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &r.presentReady[imageIndex];
    QueueSync();   // (the frame's last submit may still be on the submit thread)
    VK_CHECK(vkResetFences(g_vk.device, 1, &f.fence));
    VK_CHECK(vkQueueSubmit(g_vk.queue, 1, &si, f.fence));
    if (res) res->img.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    WriteDumps();
    if (r.pendingShot)
    {
        r.pendingShot = false;
        vkQueueWaitIdle(g_vk.queue);
        if (FILE* out = fopen(r.shotPath.c_str(), "wb"))
        {
            fprintf(out, "P6\n%u %u\n255\n", r.shotW, r.shotH);
            const uint8_t* px = (const uint8_t*)r.shotBuf->mapped;
            for (uint32_t i = 0; i < r.shotW * r.shotH; i++) fwrite(px + i * 4, 1, 3, out);
            fclose(out);
            Log("renderer: screenshot written to %s", r.shotPath.c_str());
        }
    }

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &r.presentReady[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_vk.swapchain;
    pi.pImageIndices = &imageIndex;
    auto tp = std::chrono::steady_clock::now();
    VkResult pr = vkQueuePresentKHR(g_vk.queue, &pi);
    r.presentMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count();
#ifdef __ANDROID__
    // identity preTransform in landscape is always "suboptimal" (the compositor rotates): keep it
    if (pr == VK_SUBOPTIMAL_KHR) pr = VK_SUCCESS;
#endif
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || g_vk.swapchainDirty)
    {
        VkRecreateSwapchain();
        CreatePresentSemaphores();
    }
    r.frameIndex = (r.frameIndex + 1) % kFrames;
    r.swapCount++;
    if (static const char* sw = getenv("BO3_SCALE_SWITCH"); sw)   // diagnostics: "<frame>:<scale>"
        if (r.swapCount == strtoull(sw, nullptr, 10) && strchr(sw, ':')) g_pendingScale = uint32_t(R(float(atof(strchr(sw, ':') + 1)) * 6));
    if (uint32_t ps6 = g_pendingScale.exchange(0); ps6 && std::fabs(float(ps6) / 6.0f - g_scale) > 0.01f)
    {
        // New internal resolution: drop every EDRAM render target and resolve image (their
        // contents are rebuilt by the next frames).
        QueueSync();
        vkDeviceWaitIdle(g_vk.device);
        TextureCacheClearResolves();
        for (auto& [k, rt] : r.rts) { TextureCacheForgetImage(rt.img.image); VkDestroyImage(rt.img); }
        for (auto& [k, res] : r.resolved) { TextureCacheForgetImage(res.img.image); VkDestroyImage(res.img); }
        for (auto& [k, res] : r.resolvedDepth) { TextureCacheForgetImage(res.img.image); VkDestroyImage(res.img); }
        r.rts.clear();
        for (auto& m : r.rtMemo) m = {};
        r.resolved.clear();
        r.resolvedDepth.clear();
        r.inPass = false;
        float ps = float(ps6) / 6.0f;
        Log("renderer: internal resolution x%g -> x%g", g_scale, ps);
        g_scale = ps;
    }
    {
        static const uint64_t logFrame = getenv("BO3_FRAME_LOG") ? strtoull(getenv("BO3_FRAME_LOG"), nullptr, 10) : 0;
        if (g_frameLog) Log("frame: ===== end of frame %llu =====", (unsigned long long)r.swapCount - 1);
        g_frameLog = logFrame && r.swapCount == logFrame;
        if (g_frameLog) Log("frame: ===== frame %llu =====", (unsigned long long)r.swapCount);
    }
    static auto last = std::chrono::steady_clock::now();
    static auto lastSwap = std::chrono::steady_clock::now();
    {
        auto now = std::chrono::steady_clock::now();
        double frameMs = std::chrono::duration<double, std::milli>(now - lastSwap).count();
        r.worstFrameMs = std::max(r.worstFrameMs, frameMs);
        // BO3_SPIKE_LOG=<ms>: every frame longer than that, with what happened in it
        static const double spikeMs = getenv("BO3_SPIKE_LOG") ? atof(getenv("BO3_SPIKE_LOG")) : 0;
        if (spikeMs > 0)
        {
            extern uint32_t g_tcNew[8];
            extern double g_cpIdleTotalMs;
            uint64_t TextureCacheUploadedSoFar();
            static uint32_t prevNew[8];
            static uint64_t prevDraws = 0, prevUp = 0, prevPipe = 0;
            static double prevShader = 0, prevIdle = 0;
            uint64_t up = TextureCacheUploadedSoFar();
            if (frameMs > spikeMs)
                Log("spike: frame %llu %.1f ms: %llu draws, %.0f KB uploads, %u images %u views %u sets %u pools, shader load %.2f ms, cp idle %.2f ms, %llu draws waiting for pipelines",
                    (unsigned long long)r.swapCount, frameMs, (unsigned long long)(r.drawCount - prevDraws), (up - prevUp) / 1024.0,
                    g_tcNew[6] + g_tcNew[2] - prevNew[6] - prevNew[2], g_tcNew[0] - prevNew[0], g_tcNew[7] - prevNew[7], g_tcNew[3] - prevNew[3],
                    r.shaderMs - prevShader, g_cpIdleTotalMs - prevIdle, (unsigned long long)(r.pipelinePendingSkips - prevPipe));
            memcpy(prevNew, g_tcNew, sizeof(prevNew));
            prevDraws = r.drawCount; prevUp = up; prevShader = r.shaderMs; prevIdle = g_cpIdleTotalMs; prevPipe = r.pipelinePendingSkips;
        }
        lastSwap = now;
    }
    if (r.swapCount % 120 == 0)
    {
        auto now = std::chrono::steady_clock::now();
        Log("renderer: worst frame %.1f ms, shader load %.1f ms (cp thread), pipeline creation %.1f ms (background), %d shaders + %d pipelines compiling, %llu + %llu draws skipped for them",
            r.worstFrameMs, r.shaderMs, g_pipelineUs.exchange(0) / 1000.0, s_jobsPending.load(), s_pipeJobsPending.load(),
            (unsigned long long)r.shadersPendingSkips, (unsigned long long)r.pipelinePendingSkips);
        r.shadersPendingSkips = r.pipelinePendingSkips = 0;
        extern double g_cpIdleMs, g_cpWaitMs, g_cpIntrMs;
        Log("renderer: CP thread per frame: idle %.2f ms, WAIT_REG_MEM %.2f ms, interrupts %.2f ms, acquire+present %.2f ms%s", g_cpIdleMs / 120, g_cpWaitMs / 120, g_cpIntrMs / 120, r.presentMs / 120,
            getenv("BO3_AB_TOGGLE") ? (g_ab ? " [B]" : " [A]") : "");
        static const uint32_t abEvery = getenv("BO3_AB_TOGGLE") ? uint32_t(atoi(getenv("BO3_AB_TOGGLE"))) : 0;
        if (abEvery && (r.swapCount / 120) % abEvery == 0) g_ab = !g_ab;
        r.presentMs = 0;
        g_cpIdleMs = g_cpWaitMs = g_cpIntrMs = 0;
        Log("renderer: %.0f vertices/frame", r.vertCount / 120.0);
        r.vertCount = 0;
        Log("renderer: GPU busy %.2f ms/frame, %.1f barriers/frame, %.1f passes/frame", r.gpuBusyMs / 120.0, (g_vkBarrierCount - r.barriers) / 120.0, r.passCount / 120.0);
        r.gpuBusyMs = 0; r.barriers = g_vkBarrierCount;
        Log("renderer: %.1f mid-frame submits/frame, %.1f fence batches/frame, %.1f interrupt waits/frame, %.0f constant blocks/frame", r.submits / 120.0, r.fenceBatches / 120.0, r.interruptWaits / 120.0, r.blockPushes / 120.0);
        r.blockPushes = 0;
        if (getenv("BO3_STATE_STATS"))
        {
            static const char* kNames[] = { "BindPipeline", "PushConstants", "BindDescriptorSets", "BindDescriptorSets", "SetViewport", "SetScissor", "SetBlendConstants", "SetCullMode", "SetFrontFace", "SetPrimitiveTopology", "SetDepthTestEnable", "SetDepthWriteEnable", "SetDepthCompareOp", "SetStencilTestEnable", "SetDepthBias", "SetStencilOp", "SetStencilCompareMask", "SetStencilWriteMask", "SetStencilReference", "FragmentShadingRate" };
            std::string st;
            for (size_t k = 0; k < sizeof(kNames) / sizeof(kNames[0]); k++) { char b[64]; snprintf(b, sizeof(b), " %s %.0f", kNames[k], r.stateChanges[k] / 120.0); st += b; }
            Log("renderer: state commands per frame:%s", st.c_str());
        }
        memset(r.stateChanges, 0, sizeof(r.stateChanges));
        extern uint32_t g_tcNew[8];
        Log("renderer: texture cache created %u views, %u samplers, %u 3D images, %u set pools, %u fit copies, %u remaps, %u images, %u sets",
            g_tcNew[0], g_tcNew[1], g_tcNew[2], g_tcNew[3], g_tcNew[4], g_tcNew[5], g_tcNew[6], g_tcNew[7]);
        memset(g_tcNew, 0, sizeof(g_tcNew));
        r.submits = r.fenceBatches = r.interruptWaits = 0;
        r.worstFrameMs = r.shaderMs = r.pipelineMs = 0;
        Log("renderer: %llu swaps, %.1f fps, %llu draws, %llu resolves, %zu shaders, %zu pipelines, %zu RTs, %zu resolve targets",
            (unsigned long long)r.swapCount, 120.0 / std::chrono::duration<double>(now - last).count(),
            (unsigned long long)r.drawCount, (unsigned long long)r.resolveCount, r.shaders.size(), r.pipelines.size(),
            r.rts.size(), r.resolved.size());
        Log("renderer: %.1f passes/frame, %.2f MB/frame texture uploads, %llu memexport draws, %llu guest-write flushes", r.passCount / 120.0, TextureCacheUploadedBytes() / 120.0 / 1048576.0, (unsigned long long)r.memexportDraws, (unsigned long long)r.guestWriteFlushes);
        r.passCount = 0;
        Log("renderer: skipped draws: mode %llu, empty %llu, prim %llu, shader %llu, pitch %llu, no-target %llu, pipeline %llu, textures %llu, uniforms %llu, scissor %llu",
            (unsigned long long)r.skip[0], (unsigned long long)r.skip[1], (unsigned long long)r.skip[2], (unsigned long long)r.skip[3],
            (unsigned long long)r.skip[4], (unsigned long long)r.skip[5], (unsigned long long)r.skip[6], (unsigned long long)r.skip[7],
            (unsigned long long)r.skip[8], (unsigned long long)r.skip[9]);
        last = now;
    }
}

// The guest learns that GPU work is done from fences the command processor writes (memory
// writes, scratch writeback, interrupts) - but our Vulkan work runs later. When that work
// writes guest memory (resolves to memory, memexport), the guest could reuse the memory
// before the GPU writes land (e.g. the fastfile loader reusing a zone's memory). So before
// any CPU-visible fence, finish such work.
void RendererFlushGuestWrites()
{
    static const bool noFlush = getenv("BO3_DBG_NO_FLUSH") != nullptr;
    if (noFlush || !r.ready || (!r.guestWritesRecorded && !r.guestWritesInFlight)) return;
    Frame& f = Cur();
    if (r.guestWritesRecorded && f.recording)
    {
        EndPass();
        VK_CHECK(vkEndCommandBuffer(f.cmd));
        f.recording = false;
        f.timingPending = false;
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        QueueSync();
        VK_CHECK(vkResetFences(g_vk.device, 1, &f.fence));
        VK_CHECK(vkQueueSubmit(g_vk.queue, 1, &si, f.fence));
        r.midFrameRestart = true;
    }
    QueueSync();
    vkQueueWaitIdle(g_vk.queue);
    r.guestWritesRecorded = r.guestWritesInFlight = false;
    r.guestWriteFlushes++;
}

// ---------------------------------------------------------------- deferred fence writes
// The guest learns that the GPU is done with memory from fence values the command processor
// writes. Our Vulkan work runs after the CP has processed the commands, so writing fences
// at processing time lets the guest reuse memory (bone matrices, dynamic vertex buffers, a
// fastfile zone, ...) that the GPU has not read - or written - yet. Fence writes are queued
// and applied by a completion thread once the GPU work submitted before them has finished.
namespace
{
struct DeferredBatch { VkFence fence; std::vector<std::function<void()>> writes; std::vector<std::pair<uint32_t, uint32_t>> words; };
std::vector<VkFence> s_freeFences;   // recycled batch fences (s_deferLock)
std::mutex s_deferLock;
std::condition_variable s_deferCv;
std::deque<DeferredBatch> s_deferQueue;
std::atomic<int> s_deferInFlight{ 0 };
std::condition_variable s_deferDoneCv;   // signalled when a batch's writes are applied
std::deque<std::pair<uint32_t, uint32_t>> s_inFlightWords;   // 32-bit writes of submitted batches, FIFO (s_deferLock)

void CompletionThread()
{
    pthread_setname_np(pthread_self(), "gpu-fences");
    for (;;)
    {
        DeferredBatch b;
        {
            std::unique_lock lock(s_deferLock);
            s_deferCv.wait(lock, [] { return !s_deferQueue.empty(); });
            b = std::move(s_deferQueue.front());
            s_deferQueue.pop_front();
        }
        if (b.fence) vkWaitForFences(g_vk.device, 1, &b.fence, VK_TRUE, UINT64_MAX);
        for (auto& w : b.writes) w();
        {
            std::lock_guard lock(s_deferLock);
            if (b.fence) { vkResetFences(g_vk.device, 1, &b.fence); s_freeFences.push_back(b.fence); }
            s_inFlightWords.erase(s_inFlightWords.begin(), s_inFlightWords.begin() + std::min(b.words.size(), s_inFlightWords.size()));
            s_deferInFlight--;
        }
        s_deferDoneCv.notify_all();
    }
}

// vkQueueSubmit costs ~70 us of kernel time: the command processor hands its command
// buffers to this thread (in order) and keeps recording. Anything else that uses the queue
// directly (present, waits) calls QueueSync() first - a queue is used by one thread at a time.
std::mutex s_subLock;
std::condition_variable s_subCv, s_subIdleCv;
std::deque<std::function<void()>> s_subQueue;
bool s_subBusy = false;

void SubmitThread()
{
    pthread_setname_np(pthread_self(), "gpu-submit");
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(s_subLock);
            s_subCv.wait(lock, [] { return !s_subQueue.empty(); });
            job = std::move(s_subQueue.front());
            s_subQueue.pop_front();
            s_subBusy = true;
        }
        job();
        {
            std::lock_guard lock(s_subLock);
            s_subBusy = false;
            if (s_subQueue.empty()) s_subIdleCv.notify_all();
        }
    }
}

void QueueOp(std::function<void()> fn)
{
    static const bool sync = getenv("BO3_SYNC_SUBMIT") != nullptr;   // diagnostics
    static const bool ab = getenv("BO3_AB_TOGGLE") != nullptr;
    if (sync || (ab && AbOld("submit"))) { QueueSync(); fn(); return; }
    {
        std::lock_guard lock(s_subLock);
        s_subQueue.push_back(std::move(fn));
    }
    s_subCv.notify_one();
}

void QueueSync()
{
    std::unique_lock lock(s_subLock);
    s_subIdleCv.wait(lock, [] { return s_subQueue.empty() && !s_subBusy; });
}

void SubmitOne(VkCommandBuffer cb, VkFence fence)
{
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(g_vk.queue, cb ? 1 : 0, cb ? &si : nullptr, fence));
}

// After submitting work: the pending writes become visible when it completes.
// Submit `si` (one command buffer, or nothing); pending deferred writes become a batch
// signalled by the same submission (one vkQueueSubmit for both).
void SubmitDeferredBatch(const VkSubmitInfo* si)
{
    VkCommandBuffer cb = si ? si->pCommandBuffers[0] : VK_NULL_HANDLE;
    if (r.deferred.empty())
    {
        if (cb) QueueOp([cb] { SubmitOne(cb, VK_NULL_HANDLE); });
        return;
    }
    VkFence fence = VK_NULL_HANDLE;
    {
        std::lock_guard lock(s_deferLock);
        if (!s_freeFences.empty()) { fence = s_freeFences.back(); s_freeFences.pop_back(); }
    }
    if (!fence)
    {
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_CHECK(vkCreateFence(g_vk.device, &fi, nullptr, &fence));
    }
    r.fenceBatches++;
    s_deferInFlight++;
    {
        std::lock_guard lock(s_deferLock);
        s_inFlightWords.insert(s_inFlightWords.end(), r.deferredWrites.begin(), r.deferredWrites.end());
    }
    // The completion thread gets the batch once its fence is really submitted (the fence
    // signals after this and all prior submissions).
    auto batch = std::make_shared<DeferredBatch>(DeferredBatch{ fence, std::move(r.deferred), std::move(r.deferredWrites) });
    r.deferred.clear();
    r.deferredWrites.clear();
    QueueOp([cb, batch] {
        SubmitOne(cb, batch->fence);
        {
            std::lock_guard lock(s_deferLock);
            s_deferQueue.push_back(std::move(*batch));
        }
        s_deferCv.notify_one();
    });
}
}


// Submit and wait until every deferred write is visible (before a CP interrupt: its handler
// reads what the CP wrote before it).
void RendererWaitPendingWrites()
{
    if (!r.ready) return;
    RendererSubmitPending();
    {
        std::unique_lock lock(s_deferLock);
        s_deferDoneCv.wait(lock, [] { return s_deferInFlight.load() == 0; });
    }
    r.interruptWaits++;
}

void RendererDeferGuestWrite32(uint32_t addr, uint32_t hostBytes)
{
    auto w = [addr, hostBytes] { memcpy(g_base + addr, &hostBytes, 4); };
    if (!r.ready || (!Cur().recording && r.deferred.empty() && s_deferInFlight == 0)) { w(); return; }
    r.deferred.push_back(w);
    r.deferredWrites.push_back({ addr, hostBytes });
}

// The latest pending (not yet visible) 32-bit write to `addr`, if any. The command processor
// uses it for WAIT_REG_MEM on its own fences: the Vulkan queue is in order, so waiting for
// the GPU there would only serialize the CPU and the GPU.
bool RendererPendingGuestWrite(uint32_t addr, uint32_t& hostBytes)
{
    for (auto it = r.deferredWrites.rbegin(); it != r.deferredWrites.rend(); ++it)
        if (it->first == addr) { hostBytes = it->second; return true; }
    std::lock_guard lock(s_deferLock);
    for (auto it = s_inFlightWords.rbegin(); it != s_inFlightWords.rend(); ++it)
        if (it->first == addr) { hostBytes = it->second; return true; }
    return false;
}

void RendererDeferGuestWrite(std::function<void()> write)
{
    if (!r.ready) { write(); return; }
    // nothing recorded and nothing in flight: the GPU is idle, write now (keeps order)
    if (!Cur().recording && r.deferred.empty() && s_deferInFlight == 0) { write(); return; }
    r.deferred.push_back(std::move(write));
}

// Submit what has been recorded so far (no wait) so pending fence writes can complete - the
// CP is about to idle or to wait on memory the guest updates after such a fence.
// Submit the recorded part of the frame (no wait) and keep recording in a new command buffer.
namespace {
void SubmitRecorded()
{
    Frame& f = Cur();
    r.drawsSinceSubmit = 0;
    if (f.recording)
    {
        // Submit the recorded part and keep recording this frame in a fresh command buffer
        // of the same pool: no wait, and the frame's uniform ring / descriptor pool / staging
        // stay valid (they are recycled when the frame slot's fence - the present - signals).
        EndPass();
        BusyMark(f, true);
        VK_CHECK(vkEndCommandBuffer(f.cmd));
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        SubmitDeferredBatch(&si);
        r.submits++;
        if (++f.cmdIndex >= f.cmds.size())
        {
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = f.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkCommandBuffer nc;
            VK_CHECK(vkAllocateCommandBuffers(g_vk.device, &ai, &nc));
            f.cmds.push_back(nc);
        }
        f.cmd = f.cmds[f.cmdIndex];
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
        g_idxBound = -1;
        BusyMark(f, false);
    }
}
}

void RendererSubmitPending()
{
    if (!r.ready || r.deferred.empty()) return;
    if (Cur().recording) SubmitRecorded();   // (attaches the batch)
    else SubmitDeferredBatch();
}

float RendererScale() { return g_scale; }
void RendererSetScale(float s) { g_pendingScale = uint32_t(R(std::clamp(s, 0.5f, 4.0f) * 6)); Log("renderer: scale request x%g (now x%g)", s, g_scale); }   // (1/6 steps: 0.5 = 360p)

bool g_ab = false;
bool AbOld(const char* name)
{
    static const char* want = getenv("BO3_AB_FEATURE");
    return g_ab && (!want || !strcmp(want, name));
}
bool g_computeDispatched = false;   // a compute dispatch may have disturbed graphics push constants
