#include "vk.h"
#include "../log.h"
#include "../memory.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#ifdef __ANDROID__
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <vector>
#include <mutex>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <sys/mman.h>
#include <chrono>
#include <string>

VkCtx g_vk;

void VkFail(const char* what, VkResult r, const char* file, int line)
{
    Fatal("Vulkan call failed (%d): %s at %s:%d", int(r), what, file, line);
}

void VkCreateWindow(const char* title, int width, int height)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        Fatal("SDL_Init failed: %s", SDL_GetError());
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    // landscape only (SDL otherwise unlocks rotation for resizable windows), system bars hidden
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    g_vk.window = SDL_CreateWindow(title, width, height, flags);
    if (!g_vk.window) Fatal("SDL_CreateWindow failed: %s", SDL_GetError());
    Log("vk: window %dx%d created", width, height);
}

uint32_t VkFindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < g_vk.memProps.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (g_vk.memProps.memoryTypes[i].propertyFlags & flags) == flags) return i;
    Fatal("vk: no memory type for bits 0x%X flags 0x%X", typeBits, flags);
}

static bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    for (auto& e : exts)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

static VkBuffer CreateGuestBuffer(VkExternalMemoryHandleTypeFlagBits type)
{
    VkExternalMemoryBufferCreateInfo ext{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ext.handleTypes = type;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext };
    bi.size = 0x20000000;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    VK_CHECK(vkCreateBuffer(g_vk.device, &bi, nullptr, &buf));
    return buf;
}

// Guest physical memory (512 MiB) as GPU memory exported as a dma-buf, which the CPU side
// maps at the guest's three physical views. Physical address P is at buffer offset P.
int VkExportGuestMemory()
{
    if (!g_vk.hasDmaBuf) return -1;
    VkPhysicalDeviceExternalBufferInfo q{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
    q.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    q.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkExternalBufferProperties ep{ VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(g_vk.phys, &q, &ep);
    if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
    {
        LogWarn("vk: dma-buf export not supported for buffers");
        return -1;
    }

    VkBuffer buf = CreateGuestBuffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.device, buf, &req);
    int type = -1;
    for (VkMemoryPropertyFlags want : { VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                        VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) })
    {
        for (uint32_t i = 0; i < g_vk.memProps.memoryTypeCount && type < 0; i++)
            if ((req.memoryTypeBits & (1u << i)) && (g_vk.memProps.memoryTypes[i].propertyFlags & want) == want) type = int(i);
        if (type >= 0) break;
    }
    if (type < 0) { vkDestroyBuffer(g_vk.device, buf, nullptr); return -1; }

    VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.buffer = buf;
    VkExportMemoryAllocateInfo exp{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded };
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(type);
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(g_vk.device, &ai, nullptr, &mem);
    if (r != VK_SUCCESS)
    {
        LogWarn("vk: exportable guest memory allocation failed (%d)", int(r));
        vkDestroyBuffer(g_vk.device, buf, nullptr);
        return -1;
    }
    auto getFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_vk.device, "vkGetMemoryFdKHR");
    VkMemoryGetFdInfoKHR gi{ VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gi.memory = mem;
    gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (!getFd || getFd(g_vk.device, &gi, &fd) != VK_SUCCESS || fd < 0)
    {
        LogWarn("vk: vkGetMemoryFdKHR failed");
        vkFreeMemory(g_vk.device, mem, nullptr);
        vkDestroyBuffer(g_vk.device, buf, nullptr);
        return -1;
    }
    VK_CHECK(vkBindBufferMemory(g_vk.device, buf, mem, 0));
    g_vk.guestBuffer = buf;
    g_vk.guestMemory = mem;
    g_vk.guestSize = 0x20000000;
    VkMemoryPropertyFlags tf = g_vk.memProps.memoryTypes[type].propertyFlags;
    Log("vk: guest physical memory is GPU memory (512 MiB dma-buf, memory type %d: %s)", type,
        (tf & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "cached, coherent" : "UNCACHED");
    if (!(tf & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        LogWarn("vk: no cached + coherent GPU memory on this device: every guest memory access is uncached (very slow)");
    return fd;
}

// Fallback (Android): import the guest memory (a memfd mapped at the three physical views) at
// guest 0xA0000000 as host memory. RADV refuses shared memory here (amdgpu userptr is
// anonymous-only); desktop drivers export a dma-buf instead.
void VkSavePipelineCache()
{
    if (!g_vk.pipelineCache) return;
    static std::mutex lock;
    std::lock_guard guard(lock);
    static size_t lastSize = 0;
    size_t n = 0;
    if (vkGetPipelineCacheData(g_vk.device, g_vk.pipelineCache, &n, nullptr) != VK_SUCCESS || n == lastSize) return;
    std::vector<char> blob(n);
    if (vkGetPipelineCacheData(g_vk.device, g_vk.pipelineCache, &n, blob.data()) != VK_SUCCESS) return;
    mkdir("shadercache", 0755);
    if (FILE* f = fopen("shadercache/pipelines.bin.tmp", "wb"))
    {
        bool ok = fwrite(blob.data(), 1, n, f) == n;
        fclose(f);
        if (ok && rename("shadercache/pipelines.bin.tmp", "shadercache/pipelines.bin") == 0) lastSize = n;
    }
}

void VkImportGuestMemory()
{
    if (!g_vk.hasHostImport) Fatal("vk: the driver neither exports dma-bufs nor imports host memory - guest memory cannot be shared");
    void* host = g_base + 0xA0000000ull;
    VkDeviceSize size = 0x20000000;

    auto getProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.device, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT hp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    VK_CHECK(getProps(g_vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp));

    g_vk.guestBuffer = CreateGuestBuffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.device, g_vk.guestBuffer, &req);

    VkImportMemoryHostPointerInfoEXT imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = host;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp };
    ai.allocationSize = size;
    ai.memoryTypeIndex = VkFindMemoryType(hp.memoryTypeBits & req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (VkResult r = vkAllocateMemory(g_vk.device, &ai, nullptr, &g_vk.guestMemory); r != VK_SUCCESS)
        Fatal("vk: the driver cannot import the guest memory (memfd, shared) as host memory (%d) and does not export a dma-buf - "
              "this GPU / driver combination is not supported yet", int(r));
    VK_CHECK(vkBindBufferMemory(g_vk.device, g_vk.guestBuffer, g_vk.guestMemory, 0));
    g_vk.guestSize = size;
    Log("vk: imported guest physical memory (512 MiB memfd, zero-copy)");
}

#ifdef __ANDROID__
bool VkMapGuestMemoryPlaced()
{
    if (!g_vk.hasPlacedMap) return false;
    const VkDeviceSize size = 0x20000000;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    VK_CHECK(vkCreateBuffer(g_vk.device, &bi, nullptr, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.device, buf, &req);
    int type = -1;
    for (VkMemoryPropertyFlags want : { VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                        VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) })
    {
        for (uint32_t i = 0; i < g_vk.memProps.memoryTypeCount && type < 0; i++)
            if ((req.memoryTypeBits & (1u << i)) && (g_vk.memProps.memoryTypes[i].propertyFlags & want) == want) type = int(i);
        if (type >= 0) break;
    }
    if (type < 0) { vkDestroyBuffer(g_vk.device, buf, nullptr); return false; }
    VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.buffer = buf;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(type);
    VkDeviceMemory mem;
    VK_CHECK(vkAllocateMemory(g_vk.device, &ai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(g_vk.device, buf, mem, 0));
    // The driver's own mapping keeps the cached / IO-coherent attributes (an mmap of the exported
    // dma-buf came out uncached and the guest's atomics raised SIGBUS): map it once, find the KGSL
    // file and offset behind it, and map that object again at the three physical views.
    uint8_t* a0 = g_base + 0xA0000000u;
    void* drv = nullptr;
    if (vkMapMemory(g_vk.device, mem, 0, VK_WHOLE_SIZE, 0, &drv) != VK_SUCCESS || !drv) { LogWarn("vk: vkMapMemory failed"); return false; }
    // the KGSL file and offset behind it, and the driver's descriptor for that file
    char path[256] = {};
    unsigned long long pgoff = 0;
    if (FILE* m = fopen("/proc/self/maps", "r"))
    {
        char line[512];
        while (fgets(line, sizeof(line), m))
        {
            unsigned long long lo, hi, off;
            char perms[8], dev[16], file[256] = {};
            unsigned long inode;
            if (sscanf(line, "%llx-%llx %7s %llx %15s %lu %255s", &lo, &hi, perms, &off, dev, &inode, file) >= 6 && lo == uint64_t(uintptr_t(drv)))
            {
                strncpy(path, file, sizeof(path) - 1);
                pgoff = off;
                break;
            }
        }
        fclose(m);
    }
    int fd = -1;
    if (path[0])
        for (int i = 0; i < 1024 && fd < 0; i++)
        {
            char link[64], target[256] = {};
            snprintf(link, sizeof(link), "/proc/self/fd/%d", i);
            if (readlink(link, target, sizeof(target) - 1) > 0 && !strcmp(target, path)) fd = i;
        }
    if (fd < 0) { LogWarn("vk: cannot find the driver file behind the guest memory (%s)", path[0] ? path : "?"); return false; }
    // 0xC0000000 view, then 0xE0000000 (physical 0x1000.. : the object mapped at 0xDFFFF000, which
    // replaces the last page of the 0xC0000000 view - physical 0x1FFFF000 stays reachable at 0xBFFFF000)
    if (mmap(a0, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(pgoff)) == MAP_FAILED ||
        mmap(g_base + 0xC0000000u, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(pgoff)) == MAP_FAILED ||
        mmap(g_base + 0xDFFFF000u, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(pgoff)) == MAP_FAILED)
    {
        LogWarn("vk: mapping the extra physical views failed (%s)", strerror(errno));
        return false;
    }
    // alias check + read speed (cached memory reads at several GB/s)
    volatile uint32_t* pa = reinterpret_cast<volatile uint32_t*>(a0 + 0x1000);
    *pa = 0x12345678u;
    bool alias = *reinterpret_cast<volatile uint32_t*>(g_base + 0xC0001000u) == 0x12345678u &&
                 *reinterpret_cast<volatile uint32_t*>(g_base + 0xE0000000u) == 0x12345678u;
    *pa = 0;
    auto t0 = std::chrono::steady_clock::now();
    uint64_t sum = 0;
    for (size_t i = 0; i < (16u << 20) / 8; i++) sum += reinterpret_cast<volatile uint64_t*>(a0 + 0x100000)[i];
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_vk.guestBuffer = buf;
    g_vk.guestMemory = mem;
    g_vk.guestSize = size;
    VkMemoryPropertyFlags tf = g_vk.memProps.memoryTypes[type].propertyFlags;
    Log("vk: guest physical memory mapped by the driver at the three views (%s, memory type %d %s, %s, read %.0f MB/s%s)", path, type,
        (tf & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "cached" : "UNCACHED", alias ? "aliased" : "NOT ALIASED", 16.0 / (ms / 1000.0), sum == 1 ? "" : "");
    if (!alias) Fatal("vk: the physical views do not alias");
    return true;
}
#endif

void VkInit()
{
    if (g_vk.device) return;
#ifdef __ANDROID__
    // Turnip (Mesa, Vulkan 1.3 on Adreno 6xx/7xx) shipped in the APK, loaded through libadrenotools
    // (the system's Vulkan loader in a private namespace, redirected to it). A file
    // "use_stock_driver" in the data folder keeps the phone's own driver.
    bool stock = access("use_stock_driver", F_OK) == 0;
    if (!stock)
    {
        Dl_info info{};
        dladdr(reinterpret_cast<void*>(&VkInit), &info);
        std::string libDir = info.dli_fname ? std::string(info.dli_fname) : std::string();
        libDir = libDir.substr(0, libDir.rfind('/') + 1);   // the APK's native library folder
        std::string tmp = std::string(SDL_GetAndroidInternalStoragePath() ? SDL_GetAndroidInternalStoragePath() : ".") + "/adrenotools/";
        mkdir(tmp.c_str(), 0700);
        void* h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp.c_str(), libDir.c_str(), libDir.c_str(),
                                             "libvulkan_freedreno.so", nullptr, nullptr);
        auto gipa = h ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr")) : nullptr;
        if (gipa)
        {
            volkInitializeCustom(gipa);
            g_vk.customDriver = true;
            Log("vk: Turnip loaded through libadrenotools (%s)", libDir.c_str());
        }
        else LogWarn("vk: libadrenotools could not load Turnip (%s) - using the system driver", dlerror() ? dlerror() : "?");
    }
    if (!g_vk.customDriver && volkInitialize() != VK_SUCCESS) Fatal("vk: no Vulkan loader (libvulkan) found");
#else
    if (volkInitialize() != VK_SUCCESS) Fatal("vk: no Vulkan loader (libvulkan) found");
#endif
    uint32_t n = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&n);
    std::vector<const char*> instExts(sdlExts, sdlExts + n);

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "BO3 Recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(instExts.size());
    ici.ppEnabledExtensionNames = instExts.data();
    std::vector<const char*> layers;
    if (getenv("BO3_VK_VALIDATION")) layers.push_back("VK_LAYER_KHRONOS_validation");
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &g_vk.instance));
    volkLoadInstance(g_vk.instance);

#ifdef __ANDROID__
    // SDL's Vulkan calls go through its own copy of the loader, not the one this instance came
    // from (libadrenotools): create the surface here.
    {
        auto* win = static_cast<ANativeWindow*>(SDL_GetPointerProperty(SDL_GetWindowProperties(g_vk.window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
        VkAndroidSurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
        sci.window = win;
        if (!win || vkCreateAndroidSurfaceKHR(g_vk.instance, &sci, nullptr, &g_vk.surface) != VK_SUCCESS)
            Fatal("vk: cannot create the Android surface");
    }
#else
    if (!SDL_Vulkan_CreateSurface(g_vk.window, g_vk.instance, nullptr, &g_vk.surface))
        Fatal("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
#endif

    // Pick the first discrete/integrated GPU that is not a software rasterizer.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(g_vk.instance, &count, devs.data());
    for (auto d : devs)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && g_vk.phys) continue;
        if (!g_vk.phys || p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { g_vk.phys = d; g_vk.props = p; }
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) break;
    }
    if (!g_vk.phys) Fatal("vk: no Vulkan device");
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.memProps);
    g_vk.maxAnisotropy = g_vk.props.limits.maxSamplerAnisotropy;
    Log("vk: device %s (Vulkan %u.%u)", g_vk.props.deviceName, VK_API_VERSION_MAJOR(g_vk.props.apiVersion),
        VK_API_VERSION_MINOR(g_vk.props.apiVersion));

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &qn, qf.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; i++)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { g_vk.queueFamily = i; found = true; break; }
    }
    if (!found) Fatal("vk: no graphics+present queue");

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, nullptr, &en, exts.data());
    std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    // host memory import: only the fallback when there is no dma-buf export (Turnip has dma-buf)
    g_vk.hasHostImport = HasExtension(exts, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (g_vk.hasHostImport) devExts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (HasExtension(exts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && HasExtension(exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME))
    {
        devExts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        devExts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        g_vk.hasDmaBuf = true;
    }
#ifdef __ANDROID__
    int placedF = 0;
    // KGSL (Turnip): guest memory mapped from the driver's own GPU object (VkMapGuestMemoryPlaced)
    g_vk.hasPlacedMap = g_vk.customDriver && getenv("BO3_KGSL_VIEWS") != nullptr;   // (does not work: KGSL maps an object once)
#endif
    if (HasExtension(exts, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME))
    {
        devExts.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
        g_vk.hasStencilExport = true;
    }

    VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrsF{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    if (HasExtension(exts, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) && !getenv("BO3_NO_VRS"))
    {
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR q{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
        VkPhysicalDeviceFeatures2 qf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q };
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &qf);
        if (q.pipelineFragmentShadingRate)
        {
            devExts.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
            vrsF.pipelineFragmentShadingRate = VK_TRUE;
            g_vk.hasVrs = true;
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g_vk.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    void* featNext = g_vk.hasVrs ? static_cast<void*>(&vrsF) : nullptr;
#ifdef __ANDROID__
    (void)placedF;
#endif
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, featNext };
    f13.dynamicRendering = VK_TRUE;
    f13.robustImageAccess = VK_TRUE;   // out-of-range accesses read 0 instead of faulting the GPU
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    f12.storageBuffer8BitAccess = VK_TRUE;
    f12.shaderInt8 = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.samplerMirrorClampToEdge = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    f2.features.geometryShader = VK_TRUE;
    f2.features.robustBufferAccess = VK_TRUE;
    f2.features.independentBlend = VK_TRUE;
    f2.features.fillModeNonSolid = VK_TRUE;
    f2.features.depthClamp = VK_TRUE;
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.features.textureCompressionBC = VK_TRUE;
    f2.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    f2.features.fragmentStoresAndAtomics = VK_TRUE;
    f2.features.shaderInt16 = VK_TRUE;

    // Drop (and report) requested features the device lacks instead of failing device creation.
    {
        VkPhysicalDeviceVulkan13Features s13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features s12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s13 };
        VkPhysicalDeviceFeatures2 s2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s12 };
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &s2);
        std::string missing;
#define BO3_FEATURE(want, have, name) if (want.name && !have.name) { want.name = VK_FALSE; missing += " " #name; }
        BO3_FEATURE(f13, s13, dynamicRendering) BO3_FEATURE(f13, s13, robustImageAccess) BO3_FEATURE(f13, s13, synchronization2)
        BO3_FEATURE(f12, s12, storageBuffer8BitAccess) BO3_FEATURE(f12, s12, shaderInt8) BO3_FEATURE(f12, s12, scalarBlockLayout)
        BO3_FEATURE(f12, s12, descriptorIndexing) BO3_FEATURE(f12, s12, runtimeDescriptorArray)
        BO3_FEATURE(f12, s12, shaderSampledImageArrayNonUniformIndexing) BO3_FEATURE(f12, s12, samplerMirrorClampToEdge)
        BO3_FEATURE(f2.features, s2.features, geometryShader) BO3_FEATURE(f2.features, s2.features, robustBufferAccess)
        BO3_FEATURE(f2.features, s2.features, independentBlend) BO3_FEATURE(f2.features, s2.features, fillModeNonSolid)
        BO3_FEATURE(f2.features, s2.features, depthClamp) BO3_FEATURE(f2.features, s2.features, samplerAnisotropy)
        BO3_FEATURE(f2.features, s2.features, textureCompressionBC) BO3_FEATURE(f2.features, s2.features, vertexPipelineStoresAndAtomics)
        BO3_FEATURE(f2.features, s2.features, fragmentStoresAndAtomics) BO3_FEATURE(f2.features, s2.features, shaderInt16)
#undef BO3_FEATURE
        if (!missing.empty()) LogWarn("vk: device lacks:%s (continuing without them)", missing.c_str());
        g_vk.hasAnisotropy = f2.features.samplerAnisotropy;
        if (!g_vk.hasAnisotropy) g_vk.maxAnisotropy = 1.0f;   // samplers then never request it
    }

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2 };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK(vkCreateDevice(g_vk.phys, &dci, nullptr, &g_vk.device));
    volkLoadDevice(g_vk.device);   // device functions called straight into the driver
    vkGetDeviceQueue(g_vk.device, g_vk.queueFamily, 0, &g_vk.queue);
    if (g_vk.hasVrs)
    {
        g_vk.cmdSetFragmentShadingRate = (PFN_vkCmdSetFragmentShadingRateKHR)vkGetDeviceProcAddr(g_vk.device, "vkCmdSetFragmentShadingRateKHR");
        g_vk.hasVrs = g_vk.cmdSetFragmentShadingRate != nullptr;
        Log("vk: fragment shading rate %s", g_vk.hasVrs ? "enabled" : "unavailable");
    }
    // Pipeline cache on disk: drivers without their own disk cache (Turnip on Android: Mesa's is
    // off there) otherwise recompile every pipeline each run (~250 ms each on an Adreno 618).
    {
        std::vector<char> blob;
        if (FILE* f = fopen("shadercache/pipelines.bin", "rb"))
        {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) { blob.resize(size_t(n)); if (fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear(); }
            fclose(f);
        }
        VkPipelineCacheCreateInfo pc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        pc.initialDataSize = blob.size();
        pc.pInitialData = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(g_vk.device, &pc, nullptr, &g_vk.pipelineCache) != VK_SUCCESS)
        {
            pc.initialDataSize = 0; pc.pInitialData = nullptr;   // stale / other driver: start empty
            vkCreatePipelineCache(g_vk.device, &pc, nullptr, &g_vk.pipelineCache);
        }
        Log("vk: pipeline cache %zu KiB loaded", blob.size() / 1024);
    }

    VkRecreateSwapchain();
}

void VkRecreateSwapchain()
{
    vkDeviceWaitIdle(g_vk.device);
    for (auto v : g_vk.swapViews) vkDestroyImageView(g_vk.device, v, nullptr);
    g_vk.swapViews.clear();

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &fn, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { fmt = f; break; }
    g_vk.swapFormat = fmt.format;

    int w = 1280, h = 720;
    SDL_GetWindowSizeInPixels(g_vk.window, &w, &h);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFF) ext = { uint32_t(w), uint32_t(h) };
    ext.width = std::clamp(ext.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    ext.height = std::clamp(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    g_vk.swapExtent = ext;

    VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = g_vk.surface;
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // We never pre-rotate: identity lets the compositor rotate (Android in landscape reports a
    // 90/270 current transform, which we would otherwise claim to have applied ourselves).
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    if (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))
    {
        // window size is authoritative for the un-rotated image
        if (w > 0 && h > 0 && (w > h) != (ext.width > ext.height)) std::swap(ext.width, ext.height);
        g_vk.swapExtent = ext;
        sci.imageExtent = ext;
    }
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // The game paces itself on the emulated 60 Hz vsync interrupt; a blocking FIFO present on
    // the GPU thread would stack a second vblank wait (measured: 20 fps). Prefer MAILBOX.
    uint32_t pmn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &pmn, nullptr);
    std::vector<VkPresentModeKHR> pms(pmn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &pmn, pms.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(pms.begin(), pms.end(), m) != pms.end(); };
    sci.presentMode = has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                    : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = g_vk.swapchain;
    VkSwapchainKHR sc;
    VK_CHECK(vkCreateSwapchainKHR(g_vk.device, &sci, nullptr, &sc));
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.device, g_vk.swapchain, nullptr);
    g_vk.swapchain = sc;

    uint32_t in = 0;
    vkGetSwapchainImagesKHR(g_vk.device, sc, &in, nullptr);
    g_vk.swapImages.resize(in);
    vkGetSwapchainImagesKHR(g_vk.device, sc, &in, g_vk.swapImages.data());
    for (auto img : g_vk.swapImages)
    {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView v;
        VK_CHECK(vkCreateImageView(g_vk.device, &vi, nullptr, &v));
        g_vk.swapViews.push_back(v);
    }
    g_vk.swapchainDirty = false;
    Log("vk: surface transform 0x%x (window %dx%d)", unsigned(caps.currentTransform), w, h);
    Log("vk: swapchain %ux%u, %u images, present mode %d", ext.width, ext.height, in, int(sci.presentMode));
}

VkImageObj VkCreateImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, uint32_t mips, uint32_t layers, bool cube)
{
    VkImageObj o;
    o.width = w; o.height = h; o.format = fmt; o.mips = mips; o.layers = layers;
    bool depth = fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D16_UNORM;
    bool stencil = fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    o.aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = mips;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VK_CHECK(vkCreateImage(g_vk.device, &ci, nullptr, &o.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.device, o.image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = VkFindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g_vk.device, &ai, nullptr, &o.memory));
    VK_CHECK(vkBindImageMemory(g_vk.device, o.image, o.memory, 0));
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = o.image;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : (layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    vi.format = fmt;
    vi.subresourceRange = { depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : o.aspect, 0, mips, 0, layers };
    VK_CHECK(vkCreateImageView(g_vk.device, &vi, nullptr, &o.view));
    return o;
}

void VkDestroyImage(VkImageObj& img)
{
    if (img.view) vkDestroyImageView(g_vk.device, img.view, nullptr);
    if (img.image) vkDestroyImage(g_vk.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(g_vk.device, img.memory, nullptr);
    img = {};
}

uint64_t g_vkBarrierCount = 0;
void VkTransitionRaw(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
    g_vkBarrierCount++;
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void VkTransition(VkCommandBuffer cmd, VkImageObj& img, VkImageLayout to)
{
    if (img.layout == to) return;
    VkTransitionRaw(cmd, img.image, img.aspect, img.layout, to);
    img.layout = to;
}

VkBufferObj VkCreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible)
{
    VkBufferObj b;
    b.size = size;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    VK_CHECK(vkCreateBuffer(g_vk.device, &bi, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.device, b.buffer, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = VkFindMemoryType(req.memoryTypeBits, hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g_vk.device, &ai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(g_vk.device, b.buffer, b.memory, 0));
    if (hostVisible) VK_CHECK(vkMapMemory(g_vk.device, b.memory, 0, size, 0, &b.mapped));
    return b;
}
