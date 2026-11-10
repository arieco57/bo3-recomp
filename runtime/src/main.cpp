// BO3 Recomp runtime entry point.
//
//   bo3_mp [--game DIR] [--update DIR] [--trace]
//
// DIR defaults are relative to the repository root (the current directory).
#include "guest.h"
#include "setup.h"
#include "kernel_vars.h"
#include "loader.h"
#include "stubs.h"
#include "thread.h"
#include "vfs.h"
#include "gpu/vk.h"
#include "cloud.h"
#include "host_input.h"
#include "video_options.h"
#include "platform.h"
#include "gpu/overlay.h"
#include "profiler.h"
#include "xma.h"
#include "write_watch.h"

#include <SDL3/SDL.h>
#include <unistd.h>
#include <vector>
#include <atomic>
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>   // main() becomes SDL_main, called by the Java activity
#endif
#include <thread>
#include <chrono>
#include <cstring>

#include <csignal>
#include <mutex>
#include <set>
#include <string>
#include <ucontext.h>

#ifndef BO3_EXE_KIND
#define BO3_EXE_KIND "mp"
#endif

void ImportUnimplemented(PPCContext& ctx, const char* name)
{
    static std::mutex m;
    static std::set<const char*> seen;
    bool first;
    {
        std::lock_guard lock(m);
        first = seen.insert(name).second;
    }
    if (first || g_logImports)
        LogWarn("UNIMPLEMENTED %s(0x%X, 0x%X, 0x%X, 0x%X) lr=0x%08X", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
}

// Indirect call (bctrl / bctr) to an address that is not a recompiled function start.
static PPC_FUNC(UnknownIndirectCall)
{
    Fatal("indirect call to 0x%08X, which is not a known function start (lr=0x%08X) - "
          "add it to the config's functions list", ctx.ctr.u32, uint32_t(ctx.lr));
}

static void OnCrash(int sig, siginfo_t* info, void* uctx)
{
    // guest store to an emulated MMIO register page (XMA): handled, execution continues
    if (sig == SIGSEGV && XmaHandleFault(info->si_addr, uctx)) return;
    // store to a write-tracked physical page (texture cache): unprotected, the store is retried
    if (sig == SIGSEGV && WatchHandleFault(info->si_addr)) return;
    uintptr_t fault = (uintptr_t)info->si_addr;
    GuestThread* t = CurrentThread();
    if (fault >= (uintptr_t)g_base && fault < (uintptr_t)g_base + 0x100000000ull)
        fprintf(stderr, "\n*** signal %d: guest memory access at 0x%08X (thread %u)\n", sig,
                uint32_t(fault - (uintptr_t)g_base), t ? t->id : 0);
    else
        fprintf(stderr, "\n*** signal %d at host address %p (thread %u)\n", sig, info->si_addr, t ? t->id : 0);
    if (t && t->ctx)
    {
        // (registers the compiled code keeps in host registers may be stale)
        const PPCContext& c = *t->ctx;
        const uint32_t r[32] = {c.r0.u32, c.r1.u32, c.r2.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32, c.r9.u32, c.r10.u32, c.r11.u32, c.r12.u32, c.r13.u32, c.r14.u32, c.r15.u32, c.r16.u32, c.r17.u32, c.r18.u32, c.r19.u32, c.r20.u32, c.r21.u32, c.r22.u32, c.r23.u32, c.r24.u32, c.r25.u32, c.r26.u32, c.r27.u32, c.r28.u32, c.r29.u32, c.r30.u32, c.r31.u32};
        for (int i = 0; i < 32; i++)
            fprintf(stderr, "%s r%-2d=%08X", i % 8 == 0 ? "\n   " : "", i, r[i]);
        fprintf(stderr, "\n    guest backtrace: %s\n", GuestBacktrace(c).c_str());
    }
    signal(sig, SIG_DFL);
    raise(sig);
}


#ifdef __ANDROID__
#include <csetjmp>
#include <chrono>
static sigjmp_buf s_diagJmp;
static void DiagBus(int) { siglongjmp(s_diagJmp, 1); }
// Guest physical memory as the CPU sees it: read speed vs normal memory and whether atomic
// instructions work on it (SIGBUS on uncached / device memory).
static void MemoryDiagnose()
{
    auto speed = [](const uint8_t* p) {
        auto t0 = std::chrono::steady_clock::now();
        uint64_t s = 0;
        for (size_t i = 0; i < (8u << 20) / 8; i++) s += reinterpret_cast<const volatile uint64_t*>(p)[i];
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return 8.0 / (ms / 1000.0) + double(s & 0);
    };
    std::vector<uint8_t> normal(8u << 20, 1);
    double sp = speed(g_base + 0xA1000000u), sn = speed(normal.data());
    struct sigaction sa{}, old{};
    sa.sa_handler = DiagBus;
    sigaction(SIGBUS, &sa, &old);
    bool atomicOk = false;
    if (sigsetjmp(s_diagJmp, 1) == 0)
    {
        auto* w = reinterpret_cast<std::atomic<uint32_t>*>(g_base + 0xA1000000u);
        uint32_t e = w->load();
        w->compare_exchange_strong(e, e);
        atomicOk = true;
    }
    sigaction(SIGBUS, &old, nullptr);
    Log("memory: guest memory read %.0f MB/s (normal memory %.0f MB/s), atomics %s", sp, sn, atomicOk ? "ok" : "FAULT (SIGBUS)");
}
#endif

char** g_argv = nullptr;   // (title relaunch: xam.cpp)

int main(int argc, char** argv)
{
    g_argv = argv;
#ifdef __ANDROID__
    // game data, saves, shader cache and bo3recomp.ini live in the app's external files folder
    // (/sdcard/Android/data/<package>/files: copy extracted/, content/ there with adb push)
    if (const char* dir = SDL_GetAndroidExternalStoragePath()) chdir(dir);
    // no shell environment on Android: bo3env.txt there holds KEY=VALUE lines (debug / test vars)
    if (FILE* ef = fopen("bo3env.txt", "r"))
    {
        char line[1024];
        while (fgets(line, sizeof(line), ef))
        {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
        }
        fclose(ef);
    }
#endif
    std::string root = ".";
    std::string game = "extracted/game";
    std::string update = "content/0000000000000000/4156091D/000B0000/tu00000001_00000000";
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--trace") g_logImports = true;
        else if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--update" && i + 1 < argc) update = argv[++i];
        else if (a == "--fullscreen") setenv("BO3_FULLSCREEN", "1", 1);
        else if (a == "--windowed") setenv("BO3_FULLSCREEN", "0", 1);
        else { fprintf(stderr, "usage: %s [--game DIR] [--update DIR] [--trace] [--fullscreen|--windowed]\n", argv[0]); return 1; }
    }

    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    Log("BO3 Recomp runtime (%s)", BO3_EXE_KIND);
    const bool windowed = !getenv("BO3_NO_WINDOW");
    if (windowed)
    {
        // Vulkan first: guest physical memory is allocated by the GPU driver (dma-buf) so the
        // CPU can alias it at all physical views while the GPU reads it without copies.
        VkCreateWindow(BO3_EXE_KIND[0] == 'z' ? "Black Ops III - Zombies (BO3 Recomp)" : "Black Ops III (BO3 Recomp)", 1280, 720);
        VkInit();
        // GPU memory exported as a dma-buf (desktop drivers); otherwise (Android) a memfd at
        // the three physical views, imported into Vulkan as host memory (BO3_NO_DMABUF=1 tests it).
#ifdef __ANDROID__
        bool placed = false;
        if (g_vk.hasPlacedMap)
        {
            MemoryInit(kPhysExternal);
            placed = VkMapGuestMemoryPlaced();
            if (!placed) Fatal("vk: guest memory could not be mapped by the driver");
        }
        if (!placed)
#endif
        {
            bool preferHost = getenv("BO3_NO_DMABUF") != nullptr;
#ifdef __ANDROID__
            // KGSL dma-buf mmaps come out uncached and fault on atomics: import cached CPU memory
            // instead when the driver can (our Turnip build adds VK_EXT_external_memory_host).
            if (g_vk.hasHostImport && !getenv("BO3_DMABUF")) preferHost = true;
#endif
            int fd = preferHost ? -1 : VkExportGuestMemory();
            MemoryInit(fd);
            if (fd < 0) VkImportGuestMemory();
#ifdef __ANDROID__
            MemoryDiagnose();
#endif
        }
    }
    else
        MemoryInit();
    if (getenv("BO3_MEMBENCH"))
    {
        // CPU bandwidth of the physical-memory mapping vs plain anonymous memory
        auto bench = [](uint32_t addr) {
            volatile uint64_t sum = 0;
            memset(g_base + addr, 1, 64 << 20);
            auto t0 = std::chrono::steady_clock::now();
            for (int rep = 0; rep < 4; rep++)
                for (size_t i = 0; i < (64u << 20) / 8; i += 8) sum += reinterpret_cast<uint64_t*>(g_base + addr)[i];
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            Log("membench 0x%08X: %.1f ms for 256 MiB of reads (%.1f GB/s)", addr, ms, 256.0 / 1024 / (ms / 1000));
        };
        bench(0x50000000);
        bench(0xB0000000);
        QuickExit(0);
    }

    XmaInit();

    // Function table used by indirect calls (PPC_LOOKUP_FUNC). Every slot that is not a known
    // function start traps with a useful message instead of jumping to host address 0.
    for (uint64_t a = PPC_CODE_BASE; a < PPC_CODE_BASE + PPC_CODE_SIZE; a += 4)
        PPC_LOOKUP_FUNC(g_base, a) = UnknownIndirectCall;
    size_t n = 0;
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; m++, n++)
        PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;
    Log("function table: %zu functions", n);

    // first run: install the game from the player's own copy (disc image, Title Update, DLC)
    if (!SetupGameData()) return 1;
    CloudStartupSync();   // newer saves from the cloud before the game reads them (bo3recomp.ini [online])
    VfsInit(game, update);
    LoadModule(std::string("private/") + BO3_EXE_KIND + "/default_patched.xex");
    KernelVarsStartTimeThread();

    if (!windowed)
    {
        int code = RunMainThread(g_module.entryPoint);
        Log("main thread exited with 0x%X", code);
        return code;
    }
    // The window and its event loop live on the host main thread (SDL requirement); the game
    // runs on guest threads and the GPU thread renders/presents.
    std::thread([] {
        int code = RunMainThread(g_module.entryPoint);
        Log("main thread exited with 0x%X", code);
        QuickExit(code);
    }).detach();
    ProfilerStartWatch();
    HostInputInit();
    HostInputSetWindow(g_vk.window);
    // Resolution / fullscreen from bo3recomp.ini (F1 options dialog; the renderer letterboxes
    // the 16:9 image into whatever size the window has).
    VideoOptionsInit(g_vk.window);
    for (;;)
    {
        SDL_Event e;
#ifdef __ANDROID__
        // SDL's timed wait polls in a loop on Android (a whole core): poll, then sleep
        SDL_Delay(4);
        while (SDL_PollEvent(&e))
#else
        while (SDL_WaitEventTimeout(&e, 4))
#endif
        {
            if (e.type == SDL_EVENT_WILL_ENTER_BACKGROUND || e.type == SDL_EVENT_TERMINATING) { VkSavePipelineCache(); CloudFlush(); }
            if (e.type == SDL_EVENT_QUIT) { VkSavePipelineCache(); CloudFlush(); Log("window closed"); QuickExit(0); }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                VideoOptionsSetFullscreen(g_vk.window, !VideoOptionsFullscreen());
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F2) VideoOptionsCycleOverlay();
            bool optionsTap = false;
            if (PlatformTouchUi() && (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN))
            {
                // finger coordinates are normalized; mouse ones are window points
                float x, y;
                if (e.type == SDL_EVENT_FINGER_DOWN) { x = e.tfinger.x; y = e.tfinger.y; }
                else
                {
                    int ww = 1, wh = 1;
                    SDL_GetWindowSize(g_vk.window, &ww, &wh);
                    x = e.button.x / float(std::max(ww, 1)); y = e.button.y / float(std::max(wh, 1));
                }
                float r[4];
                OverlayButtonRect(r);
                optionsTap = x >= r[0] && x <= r[2] && y >= r[1] && y <= r[3];
                if (!optionsTap && PlatformGamepadOnly()) continue;   // taps elsewhere do nothing
            }
            if (optionsTap || (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F1))
            {
                HostInputNeutral();   // the game keeps running while the dialog is open
                VideoOptionsDialog(g_vk.window);
                g_vk.swapchainDirty = true;
                continue;
            }
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) g_vk.swapchainDirty = true;
            HostInputEvent(e);
        }
        HostInputUpdate();
    }
}
