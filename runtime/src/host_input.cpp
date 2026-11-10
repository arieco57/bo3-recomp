// Host input (main thread): SDL keyboard + mouse + gamepad -> XInput pad 0.
//
// Keyboard/mouse (PC-style layout on top of BO3's default controller bindings):
//   WASD move (left stick), mouse look (right stick; relative mouse mode, captured on click)
//   LMB fire (RT), RMB aim (LT), Space jump (A), C/LCtrl crouch-slide (B), LShift sprint (L3),
//   V / mouse side button melee (R3), R/F reload-use (X), 1/2/wheel switch weapon (Y),
//   G lethal (RB), Q tactical (LB), X specialist (LB+RB), 3-6 and arrows D-pad,
//   Esc pause menu (START), Tab scoreboard (BACK), Enter accept (A), Backspace back (B).
//   F10 releases/captures the mouse. (F1 video options, F11 / Alt+Enter fullscreen: video_options.cpp.)
// BO3_MOUSE_SENS (default 1.0) scales mouse look. BO3_AUTOPRESS="5:START,8:A" presses
// buttons at the given seconds (testing).
#include "host_input.h"
#include "platform.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "profiler.h"
#include "video_options.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
SDL_Gamepad* s_pad = nullptr;
struct AutoPress { double t; uint16_t buttons; bool done; };
std::vector<AutoPress> s_auto;
auto s_start = std::chrono::steady_clock::now();

// mouse look state
SDL_Window* s_window = nullptr;
bool s_captured = false;
float s_accX = 0, s_accY = 0;        // mouse motion since the last update (pixels)
float s_velX = 0, s_velY = 0;        // smoothed motion rate (pixels / second)
auto s_lastUpdate = std::chrono::steady_clock::now();
double s_wheelUntil = 0;             // Y pulse from the mouse wheel
float s_sens = 1.0f;
float s_yawRatio = 2.0f;   // horizontal sensitivity relative to vertical (mouse_ratio)
// direct mouse look (see HostInputTakeMouseAngles)
std::mutex s_lookLock;
float s_lookYaw = 0, s_lookPitch = 0;            // pending degrees (direct path, applied by the game hook)
float s_mouseYaw = 0;                             // mouse yaw degrees not yet handed to the stick controller
std::atomic<double> s_lastLookTake{ -10.0 };      // last time the game hook consumed them
bool DirectLookActive();

uint16_t ButtonByName(const std::string& n)
{
    static const struct { const char* n; uint16_t b; } map[] = {
        { "START", PAD_START }, { "BACK", PAD_BACK }, { "A", PAD_A }, { "B", PAD_B }, { "X", PAD_X }, { "Y", PAD_Y },
        { "UP", PAD_DPAD_UP }, { "DOWN", PAD_DPAD_DOWN }, { "LEFT", PAD_DPAD_LEFT }, { "RIGHT", PAD_DPAD_RIGHT },
        { "LB", PAD_LB }, { "RB", PAD_RB } };
    for (auto& m : map) if (n == m.n) return m.b;
    return 0;
}

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count(); }

// on-screen controller (Android VirtualPad.java): merged with the physical gamepad
std::atomic<uint32_t> s_vpButtons{ 0 };
std::atomic<int32_t> s_vpAxes[6] = {};   // lx ly rx ry (int16 range), lt rt (0..255)

void SetCapture(bool on)
{
    if (!s_window || on == s_captured) return;
    s_captured = on;
    SDL_SetWindowRelativeMouseMode(s_window, on);
    s_accX = s_accY = s_velX = s_velY = 0;
}

// Mouse rate -> stick deflection. The game has a stick dead zone and its own acceleration,
// so small motions start just above the dead zone and the response is linear after it.
int16_t StickFromRate(float rate)
{
    const float kDeadZone = 0.24f;
    float v = rate * s_sens / 2500.0f;   // pixels/s for full deflection at sensitivity 1
    if (std::fabs(v) < 0.002f) return 0;
    float mag = std::min(1.0f, kDeadZone + (1.0f - kDeadZone) * std::min(1.0f, std::fabs(v)));
    return int16_t(std::copysign(mag, v) * 32767.0f);
}
}

// BO3_ANGLE_SCAN=<seconds>: diagnostics to locate the view angles in guest memory. Snapshots
// floats, turns right with the stick, snapshots, turns left, snapshots, and logs the floats
// that moved one way and then back.
struct AngleCand { uint32_t addr; float v; };
std::vector<AngleCand> s_cands;
float ReadF(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); v = __builtin_bswap32(v); float f; memcpy(&f, &v, 4); return f; }
// BO3_ACCUM_SCAN=<t1>,<t2>,<cmd yaw addr>: floats equal (mod 360) to the command yaw angle at
// t1 and again at t2 (with stick turns before each, BO3_STICK_TEST2) - the yaw accumulator.
void AccumScan(double t)
{
    static const char* e = getenv("BO3_ACCUM_SCAN");
    if (!e) return;
    static double t1 = -1, t2 = -1;
    static uint32_t ref = 0;
    static int phase = 0;
    static float a0 = 0;
    static std::vector<std::pair<uint32_t, float>> cands;
    if (t1 < 0) sscanf(e, "%lf,%lf,%x", &t1, &t2, &ref);
    if (phase >= 2 || t < (phase == 0 ? t1 : t2)) return;
    float a = float(GuestRead32(ref) & 0xFFFF) * (360.0f / 65536.0f);
    if (phase == 0)
    {
        a0 = a;
        static const uint32_t ranges[][2] = { { 0x40000000, 0x50000000 }, { 0x82000000, 0x84A00000 } };
        for (auto& r : ranges)
            for (uint32_t ad = r[0]; ad < r[1]; ad += 4)
            {
                float f = ReadF(ad);
                if (std::isfinite(f) && f != 0.0f && std::fabs(f) <= 100000.0f) cands.push_back({ ad, f });
            }
        Log("accumscan: phase 0 angle %.3f: %zu candidates", a, cands.size());
    }
    else
    {
        float want = a - a0;
        std::vector<std::pair<uint32_t, float>> keep;
        for (auto& [ad, f0] : cands)
        {
            float f1 = ReadF(ad);
            if (!std::isfinite(f1) || std::fabs(f1) > 100000.0f || std::fabs(f1 - f0) < 1.0f) continue;
            // the same change, in degrees (mod 360), either sign
            float d1 = std::remainder((f1 - f0) - want, 360.0f), d2 = std::remainder((f1 - f0) + want, 360.0f);
            if (std::fabs(d1) < 0.05f || std::fabs(d2) < 0.05f) keep.push_back({ ad, f1 });
        }
        Log("accumscan: phase 1 angle %.3f (change %.3f): %zu candidates", a, want, keep.size());
        for (size_t i = 0; i < keep.size() && i < 50; i++)
            Log("accumscan: 0x%08X = %.3f (was %.3f)", keep[i].first, keep[i].second,
                std::find_if(cands.begin(), cands.end(), [&](auto& c) { return c.first == keep[i].first; })->second);
    }
    phase++;
}

// BO3_CMD_FIND=<seconds>,<addr>: other copies of the 16 bytes at <addr> (a user command: its
// server time makes it unique) (diagnostics)
void CmdFind(double t)
{
    static const char* e = getenv("BO3_CMD_FIND");
    if (!e || !strchr(e, ',')) return;
    static double at = atof(e);
    static uint32_t ref = uint32_t(strtoul(strchr(e, ',') + 1, nullptr, 16));
    static bool done = false;
    if (done || t < at) return;
    done = true;
    uint8_t key[16];
    memcpy(key, g_base + ref, 16);
    std::string hex;
    for (int i = 0; i < 16; i++) { char b[4]; snprintf(b, sizeof(b), "%02X", key[i]); hex += b; }
    Log("cmdfind: key %s", hex.c_str());
    static const uint32_t ranges[][2] = { { 0x40000000, 0x50000000 }, { 0x82000000, 0x84A00000 }, { 0xA0000000, 0xC0000000 } };
    for (auto& r : ranges)
        for (uint32_t a = r[0]; a < r[1]; a += 4)
            if (!memcmp(g_base + a, key, 16))
            {
                Log("cmdfind: copy at 0x%08X", a);
                // BO3_CMD_WATCH=<index>: watch the yaw (+20) of that copy (heap copies only)
                static int idx = 0;
                static const int want = getenv("BO3_CMD_WATCH") ? atoi(getenv("BO3_CMD_WATCH")) : -1;
                static const bool stackOnly = getenv("BO3_CMD_WATCH_STACK") != nullptr;
                bool ok = stackOnly ? (a >= 0x40000000 && a < 0x40800000) : a >= 0xA0000000;
                if (ok && idx++ == want) ProfilerWatchAddress(a + 20, 0.0);
            }
}

// BO3_VALUE_TRACK2=<t0>,<t1>,<addr>: dwords equal to the value at <addr> at t0 that equal its
// (changed) value again at t1 - copies of a value that is stable at both moments (diagnostics).
void ValueTrack2(double t)
{
    static const char* e = getenv("BO3_VALUE_TRACK2");
    if (!e) return;
    static double t0 = -1, t1 = -1;
    static uint32_t ref = 0;
    static int phase = 0;
    static std::vector<uint32_t> cands;
    if (t0 < 0) { sscanf(e, "%lf,%lf,%x", &t0, &t1, &ref); }
    uint32_t v;
    memcpy(&v, g_base + ref, 4);
    if (phase == 0 && t >= t0)
    {
        static const uint32_t ranges[][2] = { { 0x40000000, 0x50000000 }, { 0x82000000, 0x84A00000 }, { 0xA0000000, 0xC0000000 } };
        for (auto& r : ranges)
            for (uint32_t a = r[0]; a < r[1]; a += 4) { uint32_t w; memcpy(&w, g_base + a, 4); if (w == v) cands.push_back(a); }
        Log("valuetrack2: value 0x%08X at t0: %zu candidates", __builtin_bswap32(v), cands.size());
        phase = 1;
    }
    else if (phase == 1 && t >= t1)
    {
        std::vector<uint32_t> keep;
        for (uint32_t a : cands) { uint32_t w; memcpy(&w, g_base + a, 4); if (w == v) keep.push_back(a); }
        Log("valuetrack2: value 0x%08X at t1: %zu candidates", __builtin_bswap32(v), keep.size());
        for (size_t i = 0; i < keep.size() && i < 60; i++) Log("valuetrack2: 0x%08X", keep[i]);
        phase = 2;
    }
}

// Field of view: the cg_fov dvar (name hash 0x31DCBADD, current value at +8, default at +0x28,
// range 1..160) is read straight from the dvar by the camera, so the chosen FOV is written into
// it. Found once by scanning the executable's dvar pool (MP and ZM differ).
void ApplyFov(double t)
{
    static const bool on = getenv("BO3_FOV_EXPERIMENT") != nullptr;   // writing cg_fov had no visible effect yet
    if (!on) return;
    static uint32_t dvar = 0;
    static double nextScan = 3.0;
    if (!dvar)
    {
        if (t < nextScan) return;
        nextScan = t + 5.0;
        for (uint32_t a = 0x83000000; a < 0x84A00000; a += 4)
            if (GuestRead32(a) == 0x31DCBADD && GuestRead32(a + 4) == 0x200 && GuestRead32(a + 8) != 0)
            {
                dvar = a;
                Log("options: cg_fov dvar at 0x%08X (%.1f)", a, ReadF(a + 8));
                break;
            }
        if (!dvar) return;
    }
    int fov = VideoOptionsFov();
    // experiment: BO3_FOV_SCALE_DVAR=1 -> the neighbouring dvar (hash 0x3E7A499D, 1.0) as a scale
    static const bool scaleDvar = getenv("BO3_FOV_SCALE_DVAR") != nullptr;
    if (scaleDvar && GuestRead32(dvar + 0x50) == 0x3E7A499D)
    {
        float sc = fov > 0 ? float(fov) / 65.0f : 1.0f;
        if (ReadF(dvar + 0x58) != sc) { uint32_t v; memcpy(&v, &sc, 4); GuestWrite32(dvar + 0x58, v); GuestWrite32(dvar + 0x68, v); }
        return;
    }
    float want = fov > 0 ? float(fov) : ReadF(dvar + 0x28);
    if (ReadF(dvar + 8) != want)
    {
        uint32_t v;
        memcpy(&v, &want, 4);
        GuestWrite32(dvar + 8, v);
        GuestWrite32(dvar + 0x18, v);
    }
}

// BO3_VALUE_TRACK=<seconds>,<addr>: finds dwords equal to the value at <addr> three times
// (0.5 s apart) - copies of a changing value (diagnostics).
void ValueTrack(double t)
{
    static const char* e = getenv("BO3_VALUE_TRACK");
    if (!e || !strchr(e, ',')) return;
    static const double start = atof(e);
    static const uint32_t ref = uint32_t(strtoul(strchr(e, ',') + 1, nullptr, 16));
    static int phase = 0;
    static std::vector<uint32_t> cands;
    if (phase > 3 || t < start + phase * 0.5) return;
    uint32_t v;
    memcpy(&v, g_base + ref, 4);
    if (phase == 0)
    {
        static const uint32_t ranges[][2] = { { 0x40000000, 0x50000000 }, { 0x82000000, 0x84A00000 }, { 0xA0000000, 0xC0000000 } };
        for (auto& r : ranges)
            for (uint32_t a = r[0]; a < r[1]; a += 4) { uint32_t w; memcpy(&w, g_base + a, 4); if (w == v) cands.push_back(a); }
    }
    else
    {
        std::vector<uint32_t> keep;
        for (uint32_t a : cands) { uint32_t w; memcpy(&w, g_base + a, 4); if (w == v) keep.push_back(a); }
        cands.swap(keep);
    }
    Log("valuetrack: phase %d value 0x%08X: %zu candidates", phase, __builtin_bswap32(v), cands.size());
    if (phase == 3) for (size_t i = 0; i < cands.size() && i < 60; i++) Log("valuetrack: 0x%08X", cands[i]);
    phase++;
}

// BO3_ANGLE_POKE=<seconds>,<addr>[,<addr>...]: adds 30 degrees to the yaw float at each
// address in turn (1.5 s apart) and logs what the other known copies read afterwards.
void AnglePoke(double t)
{
    static const char* e = getenv("BO3_ANGLE_POKE");
    if (!e) return;
    static std::vector<uint32_t> addrs;
    static double start = -1;
    static size_t next = 0;
    static double pokedAt = -1;
    if (start < 0)
    {
        start = atof(e);
        for (const char* p = strchr(e, ','); p; p = strchr(p + 1, ',')) addrs.push_back(uint32_t(strtoul(p + 1, nullptr, 16)));
    }
    static const uint32_t watch[] = { 0x83688188, 0x837B2A20, 0x837B2B20, 0x845718B0, 0x84574730, 0x8457771C, 0x8414F43C };
    if (pokedAt >= 0 && t - pokedAt > 0.5)
    {
        std::string line;
        for (uint32_t w : watch) { char b[48]; snprintf(b, sizeof(b), " %08X=%.2f", w, ReadF(w)); line += b; }
        Log("anglepoke: after poking %08X:%s cmdyaw %u", addrs[next - 1], line.c_str(), GuestRead32(0x84521C18) & 0xFFFF);
        pokedAt = -1;
    }
    if (next < addrs.size() && t >= start + next * 1.5)
    {
        std::string line;
        for (uint32_t w : watch) { char b[48]; snprintf(b, sizeof(b), " %08X=%.2f", w, ReadF(w)); line += b; }
        Log("anglepoke: before %08X:%s cmdyaw %u", addrs[next], line.c_str(), GuestRead32(0x84521C18) & 0xFFFF);
        uint32_t a = addrs[next];
        float f = ReadF(a) + 30.0f;
        uint32_t v;
        memcpy(&v, &f, 4);
        v = __builtin_bswap32(v);
        memcpy(g_base + a, &v, 4);
        pokedAt = t;
        next++;
    }
}

// returns stick override: x in low 16 bits, y in high 16 bits (packed), 0 = none
uint32_t AngleScan(double t)
{
    static const double start = getenv("BO3_ANGLE_SCAN") ? atof(getenv("BO3_ANGLE_SCAN")) : -1;
    static int phase = 0;
    static double t0 = 0;
    static std::vector<float> ref, refPitch;
    if (start < 0 || t < start || phase >= 10) return 0;
    static const bool imageOnly = getenv("BO3_ANGLE_SCAN_IMAGE") != nullptr;
    static const uint32_t allRanges[][2] = { { 0x40000000, 0x50000000 }, { 0x82000000, 0x84A00000 }, { 0xA0000000, 0xC0000000 } };
    static const uint32_t imgRanges[][2] = { { 0x83000000, 0x84A00000 } };
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (imageOnly) for (auto& r : imgRanges) ranges.push_back({ r[0], r[1] });
    else for (auto& r : allRanges) ranges.push_back({ r[0], r[1] });
    auto pack = [](int16_t x, int16_t y) { return uint32_t(uint16_t(x)) | (uint32_t(uint16_t(y)) << 16); };
    double d = t - t0;
    auto ok = [](float f) { return std::isfinite(f) && std::fabs(f) <= 720.0f; };
    switch (phase)
    {
    case 0:   // snapshot A
        for (auto& r : ranges)
            for (uint32_t a = r.first; a < r.second; a += 4)
            {
                float f = ReadF(a);
                if (f != 0.0f && ok(f)) s_cands.push_back({ a, f });
            }
        Log("anglescan: %zu candidates", s_cands.size());
        phase = 1; t0 = Now();
        return 0;
    case 1:   // no input for 0.6 s: drop everything that moves by itself
    {
        if (d < 0.6) return 0;
        std::vector<AngleCand> keep;
        for (auto& c : s_cands) if (ReadF(c.addr) == c.v) keep.push_back(c);
        s_cands.swap(keep);
        Log("anglescan: %zu stable", s_cands.size());
        phase = 2; t0 = Now();
        return 0;
    }
    case 2:   // turn right
    {
        if (d < 1.6) return d > 0.1 && d < 1.1 ? pack(32767, 0) : 0;   // turn, then let it settle
        std::vector<AngleCand> keep;
        for (auto& c : s_cands) { float f = ReadF(c.addr); if (ok(f) && std::fabs(f - c.v) > 10.0f) keep.push_back({ c.addr, f }); }
        s_cands.swap(keep);
        refPitch.clear();
        for (auto& c : s_cands) refPitch.push_back(ReadF(c.addr - 4));
        Log("anglescan: %zu moved with yaw", s_cands.size());
        phase = 3; t0 = Now();
        return 0;
    }
    case 3:   // look up: yaw must stay, the float before it (pitch) must move
    {
        if (d < 1.1) return d > 0.1 && d < 0.6 ? pack(0, 32767) : 0;
        std::vector<AngleCand> keep;
        for (size_t i = 0; i < s_cands.size(); i++)
        {
            float y = ReadF(s_cands[i].addr), p = ReadF(s_cands[i].addr - 4);
            if (std::fabs(y - s_cands[i].v) < 0.5f && ok(p) && std::fabs(p - refPitch[i]) > 3.0f) keep.push_back(s_cands[i]);
        }
        s_cands.swap(keep);
        Log("anglescan: %zu with pitch before", s_cands.size());
        phase = 4; t0 = Now();
        return 0;
    }
    case 4:   // turn left: yaw comes back
    {
        if (d < 1.6) return d > 0.1 && d < 1.1 ? pack(-32767, 0) : 0;
        for (auto& c : s_cands)
            Log("anglescan: 0x%08X yaw %.3f -> %.3f, pitch %.3f, roll %.3f", c.addr, c.v, ReadF(c.addr), ReadF(c.addr - 4), ReadF(c.addr + 4));
        Log("anglescan: done");
        phase = 10;
    }
    }
    return 0;
}

void HostInputInit()
{
    if (const char* a = getenv("BO3_AUTOPRESS"))
    {
        std::string s = a;
        size_t p = 0;
        while (p < s.size())
        {
            size_t e = s.find(',', p);
            if (e == std::string::npos) e = s.size();
            std::string item = s.substr(p, e - p);
            size_t c = item.find(':');
            if (c != std::string::npos) s_auto.push_back({ atof(item.substr(0, c).c_str()), ButtonByName(item.substr(c + 1)), false });
            p = e + 1;
        }
    }
    if (const char* s = getenv("BO3_MOUSE_SENS")) s_sens = std::max(0.05f, float(atof(s)));
}

void HostInputSetWindow(SDL_Window* w) { s_window = w; }
void HostInputReleaseMouse() { SetCapture(false); }
void HostInputSetMouseSens(float sens) { s_sens = std::max(0.05f, sens); }
void HostInputSetMouseRatio(float ratio) { s_yawRatio = std::clamp(ratio, 0.25f, 4.0f); }
void HostInputNeutral() { InputSetPad(HostPadState{}); }

void HostInputEvent(const SDL_Event& e)
{
    // Android build: only a physical gamepad (no keyboard / mouse / touch controls)
    if (PlatformGamepadOnly() && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                  e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL))
        return;
    switch (e.type)
    {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!s_pad)
        {
            s_pad = SDL_OpenGamepad(e.gdevice.which);
            if (s_pad) Log("input: gamepad \"%s\" connected", SDL_GetGamepadName(s_pad));
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (s_pad && e.gdevice.which == SDL_GetGamepadID(s_pad))
        {
            SDL_CloseGamepad(s_pad);
            s_pad = nullptr;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!s_captured) SetCapture(true);   // first click captures the mouse
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (s_captured)
        {
            s_accX += e.motion.xrel; s_accY += e.motion.yrel;
            // direct look: degrees per mouse count (BO3_MOUSE_SENS scales; 0.022 deg/count x 2.5)
            std::lock_guard lock(s_lookLock);
            float k = 0.055f * s_sens;
            if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) k *= 0.55f;   // aiming down sights
            s_lookYaw += e.motion.xrel * k;    // degrees, consumed by the client look hook
            s_lookPitch += e.motion.yrel * k;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e.wheel.y != 0) s_wheelUntil = Now() + 0.12;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.scancode == SDL_SCANCODE_F10) SetCapture(!s_captured);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        SetCapture(false);
        break;
    default:
        break;
    }
}

void HostInputUpdate()
{
    HostPadState st;
    const bool* k = SDL_GetKeyboardState(nullptr);
    auto key = [&](SDL_Scancode sc) { return k[sc]; };
    const bool alt = key(SDL_SCANCODE_LALT) || key(SDL_SCANCODE_RALT);
    double t = Now();

    // buttons
    if (key(SDL_SCANCODE_ESCAPE)) st.buttons |= PAD_START;
    if (key(SDL_SCANCODE_TAB)) st.buttons |= PAD_BACK;
    if (key(SDL_SCANCODE_SPACE) || (key(SDL_SCANCODE_RETURN) && !alt)) st.buttons |= PAD_A;
    if (key(SDL_SCANCODE_C) || key(SDL_SCANCODE_LCTRL) || key(SDL_SCANCODE_BACKSPACE)) st.buttons |= PAD_B;
    if (key(SDL_SCANCODE_R) || key(SDL_SCANCODE_F)) st.buttons |= PAD_X;
    if (key(SDL_SCANCODE_1) || key(SDL_SCANCODE_2) || t < s_wheelUntil) st.buttons |= PAD_Y;
    if (key(SDL_SCANCODE_Q) || key(SDL_SCANCODE_X)) st.buttons |= PAD_LB;
    if (key(SDL_SCANCODE_G) || key(SDL_SCANCODE_X)) st.buttons |= PAD_RB;
    if (key(SDL_SCANCODE_LSHIFT)) st.buttons |= PAD_LTHUMB;
    if (key(SDL_SCANCODE_V)) st.buttons |= PAD_RTHUMB;
    if (key(SDL_SCANCODE_UP) || key(SDL_SCANCODE_3)) st.buttons |= PAD_DPAD_UP;
    if (key(SDL_SCANCODE_DOWN) || key(SDL_SCANCODE_4)) st.buttons |= PAD_DPAD_DOWN;
    if (key(SDL_SCANCODE_LEFT) || key(SDL_SCANCODE_5)) st.buttons |= PAD_DPAD_LEFT;
    if (key(SDL_SCANCODE_RIGHT) || key(SDL_SCANCODE_6)) st.buttons |= PAD_DPAD_RIGHT;

    // left stick: WASD (+ arrows-free I/J/K/L as a keyboard right stick for menus)
    st.lx = int16_t((key(SDL_SCANCODE_D) - key(SDL_SCANCODE_A)) * 32767);
    st.ly = int16_t((key(SDL_SCANCODE_W) - key(SDL_SCANCODE_S)) * 32767);
    st.rx = int16_t((key(SDL_SCANCODE_L) - key(SDL_SCANCODE_J)) * 32767);
    st.ry = int16_t((key(SDL_SCANCODE_I) - key(SDL_SCANCODE_K)) * 32767);

    // mouse buttons
    SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
    if (mb & SDL_BUTTON_LMASK) st.rightTrigger = 255;
    if (mb & SDL_BUTTON_RMASK) st.leftTrigger = 255;
    if (mb & (SDL_BUTTON_X1MASK | SDL_BUTTON_X2MASK | SDL_BUTTON_MMASK)) st.buttons |= PAD_RTHUMB;

    // mouse look: motion rate, smoothed over ~25 ms, mapped to the right stick
    auto now = std::chrono::steady_clock::now();
    float dt = std::max(1e-4f, std::chrono::duration<float>(now - s_lastUpdate).count());
    s_lastUpdate = now;
    float a = 1.0f - std::exp(-dt / 0.025f);
    s_velX += (s_accX / dt - s_velX) * a;
    s_velY += (s_accY / dt - s_velY) * a;
    s_accX = s_accY = 0;
    // Mouse yaw through the right stick, like a pad: the yaw delta path updates the predicted
    // view only when the server copies it (~20 Hz steps); the stick goes through the user
    // command and the client prediction. The game's stick curve was measured (BO3_STICK_SWEEP):
    // dead zone 0.22, rate = 276 * x^2.52 deg/s with x = (d - 0.22) / 0.78. The degrees the
    // mouse moved are "owed"; each update the stick is set to pay them back over ~35 ms with the
    // inverse curve, and whatever exceeds the stick's top speed goes through the direct path.
    static const bool stickCtl = getenv("BO3_MOUSE_STICK_CTL") != nullptr;   // experiment: mouse yaw via the stick (has the game's turn ramp)
    if (stickCtl && s_captured && DirectLookActive())
    {
        static float owed = 0;
        static auto last = std::chrono::steady_clock::now();
        float dts = std::max(1e-4f, std::chrono::duration<float>(now - last).count());
        last = now;
        {
            std::lock_guard lock(s_lookLock);
            owed += s_mouseYaw;
            s_mouseYaw = 0;
        }
        constexpr float kDead = 0.22f, kMaxRate = 276.0f, kExp = 2.52f, kTau = 0.035f;
        auto rateOf = [&](float d) { float x = (std::fabs(d) - kDead) / (1.0f - kDead); return x <= 0 ? 0.0f : std::copysign(kMaxRate * std::pow(x, kExp), d); };
        static float lastDefl = 0;
        owed -= rateOf(lastDefl) * dts;   // what the stick turned since the last update
        if (std::fabs(owed) < 0.01f) owed = 0;
        const float cap = kMaxRate * 0.96f;
        float overflow = std::fabs(owed) - cap * 0.12f;   // more than ~120 ms behind: hand over the rest
        if (overflow > 0)
        {
            float o = std::copysign(overflow, owed);
            owed -= o;
            std::lock_guard lock(s_lookLock);
            s_lookYaw += o;
        }
        float rate = std::clamp(owed / kTau, -cap, cap);
        float defl = std::fabs(rate) < 0.05f ? 0.0f : std::copysign(kDead + (1.0f - kDead) * std::pow(std::fabs(rate) / kMaxRate, 1.0f / kExp), rate);
        lastDefl = defl;
        if (defl != 0) st.rx = int16_t(std::clamp(defl, -1.0f, 1.0f) * 32767.0f);
    }
    else if (s_captured)   // menus / no hook: mouse as the right stick (velocity mapping)
    {
        std::lock_guard lock(s_lookLock);
        s_mouseYaw = 0;
    }
    if (s_captured && !DirectLookActive())   // fallback (menus / no hook): mouse as the right stick
    {
        int16_t mx = StickFromRate(s_velX), my = StickFromRate(-s_velY);   // XInput: up is positive
        if (mx) st.rx = mx;
        if (my) st.ry = my;
    }

    if (s_pad)
    {
        static const struct { SDL_GamepadButton b; uint16_t x; } map[] = {
            { SDL_GAMEPAD_BUTTON_SOUTH, PAD_A }, { SDL_GAMEPAD_BUTTON_EAST, PAD_B }, { SDL_GAMEPAD_BUTTON_WEST, PAD_X },
            { SDL_GAMEPAD_BUTTON_NORTH, PAD_Y }, { SDL_GAMEPAD_BUTTON_START, PAD_START }, { SDL_GAMEPAD_BUTTON_BACK, PAD_BACK },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_LB }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_RB },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_LTHUMB }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_RTHUMB },
            { SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DPAD_DOWN },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_DPAD_RIGHT },
            { SDL_GAMEPAD_BUTTON_GUIDE, PAD_GUIDE } };
        for (auto& m : map) if (SDL_GetGamepadButton(s_pad, m.b)) st.buttons |= m.x;
        auto axis = [&](SDL_GamepadAxis ax) { return SDL_GetGamepadAxis(s_pad, ax); };
        auto pick = [](int16_t x, int16_t y) { return std::abs(x) > std::abs(y) ? x : y; };
        st.lx = pick(st.lx, axis(SDL_GAMEPAD_AXIS_LEFTX));
        // XInput: up is positive. Negate in int and clamp: -(-32768) does not fit in int16 (it
        // wrapped to -32768 - full deflection up turned into full down)
        auto neg = [](int16_t v) { return int16_t(std::clamp(-int(v), -32768, 32767)); };
        st.ly = pick(st.ly, neg(axis(SDL_GAMEPAD_AXIS_LEFTY)));
        st.rx = pick(st.rx, axis(SDL_GAMEPAD_AXIS_RIGHTX));
        st.ry = pick(st.ry, neg(axis(SDL_GAMEPAD_AXIS_RIGHTY)));
        st.leftTrigger = std::max<uint8_t>(st.leftTrigger, uint8_t(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7));
        st.rightTrigger = std::max<uint8_t>(st.rightTrigger, uint8_t(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7));
    }

    // on-screen controller
    if (uint32_t vb = s_vpButtons.load(); vb || s_vpAxes[0] || s_vpAxes[1] || s_vpAxes[2] || s_vpAxes[3] || s_vpAxes[4] || s_vpAxes[5])
    {
        auto pick = [](int16_t a, int32_t b) { return std::abs(int(a)) >= std::abs(b) ? a : int16_t(b); };
        st.buttons |= uint16_t(vb);
        st.lx = pick(st.lx, s_vpAxes[0]); st.ly = pick(st.ly, s_vpAxes[1]);
        st.rx = pick(st.rx, s_vpAxes[2]); st.ry = pick(st.ry, s_vpAxes[3]);
        st.leftTrigger = std::max<uint8_t>(st.leftTrigger, uint8_t(s_vpAxes[4].load()));
        st.rightTrigger = std::max<uint8_t>(st.rightTrigger, uint8_t(s_vpAxes[5].load()));
    }

    AnglePoke(t);
    ApplyFov(t);
    // BO3_DUMP_AT=<seconds>,<addr>: dump 0x80 bytes as floats (diagnostics)
    if (static const char* da = getenv("BO3_DUMP_AT"); da)
    {
        static bool done = false;
        if (!done && t >= atof(da))
        {
            done = true;
            uint32_t a = uint32_t(strtoul(strchr(da, ',') + 1, nullptr, 16));
            for (uint32_t o = 0; o < 0x80; o += 4) Log("dump +0x%02X: 0x%08X %f", o, GuestRead32(a + o), ReadF(a + o));
        }
    }
    ValueTrack(t);
    ValueTrack2(t);
    AccumScan(t);
    CmdFind(t);
    // BO3_MOUSE_EVENTS=<seconds>: real SDL relative-motion events at 1 kHz (2 counts right for 1 s,
    // then 2 counts down for 0.5 s), through the normal event path (diagnostics)
    if (static const double me = getenv("BO3_MOUSE_EVENTS") ? atof(getenv("BO3_MOUSE_EVENTS")) : -1; me >= 0 && t >= me)
    {
        static bool started = false;
        if (!started)
        {
            started = true;
            s_captured = true;
            Log("mouseevents: start");
            std::thread([] {
                for (int i = 0; i < 2500; i++)
                {
                    SDL_Event ev{};
                    ev.type = SDL_EVENT_MOUSE_MOTION;
                    static const float scale = getenv("BO3_MOUSE_EVENTS_SCALE") ? float(atof(getenv("BO3_MOUSE_EVENTS_SCALE"))) : 1.0f;
                    ev.motion.xrel = (i < 1000 ? 2.0f : 0.0f) * scale;
                    ev.motion.yrel = (i < 1000 ? 0.0f : i < 2000 ? -4.0f : 2.0f) * scale;   // up past the clamp, then back down
                    SDL_PushEvent(&ev);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }).detach();
        }
    }
    // BO3_MOUSE_TEST=<seconds>: synthetic mouse motion (right for 1 s, then down for 0.5 s)
    if (static const double mt = getenv("BO3_MOUSE_TEST") ? atof(getenv("BO3_MOUSE_TEST")) : -1; mt >= 0 && t >= mt && t < mt + 1.5)
    {
        static double lastT = t;
        float dt = float(t - lastT);
        lastT = t;
        std::lock_guard lock(s_lookLock);
        if (t < mt + 1.0) s_lookYaw += 90.0f * dt; else s_lookPitch += 40.0f * dt;
    }
    // BO3_STICK_SWEEP=<seconds>: right stick held at 0.1 .. 1.0 (1 s per level); logs the yaw rate the
    // game turns at for each level, read from the MP client view yaw (0x845718B0) (diagnostics)
    if (static const double sw = getenv("BO3_STICK_SWEEP") ? atof(getenv("BO3_STICK_SWEEP")) : -1; sw >= 0 && t >= sw && t < sw + 20.0)
    {
        int level = int((t - sw) / 1.0);   // 0..19: 0.05 steps
        double phase = (t - sw) - level;
        float defl = 0.05f * float(level + 1);
        static int lastLevel = -1;
        static float y0 = 0, yPrev = 0, acc = 0;
        static double t0 = 0;
        float y = ReadF(0x845718B0);
        if (level != lastLevel)
        {
            if (lastLevel >= 0) Log("sticksweep: deflection %.2f -> %.1f deg/s", 0.05f * float(lastLevel + 1), acc / float(t - t0));
            lastLevel = level; acc = 0; yPrev = y; t0 = t + 0.3;
        }
        if (phase >= 0.3) acc += std::fabs(std::remainder(y - yPrev, 360.0f));   // skip the first 0.3 s (ramp)
        yPrev = y;
        st.rx = int16_t(defl * 32767.0f);
    }
    // BO3_STICK_SWEEP_PITCH=<seconds>: same for pitch (0x845718AC), alternating up/down per level
    if (static const double sw = getenv("BO3_STICK_SWEEP_PITCH") ? atof(getenv("BO3_STICK_SWEEP_PITCH")) : -1; sw >= 0 && t >= sw && t < sw + 10.0)
    {
        int level = int((t - sw) / 0.5);   // 0..19
        double phase = (t - sw) - level * 0.5;
        float defl = 0.05f * float(level + 1);
        static int lastLevel = -1;
        static float yPrev = 0, acc = 0;
        static double t0 = 0;
        float y = ReadF(0x845718AC);
        if (level != lastLevel)
        {
            if (lastLevel >= 0) Log("sticksweep pitch: deflection %.2f -> %.1f deg/s", 0.05f * float(lastLevel + 1), acc / float(t - t0));
            lastLevel = level; acc = 0; yPrev = y; t0 = t + 0.15;
        }
        if (phase >= 0.15) acc += std::fabs(y - yPrev);
        yPrev = y;
        st.ry = int16_t(defl * 32767.0f * ((level & 1) ? -1.0f : 1.0f));
    }
    // BO3_AUTOMOVE=<start s>,<duration s>: walk forward while turning (performance tests while
    // moving: new areas come into view and stream in); the turn reverses every 6 s.
    if (static const char* am = getenv("BO3_AUTOMOVE"); am)
    {
        static double a = -1, d = 0;
        if (a < 0) sscanf(am, "%lf,%lf", &a, &d);
        if (t >= a && t < a + d)
        {
            st.ly = 32767;
            st.rx = int16_t((int(t - a) / 6) % 2 ? -14000 : 14000);
        }
    }
    // BO3_STICK_TEST=<seconds>: right stick fully right for 3 s (diagnostics)
    if (static const double stt = getenv("BO3_STICK_TEST") ? atof(getenv("BO3_STICK_TEST")) : -1; stt >= 0 && t >= stt && t < stt + 3.0) st.rx = 32767;
    if (static const char* st2 = getenv("BO3_STICK_TEST2"); st2)
    {
        static double a = -1, b = -1;
        if (a < 0) sscanf(st2, "%lf,%lf", &a, &b);
        if ((t >= a && t < a + 0.6) || (b > 0 && t >= b && t < b + 0.4)) st.rx = 32767;
    }
    if (uint32_t scan = AngleScan(t)) { st.rx = int16_t(scan & 0xFFFF); st.ry = int16_t(scan >> 16); }
    // remote testing: press.txt in the files folder = "A,DOWN,A" (pressed 0.6 s apart, then deleted).
    // Desktop: only with BO3_PRESS_FILE=<path>.
#ifdef __ANDROID__
    static const char* pressFile = "press.txt";
#else
    static const char* pressFile = getenv("BO3_PRESS_FILE");
#endif
    if (static double nextCheck = 0; pressFile && t >= nextCheck)
    {
        nextCheck = t + 0.5;
        if (FILE* pf = fopen(pressFile, "r"))
        {
            char buf[256] = {};
            size_t n = fread(buf, 1, sizeof(buf) - 1, pf);
            fclose(pf);
            remove(pressFile);
            buf[n] = 0;
            double at = t + 0.1;
            for (char* tok = strtok(buf, ", \r\n"); tok; tok = strtok(nullptr, ", \r\n"), at += 0.6)
                s_auto.push_back({ at, ButtonByName(tok), false });
        }
    }
    for (auto& ap : s_auto)
        if (t >= ap.t && t < ap.t + 0.25) st.buttons |= ap.buttons;
        else if (t >= ap.t + 0.25 && !ap.done) { ap.done = true; Log("input: autopress %.1fs done", ap.t); }
    InputSetPad(st);
}

namespace
{
bool DirectLookActive()
{
    static const bool off = getenv("BO3_MOUSE_STICK") != nullptr;   // force the old right-stick emulation
    return !off && Now() - s_lastLookTake.load() < 0.25;
}
}

bool HostInputTakeMouseAngles(float& dyaw, float& dpitch)
{
    static const bool off = getenv("BO3_MOUSE_STICK") != nullptr;
    if (off) return false;
    double t = Now();
    std::lock_guard lock(s_lookLock);
    // deltas that piled up while nothing consumed them (menus, loading) are dropped
    bool stale = t - s_lastLookTake.load() > 0.25;
    s_lastLookTake = t;
    dyaw = s_lookYaw;
    dpitch = s_lookPitch;
    s_lookYaw = s_lookPitch = 0;
    return !stale && (dyaw != 0 || dpitch != 0);
}

// Mouse look: the view angles are rebuilt every frame from the user command's angles (16-bit
// units per axis, accumulated by the game's stick input) plus the server delta angles - so
// the mouse goes into the command angles, before that rebuild (cmd + 16 = pitch, + 20 = yaw).
// diagnostics (BO3_MOUSE_TRACE=<seconds>): per-call view angles for 1.5 s after <seconds>
void HostInputMouseLookTrace(uint32_t clientState, uint32_t cmd)
{
    static const double at = getenv("BO3_MOUSE_TRACE") ? atof(getenv("BO3_MOUSE_TRACE")) : -1;
    if (at < 0 || clientState < 0x82000000 || clientState >= 0x86000000) return;
    double t = Now();
    if (t < at || t > at + 3.0) return;
    auto f = [](uint32_t a) { uint32_t v = GuestRead32(a); float x; memcpy(&x, &v, 4); return x; };
    static const char* extra = getenv("BO3_MOUSE_TRACE_ADDRS");   // more yaw copies to print: "addr,addr,..."
    std::string ex;
    for (const char* p = extra; p && *p;)
    {
        uint32_t a = uint32_t(strtoul(p, nullptr, 16));
        char b[32];
        snprintf(b, sizeof(b), " %.3f", f(a));
        ex += b;
        p = strchr(p, ',');
        if (p) p++;
    }
    Log("mousetrace %.4f view %.3f %.3f delta %.3f %.3f cmd %d %d |%s", t, f(clientState + 0x28C), f(clientState + 0x290), f(clientState + 188),
        f(clientState + 192), int32_t(GuestRead32(cmd + 16)), int32_t(GuestRead32(cmd + 20)), ex.c_str());
}

// Mouse look as rates in the client's gamepad look function (sub_822E3E38 / ZM equivalent):
// it stores the frame's view turn rates (deg/s) at client + 12704 (pitch) / 12708 (yaw, negative
// = right), which the client integrates into its view angles and user command - the same path
// as the stick, without its response curve and turn ramp, and through the client prediction
// (instant and smooth). The mouse degrees since the last call are divided by the time since then.
void HostInputApplyLookRates(uint32_t client)
{
    if (!client) return;
    using clk = std::chrono::steady_clock;
    static clk::time_point last = clk::now();
    clk::time_point now = clk::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    float dyaw, dpitch;
    if (!HostInputTakeMouseAngles(dyaw, dpitch)) return;
    if (dt <= 0.0005f || dt > 0.25f) return;   // first call / stall: drop
    static const bool invert = getenv("BO3_MOUSE_INVERT") != nullptr;
    auto rdf = [](uint32_t a) { uint32_t v = GuestRead32(a); float f; memcpy(&f, &v, 4); return f; };
    auto wrf = [](uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); GuestWrite32(a, v); };
    // The game multiplies these rates by its own turn speeds (measured: yaw x111.6, pitch x49.3 -
    // a stored 1 deg/s turned 111.6 deg/s), so divide them out: the mouse then turns exactly
    // `degrees` (sensitivity 1.0 = 0.055 deg per count). BO3_LOOK_GAIN scales both (tuning).
    static const float gain = getenv("BO3_LOOK_GAIN") ? float(atof(getenv("BO3_LOOK_GAIN"))) : 1.0f;
    constexpr float kYawFactor = 111.6f, kPitchFactor = 49.3f;
    wrf(client + 12708, rdf(client + 12708) - gain * s_yawRatio * dyaw / dt / kYawFactor);
    wrf(client + 12704, rdf(client + 12704) + gain * (invert ? -dpitch : dpitch) / dt / kPitchFactor);
}

void HostInputApplyMouseLook(uint32_t clientState, uint32_t cmd)
{
    static const bool legacy = getenv("BO3_MOUSE_LEGACY") != nullptr;   // old path: server-side view angles (stepped rendering)
    if (!legacy) return;
    // The function also runs for another copy (heap); the client state is a global of the
    // executable image - only that one feeds the user commands.
    if (clientState < 0x82000000 || clientState >= 0x86000000 || cmd < 0x10000) return;
    float dyaw, dpitch;
    if (!HostInputTakeMouseAngles(dyaw, dpitch)) return;
    static const bool invert = getenv("BO3_MOUSE_INVERT") != nullptr;
    const float k = 65536.0f / 360.0f;
    auto rdf = [](uint32_t a) { uint32_t v = GuestRead32(a); float f; memcpy(&f, &v, 4); return f; };
    auto wrf = [](uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); GuestWrite32(a, v); };
    // Pitch: view = command pitch (16-bit, unsigned in memory) + delta pitch; the game clamps
    // the view (and then adjusts the delta). Aim at a clamped target view pitch and write the
    // exact command value for it - accumulating into the command broke at the clamp (the view
    // snapped from -80 to +8 and got stuck).
    float viewPitch = rdf(clientState + 0x28C), deltaPitch = rdf(clientState + 188);
    if (std::isfinite(viewPitch) && std::isfinite(deltaPitch) && dpitch != 0)
    {
        float target = std::clamp(viewPitch + (invert ? -dpitch : dpitch), -79.5f, 79.5f);
        int32_t units = int32_t(std::lround(std::remainder(target - deltaPitch, 360.0f) * k)) & 0xFFFF;
        GuestWrite32(cmd + 16, uint32_t(units));
    }
    // Yaw: through the delta yaw (+192, degrees) - the game rebuilds the command yaw from its
    // own accumulator every frame (and folds it into the delta when the stick is released).
    float deltaYaw = rdf(clientState + 192);
    if (std::isfinite(deltaYaw) && dyaw != 0) wrf(clientState + 192, std::remainder(deltaYaw - dyaw, 360.0f));
    static const bool test = getenv("BO3_MOUSE_TEST") != nullptr;
    static int nlog = 0;
    if (test && nlog++ % 10 == 0) Log("mouselook: view pitch %.2f delta pitch %.2f yaw delta %.2f += (%.3f, %.3f)", viewPitch, deltaPitch, deltaYaw, dpitch, -dyaw);
}

#ifdef __ANDROID__
#include <jni.h>
// VirtualPad.nativeUpdate(buttons, lx, ly, rx, ry, lt, rt): axes -1..1 (up positive), triggers 0..1
extern "C" JNIEXPORT void JNICALL Java_com_bo3recomp_VirtualPad_nativeUpdate(JNIEnv*, jclass, jint buttons, jfloat lx, jfloat ly,
                                                                            jfloat rx, jfloat ry, jfloat lt, jfloat rt)
{
    auto ax = [](float v) { return int32_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
    s_vpAxes[0] = ax(lx); s_vpAxes[1] = ax(ly); s_vpAxes[2] = ax(rx); s_vpAxes[3] = ax(ry);
    s_vpAxes[4] = int32_t(std::clamp(lt, 0.0f, 1.0f) * 255.0f);
    s_vpAxes[5] = int32_t(std::clamp(rt, 0.0f, 1.0f) * 255.0f);
    s_vpButtons = uint32_t(buttons) & 0xFFFF;
}
#endif
