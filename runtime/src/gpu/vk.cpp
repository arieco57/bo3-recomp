#include "vk.h"
#include "shader_translator.h"
#include "../log.h"
#include "../memory.h"
#include "../write_watch.h"
#include "../ui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#ifdef __ANDROID__
#include <adrenotools/driver.h>
#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <csetjmp>
#include <csignal>
#include <algorithm>
#include <atomic>
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
#ifdef __ANDROID__
static bool ProbeCpuMapping(int fd, double& mbps, bool& atomicOk);
#endif

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
#ifdef __ANDROID__
    // no native window while the screen is off / the app is in the background: wait for it
    for (int waited = 0; !g_vk.window; waited++)
    {
        if (waited == 0) Log("vk: no native window yet (%s), waiting", SDL_GetError());
        SDL_PumpEvents();
        SDL_Delay(200);
        g_vk.window = SDL_CreateWindow(title, width, height, flags);
    }
#endif
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
#ifdef __ANDROID__
    {
        double mbps;
        bool atomicOk;
        if (!ProbeCpuMapping(fd, mbps, atomicOk))
        {
            LogWarn("vk: the exported dma-buf's CPU mapping is not usable for guest memory (read %.0f MB/s, atomics %s) - not used",
                    mbps, atomicOk ? "ok" : "FAULT");
            close(fd);
            vkFreeMemory(g_vk.device, mem, nullptr);
            vkDestroyBuffer(g_vk.device, buf, nullptr);
            return -1;
        }
    }
#endif
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

// Copy mode: GPU memory of its own for guest physical memory (no sharing possible), mapped for the
// CPU-side copies (write_watch.cpp). Cached host memory if the driver has it (faster CPU writes).
static VkMappedMemoryRange s_copyRange{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
static bool s_copyCoherent = true;
static VkDeviceSize s_atom = 64;
static void CopyFlush(uint32_t off, uint32_t n)
{
    if (s_copyCoherent) return;
    VkMappedMemoryRange r = s_copyRange;
    r.offset = off & ~(s_atom - 1);
    r.size = std::min<VkDeviceSize>(((uint64_t(off) + n + s_atom - 1) & ~(s_atom - 1)) - r.offset, 0x20000000 - r.offset);
    vkFlushMappedMemoryRanges(g_vk.device, 1, &r);
}
static void CopyInvalidate(uint32_t off, uint32_t n)
{
    if (s_copyCoherent) return;
    VkMappedMemoryRange r = s_copyRange;
    r.offset = off & ~(s_atom - 1);
    r.size = std::min<VkDeviceSize>(((uint64_t(off) + n + s_atom - 1) & ~(s_atom - 1)) - r.offset, 0x20000000 - r.offset);
    vkInvalidateMappedMemoryRanges(g_vk.device, 1, &r);
}
void VkGuestCopyInvalidate(uint32_t off, uint32_t n) { CopyInvalidate(off, n); }

static bool CopyGuestMemory()
{
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = 0x20000000;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g_vk.device, &bi, nullptr, &g_vk.guestBuffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.device, g_vk.guestBuffer, &req);
    // Android: device-local write-combined first - the GPU reads it fast (cached coherent memory on Adreno
    // stock drivers: 430 ms of GPU per frame, snooping the CPU caches); the CPU only writes it (copies up).
    // Desktop: cached first (CPU copies), then coherent, then anything mappable.
    int type = -1;
    struct Want { VkMemoryPropertyFlags need, avoid; };
#ifdef __ANDROID__
    const bool wc = !getenv("BO3_COPY_CACHED");
#else
    const bool wc = getenv("BO3_COPY_WC") != nullptr;
#endif
    const VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, HC = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                CA = VK_MEMORY_PROPERTY_HOST_CACHED_BIT, DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    std::vector<Want> tries;
    if (wc) tries.push_back({ DL | HV | HC, CA });
    tries.push_back({ HV | CA | HC, 0 });
    tries.push_back({ HV | CA, 0 });
    tries.push_back({ HV | HC, 0 });
    tries.push_back({ HV, 0 });
    for (auto want : tries)
    {
        for (uint32_t i = 0; i < g_vk.memProps.memoryTypeCount && type < 0; i++)
        {
            VkMemoryPropertyFlags pf = g_vk.memProps.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) && (pf & want.need) == want.need && !(pf & want.avoid)) type = int(i);
        }
        if (type >= 0) break;
    }
    if (type < 0) return false;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(type);
    if (vkAllocateMemory(g_vk.device, &ai, nullptr, &g_vk.guestMemory) != VK_SUCCESS) return false;
    VK_CHECK(vkBindBufferMemory(g_vk.device, g_vk.guestBuffer, g_vk.guestMemory, 0));
    void* map = nullptr;
    if (vkMapMemory(g_vk.device, g_vk.guestMemory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) return false;
    VkMemoryPropertyFlags f = g_vk.memProps.memoryTypes[type].propertyFlags;
    s_copyCoherent = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    s_copyRange.memory = g_vk.guestMemory;
    s_atom = std::max<VkDeviceSize>(g_vk.props.limits.nonCoherentAtomSize, 1);
    g_vk.guestSize = 0x20000000;
    g_vk.guestCopy = static_cast<uint8_t*>(map);
    Log("vk: guest memory in copy mode (memory type %d:%s%s%s)", type, (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " device-local" : "",
        (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " cached" : "", s_copyCoherent ? " coherent" : " non-coherent");
    return true;
}

bool VkCopyGuestMemory()
{
    if (!CopyGuestMemory())
    {
#ifdef __ANDROID__
        UiMessage("BO3 Recomp", std::string("Tu GPU / driver (") + g_vk.props.deviceName + ") todavía no es compatible: no deja reservar la memoria del juego en la GPU.\n\nMandá el archivo Descargas/BO3Recomp/bo3-*-log.txt para que lo veamos.");
#endif
        Fatal("vk: cannot allocate a 512 MiB host-visible buffer for guest memory (copy mode)");
    }
    WatchEnableCopyMode(g_vk.guestCopy, s_copyCoherent ? nullptr : CopyFlush);
    return true;
}

void VkImportGuestMemory()
{
    if (!g_vk.hasHostImport || getenv("BO3_COPY_MEMORY"))
    {
        VkCopyGuestMemory();   // (no sharing: the GPU gets a copy)
        return;
    }
    if (false)
    {
#ifdef __ANDROID__
        UiMessage("BO3 Recomp", std::string("Tu GPU / driver (") + g_vk.props.deviceName + ") todavía no es compatible: no deja compartir la memoria del juego con la GPU.\n\nMandá el archivo Descargas/BO3Recomp/bo3-*-log.txt para que lo veamos.");
#endif
        Fatal("vk: the driver neither exports dma-bufs nor imports host memory - guest memory cannot be shared");
    }
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
    {
        LogWarn("vk: host memory import failed (%d): copy mode", int(r));
        vkDestroyBuffer(g_vk.device, g_vk.guestBuffer, nullptr);
        g_vk.guestBuffer = VK_NULL_HANDLE;
        VkCopyGuestMemory();
        return;
#ifdef __ANDROID__
        UiMessage("BO3 Recomp", std::string("Tu GPU / driver (") + g_vk.props.deviceName + ") todavía no es compatible: no deja compartir la memoria del juego con la GPU.\n\nMandá el archivo Descargas/BO3Recomp/bo3-*-log.txt para que lo veamos.");
#endif
        Fatal("vk: the driver cannot import the guest memory (memfd, shared) as host memory (%d) and does not export a dma-buf - "
              "this GPU / driver combination is not supported yet", int(r));
    }
    VK_CHECK(vkBindBufferMemory(g_vk.device, g_vk.guestBuffer, g_vk.guestMemory, 0));
    g_vk.guestSize = size;
    Log("vk: imported guest physical memory (512 MiB memfd, zero-copy)");
}

#ifdef __ANDROID__
// The dma-buf behind an AHardwareBuffer: from its native handle (exported by libnativewindow, not
// in the NDK headers), else through the public AHardwareBuffer_sendHandleToUnixSocket.
static int AhbDmaBufFd(AHardwareBuffer* ahb)
{
    struct NativeHandle { int version, numFds, numInts; int data[1]; };
    using GetHandle = const NativeHandle* (*)(const AHardwareBuffer*);
    void* lib = dlopen("libnativewindow.so", RTLD_NOW);
    auto get = lib ? reinterpret_cast<GetHandle>(dlsym(lib, "AHardwareBuffer_getNativeHandle")) : nullptr;
    if (get)
        if (const NativeHandle* h = get(ahb); h && h->numFds >= 1) return dup(h->data[0]);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return -1;
    int fd = -1;
    if (AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0]) == 0)
    {
        char data[4096];
        alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int) * 32)];
        iovec iov{ data, sizeof(data) };
        msghdr m{};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = ctl;
        m.msg_controllen = sizeof(ctl);
        if (recvmsg(sv[1], &m, 0) > 0)
            for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                {
                    int nfd = int((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    int* fds = reinterpret_cast<int*>(CMSG_DATA(c));
                    for (int i = 0; i < nfd; i++)
                        if (fd < 0) fd = fds[i]; else close(fds[i]);
                }
    }
    close(sv[0]);
    close(sv[1]);
    return fd;
}

// The CPU side of a dma-buf must be cached memory where atomics work: Qualcomm's gralloc hands out
// uncached mappings (~0.5 GB/s, atomics fault with SIGBUS) - useless for guest memory.
static sigjmp_buf s_probeJmp;
static void ProbeBus(int) { siglongjmp(s_probeJmp, 1); }
static bool ProbeCpuMapping(int fd, double& mbps, bool& atomicOk)
{
    const size_t len = 8u << 20;
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    mbps = 0;
    atomicOk = false;
    if (p == MAP_FAILED) return false;
    auto t0 = std::chrono::steady_clock::now();
    uint64_t sum = 0;
    for (size_t i = 0; i < len / 8; i++) sum += reinterpret_cast<volatile uint64_t*>(p)[i];
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mbps = 8.0 / (ms / 1000.0) + double(sum & 0);
    struct sigaction sa{}, oldBus{}, oldSegv{};
    sa.sa_handler = ProbeBus;
    sigaction(SIGBUS, &sa, &oldBus);
    sigaction(SIGSEGV, &sa, &oldSegv);
    if (sigsetjmp(s_probeJmp, 1) == 0)
    {
        auto* w = reinterpret_cast<std::atomic<uint32_t>*>(p);
        uint32_t e = w->load();
        w->compare_exchange_strong(e, e);
        atomicOk = true;
    }
    sigaction(SIGBUS, &oldBus, nullptr);
    sigaction(SIGSEGV, &oldSegv, nullptr);
    munmap(p, len);
    return atomicOk && mbps > 2000;
}

int VkAhbGuestMemory()
{
    if (!g_vk.hasAhb) return -1;
    const VkDeviceSize size = 0x20000000;
    AHardwareBuffer_Desc d{};
    d.width = uint32_t(size);
    d.height = 1;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_BLOB;
    d.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    AHardwareBuffer* ahb = nullptr;
    if (int e = AHardwareBuffer_allocate(&d, &ahb); e != 0 || !ahb)
    {
        LogWarn("vk: AHardwareBuffer_allocate (512 MiB blob) failed (%d)", e);
        return -1;
    }
    int fd = AhbDmaBufFd(ahb);
    off_t fdSize = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (fd < 0 || (fdSize > 0 && fdSize < off_t(size)))
    {
        LogWarn("vk: no usable dma-buf behind the AHardwareBuffer (fd %d, size %lld)", fd, (long long)fdSize);
        if (fd >= 0) close(fd);
        AHardwareBuffer_release(ahb);
        return -1;
    }
    double mbps;
    bool atomicOk;
    if (!ProbeCpuMapping(fd, mbps, atomicOk))
    {
        LogWarn("vk: the AHardwareBuffer's CPU mapping is not usable for guest memory (read %.0f MB/s, atomics %s) - not used",
                mbps, atomicOk ? "ok" : "FAULT");
        close(fd);
        AHardwareBuffer_release(ahb);
        return -1;
    }
    Log("vk: AHardwareBuffer CPU mapping: read %.0f MB/s, atomics ok", mbps);
    auto getProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(g_vk.device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    VkAndroidHardwareBufferPropertiesANDROID ap{ VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    VkResult r = getProps ? getProps(g_vk.device, ahb, &ap) : VK_ERROR_EXTENSION_NOT_PRESENT;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int type = -1;
    if (r == VK_SUCCESS)
    {
        buf = CreateGuestBuffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_vk.device, buf, &req);
        uint32_t bits = ap.memoryTypeBits & req.memoryTypeBits;
        for (uint32_t i = 0; i < g_vk.memProps.memoryTypeCount && type < 0; i++)
            if ((bits & (1u << i)) && (g_vk.memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = int(i);
        for (uint32_t i = 0; i < g_vk.memProps.memoryTypeCount && type < 0; i++)
            if (bits & (1u << i)) type = int(i);
        VkImportAndroidHardwareBufferInfoANDROID imp{ VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID };
        imp.buffer = ahb;
        VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp };
        ded.buffer = buf;
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded };
        ai.allocationSize = ap.allocationSize;
        ai.memoryTypeIndex = uint32_t(std::max(type, 0));
        r = type < 0 ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(g_vk.device, &ai, nullptr, &mem);
        if (r != VK_SUCCESS)
        {
            // some drivers refuse the dedicated info for blob buffers
            ai.pNext = &imp;
            if (type >= 0) r = vkAllocateMemory(g_vk.device, &ai, nullptr, &mem);
        }
        if (r == VK_SUCCESS) r = vkBindBufferMemory(g_vk.device, buf, mem, 0);
    }
    if (r != VK_SUCCESS)
    {
        LogWarn("vk: importing the AHardwareBuffer failed (%d, memory bits 0x%X)", int(r), ap.memoryTypeBits);
        if (mem) vkFreeMemory(g_vk.device, mem, nullptr);
        if (buf) vkDestroyBuffer(g_vk.device, buf, nullptr);
        close(fd);
        AHardwareBuffer_release(ahb);
        return -1;
    }
    g_vk.guestBuffer = buf;
    g_vk.guestMemory = mem;
    g_vk.guestSize = size;
    Log("vk: guest physical memory is an AHardwareBuffer (512 MiB blob, dma-buf fd, memory type %d)", type);
    return fd;   // (the AHardwareBuffer stays alive: Vulkan and the mappings hold it)
}

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

#ifdef __ANDROID__
// Phone / SoC / kernel in the log (testers send bo3.log: Descargas/BO3Recomp)
static void LogDeviceInfo()
{
    auto prop = [](const char* k) { char v[PROP_VALUE_MAX] = {}; __system_property_get(k, v); return std::string(v); };
    struct utsname u{};
    uname(&u);
    Log("device: %s %s (%s), SoC %s %s / %s, Android %s (API %s), kernel %s", prop("ro.product.manufacturer").c_str(),
        prop("ro.product.model").c_str(), prop("ro.product.device").c_str(), prop("ro.soc.manufacturer").c_str(), prop("ro.soc.model").c_str(),
        prop("ro.board.platform").c_str(), prop("ro.build.version.release").c_str(), prop("ro.build.version.sdk").c_str(), u.release);
}

// gpu_state in the data folder: "starting <driver>" until frames are presented, then "ok <driver>".
// The game activity reads it at the next start (the last run never showed a picture -> offer
// the other driver).
void VkWriteGpuState(const char* state)
{
    if (FILE* f = fopen("gpu_state", "w"))
    {
        fprintf(f, "%s %s\n", state, g_vk.customDriver ? "turnip" : "stock");
        fclose(f);
    }
}
#endif

static bool CreateInstance(std::vector<const char*>& instExts)
{
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
    VkResult r = vkCreateInstance(&ici, nullptr, &g_vk.instance);
    if (r != VK_SUCCESS) { LogWarn("vk: vkCreateInstance failed (%d)", int(r)); g_vk.instance = VK_NULL_HANDLE; return false; }
    volkLoadInstance(g_vk.instance);
    return true;
}

static uint32_t CountDevices()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &count, nullptr);
    return count;
}

void VkInit()
{
    if (g_vk.device) return;
    uint32_t n = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&n);
    std::vector<const char*> instExts(sdlExts, sdlExts + n);
#ifdef __ANDROID__
    LogDeviceInfo();
    // Turnip (Mesa, Vulkan 1.3 on Adreno 6xx/7xx/8xx) shipped in the APK, loaded through libadrenotools
    // (the system's Vulkan loader in a private namespace, redirected to it). Only on Adreno (KGSL);
    // a Turnip that sees no GPU (an Adreno it does not know) falls back to the phone's driver.
    // Data folder files: "use_stock_driver" = always the phone's driver, "use_turnip" = always Turnip.
    bool kgsl = access("/dev/kgsl-3d0", F_OK) == 0;
    bool stock = access("use_stock_driver", F_OK) == 0;
    bool forceTurnip = access("use_turnip", F_OK) == 0;
    // a driver the player installed ("Driver de video" > personalizado: a Turnip / Mesa .zip like the ones
    // emulators use): its library in <internal storage>/drivers/custom/, its file name in use_custom_driver
    std::string customLib;
    if (FILE* cf = fopen("use_custom_driver", "r"))
    {
        char b[256] = {};
        if (fgets(b, sizeof(b), cf)) customLib = b;
        fclose(cf);
        while (!customLib.empty() && (customLib.back() == '\n' || customLib.back() == '\r' || customLib.back() == ' ')) customLib.pop_back();
        if (customLib.find('/') != std::string::npos) customLib.clear();   // (a plain file name only)
    }
    // Turnip's default autotune keeps the main scene pass in sysmem; "profiled" measures both modes per
    // render pass and picks GMEM there (Adreno 618, Freerun: GPU 20.8 -> 16.8 ms/frame). Other drivers
    // ignore it; a value in bo3env.txt wins.
    setenv("TU_AUTOTUNE_ALGO", "profiled", 0);
    if (!customLib.empty() && !stock)
    {
        Dl_info info{};
        dladdr(reinterpret_cast<void*>(&VkInit), &info);
        std::string libDir = info.dli_fname ? std::string(info.dli_fname) : std::string();
        libDir = libDir.substr(0, libDir.rfind('/') + 1);
        std::string internal = SDL_GetAndroidInternalStoragePath() ? SDL_GetAndroidInternalStoragePath() : ".";
        std::string tmp = internal + "/adrenotools/", drvDir = internal + "/drivers/custom/";
        mkdir(tmp.c_str(), 0700);
        void* h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp.c_str(), libDir.c_str(), drvDir.c_str(),
                                             customLib.c_str(), nullptr, nullptr);
        auto gipa = h ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr")) : nullptr;
        if (gipa)
        {
            volkInitializeCustom(gipa);
            g_vk.customDriver = true;
            Log("vk: custom driver %s loaded through libadrenotools", customLib.c_str());
            if (!CreateInstance(instExts) || CountDevices() == 0)
            {
                LogWarn("vk: the custom driver %s sees no GPU - using the phone's driver", customLib.c_str());
                if (g_vk.instance) vkDestroyInstance(g_vk.instance, nullptr);
                g_vk.instance = VK_NULL_HANDLE;
                g_vk.customDriver = false;
            }
            else stock = true;   // (no Turnip below)
        }
        else
        {
            LogWarn("vk: the custom driver %s could not be loaded (%s) - using the phone's driver", customLib.c_str(), dlerror() ? dlerror() : "?");
            stock = true;
        }
    }
    Log("vk: GPU kernel driver %s, driver choice: %s", kgsl ? "KGSL (Adreno)" : "not KGSL (Mali / PowerVR / Xclipse...)",
        stock ? "phone (use_stock_driver)" : forceTurnip ? "Turnip (use_turnip)" : "automatic");
    if (!stock && (kgsl || forceTurnip))
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
            uint32_t devs = CreateInstance(instExts) ? CountDevices() : 0;
            // Adreno 8xx (Snapdragon 8 Elite: AYN Odin 3, Galaxy S25...): Turnip's a8xx support is new
            // (garbage frames and VK_ERROR_DEVICE_LOST reported); their own driver has Vulkan 1.3+.
            // Automatic = the phone's driver there; "use_turnip" still forces Turnip.
            bool a8xx = false;
            if (devs && !forceTurnip)
            {
                std::vector<VkPhysicalDevice> pd(devs);
                vkEnumeratePhysicalDevices(g_vk.instance, &devs, pd.data());
                VkPhysicalDeviceProperties p{};
                vkGetPhysicalDeviceProperties(pd[0], &p);
                a8xx = strstr(p.deviceName, "Adreno (TM) 8") != nullptr;
                if (a8xx) Log("vk: %s - Turnip's Adreno 8xx support is new, using the phone's driver (use_turnip forces Turnip)", p.deviceName);
            }
            if (devs == 0 || a8xx)
            {
                if (!a8xx) LogWarn("vk: Turnip found no GPU it supports - using the phone's driver");
                if (g_vk.instance) vkDestroyInstance(g_vk.instance, nullptr);
                g_vk.instance = VK_NULL_HANDLE;
                g_vk.customDriver = false;
            }
        }
        else LogWarn("vk: libadrenotools could not load Turnip (%s) - using the phone's driver", dlerror() ? dlerror() : "?");
    }
    if (!g_vk.customDriver)
    {
        if (volkInitialize() != VK_SUCCESS) Fatal("vk: no Vulkan loader (libvulkan) found");
        if (!CreateInstance(instExts)) Fatal("vk: the phone's Vulkan driver cannot create an instance");
    }
    VkWriteGpuState("starting");
#else
    if (volkInitialize() != VK_SUCCESS) Fatal("vk: no Vulkan loader (libvulkan) found");
    if (!CreateInstance(instExts)) Fatal("vk: vkCreateInstance failed");
#endif

#ifdef __ANDROID__
    // SDL's Vulkan calls go through its own copy of the loader, not the one this instance came
    // from (libadrenotools): create the surface here.
    // SDL's window pointer property goes stale when the screen turns off before this point (surface
    // destroyed: crash in libvulkan): wait until the activity has a valid Surface and hold our own
    // reference to its window.
    {
        ANativeWindow* win = nullptr;
        for (int waited = 0; !win; waited++)
        {
            JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
            jobject act = (jobject)SDL_GetAndroidActivity();
            if (env && act)
            {
                jclass cls = env->GetObjectClass(act);
                jmethodID get = env->GetStaticMethodID(cls, "getNativeSurface", "()Landroid/view/Surface;");
                jobject surf = get ? env->CallStaticObjectMethod(cls, get) : nullptr;
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (surf)
                {
                    jclass sc = env->GetObjectClass(surf);
                    jmethodID valid = env->GetMethodID(sc, "isValid", "()Z");
                    if (valid && env->CallBooleanMethod(surf, valid)) win = ANativeWindow_fromSurface(env, surf);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    env->DeleteLocalRef(sc);
                    env->DeleteLocalRef(surf);
                }
                env->DeleteLocalRef(cls);
                env->DeleteLocalRef(act);
            }
            if (win) break;
            if (waited == 0) Log("vk: no window surface yet (screen off / app in background), waiting");
            SDL_PumpEvents();
            SDL_Delay(100);
        }
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
    {
        VkPhysicalDeviceDriverProperties dp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp };
        if (g_vk.props.apiVersion >= VK_API_VERSION_1_2) vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        Log("vk: device %s (Vulkan %u.%u.%u, vendor 0x%04X device 0x%08X, driver %s %s, version 0x%08X)", g_vk.props.deviceName,
            VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion), VK_API_VERSION_PATCH(g_vk.props.apiVersion),
            g_vk.props.vendorID, g_vk.props.deviceID, dp.driverName[0] ? dp.driverName : "?", dp.driverInfo, g_vk.props.driverVersion);
    }
    // Vulkan 1.1 is enough: what 1.2 / 1.3 made core comes from extensions or has a fallback
    if (g_vk.props.apiVersion < VK_API_VERSION_1_1)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Tu GPU (%s) tiene Vulkan %u.%u y el juego necesita Vulkan 1.1 o más.%s", g_vk.props.deviceName,
                 VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
                 g_vk.customDriver ? "" : "\n\nSi es una Adreno, probá con el driver Turnip (mantené apretado el ícono > Driver de video).");
        UiMessage("BO3 Recomp", msg);
        Fatal("vk: Vulkan 1.1 required (device has %u.%u)", VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion));
    }

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
    // any Android driver: guest memory as an AHardwareBuffer (VkAhbGuestMemory)
    if (HasExtension(exts, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) &&
        HasExtension(exts, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME))
    {
        devExts.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
        devExts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        g_vk.hasAhb = true;
    }
    {
        std::string list;
        for (auto& e : exts) { list += ' '; list += e.extensionName; }
        Log("vk: %u device extensions:%s", en, list.c_str());
    }
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

    const uint32_t api = g_vk.props.apiVersion;
    const bool v12 = api >= VK_API_VERSION_1_2, v13 = api >= VK_API_VERSION_1_3;
    // 1.3 features: core, or their extensions (BO3_NO_DYNAMIC_RENDERING / BO3_NO_SYNC2 / BO3_NO_EDS test the fallbacks)
    g_vk.hasDynRendering = !getenv("BO3_NO_DYNAMIC_RENDERING") && (v13 || HasExtension(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME));
    g_vk.hasSync2 = !getenv("BO3_NO_SYNC2") && (v13 || HasExtension(exts, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME));
    g_vk.hasEds = !getenv("BO3_NO_EDS") && (v13 || HasExtension(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME));
    auto addExt = [&](const char* name) { if (HasExtension(exts, name) && std::find_if(devExts.begin(), devExts.end(), [&](const char* e) { return !strcmp(e, name); }) == devExts.end()) devExts.push_back(name); };
    if (!v13)
    {
        if (g_vk.hasDynRendering)
        {
            addExt(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
            addExt(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME); addExt(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
            addExt(VK_KHR_MULTIVIEW_EXTENSION_NAME); addExt(VK_KHR_MAINTENANCE_2_EXTENSION_NAME);
        }
        if (g_vk.hasSync2) addExt(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        if (g_vk.hasEds) addExt(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    }
    if (!v12)
    {
        addExt(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
        addExt(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
        addExt(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
        addExt(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    }
    // float controls: the inf / nan preservation the shaders ask for (else they are built without it)
    {
        VkPhysicalDeviceFloatControlsProperties fc{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &fc };
        bool haveExt = v12 || HasExtension(exts, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
        if (haveExt) vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        g_vk.hasFloatControls = haveExt && fc.shaderSignedZeroInfNanPreserveFloat32 && !getenv("BO3_NO_FLOAT_CONTROLS");
    }
    Log("vk: Vulkan %u.%u: dynamic rendering %s, synchronization2 %s, extended dynamic state %s, inf/nan preserve %s", VK_API_VERSION_MAJOR(api),
        VK_API_VERSION_MINOR(api), g_vk.hasDynRendering ? "yes" : "no (render passes)", g_vk.hasSync2 ? "yes" : "no (classic barriers)",
        g_vk.hasEds ? "yes" : "no (static pipeline state)", g_vk.hasFloatControls ? "yes" : "no");

    void* featNext = g_vk.hasVrs ? static_cast<void*>(&vrsF) : nullptr;
#ifdef __ANDROID__
    (void)placedF;
#endif
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, featNext };
    f13.dynamicRendering = g_vk.hasDynRendering;
    f13.robustImageAccess = VK_TRUE;   // out-of-range accesses read 0 instead of faulting the GPU
    f13.synchronization2 = g_vk.hasSync2;
    // (before 1.3: the same features from their extensions' structures)
    VkPhysicalDeviceDynamicRenderingFeaturesKHR fDr{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
    VkPhysicalDeviceSynchronization2FeaturesKHR fS2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT fEds{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceScalarBlockLayoutFeatures fSb{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES };
    void* next13 = featNext;
    if (!v13)
    {
        if (g_vk.hasDynRendering) { fDr.dynamicRendering = VK_TRUE; fDr.pNext = next13; next13 = &fDr; }
        if (g_vk.hasSync2) { fS2.synchronization2 = VK_TRUE; fS2.pNext = next13; next13 = &fS2; }
        if (g_vk.hasEds) { fEds.extendedDynamicState = VK_TRUE; fEds.pNext = next13; next13 = &fEds; }
    }
    else next13 = &f13;
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, next13 };
    f12.storageBuffer8BitAccess = VK_TRUE;
    f12.shaderInt8 = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.samplerMirrorClampToEdge = VK_TRUE;
    void* next12 = &f12;
    if (!v12) { fSb.scalarBlockLayout = VK_TRUE; fSb.pNext = next13; next12 = &fSb; }
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, next12 };
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
        VkPhysicalDeviceVulkan12Features s12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, v13 ? static_cast<void*>(&s13) : nullptr };
        VkPhysicalDeviceScalarBlockLayoutFeatures sSb{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES };
        VkPhysicalDeviceFeatures2 s2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v12 ? static_cast<void*>(&s12) : static_cast<void*>(&sSb) };
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &s2);
        std::string missing;
#define BO3_FEATURE(want, have, name) if (want.name && !have.name) { want.name = VK_FALSE; missing += " " #name; }
        if (v13) { BO3_FEATURE(f13, s13, dynamicRendering) BO3_FEATURE(f13, s13, robustImageAccess) BO3_FEATURE(f13, s13, synchronization2) }
        if (v12)
        {
            BO3_FEATURE(f12, s12, storageBuffer8BitAccess) BO3_FEATURE(f12, s12, shaderInt8) BO3_FEATURE(f12, s12, scalarBlockLayout)
            BO3_FEATURE(f12, s12, descriptorIndexing) BO3_FEATURE(f12, s12, runtimeDescriptorArray)
            BO3_FEATURE(f12, s12, shaderSampledImageArrayNonUniformIndexing) BO3_FEATURE(f12, s12, samplerMirrorClampToEdge)
        }
        else BO3_FEATURE(fSb, sSb, scalarBlockLayout)
        BO3_FEATURE(f2.features, s2.features, geometryShader) BO3_FEATURE(f2.features, s2.features, robustBufferAccess)
        BO3_FEATURE(f2.features, s2.features, independentBlend) BO3_FEATURE(f2.features, s2.features, fillModeNonSolid)
        BO3_FEATURE(f2.features, s2.features, depthClamp) BO3_FEATURE(f2.features, s2.features, samplerAnisotropy)
        BO3_FEATURE(f2.features, s2.features, textureCompressionBC) BO3_FEATURE(f2.features, s2.features, vertexPipelineStoresAndAtomics)
        BO3_FEATURE(f2.features, s2.features, fragmentStoresAndAtomics) BO3_FEATURE(f2.features, s2.features, shaderInt16)
#undef BO3_FEATURE
        if (v13 && !f13.dynamicRendering) g_vk.hasDynRendering = false;
        if (v13 && !f13.synchronization2) g_vk.hasSync2 = false;
        if (!missing.empty()) LogWarn("vk: device lacks:%s (continuing without them)", missing.c_str());
        if (getenv("BO3_NO_GS")) f2.features.geometryShader = VK_FALSE;   // (tests the path of GPUs without them)
        g_vk.hasGs = f2.features.geometryShader;
        if (!g_vk.hasGs) LogWarn("vk: no geometry shaders: rectangles and quads are drawn by vertex shader variants");
        g_vk.hasAnisotropy = f2.features.samplerAnisotropy;
        g_vk.hasBC = f2.features.textureCompressionBC;
        if (!g_vk.hasBC) LogWarn("vk: no BC (DXT) textures on this GPU: decoded on the CPU (slower loading)");
        if (!g_vk.hasAnisotropy) g_vk.maxAnisotropy = 1.0f;   // samplers then never request it
    }
    int spvMinor = v13 ? 3 : v12 ? 2 : 1;
    if (const char* e = getenv("BO3_SPIRV_VK")) spvMinor = std::clamp(atoi(e), 1, spvMinor);   // (tests the SPIR-V of older drivers)
    ShaderCompilerSetVulkanVersion(spvMinor);
    ShaderTranslatorSetDevice(spvMinor, g_vk.hasFloatControls);

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2 };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK(vkCreateDevice(g_vk.phys, &dci, nullptr, &g_vk.device));
    volkLoadDevice(g_vk.device);   // device functions called straight into the driver
    // before 1.3: the core names point to the extensions' functions
    if (!vkCmdBeginRendering) vkCmdBeginRendering = vkCmdBeginRenderingKHR;
    if (!vkCmdEndRendering) vkCmdEndRendering = vkCmdEndRenderingKHR;
    if (!vkCmdPipelineBarrier2) vkCmdPipelineBarrier2 = vkCmdPipelineBarrier2KHR;
    if (!vkCmdSetCullMode) vkCmdSetCullMode = vkCmdSetCullModeEXT;
    if (!vkCmdSetFrontFace) vkCmdSetFrontFace = vkCmdSetFrontFaceEXT;
    if (!vkCmdSetPrimitiveTopology) vkCmdSetPrimitiveTopology = vkCmdSetPrimitiveTopologyEXT;
    if (!vkCmdSetDepthTestEnable) vkCmdSetDepthTestEnable = vkCmdSetDepthTestEnableEXT;
    if (!vkCmdSetDepthWriteEnable) vkCmdSetDepthWriteEnable = vkCmdSetDepthWriteEnableEXT;
    if (!vkCmdSetDepthCompareOp) vkCmdSetDepthCompareOp = vkCmdSetDepthCompareOpEXT;
    if (!vkCmdSetStencilTestEnable) vkCmdSetStencilTestEnable = vkCmdSetStencilTestEnableEXT;
    if (!vkCmdSetStencilOp) vkCmdSetStencilOp = vkCmdSetStencilOpEXT;
    if (g_vk.hasDynRendering && !vkCmdBeginRendering) { g_vk.hasDynRendering = false; LogWarn("vk: dynamic rendering functions missing: render passes"); }
    if (g_vk.hasSync2 && !vkCmdPipelineBarrier2) g_vk.hasSync2 = false;
    if (g_vk.hasEds && !vkCmdSetCullMode) { g_vk.hasEds = false; LogWarn("vk: extended dynamic state functions missing: static pipeline state"); }
    vkGetDeviceQueue(g_vk.device, g_vk.queueFamily, 0, &g_vk.queue);
    if (g_vk.hasVrs)
    {
        g_vk.cmdSetFragmentShadingRate = (PFN_vkCmdSetFragmentShadingRateKHR)vkGetDeviceProcAddr(g_vk.device, "vkCmdSetFragmentShadingRateKHR");
        g_vk.hasVrs = g_vk.cmdSetFragmentShadingRate != nullptr;
        Log("vk: fragment shading rate %s", g_vk.hasVrs ? "enabled" : "unavailable");
    }
    // texture formats the renderer samples that this GPU cannot filter / sample (diagnostics in bo3.log)
    {
        static const std::pair<VkFormat, const char*> kFormats[] = {
            { VK_FORMAT_R8_UNORM, "R8" }, { VK_FORMAT_R8G8_UNORM, "RG8" }, { VK_FORMAT_R8G8B8A8_UNORM, "RGBA8" },
            { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "RGB10A2" }, { VK_FORMAT_R16_UNORM, "R16" }, { VK_FORMAT_R16G16_UNORM, "RG16" },
            { VK_FORMAT_R16G16B16A16_UNORM, "RGBA16" }, { VK_FORMAT_R16_SFLOAT, "R16F" }, { VK_FORMAT_R16G16_SFLOAT, "RG16F" },
            { VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F" }, { VK_FORMAT_R32_SFLOAT, "R32F" }, { VK_FORMAT_R32G32_SFLOAT, "RG32F" },
            { VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32F" }, { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1" }, { VK_FORMAT_BC2_UNORM_BLOCK, "BC2" },
            { VK_FORMAT_BC3_UNORM_BLOCK, "BC3" }, { VK_FORMAT_BC4_UNORM_BLOCK, "BC4" }, { VK_FORMAT_BC5_UNORM_BLOCK, "BC5" },
            { VK_FORMAT_D24_UNORM_S8_UINT, "D24S8" }, { VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8" } };
        std::string noFilter, noSample;
        for (auto& [f, name] : kFormats)
        {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { noSample += ' '; noSample += name; }
            else if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) { noFilter += ' '; noFilter += name; }
        }
        Log("vk: formats without linear filtering:%s; not sampleable:%s", noFilter.empty() ? " none" : noFilter.c_str(),
            noSample.empty() ? " none" : noSample.c_str());
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
    VkCmdBarrier2(cmd, &di);
}

// synchronization2 barriers as classic ones (drivers without it: the flags below bit 32 are the same)
static VkPipelineStageFlags LegacyStages(VkPipelineStageFlags2 s, bool src)
{
    if (s >> 32) return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags f = VkPipelineStageFlags(s);
    return f ? f : src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
}
static VkAccessFlags LegacyAccess(VkAccessFlags2 a)
{
    VkAccessFlags f = VkAccessFlags(a);
    if (a & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) f |= VK_ACCESS_SHADER_READ_BIT;
    if (a & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) f |= VK_ACCESS_SHADER_WRITE_BIT;
    return f;
}
void VkCmdBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* di)
{
    if (g_vk.hasSync2) { vkCmdPipelineBarrier2(cmd, di); return; }
    VkPipelineStageFlags src = 0, dst = 0;
    std::vector<VkMemoryBarrier> mb(di->memoryBarrierCount);
    std::vector<VkImageMemoryBarrier> ib(di->imageMemoryBarrierCount);
    std::vector<VkBufferMemoryBarrier> bb(di->bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < di->memoryBarrierCount; i++)
    {
        auto& b = di->pMemoryBarriers[i];
        mb[i] = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, LegacyAccess(b.srcAccessMask), LegacyAccess(b.dstAccessMask) };
        src |= LegacyStages(b.srcStageMask, true); dst |= LegacyStages(b.dstStageMask, false);
    }
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++)
    {
        auto& b = di->pImageMemoryBarriers[i];
        ib[i] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, LegacyAccess(b.srcAccessMask), LegacyAccess(b.dstAccessMask), b.oldLayout, b.newLayout,
                  b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, b.image, b.subresourceRange };
        src |= LegacyStages(b.srcStageMask, true); dst |= LegacyStages(b.dstStageMask, false);
    }
    for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; i++)
    {
        auto& b = di->pBufferMemoryBarriers[i];
        bb[i] = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, LegacyAccess(b.srcAccessMask), LegacyAccess(b.dstAccessMask),
                  b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, b.buffer, b.offset, b.size };
        src |= LegacyStages(b.srcStageMask, true); dst |= LegacyStages(b.dstStageMask, false);
    }
    vkCmdPipelineBarrier(cmd, src ? src : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst ? dst : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         uint32_t(mb.size()), mb.data(), uint32_t(bb.size()), bb.data(), uint32_t(ib.size()), ib.data());
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
