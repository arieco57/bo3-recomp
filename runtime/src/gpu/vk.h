#pragma once
// Vulkan context: SDL3 window, device, swapchain, and the guest physical memory imported as a
// buffer (VK_EXT_external_memory_host) so shaders read vertex/index/texture data in place.
#include "volk.h"   // Vulkan functions loaded at runtime (Android: 1.3 entry points are not in libvulkan.so)

#include <cstdint>
#include <vector>

struct SDL_Window;

#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) VkFail(#x, _r, __FILE__, __LINE__); } while (0)
[[noreturn]] void VkFail(const char* what, VkResult r, const char* file, int line);

struct VkCtx
{
    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    bool swapchainDirty = false;

    // Guest physical memory (0xA0000000..0xC0000000 in our address space = physical 0..512 MiB).
    VkDeviceMemory guestMemory = VK_NULL_HANDLE;
    VkBuffer guestBuffer = VK_NULL_HANDLE;
    VkDeviceSize guestSize = 0;
    bool hasDmaBuf = false;
    bool customDriver = false;
    bool hasHostImport = false;   // VK_EXT_external_memory_host
    bool hasAnisotropy = true;
    bool hasPlacedMap = false;   // VK_EXT_map_memory_placed   // Android: Turnip loaded through libadrenotools

    bool hasVrs = false;   // VK_KHR_fragment_shading_rate (pipeline rate): MSAA-like shading of sample-space targets
    PFN_vkCmdSetFragmentShadingRateKHR cmdSetFragmentShadingRate = nullptr;
    bool hasStencilExport = false;
    float maxAnisotropy = 1.0f;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;   // persisted in shadercache/pipelines.bin
};

extern VkCtx g_vk;

// Main thread: create the window (must happen before the guest starts).
void VkCreateWindow(const char* title, int width, int height);
// Any thread (the GPU thread): create instance/device/swapchain + import guest memory.
void VkInit();                 // instance, device, swapchain (idempotent)
int VkExportGuestMemory();     // guest physical memory as an exported dma-buf fd, or -1
void VkImportGuestMemory();    // fallback: import host memory at guest 0xA0000000
// Android (KGSL): guest memory allocated cached + IO-coherent and mapped by the driver at the three
// physical views (VK_EXT_map_memory_placed + the same KGSL object mapped twice more). After
// MemoryInit(kPhysExternal). False if not supported.
bool VkMapGuestMemoryPlaced();
void VkRecreateSwapchain();
void VkSavePipelineCache();    // writes the driver pipeline cache to disk (cheap when unchanged)

uint32_t VkFindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags);

// Helpers
struct VkImageObj
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, mips = 1, layers = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};
VkImageObj VkCreateImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, uint32_t mips = 1,
                         uint32_t layers = 1, bool cube = false);
void VkDestroyImage(VkImageObj& img);
void VkTransition(VkCommandBuffer cmd, VkImageObj& img, VkImageLayout to);
extern uint64_t g_vkBarrierCount;   // image layout transitions recorded (stats)
void VkTransitionRaw(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to);

struct VkBufferObj
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};
VkBufferObj VkCreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
