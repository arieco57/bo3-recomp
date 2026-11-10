#pragma once
// Texture cache: guest textures (tiled/linear, big-endian, Xenos formats) -> Vulkan images,
// plus render-to-texture through resolve targets.
#include "vk.h"

#include <functional>

struct TranslatedShader;

void TextureCacheInit(VkDescriptorSetLayout set1Layout);
void TextureCacheBeginFrame(uint32_t frameIndex, bool newFrame);   // newFrame: a new guest frame (re-validate textures)

// A resolve wrote `img` at guest physical address `phys` (textures at that address sample it).
void TextureCacheRegisterResolve(uint32_t phys, VkImageObj* img, uint32_t width, uint32_t height, bool swapRB, float scale);
// A depth resolve: fetches of 24_8 / 24_8_FLOAT textures at `phys` sample its depth.
void TextureCacheRegisterDepthResolve(uint32_t phys, VkImageObj* img, float scale);

// Builds descriptor set 1 for a draw. `endPass` is called before any command that cannot be
// recorded inside a rendering pass (uploads, layout transitions). Returns VK_NULL_HANDLE on failure.
VkDescriptorSet TextureCacheBuildSet(VkCommandBuffer cmd, VkDescriptorPool pool, const TranslatedShader& vs,
                                     const TranslatedShader& ps, const std::function<void()>& endPass);

uint64_t TextureCacheUploadedBytes();   // since the last call

extern bool g_frameLog;   // BO3_FRAME_LOG: log every draw/resolve of one frame
// Whether a resolve to `phys` must also be written to guest memory (some fetch reads it there).
bool TextureCacheResolveNeedsMemory(uint32_t phys);
// Call before destroying an image that may have been sampled (drops cached views/sets).
void TextureCacheForgetImage(VkImage image);
// Destroy an image once no recorded/in-flight command buffer can reference it.
void TextureCacheDeferDestroy(const VkImageObj& img);

// Forget every registered resolve image (the renderer is about to destroy them all).
void TextureCacheClearResolves();
