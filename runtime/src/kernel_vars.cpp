#include "kernel_vars.h"
#include "guest.h"
#include "loader.h"
#include "ktime.h"

#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>

static std::unordered_map<std::string, uint32_t> s_vars;

static uint32_t Make(const std::string& name, uint32_t size)
{
    uint32_t a = GuestAlloc(size);
    s_vars[name] = a;
    return a;
}

uint32_t KernelVariableAddress(const char* name)
{
    auto it = s_vars.find(name);
    return it == s_vars.end() ? 0 : it->second;
}

uint32_t KernelVariable(const char* lib, const char* cname, uint32_t ordinal)
{
    std::string name = cname;
    if (auto it = s_vars.find(name); it != s_vars.end()) return it->second;

    uint32_t a;
    if (name == "XboxHardwareInfo")
    {
        a = Make(name, 0x10);
        GuestWrite32(a + 0, 0x00000020);    // flags (Xenia uses 0x20: has HDD)
        GuestWrite8(a + 4, 6);              // number of hardware threads
    }
    else if (name == "XboxKrnlVersion" || name == "XboxKrnlBaseVersion")
    {
        a = Make(name, 8);
        GuestWrite16(a + 0, 2); GuestWrite16(a + 2, 0); GuestWrite16(a + 4, 17559); GuestWrite16(a + 6, 0);
    }
    else if (name == "ExConsoleGameRegion")
    {
        a = Make(name, 4);
        GuestWrite32(a, 0xFFFFFFFF);        // all regions
    }
    else if (name == "XexExecutableModuleHandle")
    {
        a = Make(name, 4);
        GuestWrite32(a, g_module.ldrEntryGuest);
    }
    else if (name == "ExLoadedImageName" || name == "ExLoadedCommandLine")
    {
        a = Make(name, 0x100);
        const char* s = name == "ExLoadedImageName" ? "\\Device\\Cdrom0\\default.xex" : "default.xex";
        memcpy(g_base + a, s, strlen(s) + 1);
    }
    else if (name == "KeTimeStampBundle")
    {
        a = Make(name, 0x18);
        KTimeUpdateBundle();
    }
    else if (name == "VdGpuClockInMHz")
    {
        a = Make(name, 4);
        GuestWrite32(a, 500);
    }
    else if (name == "VdHSIOCalibrationLock")
    {
        a = Make(name, 0x1C);               // RTL_CRITICAL_SECTION
        GuestWrite32(a + 0x10, 0xFFFFFFFF); // LockCount = -1
    }
    else if (name == "KeDebugMonitorData" || name == "VdGlobalDevice" || name == "VdGlobalXamDevice" ||
             name.ends_with("ObjectType"))
    {
        a = Make(name, 0x40);               // zeroed / type marker
    }
    else
    {
        a = Make(name, 0x100);
        LogWarn("kernel variable %s!%s (#%u) unknown - zeroed placeholder at 0x%08X", lib, cname, ordinal, a);
        return a;
    }
    Log("kernel variable %s!%s -> 0x%08X", lib, cname, a);
    return a;
}

void KTimeUpdateBundle()
{
    uint32_t a = KernelVariableAddress("KeTimeStampBundle");
    if (!a) return;
    uint64_t now = KTimeSystemTime100ns();
    GuestWrite64(a + 0x00, KTimeInterruptTime100ns()); // interrupt time
    GuestWrite64(a + 0x08, now);                       // system time (FILETIME)
    GuestWrite32(a + 0x10, KTimeTickCountMs());        // tick count (ms)
}

void KernelVarsStartTimeThread()
{
    std::thread([] {
        for (;;)
        {
            KTimeUpdateBundle();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}
