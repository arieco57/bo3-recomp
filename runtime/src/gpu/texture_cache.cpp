#include "texture_cache.h"
#include "renderer.h"
#include "shader_translator.h"
#include "../log.h"
#include "../memory.h"
#include "../write_watch.h"

// objects created (stats): views, samplers, 3D images, set pools, fit copies, remaps, 2D images, sets
uint32_t g_tcNew[8];
uint64_t g_tcUploadedTotal = 0;
uint64_t TextureCacheUploadedSoFar();

#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/xenos.h"

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace xe::gpu;

bool CompileGlslStage(int stage, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& error);

namespace
{
constexpr uint32_t kStagingSize = 64 * 1024 * 1024;

struct FormatInfo
{
    VkFormat vk = VK_FORMAT_UNDEFINED;
    uint32_t blockW = 1, blockH = 1, bytesPerBlock = 4;
    int decode = 0;   // 0 = copy as is, 1 = 565 -> RGBA8, 2 = 1555 -> RGBA8, 3 = 4444 -> RGBA8
};

FormatInfo GetFormat(xenos::TextureFormat f)
{
    switch (f)
    {
    case xenos::TextureFormat::k_8: case xenos::TextureFormat::k_8_A: case xenos::TextureFormat::k_8_B: return { VK_FORMAT_R8_UNORM, 1, 1, 1 };
    case xenos::TextureFormat::k_8_8: return { VK_FORMAT_R8G8_UNORM, 1, 1, 2 };
    case xenos::TextureFormat::k_8_8_8_8: case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4 };
    case xenos::TextureFormat::k_2_10_10_10: return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 1, 4 };
    case xenos::TextureFormat::k_5_6_5: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, 1 };
    case xenos::TextureFormat::k_1_5_5_5: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, 2 };
    case xenos::TextureFormat::k_4_4_4_4: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, 3 };
    case xenos::TextureFormat::k_DXT1: case xenos::TextureFormat::k_DXT1_AS_16_16_16_16: return { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8 };
    case xenos::TextureFormat::k_DXT2_3: return { VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16 };
    case xenos::TextureFormat::k_DXT4_5: return { VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16 };
    case xenos::TextureFormat::k_DXN: return { VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 16 };
    case xenos::TextureFormat::k_DXT5A: return { VK_FORMAT_BC4_UNORM_BLOCK, 4, 4, 8 };
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16: return { VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16 };
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16: return { VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16 };
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16: return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 1, 4 };
    case xenos::TextureFormat::k_16: return { VK_FORMAT_R16_UNORM, 1, 1, 2 };
    case xenos::TextureFormat::k_16_16: return { VK_FORMAT_R16G16_UNORM, 1, 1, 4 };
    case xenos::TextureFormat::k_16_16_16_16: return { VK_FORMAT_R16G16B16A16_UNORM, 1, 1, 8 };
    case xenos::TextureFormat::k_16_FLOAT: return { VK_FORMAT_R16_SFLOAT, 1, 1, 2 };
    case xenos::TextureFormat::k_16_16_FLOAT: return { VK_FORMAT_R16G16_SFLOAT, 1, 1, 4 };
    case xenos::TextureFormat::k_16_16_16_16_FLOAT: return { VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 8 };
    case xenos::TextureFormat::k_32_FLOAT: return { VK_FORMAT_R32_SFLOAT, 1, 1, 4 };
    case xenos::TextureFormat::k_32_32_FLOAT: return { VK_FORMAT_R32G32_SFLOAT, 1, 1, 8 };
    case xenos::TextureFormat::k_32_32_32_32_FLOAT: return { VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 16 };
    default: return {};
    }
}

// Kinds of host images a fetch can need (also the descriptor binding: 0 = 2D, 1 = 3D, 2 = cube).
enum TexKind { kKind2D = 0, kKind3D = 1, kKindCube = 2 };

struct TexEntry
{
    VkImageObj img;
    TexKind kind = kKind2D;
    uint64_t hash = 0, sparseHash = 0;
    uint32_t watchStamp = 0;   // write tracking: sequence number of the last check
    VkImageView view = VK_NULL_HANDLE;   // last view handed out (swizzle `viewSwizzle`)
    uint32_t viewSwizzle = 0;
    uint32_t changeStreak = 0;   // consecutive checks that found it rewritten
    uint64_t checkedFrame = ~0ull;
    uint32_t stagger = 0;
};

struct Cache
{
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkImageObj dummy2D, dummyCube, dummy3D;
    VkBufferObj staging[2];
    uint32_t stagingOffset = 0, frame = 0;
    uint64_t frameCounter = 0;
    std::unordered_map<uint64_t, TexEntry> textures;
    std::map<uint32_t, VkImageObj*> resolves;            // phys -> resolve image (colour)
    std::map<uint32_t, VkImageObj*> depthResolves;       // phys -> resolve image (depth, 24_8 fetches)
    std::set<uint32_t> memNeeded;                        // resolve destinations read through memory
    std::set<uint32_t> swappedResolves;                  // resolves written with R/B swapped (copy_dest_swap)
    // deferred destruction: objects referenced by recorded/in-flight command buffers
    std::vector<std::pair<uint64_t, std::function<void()>>> graveyard;
    // 3D textures built on the GPU from a 2D resolve (colour-grading LUTs)
    struct Remap3D
    {
        VkImageObj img;
        VkBufferObj map;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t lastFrame = ~0ull;
        uint32_t w = 0, h = 0, d = 0;
    };
    std::map<std::tuple<uint32_t, VkImage, uint32_t, uint32_t, uint32_t>, Remap3D> remaps;
    VkDescriptorSetLayout remapSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout remapLayout = VK_NULL_HANDLE;
    VkPipeline remapPipe = VK_NULL_HANDLE;
    VkDescriptorPool remapPool = VK_NULL_HANDLE;
    struct ViewKeyHash
    {
        size_t operator()(const std::tuple<VkImage, uint32_t, int>& k) const
        {
            return std::hash<uint64_t>()(uint64_t(std::get<0>(k)) * 0x9E3779B97F4A7C15ull ^ (uint64_t(std::get<1>(k)) << 3) ^ uint64_t(std::get<2>(k)));
        }
    };
    std::unordered_map<std::tuple<VkImage, uint32_t, int>, VkImageView, ViewKeyHash> views;   // (image, swizzle, kind) -> view
    std::unordered_map<uint32_t, VkSampler> samplers;
    std::unordered_map<uint32_t, int> warnedFormats;
    bool dummiesReady = false;
    // GPU untiling of uploads (formats stored as is): guest memory -> untile buffer -> image
    VkDescriptorSetLayout untileSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout untileLayout = VK_NULL_HANDLE;
    VkPipeline untilePipe = VK_NULL_HANDLE;
    VkDescriptorPool untilePool = VK_NULL_HANDLE;
    VkBufferObj untileBuf[2];
    VkDescriptorSet untileSet[2] = {};
    uint32_t untileOffset = 0;
    bool untileFailed = false;
    uint64_t uploaded = 0;
    uint64_t poolSerial = 0;
    // Descriptor sets by content, kept across frames (allocated from persistent pools).
    struct SetEntry { uint64_t slot; VkImageView view; VkSampler sampler; };   // (no padding: compared with memcmp)
    struct CachedSet { VkDescriptorSet set; std::vector<SetEntry> entries; };
    std::unordered_map<uint64_t, CachedSet> sets;
    std::vector<VkDescriptorPool> setPools;   // last one is current
    // Per fetch-constant memo of the last resolved binding (valid for one guest frame while
    // the fetch constant and the resolve bindings do not change).
    struct SlotMemo { uint32_t dw[6]; uint32_t kindFilt; uint64_t frame, gen; bool ok; VkDescriptorImageInfo info; };
    SlotMemo memo[32] = {};
    uint64_t bindGen = 1;   // bumped when resolve bindings / views change
    std::unordered_map<uint32_t, uint64_t> resolveGen;   // phys -> bumped at every resolve there
    std::unordered_map<uint32_t, float> resolveScale;   // phys (| 0x80000000 depth) -> resolution scale of its image
    // Resolve images are sized by the copy destination (pitch-aligned, or reused from a bigger
    // resolve); a fetch of a different size samples an exact-size copy of the top-left region.
    struct Fit { VkImageObj img; uint64_t gen = ~0ull; };
    std::map<std::tuple<uint32_t, VkImage, uint32_t, uint32_t>, Fit> fits;
    VkDescriptorSet lastSet = VK_NULL_HANDLE;
    uint64_t lastKey = 0;
    // Direct-mapped front of `sets` by key (the map has ~20k entries: every find missed the
    // cache). Keys are 64-bit hashes of the entries; cleared with the map.
    struct SetFront { uint64_t key; VkDescriptorSet set; };
    SetFront setFront[4096] = {};
    size_t setsAllocated = 0;   // since the pools were last cleared
    bool poolFailed = false;
};
Cache c;

uint32_t Swap(uint32_t v, xenos::Endian e)
{
    switch (e)
    {
    case xenos::Endian::k8in16: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
    case xenos::Endian::k8in32: return __builtin_bswap32(v);
    case xenos::Endian::k16in32: return (v << 16) | (v >> 16);
    default: return v;
    }
}

VkImageObj MakeImage3D(uint32_t w = 1, uint32_t h = 1, uint32_t d = 1, VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM, VkImageUsageFlags extra = 0)
{
    VkImageObj o;
    o.width = w;
    o.height = h;
    o.format = fmt;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = fmt;
    ci.extent = { w, h, d };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra;
    VK_CHECK(vkCreateImage(g_vk.device, &ci, nullptr, &o.image));
    g_tcNew[2]++;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.device, o.image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = VkFindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g_vk.device, &ai, nullptr, &o.memory));
    VK_CHECK(vkBindImageMemory(g_vk.device, o.image, o.memory, 0));
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = o.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VK_CHECK(vkCreateImageView(g_vk.device, &vi, nullptr, &o.view));
    return o;
}

void ClearToWhite(VkCommandBuffer cmd, VkImageObj& img)
{
    VkTransition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue white{ { 1, 1, 1, 1 } };
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img.layers };
    vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    VkTransition(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VkComponentSwizzle Comp(uint32_t s)
{
    switch (s)
    {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

VkImageView View(VkImageObj& img, uint32_t swizzle, TexKind kind = kKind2D)
{
    auto key = std::make_tuple(img.image, swizzle, int(kind));
    auto it = c.views.find(key);
    if (it != c.views.end()) return it->second;
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = kind == kKind3D ? VK_IMAGE_VIEW_TYPE_3D : kind == kKindCube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = img.format;
    vi.components = { Comp(swizzle & 7), Comp((swizzle >> 3) & 7), Comp((swizzle >> 6) & 7), Comp((swizzle >> 9) & 7) };
    bool depth = (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    vi.subresourceRange = { depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, img.mips, 0,
                            kind == kKindCube ? 6u : 1u };
    VkImageView v;
    VK_CHECK(vkCreateImageView(g_vk.device, &vi, nullptr, &v));
    g_tcNew[0]++;
    c.views.emplace(key, v);
    return v;
}

VkSamplerAddressMode Address(xenos::ClampMode m)
{
    switch (uint32_t(m))
    {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

VkSampler Sampler(const xenos::xe_gpu_texture_fetch_t& f, bool point = false)
{
    uint32_t key = uint32_t(f.clamp_x) | (uint32_t(f.clamp_y) << 3) | (uint32_t(f.clamp_z) << 6) |
                   (uint32_t(f.mag_filter) << 9) | (uint32_t(f.min_filter) << 11) | (uint32_t(f.border_color) << 13) | (point ? 1u << 16 : 0u) |
                   (uint32_t(f.mip_filter) << 17) | (uint32_t(f.aniso_filter) << 19);
    auto it = c.samplers.find(key);
    if (it != c.samplers.end()) return it->second;
    static const bool forceLinear = getenv("BO3_DBG_FORCE_LINEAR") != nullptr;   // diagnostics
    xenos::xe_gpu_texture_fetch_t fl = f;
    if (forceLinear && !point) { fl.mag_filter = fl.min_filter = xenos::TextureFilter::kLinear; }
    const xenos::xe_gpu_texture_fetch_t& f2 = fl;
#define f f2
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = point || f.mag_filter == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.minFilter = point || f.min_filter == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    // mip filter: point / linear / base map only (2 = kBaseMap)
    uint32_t mipFilter = uint32_t(f.mip_filter);
    si.mipmapMode = mipFilter == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (!point && uint32_t(f.aniso_filter) > 1 && g_vk.maxAnisotropy > 1.0f)
    {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(float(1u << (uint32_t(f.aniso_filter) - 1)), g_vk.maxAnisotropy);
    }
    si.addressModeU = Address(f.clamp_x);
    si.addressModeV = Address(f.clamp_y);
    si.addressModeW = Address(f.clamp_z);
    si.maxLod = uint32_t(f.mip_filter) == 2 ? 0.0f : 16.0f;   // views limit to the uploaded levels
    si.borderColor = uint32_t(f.border_color) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                   : uint32_t(f.border_color) == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
#undef f
    VkSampler s;
    VK_CHECK(vkCreateSampler(g_vk.device, &si, nullptr, &s));
    g_tcNew[1]++;
    c.samplers.emplace(key, s);
    return s;
}

// Change detection for guest textures, once per frame: full hash for small textures, a
// sample (16 of every 256 bytes) for big ones - texture updates (streaming, video) rewrite
// whole textures, and a full hash of every bound texture every frame is too slow.
// Sparse sample for big textures, checked every frame: 1024 x 16 bytes spread over the range
// (whole-texture rewrites - video, streaming - change it).
uint64_t HashSparse(const uint8_t* p, size_t n)
{
    const uint64_t k = 1099511628211ull;
    uint64_t h0 = 0x84222325cbf29ce4ull ^ n, h1 = h0 * 31;
    size_t step = (n / 512) & ~size_t(15);
    if (step < 16) step = 16;
    for (size_t i = 0; i + 16 <= n; i += step)
    {
        uint64_t v[2];
        memcpy(v, p + i, 16);
        h0 = (h0 ^ v[0]) * k; h1 = (h1 ^ v[1]) * k;
    }
    return h0 ^ (h1 * 3);
}

uint64_t HashRange(const uint8_t* p, size_t n)
{
    // 4 independent lanes (the multiply chain is latency bound)
    const uint64_t k = 1099511628211ull;
    uint64_t h0 = 1469598103934665603ull ^ n, h1 = h0 * 31, h2 = h0 * 37, h3 = h0 * 41;
    size_t i = 0;
    if (n > 16384)
    {
        for (; i + 256 <= n; i += 256)
        {
            uint64_t v[4];
            memcpy(v, p + i, 16);
            memcpy(v + 2, p + i + 128, 16);
            h0 = (h0 ^ v[0]) * k; h1 = (h1 ^ v[1]) * k; h2 = (h2 ^ v[2]) * k; h3 = (h3 ^ v[3]) * k;
        }
        if (n >= 16) { uint64_t v[2]; memcpy(v, p + n - 16, 16); h0 = (h0 ^ v[0]) * k; h1 = (h1 ^ v[1]) * k; }
    }
    else
    {
        for (; i + 32 <= n; i += 32)
        {
            uint64_t v[4];
            memcpy(v, p + i, 32);
            h0 = (h0 ^ v[0]) * k; h1 = (h1 ^ v[1]) * k; h2 = (h2 ^ v[2]) * k; h3 = (h3 ^ v[3]) * k;
        }
        for (; i < n; i++) h0 = (h0 ^ p[i]) * k;
    }
    return h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7);
}

// Guest storage layout of the base level: slices (3D) or layers (stacked/cube).
struct Layout
{
    uint32_t bw, bh, pitchBlocks, pitchAligned, heightAligned, log2Bpb;
    uint32_t baseX = 0, baseY = 0;   // packed mip tail: position of the base level (blocks)
    uint32_t sliceStride;   // bytes between layers (2D stacked / cube)
    uint32_t totalBytes;
};

Layout GetLayout(const xenos::xe_gpu_texture_fetch_t& f, const FormatInfo& fi, uint32_t width, uint32_t height, uint32_t depth, bool is3D)
{
    Layout l;
    l.bw = (width + fi.blockW - 1) / fi.blockW;
    l.bh = (height + fi.blockH - 1) / fi.blockH;
    l.pitchBlocks = std::max<uint32_t>((f.pitch * 32) / fi.blockW, 1);
    l.pitchAligned = (l.pitchBlocks + 31) & ~31u;
    l.heightAligned = (l.bh + 31) & ~31u;
    l.log2Bpb = __builtin_ctz(fi.bytesPerBlock);
    // Packed mips: when the shorter side is <= 16 texels the base level itself lives in the
    // 32x32 mip tail tile, at (16, 0) texels (taller than wide / square) or (0, 16) (wider).
    auto log2c = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; };
    uint32_t lw = log2c(width), lh = log2c(height);
    if (f.packed_mips && !is3D && std::min(lw, lh) <= 4)
    {
        bool wide = lw > std::max(lh, 1u);
        l.baseX = (wide ? 0 : 16) / fi.blockW;
        l.baseY = (wide ? 16 : 0) / fi.blockH;
    }
    uint32_t rowsPerSlice = f.tiled ? l.heightAligned : l.bh;
    uint32_t pitchUsed = f.tiled ? l.pitchAligned : l.pitchBlocks;
    l.sliceStride = (pitchUsed * rowsPerSlice * fi.bytesPerBlock + 4095) & ~4095u;
    if (is3D && f.tiled)
        l.totalBytes = l.pitchAligned * l.heightAligned * ((depth + 3) & ~3u) * fi.bytesPerBlock;
    else
        l.totalBytes = l.sliceStride * depth;
    return l;
}

// Where one mip level of a 2D/cube texture lives in guest memory.
struct LevelSrc
{
    uint32_t base;                 // guest virtual address of the level's storage
    uint32_t width, height;        // level size in texels
    uint32_t bw, bh;               // ... in blocks
    uint32_t pitchBlocks, pitchAligned;
    uint32_t offX = 0, offY = 0;   // position inside a packed mip tail (blocks)
    uint32_t sliceStride;          // bytes between array layers / cube faces
};

uint32_t Log2Ceil(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }
uint32_t NextPow2(uint32_t v) { return 1u << Log2Ceil(std::max(v, 1u)); }

// Xenia's GetPackedMipOffset (2D): offset of `mip` inside the packed tail, in blocks.
bool PackedMipOffset(uint32_t width, uint32_t height, const FormatInfo& fi, uint32_t mip, uint32_t& x, uint32_t& y)
{
    uint32_t lw = Log2Ceil(width), lh = Log2Ceil(height), ls = std::min(lw, lh);
    x = y = 0;
    if (ls > 4 + mip) return false;
    uint32_t base = ls > 4 ? ls - 4 : 0, pm = mip - base;
    bool wide = lw > std::max(lh, 1u);
    if (pm < 3)
    {
        if (wide) y = 16 >> pm;
        else x = 16 >> pm;
    }
    else
    {
        if (wide) x = (1u << (lw - base)) >> (pm - 2);
        else y = (1u << (lh - base)) >> (pm - 2);
    }
    x /= fi.blockW;
    y /= fi.blockH;
    return true;
}

// Storage layout of mip level `level` (>= 1, or 0 for a packed tail holding the mips).
void MipStorage(const xenos::xe_gpu_texture_fetch_t& f, const FormatInfo& fi, uint32_t width, uint32_t height, uint32_t level,
                uint32_t& pitchAligned, uint32_t& stride)
{
    uint32_t rowTexels = std::max(NextPow2(width) >> level, 1u), rows = std::max(NextPow2(height) >> level, 1u);
    uint32_t pitchBlocks = (rowTexels + fi.blockW - 1) / fi.blockW;
    uint32_t align = f.tiled ? 32 : std::max(256 / fi.bytesPerBlock, 32u);
    pitchAligned = (pitchBlocks + align - 1) / align * align;
    uint32_t rowBlocks = ((rows + fi.blockH - 1) / fi.blockH + 31) & ~31u;
    stride = (pitchAligned * rowBlocks * fi.bytesPerBlock + 4095) & ~4095u;
}

// All stored levels of a 2D/cube texture (level 0 = base).
std::vector<LevelSrc> GetLevels(const xenos::xe_gpu_texture_fetch_t& f, const FormatInfo& fi, uint32_t width, uint32_t height, uint32_t layers)
{
    std::vector<LevelSrc> out;
    Layout l = GetLayout(f, fi, width, height, layers, false);
    LevelSrc b;
    b.base = 0xA0000000 | ((f.base_address << 12) & 0x1FFFFFFF);
    b.width = width;
    b.height = height;
    b.bw = l.bw;
    b.bh = l.bh;
    b.pitchBlocks = l.pitchBlocks;
    b.pitchAligned = l.pitchAligned;
    b.offX = l.baseX;
    b.offY = l.baseY;
    b.sliceStride = l.sliceStride;
    out.push_back(b);

    uint32_t maxLevel = std::min<uint32_t>(f.mip_max_level, Log2Ceil(std::max(width, height)));
    uint32_t packed = f.packed_mips ? (Log2Ceil(std::min(width, height)) > 4 ? Log2Ceil(std::min(width, height)) - 4 : 0) : 99;
    uint32_t mipBase = (f.mip_address << 12) & 0x1FFFFFFF;
    if (!mipBase && packed == 0) mipBase = (f.base_address << 12) & 0x1FFFFFFF;   // mips share the base's tail
    if (!mipBase || maxLevel == 0 || !f.tiled) return out;   // (linear mip chains: base only)

    uint32_t offset = 0;
    for (uint32_t level = 1; level <= maxLevel; level++)
    {
        // levels >= the packed level all live in one tail (stored as the packed level, or at
        // the mip address with level-0 strides when even the base is packed)
        uint32_t storage = std::min(level, packed);
        uint32_t pitchAligned, stride;
        MipStorage(f, fi, width, height, storage, pitchAligned, stride);
        LevelSrc m;
        m.base = 0xA0000000 | ((mipBase + offset) & 0x1FFFFFFF);
        m.width = std::max(width >> level, 1u);
        m.height = std::max(height >> level, 1u);
        m.bw = (m.width + fi.blockW - 1) / fi.blockW;
        m.bh = (m.height + fi.blockH - 1) / fi.blockH;
        m.pitchBlocks = pitchAligned;
        m.pitchAligned = pitchAligned;
        m.sliceStride = stride;
        if (level >= packed) PackedMipOffset(width, height, fi, level, m.offX, m.offY);
        out.push_back(m);
        if (level < packed) offset += stride * layers;   // the tail is the last stored subresource
        if (level >= packed && packed != 0 && level == packed) { }
    }
    return out;
}

// Decodes `count` layers of one level into `dst` (tight rows of blocks).
void DecodeLevel(uint8_t* dst, const LevelSrc& lv, uint32_t layers, const xenos::xe_gpu_texture_fetch_t& f, const FormatInfo& fi, bool is3D,
                 uint32_t heightAligned)
{
    uint32_t outBpb = fi.decode ? 4 : fi.bytesPerBlock;
    uint32_t log2Bpb = __builtin_ctz(fi.bytesPerBlock);
    uint32_t sliceOut = lv.bw * lv.bh * outBpb;
    for (uint32_t z = 0; z < layers; z++)
        for (uint32_t y = 0; y < lv.bh; y++)
            for (uint32_t x = 0; x < lv.bw; x++)
            {
                uint32_t srcOff;
                if (is3D)
                    srcOff = f.tiled ? uint32_t(texture_address::Tiled3D(int(x), int(y), int(z), lv.pitchAligned, heightAligned, log2Bpb))
                                     : z * lv.sliceStride + (y * lv.pitchBlocks + x) * fi.bytesPerBlock;
                else
                    srcOff = z * lv.sliceStride + (f.tiled ? uint32_t(texture_address::Tiled2D(int(x + lv.offX), int(y + lv.offY), lv.pitchAligned, log2Bpb))
                                                           : ((y + lv.offY) * lv.pitchBlocks + x + lv.offX) * fi.bytesPerBlock);
                if (srcOff >= 0x20000000u - (lv.base - 0xA0000000u)) continue;
                const uint8_t* src = g_base + lv.base + srcOff;
                uint8_t* d = dst + z * sliceOut + (y * lv.bw + x) * outBpb;
                if (!fi.decode)
                {
                    for (uint32_t k = 0; k < fi.bytesPerBlock; k += 4)
                    {
                        uint32_t v;
                        if (fi.bytesPerBlock - k >= 4) { memcpy(&v, src + k, 4); v = Swap(v, f.endianness); memcpy(d + k, &v, 4); }
                    }
                    if (fi.bytesPerBlock < 4)
                    {
                        // 8/16-bit texels: swap within the containing dword
                        uint32_t aligned = (lv.base + srcOff) & ~3u, shift = ((lv.base + srcOff) & 3) * 8, v;
                        memcpy(&v, g_base + aligned, 4);
                        v = Swap(v, f.endianness) >> shift;
                        memcpy(d, &v, fi.bytesPerBlock);
                    }
                }
                else
                {
                    uint32_t aligned = (lv.base + srcOff) & ~3u, shift = ((lv.base + srcOff) & 3) * 8, v;
                    memcpy(&v, g_base + aligned, 4);
                    uint32_t t = (Swap(v, f.endianness) >> shift) & 0xFFFF;
                    uint8_t rr, gg, bb, aa = 255;
                    if (fi.decode == 1) { rr = uint8_t((t & 31) * 255 / 31); gg = uint8_t(((t >> 5) & 63) * 255 / 63); bb = uint8_t(((t >> 11) & 31) * 255 / 31); }
                    else if (fi.decode == 2) { rr = uint8_t((t & 31) * 255 / 31); gg = uint8_t(((t >> 5) & 31) * 255 / 31); bb = uint8_t(((t >> 10) & 31) * 255 / 31); aa = (t >> 15) ? 255 : 0; }
                    else { rr = uint8_t((t & 15) * 17); gg = uint8_t(((t >> 4) & 15) * 17); bb = uint8_t(((t >> 8) & 15) * 17); aa = uint8_t(((t >> 12) & 15) * 17); }
                    d[0] = rr; d[1] = gg; d[2] = bb; d[3] = aa;
                }
            }
}

// Decodes every level (all slices/layers) into the staging buffer and records the upload.
// Untile + endian swap on the GPU, reading guest memory directly (it is GPU-visible): one
// invocation per output dword of the tightly packed level.
const char* kUntileCs = R"(#version 460
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) readonly buffer GuestMemory { uint mem[]; };
layout(set = 0, binding = 1, std430) writeonly buffer Out { uint o[]; };
layout(push_constant) uniform P {
    uint srcBase, dstDword, bw, bh;          // guest physical byte address, output dword offset, level size (blocks)
    uint layers, pitchAligned, pitchBlocks, offXY;
    uint sliceStride, bpbLog2, tiled, endian;
    uint totalDwords, is3D, heightAligned, pad2;
} pc;
uint swapE(uint v, uint e) {
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (e == 2u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (e == 3u) return (v << 16) | (v >> 16);
    return v;
}
int tiled2D(int x, int y, uint pitchAligned, uint log2bpb) {
    int outer = ((y >> 5) * int(pitchAligned >> 5) + (x >> 5)) << 6;
    int inner = (((y >> 1) & 7) << 3) | (x & 7);
    int oib = (outer | inner) << int(log2bpb);
    int bank = (y >> 4) & 1;
    int pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 15) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
int tiled3D(int x, int y, int z, uint pitchAligned, uint heightAligned, uint log2bpb) {
    int outer = (((z >> 2) * int(heightAligned >> 4) + (y >> 4)) * int(pitchAligned >> 5) + (x >> 5)) << 7;
    int inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
    int oib = (outer | inner) << int(log2bpb);
    int bank = ((y >> 3) ^ (z >> 2)) & 1;
    int pipe = ((x >> 3) & 3) ^ (bank << 1);
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 15) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
// byte address of block (x, y, z) of the level
uint blockAddr(uint x, uint y, uint z) {
    if (pc.is3D != 0u)
        return pc.srcBase + (pc.tiled != 0u ? uint(tiled3D(int(x), int(y), int(z), pc.pitchAligned, pc.heightAligned, pc.bpbLog2))
                                            : z * pc.sliceStride + ((y * pc.pitchBlocks + x) << pc.bpbLog2));
    uint ox = pc.offXY & 0xFFFFu, oy = pc.offXY >> 16;
    uint off = pc.tiled != 0u ? uint(tiled2D(int(x + ox), int(y + oy), pc.pitchAligned, pc.bpbLog2))
                              : ((y + oy) * pc.pitchBlocks + x + ox) << pc.bpbLog2;
    return pc.srcBase + z * pc.sliceStride + off;
}
uint readByte(uint addr) {
    return (swapE(mem[(addr >> 2) & 0x7FFFFFFu], pc.endian) >> ((addr & 3u) * 8u)) & 0xFFu;
}
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.totalDwords) return;
    uint bpb = 1u << pc.bpbLog2;
    uint blocksPerSlice = pc.bw * pc.bh;
    if (bpb >= 4u) {
        uint byteIdx = i * 4u, blk = byteIdx >> pc.bpbLog2, within = byteIdx & (bpb - 1u);
        uint z = blk / blocksPerSlice, r = blk % blocksPerSlice;
        uint a = blockAddr(r % pc.bw, r / pc.bw, z) + within;
        o[pc.dstDword + i] = swapE(mem[(a >> 2) & 0x7FFFFFFu], pc.endian);
    } else {
        uint v = 0u;
        for (uint b = 0u; b < 4u; b++) {
            uint byteIdx = i * 4u + b, blk = byteIdx >> pc.bpbLog2, within = byteIdx & (bpb - 1u);
            if (blk >= blocksPerSlice * pc.layers) break;
            uint z = blk / blocksPerSlice, r = blk % blocksPerSlice;
            v |= readByte(blockAddr(r % pc.bw, r / pc.bw, z) + within) << (b * 8u);
        }
        o[pc.dstDword + i] = v;
    }
}
)";
constexpr uint32_t kUntileSize = 64 * 1024 * 1024;

bool InitUntile()
{
    if (c.untilePipe) return true;
    if (c.untileFailed || !g_vk.guestBuffer) return false;
    c.untileFailed = true;
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.device, &li, nullptr, &c.untileSetLayout));
    VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &c.untileSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(g_vk.device, &pl, nullptr, &c.untileLayout));
    std::vector<uint32_t> spv;
    std::string err;
    if (!CompileGlslStage(3, kUntileCs, spv, err)) { LogError("untile shader: %s", err.c_str()); return false; }
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = spv.size() * 4;
    mi.pCode = spv.data();
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(g_vk.device, &mi, nullptr, &mod));
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr };
    ci.layout = c.untileLayout;
    VK_CHECK(vkCreateComputePipelines(g_vk.device, g_vk.pipelineCache, 1, &ci, nullptr, &c.untilePipe));
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 };
    VkDescriptorPoolCreateInfo dp{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.maxSets = 2;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(g_vk.device, &dp, nullptr, &c.untilePool));
    for (int i = 0; i < 2; i++)
    {
        c.untileBuf[i] = VkCreateBuffer(kUntileSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = c.untilePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &c.untileSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(g_vk.device, &ai, &c.untileSet[i]));
        VkDescriptorBufferInfo gi{ g_vk.guestBuffer, 0, VK_WHOLE_SIZE }, oi{ c.untileBuf[i].buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[2] = {};
        w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, c.untileSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &gi, nullptr };
        w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, c.untileSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &oi, nullptr };
        vkUpdateDescriptorSets(g_vk.device, 2, w, 0, nullptr);
    }
    c.untileFailed = false;
    Log("texture: GPU untiling enabled");
    return true;
}

bool Upload(VkCommandBuffer cmd, TexEntry& e, const xenos::xe_gpu_texture_fetch_t& f, const FormatInfo& fi,
            uint32_t width, uint32_t height, uint32_t depth, bool is3D, const std::function<void()>& endPass)
{
    std::vector<LevelSrc> levels;
    Layout l = GetLayout(f, fi, width, height, depth, is3D);
    if (is3D)
    {
        LevelSrc b;
        b.base = 0xA0000000 | ((f.base_address << 12) & 0x1FFFFFFF);
        b.width = width; b.height = height; b.bw = l.bw; b.bh = l.bh;
        b.pitchBlocks = l.pitchBlocks; b.pitchAligned = l.pitchAligned; b.sliceStride = l.sliceStride;
        levels.push_back(b);
    }
    else
        levels = GetLevels(f, fi, width, height, depth);
    levels.resize(std::min<size_t>(levels.size(), e.img.mips));

    uint32_t outBpb = fi.decode ? 4 : fi.bytesPerBlock;
    uint32_t total = 0;
    for (auto& lv : levels) total += (lv.bw * lv.bh * outBpb * depth + 15) & ~15u;

    static const bool cpuUntile = getenv("BO3_CPU_UNTILE") != nullptr;   // diagnostics
    uint32_t goff = (c.untileOffset + 15) & ~15u;
    if (!fi.decode && !cpuUntile && goff + total <= kUntileSize && InitUntile())
    {
        endPass();
        VkCommandBuffer cb = cmd;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.untilePipe);
        g_computeDispatched = true;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.untileLayout, 0, 1, &c.untileSet[c.frame], 0, nullptr);
        std::vector<VkBufferImageCopy> copies;
        uint32_t pos = 0;
        uint32_t log2Bpb = __builtin_ctz(fi.bytesPerBlock);
        for (uint32_t i = 0; i < levels.size(); i++)
        {
            auto& lv = levels[i];
            uint32_t bytes = lv.bw * lv.bh * outBpb * depth;
            uint32_t pcv[16] = { lv.base - 0xA0000000u, (goff + pos) / 4, lv.bw, lv.bh, depth, lv.pitchAligned, lv.pitchBlocks,
                                 lv.offX | (lv.offY << 16), lv.sliceStride, log2Bpb, uint32_t(f.tiled), uint32_t(f.endianness),
                                 (bytes + 3) / 4, is3D ? 1u : 0u, l.heightAligned, 0 };
            vkCmdPushConstants(cb, c.untileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, pcv);
            vkCmdDispatch(cb, (pcv[12] + 63) / 64, 1, 1);
            VkBufferImageCopy bic{};
            bic.bufferOffset = goff + pos;
            bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, is3D ? 1u : depth };
            bic.imageExtent = { lv.width, lv.height, is3D ? depth : 1u };
            copies.push_back(bic);
            pos += (bytes + 15) & ~15u;
        }
        c.untileOffset = goff + total;
        c.uploaded += total;
        VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cb, &di);
        VkTransition(cb, e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cb, c.untileBuf[c.frame].buffer, e.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
        VkTransition(cb, e.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return true;
    }

    uint32_t off = (c.stagingOffset + 15) & ~15u;
    if (off + total > kStagingSize) return false;
    uint8_t* dstBase = (uint8_t*)c.staging[c.frame].mapped + off;

    std::vector<VkBufferImageCopy> copies;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < levels.size(); i++)
    {
        auto& lv = levels[i];
        DecodeLevel(dstBase + pos, lv, depth, f, fi, is3D, l.heightAligned);
        VkBufferImageCopy bic{};
        bic.bufferOffset = off + pos;
        bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, is3D ? 1u : depth };
        bic.imageExtent = { lv.width, lv.height, is3D ? depth : 1u };
        copies.push_back(bic);
        pos += (lv.bw * lv.bh * outBpb * depth + 15) & ~15u;
    }
    static const char* texDump = getenv("BO3_TEX_DUMP");   // diagnostics: raw untiled uploads (base level)
    if (texDump && !is3D && depth == 1)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%08X_f%u_%ux%u.raw", texDump, (f.base_address << 12) & 0x1FFFFFFF, uint32_t(f.format), width, height);
        if (FILE* fp = fopen(path, "wb")) { fwrite(dstBase, 1, levels[0].bw * levels[0].bh * outBpb, fp); fclose(fp); }
    }
    c.stagingOffset = off + total;
    c.uploaded += total;
    static const bool upLog = getenv("BO3_UPLOAD_LOG") != nullptr;
    if (upLog)
        Log("upload: frame %llu phys 0x%08X fmt %u %ux%ux%u mips %zu tiled %u bytes %u", (unsigned long long)c.frameCounter,
            (f.base_address << 12) & 0x1FFFFFFF, uint32_t(f.format), width, height, depth, levels.size(), uint32_t(f.tiled), total);
    endPass();
    VkTransition(cmd, e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, c.staging[c.frame].buffer, e.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    VkTransition(cmd, e.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// A 2D resolve re-read as a 3D texture (the colour-grading LUT is rendered as 32x1024 and
// fetched as 32x32x32): on hardware this works through memory (2D tiling written, 3D tiling
// read). Build the 3D image on the GPU instead with a texel map computed once from the two
// tiling functions - no round trip through guest memory.
const char* kRemapCs = R"(#version 460
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image3D dst;
layout(set = 0, binding = 2, std430) readonly buffer Map { uint m[]; };
layout(push_constant) uniform P { uvec4 dims; } pc;
void main() {
    uint i = gl_GlobalInvocationID.x, w = pc.dims.x, h = pc.dims.y;
    if (i >= w * h * pc.dims.z) return;
    uint v = m[i];
    // scaled resolve: average the sc x sc host texels of the guest texel (its exact centre)
    int sc = int(max(pc.dims.w >> 8, 1u));
    vec4 t = vec4(0.0);
    if (v != 0xFFFFFFFFu) {
        ivec2 p = ivec2(v & 0xFFFFu, v >> 16) * sc;
        for (int j = 0; j < sc; j++) for (int i = 0; i < sc; i++) t += texelFetch(src, p + ivec2(i, j), 0);
        t /= float(sc * sc);
    }
    if ((pc.dims.w & 1u) != 0u) t = t.bgra;   // the resolve stored R and B swapped
    imageStore(dst, ivec3(i % w, (i / w) % h, i / (w * h)), t);
}
)";

bool InitRemap()
{
    if (c.remapPipe) return true;
    VkDescriptorSetLayoutBinding b[3] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 3;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.device, &li, nullptr, &c.remapSetLayout));
    VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &c.remapSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(g_vk.device, &pl, nullptr, &c.remapLayout));
    std::vector<uint32_t> spv;
    std::string err;
    if (!CompileGlslStage(3, kRemapCs, spv, err)) { LogError("remap shader: %s", err.c_str()); return false; }
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = spv.size() * 4;
    mi.pCode = spv.data();
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(g_vk.device, &mi, nullptr, &mod));
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr };
    ci.layout = c.remapLayout;
    VK_CHECK(vkCreateComputePipelines(g_vk.device, g_vk.pipelineCache, 1, &ci, nullptr, &c.remapPipe));
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 } };
    VkDescriptorPoolCreateInfo dp{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 64;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(g_vk.device, &dp, nullptr, &c.remapPool));
    return true;
}

// Returns false if this case is not handled (the caller falls back to guest memory).
bool Remap3DFromResolve(VkCommandBuffer cmd, uint32_t phys, VkImageObj& src, const xenos::xe_gpu_texture_fetch_t& f, uint32_t w,
                        uint32_t h, uint32_t d, VkDescriptorImageInfo& out, const std::function<void()>& endPass)
{
    if (f.format != xenos::TextureFormat::k_8_8_8_8 || src.format != VK_FORMAT_R8G8B8A8_UNORM || !f.tiled) return false;
    if (!InitRemap()) return false;
    auto key = std::make_tuple(phys, src.image, w, h, d);
    auto& e = c.remaps[key];
    if (!e.img.image)
    {
        // 3D image (sampled + storage)
        e.img = MakeImage3D(w, h, d, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT);
        g_tcNew[5]++;
        e.w = w; e.h = h; e.d = d;
        // texel map: 3D texel -> 2D resolve texel holding the same guest bytes
        float rsc = c.resolveScale[phys] > 0.0f ? c.resolveScale[phys] : 1.0f;
        uint32_t sw = uint32_t(std::lround(src.width / rsc)), sh = uint32_t(std::lround(src.height / rsc));   // guest size of the resolve
        uint32_t pitch2 = (sw + 31) & ~31u, pitch3 = (w + 31) & ~31u, height3 = (h + 31) & ~31u;
        std::unordered_map<uint32_t, uint32_t> byOffset;
        byOffset.reserve(size_t(sw) * sh);
        for (uint32_t y = 0; y < sh && y < 0x10000; y++)
            for (uint32_t x = 0; x < sw && x < 0x10000; x++)
                byOffset[uint32_t(texture_address::Tiled2D(int(x), int(y), pitch2, 2))] = x | (y << 16);
        std::vector<uint32_t> map(size_t(w) * h * d, 0xFFFFFFFFu);
        uint32_t found = 0;
        for (uint32_t z = 0; z < d; z++)
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    auto it = byOffset.find(uint32_t(texture_address::Tiled3D(int(x), int(y), int(z), pitch3, height3, 2)));
                    if (it != byOffset.end()) { map[(size_t(z) * h + y) * w + x] = it->second; found++; }
                }
        e.map = VkCreateBuffer(map.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        memcpy(e.map.mapped, map.data(), map.size() * 4);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = c.remapPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &c.remapSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(g_vk.device, &ai, &e.set));
        static VkSampler point = VK_NULL_HANDLE;
        if (!point)
        {
            VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            si.magFilter = si.minFilter = VK_FILTER_NEAREST;
            VK_CHECK(vkCreateSampler(g_vk.device, &si, nullptr, &point));
        }
        VkDescriptorImageInfo srcInfo{ point, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo dstInfo{ VK_NULL_HANDLE, e.img.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo mapInfo{ e.map.buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wr[3] = {};
        wr[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, e.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcInfo, nullptr, nullptr };
        wr[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, e.set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstInfo, nullptr, nullptr };
        wr[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, e.set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &mapInfo, nullptr };
        vkUpdateDescriptorSets(g_vk.device, 3, wr, 0, nullptr);
        Log("texture: 3D %ux%ux%u at 0x%08X built from a %ux%u resolve on the GPU (%u/%u texels mapped)", w, h, d, phys, src.width,
            src.height, found, w * h * d);
    }
    if (e.lastFrame != c.frameCounter)
    {
        e.lastFrame = c.frameCounter;
        endPass();
        VkTransition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkTransition(cmd, e.img, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.remapPipe);
        g_computeDispatched = true;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.remapLayout, 0, 1, &e.set, 0, nullptr);
        uint32_t dims[4] = { w, h, d, (c.swappedResolves.count(phys) ? 1u : 0u) | (uint32_t(std::max(std::lround(c.resolveScale[phys]), 1l)) << 8) };
        vkCmdPushConstants(cmd, c.remapLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, dims);
        vkCmdDispatch(cmd, (w * h * d + 63) / 64, 1, 1);
        VkTransition(cmd, e.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    out.imageView = View(e.img, f.swizzle, kKind3D);
    return true;
}

// The resolve image `src` as an image of exactly w x h (copy of its top-left region, redone
// when that resolve was written again).
VkImageObj* FitResolve(VkCommandBuffer cmd, uint32_t phys, bool depth, VkImageObj& src, uint32_t w, uint32_t h,
                       const std::function<void()>& endPass)
{
    static const bool noFit = getenv("BO3_NO_FIT") != nullptr;   // diagnostics
    if ((src.width == w && src.height == h) || noFit) return &src;
    auto& fit = c.fits[std::make_tuple(phys | (depth ? 0x80000000u : 0u), src.image, w, h)];
    uint64_t gen = c.resolveGen[phys | (depth ? 0x80000000u : 0u)];
    if (!fit.img.image)
        fit.img = VkCreateImage(w, h, src.format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT), g_tcNew[4]++;
    if (fit.gen != gen)
    {
        fit.gen = gen;
        endPass();
        VkTransition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkTransition(cmd, fit.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (w > src.width || h > src.height)
        {
            VkImageSubresourceRange range{ fit.img.aspect, 0, 1, 0, 1 };
            if (depth) { VkClearDepthStencilValue v{ 1.0f, 0 }; vkCmdClearDepthStencilImage(cmd, fit.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range); }
            else { VkClearColorValue v{}; vkCmdClearColorImage(cmd, fit.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range); }
        }
        VkImageCopy rc{};
        rc.srcSubresource = { VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1 };
        rc.dstSubresource = rc.srcSubresource;
        rc.extent = { std::min(w, src.width), std::min(h, src.height), 1 };
        vkCmdCopyImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fit.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
        VkTransition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkTransition(cmd, fit.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return &fit.img;
}

// Returns the view + sampler for texture fetch constant `t` as a host image of `kind`.
bool ResolveFetch(VkCommandBuffer cmd, uint32_t t, TexKind kind, VkDescriptorImageInfo& out, const std::function<void()>& endPass,
                  uint8_t magOverride = 3, uint8_t minOverride = 3)
{
    xenos::xe_gpu_texture_fetch_t f;
    uint32_t* dw = reinterpret_cast<uint32_t*>(&f);
    for (uint32_t i = 0; i < 6; i++) dw[i] = GpuReg(0x4800 + t * 6 + i);
    xenos::xe_gpu_texture_fetch_t fs = f;   // sampler state with the instruction's overrides
    if (magOverride != 3) fs.mag_filter = xenos::TextureFilter(magOverride);
    if (minOverride != 3) fs.min_filter = xenos::TextureFilter(minOverride);
    if (f.type != xenos::FetchConstantType::kTexture) return false;
    uint32_t phys = (f.base_address << 12) & 0x1FFFFFFF;
    bool depthFetch = f.format == xenos::TextureFormat::k_24_8 || f.format == xenos::TextureFormat::k_24_8_FLOAT;
    out.sampler = Sampler(fs, depthFetch);
    out.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Render-to-texture (2D): sample the resolved image directly (same-frame, exact values).
    if (kind == kKind2D)
    {
        auto& map = depthFetch ? c.depthResolves : c.resolves;
        auto rit = map.find(phys);
        if (rit != map.end() && rit->second->image)
        {
            if (rit->second->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) { endPass(); VkTransition(cmd, *rit->second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }
            uint32_t fw = f.size_2d.width + 1, fh = f.size_2d.height + 1;
            float sc = c.resolveScale[phys | (depthFetch ? 0x80000000u : 0u)];
            if (!(sc > 0.0f)) sc = 1.0f;   // (below 1 too: the 360p internal resolution)
            VkImageObj* img = f.dimension == xenos::DataDimension::k2DOrStacked ? FitResolve(cmd, phys, depthFetch, *rit->second, uint32_t(std::lround(fw * sc)), uint32_t(std::lround(fh * sc)), endPass) : rit->second;
            // The resolve may have stored R and B swapped: memory "R" is our image's B.
            uint32_t swz = f.swizzle;
            if (!depthFetch && c.swappedResolves.count(phys))
                for (int k = 0; k < 4; k++)
                {
                    uint32_t v = (swz >> (k * 3)) & 7;
                    if (v == 0 || v == 2) swz ^= (2u << (k * 3));   // 0 <-> 2
                }
            out.imageView = View(*img, swz);
            return true;
        }
    }
    if (depthFetch) return false;

    uint32_t width, height, depth = 1;
    bool is3D = false;
    switch (f.dimension)
    {
    case xenos::DataDimension::k1D: width = f.size_1d.width + 1; height = 1; break;
    case xenos::DataDimension::k2DOrStacked: width = f.size_2d.width + 1; height = f.size_2d.height + 1; break;
    case xenos::DataDimension::k3D: width = f.size_3d.width + 1; height = f.size_3d.height + 1; depth = f.size_3d.depth + 1; is3D = true; break;
    case xenos::DataDimension::kCube: width = f.size_2d.width + 1; height = f.size_2d.height + 1; depth = 6; break;
    default: return false;
    }
    // 3D fetch of a 2D resolve: build it on the GPU; otherwise that resolve must also be
    // written to guest memory from now on.
    if (kind == kKind3D && is3D)
    {
        auto rit = c.resolves.find(phys);
        if (rit != c.resolves.end() && rit->second->image)
        {
            if (Remap3DFromResolve(cmd, phys, *rit->second, f, width, height, depth, out, endPass)) return true;
            if (c.memNeeded.insert(phys).second) c.bindGen++, Log("texture: resolve 0x%08X is read through memory", phys);
        }
    }
    // The host image type must match the shader's sampler.
    if (kind == kKind3D && !is3D) return false;
    if (kind == kKindCube && f.dimension != xenos::DataDimension::kCube) return false;
    static const bool noCube = getenv("BO3_NO_CUBE") != nullptr;   // diagnostics
    if (kind == kKindCube && noCube) return false;
    if (kind == kKind2D) { if (is3D) return false; depth = 1; }

    FormatInfo fi = GetFormat(f.format);
    if (fi.vk == VK_FORMAT_UNDEFINED)
    {
        if (c.warnedFormats[uint32_t(f.format)]++ == 0) LogWarn("texture: format %u not supported yet", uint32_t(f.format));
        return false;
    }
    uint64_t key = (uint64_t(phys) << 32) ^ (uint64_t(dw[1] & 0xFFF) << 20) ^ uint64_t(dw[2]) ^ (uint64_t(f.tiled) << 63) ^
                   (uint64_t(f.pitch) << 50) ^ (uint64_t(kind) << 61) ^ (uint64_t(dw[5] & 0x600) << 40) ^
                   (uint64_t(dw[5]) << 13) ^ (uint64_t(dw[4]) << 7);   // mip address / levels
    auto& e = c.textures[key];
    if (!e.img.image)
    {
        e.kind = kind;
        e.stagger = uint32_t(key >> 7) * 2654435761u >> 28;
        if (kind == kKind3D) e.img = MakeImage3D(width, height, depth, fi.vk);
        else
        {
            uint32_t mips = uint32_t(GetLevels(f, fi, width, height, depth).size());
            e.img = VkCreateImage(width, height, fi.vk, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mips, depth, kind == kKindCube);
            g_tcNew[6]++;
        }
    }
    if (e.checkedFrame != c.frameCounter)
    {
        e.checkedFrame = c.frameCounter;
        Layout l = GetLayout(f, fi, width, height, depth, is3D);
        uint32_t bytes = std::min<uint32_t>(l.totalBytes, 0x20000000 - phys);
        // Small textures: dense hash every frame. Big ones: sparse sample every frame and the
        // dense hash every 16th frame (staggered) for partial updates.
        const uint8_t* src = g_base + 0xA0000000 + phys;
        uint64_t dims = width ^ (uint64_t(height) << 20) ^ (uint64_t(depth) << 40);
        bool changed;
        static const bool denseHash = getenv("BO3_DENSE_HASH") != nullptr;   // diagnostics
        static const bool watchVerify = getenv("BO3_WATCH_VERIFY") != nullptr;   // diagnostics: report missed changes
        if (WatchEnabled() && !denseHash)
        {
            // Untouched pages: no memory read at all. Written (or first seen): re-armed, then hashed.
            // Writes it cannot see (GPU paths not reported) are caught by a sparse sample every
            // 64 frames.
            bool clean = WatchCheckAndArm(phys, bytes, e.watchStamp) && e.hash != 0;
            bool audit = clean && ((c.frameCounter + e.stagger) & 63) == 0;
            if (audit && !watchVerify && (HashSparse(src, bytes) ^ dims) != e.sparseHash)
            {
                static int n = 0;
                if (n++ < 20) Log("texture: change at 0x%08X found by the sparse audit (%u bytes, fmt %u %ux%u)", phys, bytes, uint32_t(f.format), width, height);
                clean = false;
            }
            // Rewritten at every recent check (streamed / per-frame textures): upload without
            // hashing - except every 32nd time, to notice when it settles.
            if (!clean && !watchVerify && e.changeStreak >= 4 && bytes > 64 * 1024 && ((c.frameCounter + e.stagger) & 31) != 0)
            {
                changed = true;
                e.changeStreak++;
            }
            else if (!clean || watchVerify)
            {
                uint64_t h = HashRange(src, bytes) ^ dims;
                if (clean && h != e.hash && WatchClean(phys, bytes, e.watchStamp))   // (not a store that landed meanwhile)
                    LogWarn("texture: write tracking missed a change at 0x%08X (%u bytes, fmt %u %ux%u)", phys, bytes, uint32_t(f.format), width, height);
                changed = h != e.hash;
                e.hash = h;
                e.sparseHash = HashSparse(src, bytes) ^ dims;
                e.changeStreak = changed ? e.changeStreak + 1 : 0;
            }
            else changed = false;
        }
        else if (bytes <= 16 * 1024 || e.hash == 0 || denseHash)
        {
            uint64_t h = HashRange(src, bytes) ^ dims;
            changed = h != e.hash;
            e.hash = h;
            if (bytes > 16 * 1024) e.sparseHash = HashSparse(src, bytes) ^ dims;
        }
        else
        {
            uint64_t sh = HashSparse(src, bytes) ^ dims;
            changed = sh != e.sparseHash;
            e.sparseHash = sh;
            if (changed || ((c.frameCounter + e.stagger) & 15) == 0)
            {
                uint64_t h = HashRange(src, bytes) ^ dims;
                changed |= h != e.hash;
                e.hash = h;
            }
        }
        if (changed)
        {
            if (!Upload(cmd, e, f, fi, width, height, depth, is3D, endPass)) { e.hash = e.sparseHash = 0; return false; }
        }
    }
    if (!e.view || e.viewSwizzle != uint32_t(f.swizzle)) { e.view = View(e.img, f.swizzle, kind); e.viewSwizzle = f.swizzle; }
    out.imageView = e.view;
    return true;
}
}

void TextureCacheInit(VkDescriptorSetLayout set1Layout)
{
    c.layout = set1Layout;
    c.dummy2D = VkCreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    c.dummyCube = VkCreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, 6, true);
    c.dummy3D = MakeImage3D();
    for (auto& s : c.staging) s = VkCreateBuffer(kStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
}

void TextureCacheBeginFrame(uint32_t frameIndex, bool newFrame)
{
    c.frame = frameIndex;
    c.stagingOffset = 0;
    c.untileOffset = 0;
    c.poolSerial++;
    if (newFrame) c.frameCounter++;   // textures are re-validated once per guest frame
    // Objects retired 3+ frames ago are no longer referenced by any command buffer.
    for (auto it = c.graveyard.begin(); it != c.graveyard.end();)
    {
        if (c.frameCounter >= it->first + 3) { it->second(); it = c.graveyard.erase(it); }
        else ++it;
    }
}

void TextureCacheRegisterResolve(uint32_t phys, VkImageObj* img, uint32_t, uint32_t, bool swapRB, float scale)
{
    c.resolveScale[phys & 0x1FFFFFFF] = scale;
    c.resolves[phys & 0x1FFFFFFF] = img;
    c.resolveGen[phys & 0x1FFFFFFF]++;
    c.bindGen++;
    if (swapRB) c.swappedResolves.insert(phys & 0x1FFFFFFF);
    else c.swappedResolves.erase(phys & 0x1FFFFFFF);
}

// An image is about to be destroyed (resized resolve target): drop everything that refers to
// it - Vulkan may hand the same handle to the next image, and a cached view or descriptor of
// the old one would make the GPU read freed memory.
void TextureCacheForgetImage(VkImage image)
{
    for (auto it = c.views.begin(); it != c.views.end();)
    {
        if (std::get<0>(it->first) == image)
        {
            VkImageView v = it->second;
            c.graveyard.push_back({ c.frameCounter, [v] { vkDestroyImageView(g_vk.device, v, nullptr); } });
            it = c.views.erase(it);
        }
        else ++it;
    }
    for (auto it = c.remaps.begin(); it != c.remaps.end();)
    {
        if (std::get<1>(it->first) == image)
        {
            auto& e = it->second;
            // views of the 3D image itself
            std::vector<VkImageView> views;
            for (auto v = c.views.begin(); v != c.views.end();)
            {
                if (std::get<0>(v->first) == e.img.image) { views.push_back(v->second); v = c.views.erase(v); }
                else ++v;
            }
            VkImageObj img = e.img;
            VkBufferObj map = e.map;
            VkDescriptorSet set = e.set;
            c.graveyard.push_back({ c.frameCounter, [views, img, map, set]() mutable {
                for (auto v : views) vkDestroyImageView(g_vk.device, v, nullptr);
                if (set) vkFreeDescriptorSets(g_vk.device, c.remapPool, 1, &set);
                VkDestroyImage(img);
                vkDestroyBuffer(g_vk.device, map.buffer, nullptr);
                vkFreeMemory(g_vk.device, map.memory, nullptr);
            } });
            it = c.remaps.erase(it);
        }
        else ++it;
    }
    for (auto it = c.fits.begin(); it != c.fits.end();)
    {
        if (std::get<1>(it->first) == image)
        {
            VkImageObj img = it->second.img;
            for (auto v = c.views.begin(); v != c.views.end();)
            {
                if (std::get<0>(v->first) == img.image) { VkImageView vv = v->second; c.graveyard.push_back({ c.frameCounter, [vv] { vkDestroyImageView(g_vk.device, vv, nullptr); } }); v = c.views.erase(v); }
                else ++v;
            }
            c.graveyard.push_back({ c.frameCounter, [img]() mutable { VkDestroyImage(img); } });
            it = c.fits.erase(it);
        }
        else ++it;
    }
    // cached descriptor sets may reference its views: retire them all
    for (VkDescriptorPool pool : c.setPools)
        c.graveyard.push_back({ c.frameCounter, [pool] { vkDestroyDescriptorPool(g_vk.device, pool, nullptr); } });
    c.setPools.clear();
    c.sets.clear();
    memset(c.setFront, 0, sizeof(c.setFront));
    c.lastSet = VK_NULL_HANDLE;
    c.setsAllocated = 0;
    c.bindGen++;
}

void TextureCacheClearResolves()
{
    c.resolveScale.clear();
    c.resolves.clear();
    c.depthResolves.clear();
    c.swappedResolves.clear();
    c.bindGen++;
}

void TextureCacheDeferDestroy(const VkImageObj& img)
{
    VkImageObj copy = img;
    c.graveyard.push_back({ c.frameCounter, [copy]() mutable { VkDestroyImage(copy); } });
}

bool TextureCacheResolveNeedsMemory(uint32_t phys) { return c.memNeeded.count(phys & 0x1FFFFFFF) != 0; }

void TextureCacheRegisterDepthResolve(uint32_t phys, VkImageObj* img, float scale)
{
    c.resolveScale[(phys & 0x1FFFFFFF) | 0x80000000u] = scale;
    c.depthResolves[phys & 0x1FFFFFFF] = img;
    c.resolveGen[(phys & 0x1FFFFFFF) | 0x80000000u]++;
    c.bindGen++;
}

bool g_frameLog = false;

static void LogFetch(uint32_t t, uint8_t dim, const char* result)
{
    xenos::xe_gpu_texture_fetch_t f;
    uint32_t* dw = reinterpret_cast<uint32_t*>(&f);
    for (uint32_t i = 0; i < 6; i++) dw[i] = GpuReg(0x4800 + t * 6 + i);
    uint32_t w = f.size_2d.width + 1, h = f.size_2d.height + 1, d = 1;
    if (f.dimension == xenos::DataDimension::k3D) { w = f.size_3d.width + 1; h = f.size_3d.height + 1; d = f.size_3d.depth + 1; }
    Log("frame:    tex%u endian %u pitch %u packed %u mipaddr 0x%08X numfmt %u dim(shader %u fetch %u) fmt %u %ux%ux%u phys 0x%08X tiled %u signs %u%u%u%u exp %d mips %u-%u swz 0x%X -> %s", t, uint32_t(f.endianness), uint32_t(f.pitch) * 32, uint32_t(f.packed_mips), uint32_t(f.mip_address) << 12, uint32_t(f.num_format), dim,
        uint32_t(f.dimension), uint32_t(f.format), w, h, d, (f.base_address << 12) & 0x1FFFFFFF, uint32_t(f.tiled), uint32_t(f.sign_x),
        uint32_t(f.sign_y), uint32_t(f.sign_z), uint32_t(f.sign_w), int(f.exp_adjust), uint32_t(f.mip_min_level), uint32_t(f.mip_max_level),
        uint32_t(f.swizzle), result);
}

VkDescriptorSet TextureCacheBuildSet(VkCommandBuffer cmd, VkDescriptorPool pool, const TranslatedShader& vs,
                                     const TranslatedShader& ps, const std::function<void()>& endPass)
{
    if (!c.dummiesReady)
    {
        endPass();
        ClearToWhite(cmd, c.dummy2D);
        ClearToWhite(cmd, c.dummyCube);
        ClearToWhite(cmd, c.dummy3D);
        c.dummiesReady = true;
    }

    static VkSampler dummySampler = VK_NULL_HANDLE;
    if (!dummySampler)
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        VK_CHECK(vkCreateSampler(g_vk.device, &si, nullptr, &dummySampler));
    }
    // Resolve the used fetch constants (memoized per frame) into (slot, view, sampler).
    Cache::SetEntry entries[32];
    uint32_t n = 0;
    uint64_t key = 1469598103934665603ull;
    uint32_t used = vs.texturesUsed | ps.texturesUsed;
    const uint32_t* mmio = GpuRegsRaw();
    for (uint32_t t = 0; t < 32; t++)
    {
        if (!(used & (1u << t))) continue;
        const TranslatedShader& owner = (ps.texturesUsed & (1u << t)) ? ps : vs;
        uint8_t dim = owner.textureDim[t];
        TexKind kind = dim <= uint8_t(xenos::FetchOpDimension::k2D) ? kKind2D
                     : dim == uint8_t(xenos::FetchOpDimension::k3DOrStacked) ? kKind3D : kKindCube;
        uint32_t kindFilt = uint32_t(kind) | (uint32_t(owner.texMag[t]) << 8) | (uint32_t(owner.texMin[t]) << 16);
        Cache::SlotMemo& m = c.memo[t];
        const uint32_t* fdw = mmio + 0x4800 + t * 6;
        static const bool noMemo = getenv("BO3_NO_TEXMEMO") != nullptr;   // diagnostics
        bool hit = !noMemo && m.frame == c.frameCounter && m.gen == c.bindGen && m.kindFilt == kindFilt && FastEq(m.dw, fdw, 24) && !g_frameLog;
        if (!hit)
        {
            VkDescriptorImageInfo info{};
            bool ok = ResolveFetch(cmd, t, kind, info, endPass, owner.texMag[t], owner.texMin[t]);
            memcpy(m.dw, fdw, 24);
            m.kindFilt = kindFilt;
            m.frame = c.frameCounter;
            m.gen = c.bindGen;   // (ResolveFetch may bump it: then the next draw re-resolves)
            m.ok = ok;
            m.info = info;
            if (g_frameLog)
            {
                uint32_t phys = (GpuReg(0x4800 + t * 6 + 1) & ~0xFFFu) & 0x1FFFFFFF;
                LogFetch(t, dim, !ok ? "DUMMY (white)" : c.resolves.count(phys) ? "resolve target" : "texture");
            }
        }
        if (!m.ok) continue;   // dummy
        Cache::SetEntry& e = entries[n++];
        e.slot = t | (uint32_t(kind) << 5);
        e.view = m.info.imageView;
        e.sampler = m.info.sampler;
        key = (key ^ e.slot) * 1099511628211ull;
        key = (key ^ uint64_t(e.view)) * 1099511628211ull;
        key = (key ^ uint64_t(e.sampler)) * 1099511628211ull;
    }
    if (c.lastSet && key == c.lastKey) return c.lastSet;
    Cache::SetFront& front = c.setFront[(key ^ (key >> 31)) & 4095];
    if (front.set && front.key == key && !AbOld("setfront"))
    {
        c.lastSet = front.set;
        c.lastKey = key;
        return c.lastSet;
    }
    auto it = c.sets.find(key);
    if (it != c.sets.end() && it->second.entries.size() == n &&
        (n == 0 || FastEq(it->second.entries.data(), entries, n * sizeof(Cache::SetEntry))))
    {
        c.lastSet = it->second.set;
        c.lastKey = key;
        front = { key, c.lastSet };
        return c.lastSet;
    }

    // New combination: allocate a set from the persistent pools and write all 96 bindings.
    // (Counted by allocated sets, not cache entries: an entry replaced after a hash collision
    // still used a set.)
    static const size_t maxSets = getenv("BO3_SETCACHE_MAX") ? size_t(atoi(getenv("BO3_SETCACHE_MAX"))) : 24576;
    if (c.setsAllocated >= maxSets || c.poolFailed)
    {
        // full: retire every cached set (their pools are destroyed 3 frames later). The
        // last-set shortcut must go too - it pointed into those pools, and reusing it after
        // they were destroyed gave garbage descriptors (black textures after a few minutes).
        for (VkDescriptorPool p : c.setPools)
            c.graveyard.push_back({ c.frameCounter, [p] { vkDestroyDescriptorPool(g_vk.device, p, nullptr); } });
        c.setPools.clear();
        c.sets.clear();
    memset(c.setFront, 0, sizeof(c.setFront));
        c.lastSet = VK_NULL_HANDLE;
        c.setsAllocated = 0;
        static int n = 0;
        if (n++ < 20) Log("texture: descriptor set cache %s, cleared", c.poolFailed ? "out of pool memory" : "full");
        c.poolFailed = false;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 2 && !set; attempt++)
    {
        if (c.setPools.empty() || attempt == 1)
        {
            VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 96 * 1024 };
            VkDescriptorPoolCreateInfo dp{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dp.maxSets = 1024;
            dp.poolSizeCount = 1;
            dp.pPoolSizes = &ps;
            VkDescriptorPool np;
            if (VkResult pr = vkCreateDescriptorPool(g_vk.device, &dp, nullptr, &np); pr != VK_SUCCESS)
            {
                // clear the cache at the next draw (its pools are freed 3 frames later)
                static int n = 0;
                if (n++ < 10) LogWarn("texture: descriptor pool creation failed (%d) with %zu pools", int(pr), c.setPools.size());
                c.poolFailed = true;
                return VK_NULL_HANDLE;
            }
            g_tcNew[3]++;
            c.setPools.push_back(np);
        }
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = c.setPools.back();
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &c.layout;
        if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) set = VK_NULL_HANDLE;
    }
    if (!set) return VK_NULL_HANDLE;
    g_tcNew[7]++;
    c.setsAllocated++;
    (void)pool;
    VkDescriptorImageInfo infos[3][32];
    VkImageView dv[3] = { c.dummy2D.view, c.dummy3D.view, c.dummyCube.view };
    for (uint32_t b = 0; b < 3; b++)
        for (uint32_t i = 0; i < 32; i++) infos[b][i] = { dummySampler, dv[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    for (uint32_t i = 0; i < n; i++)
        infos[entries[i].slot >> 5][entries[i].slot & 31] = { entries[i].sampler, entries[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w[3] = {};
    for (uint32_t b = 0; b < 3; b++)
    {
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = set;
        w[b].dstBinding = b;
        w[b].descriptorCount = 32;
        w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[b].pImageInfo = infos[b];
    }
    vkUpdateDescriptorSets(g_vk.device, 3, w, 0, nullptr);
    c.sets[key] = { set, std::vector<Cache::SetEntry>(entries, entries + n) };
    c.lastSet = set;
    c.lastKey = key;
    c.setFront[(key ^ (key >> 31)) & 4095] = { key, set };
    return set;
}

uint64_t TextureCacheUploadedBytes()
{
    uint64_t v = c.uploaded;
    g_tcUploadedTotal += v;
    c.uploaded = 0;
    return v;
}

uint64_t TextureCacheUploadedSoFar() { return g_tcUploadedTotal + c.uploaded; }
