// Video options: internal rendering resolution, output (window / screen mode) size and
// fullscreen on/off.
//
// F1 opens the options dialog; the choice is applied right away and saved to bo3recomp.ini
// (next to the executable's working directory), which is read at startup.
//   [video]
//   width=1920
//   height=1080
//   fullscreen=1
//   render_scale=2
// width/height = 0 means "native" (the desktop resolution): the size of the window / screen
// mode. render_scale is the internal resolution the game is drawn at (1 = the console's
// 1280x720, 2 = 2560x1440, 3 = 3840x2160).
#include "video_options.h"
#include "gpu/renderer.h"
#include "host_input.h"
#include "log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
const char* kConfigPath = "bo3recomp.ini";

struct VideoConfig
{
    int width = 1280, height = 720;   // 0 = native
    bool fullscreen = false;
    float renderScale = 1.0f;          // internal resolution: 1 = 1280x720 (console), 1.5 = 1920x1080, 2 = 2560x1440, 3 = 3840x2160
    int fpsOverlay = 0;                // 0 off, 1 FPS, 2 detailed
    int fpsLimit = 60;                 // frame rate limit (the console runs at 60)
    float mouseSens = 1.0f;
    float mouseRatio = 2.0f;           // horizontal / vertical mouse sensitivity
    int fov = 0;                       // field of view (0 = game default)
    bool virtualPad = false;           // on-screen controller (Android)
    std::string gamertag, xuid, peers; // [network] (read by xam.cpp / net.cpp themselves; kept on save)
    std::string supaUrl, supaKey, email, password;   // [online] (cloud.cpp; kept on save)
};
const char* kOverlayNames[] = { "OFF", "FPS", "Detallado" };
VideoConfig s_cfg;
std::atomic<int> s_fovLive{ 0 };

void Load()
{
    FILE* f = fopen(kConfigPath, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        int v;
        if (sscanf(line, "width=%d", &v) == 1) s_cfg.width = std::max(0, v);
        else if (sscanf(line, "height=%d", &v) == 1) s_cfg.height = std::max(0, v);
        else if (sscanf(line, "fullscreen=%d", &v) == 1) s_cfg.fullscreen = v != 0;
        else if (float sv; sscanf(line, "render_scale=%f", &sv) == 1) s_cfg.renderScale = std::clamp(sv, 0.5f, 3.0f);
        else if (sscanf(line, "fps_overlay=%d", &v) == 1) s_cfg.fpsOverlay = std::clamp(v, 0, 2);
        else if (sscanf(line, "fps_limit=%d", &v) == 1) s_cfg.fpsLimit = std::clamp(v, 30, 480);
        else if (sscanf(line, "virtual_pad=%d", &v) == 1) s_cfg.virtualPad = v != 0;
        else if (sscanf(line, "fov=%d", &v) == 1) s_cfg.fov = v == 0 ? 0 : std::clamp(v, 55, 120);
        else if (float fv; sscanf(line, "mouse_sens=%f", &fv) == 1) s_cfg.mouseSens = std::clamp(fv, 0.05f, 10.0f);
        else if (float rv; sscanf(line, "mouse_ratio=%f", &rv) == 1) s_cfg.mouseRatio = std::clamp(rv, 0.25f, 4.0f);
        else
        {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (!l.compare(0, 9, "gamertag=")) s_cfg.gamertag = l.substr(9);
            else if (!l.compare(0, 5, "xuid=")) s_cfg.xuid = l.substr(5);
            else if (!l.compare(0, 6, "peers=")) s_cfg.peers = l.substr(6);
            else if (!l.compare(0, 13, "supabase_url=")) s_cfg.supaUrl = l.substr(13);
            else if (!l.compare(0, 13, "supabase_key=")) s_cfg.supaKey = l.substr(13);
            else if (!l.compare(0, 6, "email=")) s_cfg.email = l.substr(6);
            else if (!l.compare(0, 9, "password=")) s_cfg.password = l.substr(9);
        }
    }
    fclose(f);
    if (s_cfg.width == 0 || s_cfg.height == 0) s_cfg.width = s_cfg.height = 0;
}

// Writes [video] and [input] (ours); every other section of the file ([network], [online]: live,
// account and cloud settings...) is kept exactly as it was.
void Save()
{
    std::string others;
    if (FILE* in = fopen(kConfigPath, "r"))
    {
        char line[1024];
        bool keep = false;
        while (fgets(line, sizeof(line), in))
        {
            std::string l = line;
            size_t p = l.find_first_not_of(" \t");
            if (p != std::string::npos && l[p] == '[')
                keep = l.compare(p, 7, "[video]") != 0 && l.compare(p, 7, "[input]") != 0;
            if (keep) others += l;
        }
        fclose(in);
    }
    FILE* f = fopen(kConfigPath, "w");
    if (!f) { LogWarn("options: can't write %s", kConfigPath); return; }
    fprintf(f, "# BO3 Recomp - video options (F1 in game)\n[video]\nwidth=%d\nheight=%d\nfullscreen=%d\n"
               "# internal rendering resolution: 0.5 = 640x360, 1 = 1280x720 (console), 1.5 = 1920x1080, 2 = 2560x1440, 3 = 3840x2160\nrender_scale=%.4g\n"
               "# performance overlay: 0 off, 1 FPS, 2 detailed (F2 cycles)\nfps_overlay=%d\n"
               "# frame rate limit 30..480 (the game speed does not depend on it)\nfps_limit=%d\n# field of view (0 = game default)\nfov=%d\n[input]\nmouse_sens=%.2f\n# horizontal / vertical mouse sensitivity\nmouse_ratio=%.2f\n"
               "# on-screen controller (phone): 1 on, 0 off\nvirtual_pad=%d\n",
            s_cfg.width, s_cfg.height, s_cfg.fullscreen ? 1 : 0, s_cfg.renderScale, s_cfg.fpsOverlay, s_cfg.fpsLimit, s_cfg.fov, s_cfg.mouseSens, s_cfg.mouseRatio,
            s_cfg.virtualPad ? 1 : 0);
    if (!others.empty()) fputs(others.c_str(), f);
    else if (!s_cfg.gamertag.empty() || !s_cfg.xuid.empty() || !s_cfg.peers.empty())   // (first save of a file without them)
        fprintf(f, "[network]\ngamertag=%s\nxuid=%s\npeers=%s\n", s_cfg.gamertag.c_str(), s_cfg.xuid.c_str(), s_cfg.peers.c_str());
    fclose(f);
}

// Desktop size in pixels.
void DesktopPixels(SDL_Window* w, int& pw, int& ph)
{
    SDL_DisplayID d = SDL_GetDisplayForWindow(w);
    const SDL_DisplayMode* m = d ? SDL_GetDesktopDisplayMode(d) : nullptr;
    if (!m) { pw = 1920; ph = 1080; return; }
    pw = int(std::lround(m->w * m->pixel_density));
    ph = int(std::lround(m->h * m->pixel_density));
}

void TargetPixels(SDL_Window* w, int& pw, int& ph)
{
    if (s_cfg.width && s_cfg.height) { pw = s_cfg.width; ph = s_cfg.height; }
    else DesktopPixels(w, pw, ph);
}

std::string ResName(SDL_Window* w)
{
    int pw, ph;
    TargetPixels(w, pw, ph);
    char b[64];
    snprintf(b, sizeof(b), s_cfg.width ? "%dx%d" : "Nativa (%dx%d)", pw, ph);
    return b;
}

void Apply(SDL_Window* w)
{
#ifdef __ANDROID__
    // always the whole screen (system bars hidden); the resolution option only changes scaling
    SDL_SetWindowFullscreen(w, true);
    return;
#endif
    int pw, ph, dw, dh;
    TargetPixels(w, pw, ph);
    DesktopPixels(w, dw, dh);
    if (s_cfg.fullscreen)
    {
        // native size: borderless desktop fullscreen; otherwise the matching screen mode
        const SDL_DisplayMode* pick = nullptr;
        SDL_DisplayMode** modes = nullptr;
        if (pw != dw || ph != dh)
        {
            int n = 0;
            modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(w), &n);
            for (int i = 0; modes && i < n; i++)
                if (std::lround(modes[i]->w * modes[i]->pixel_density) == pw && std::lround(modes[i]->h * modes[i]->pixel_density) == ph)
                {
                    if (!pick || modes[i]->refresh_rate > pick->refresh_rate) pick = modes[i];
                }
            if (!pick) LogWarn("options: no %dx%d screen mode, using the desktop resolution", pw, ph);
        }
        SDL_SetWindowFullscreenMode(w, pick);
        SDL_SetWindowFullscreen(w, true);
        SDL_free(modes);
        Log("options: fullscreen %dx%d", pick ? pw : dw, pick ? ph : dh);
    }
    else
    {
        SDL_SetWindowFullscreen(w, false);
        // the window size is in points; HiDPI desktops scale it
        float scale = SDL_GetWindowDisplayScale(w);
        if (scale <= 0) scale = 1;
        int ww = std::min(pw, dw), wh = std::min(ph, dh);
        SDL_SetWindowSize(w, int(std::lround(ww / scale)), int(std::lround(wh / scale)));
        SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        Log("options: windowed %dx%d", ww, wh);
    }
    SDL_SyncWindow(w);
}

int Ask(SDL_Window* w, const std::string& title, const std::string& text, const std::vector<std::string>& buttons)
{
    std::vector<SDL_MessageBoxButtonData> bd;
    for (size_t i = 0; i < buttons.size(); i++)
        bd.push_back({ 0, int(i), buttons[i].c_str() });
    bd.back().flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    SDL_MessageBoxData md{};
    md.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    md.window = s_cfg.fullscreen ? nullptr : w;
    md.title = title.c_str();
    md.message = text.c_str();
    md.numbuttons = int(bd.size());
    md.buttons = bd.data();
    int id = -1;
    if (!SDL_ShowMessageBox(&md, &id)) { LogWarn("options: message box failed: %s", SDL_GetError()); return -1; }
    return id;
}

std::string ScaleName(float sc)
{
    char b[64];
    snprintf(b, sizeof(b), "%.2gx (%dx%d)", sc, int(1280 * sc + 0.5f), int(720 * sc + 0.5f));
    return b;
}

void RenderScaleDialog(SDL_Window* w)
{
    static const float kScales[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };
    std::vector<std::string> buttons = { ScaleName(0.5f), ScaleName(1) + " consola", ScaleName(1.5f), ScaleName(2), ScaleName(3), "Volver" };
    int id = Ask(w, "Resolución interna",
                 "Resolución interna actual: " + ScaleName(s_cfg.renderScale) +
                     "\n\nEs la resolución a la que se dibuja el juego (más alta = más nítido, más carga de GPU)."
                     "\nSe aplica en el próximo cuadro.",
                 buttons);
    if (id < 0 || id >= int(std::size(kScales))) return;
    s_cfg.renderScale = kScales[id];
    RendererSetScale(s_cfg.renderScale);
    Log("options: internal resolution %s", ScaleName(s_cfg.renderScale).c_str());
    Save();
}

void ResolutionDialog(SDL_Window* w)
{
    int dw, dh;
    DesktopPixels(w, dw, dh);
    static const int kSizes[][2] = { { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    std::vector<std::pair<int, int>> sizes;
    for (auto& s : kSizes)
        if (s[0] <= dw && s[1] <= dh && !(s[0] == dw && s[1] == dh)) sizes.push_back({ s[0], s[1] });
    std::vector<std::string> buttons;
    for (auto& s : sizes) buttons.push_back(std::to_string(s.first) + "x" + std::to_string(s.second));
    buttons.push_back("Nativa (" + std::to_string(dw) + "x" + std::to_string(dh) + ")");
    buttons.push_back("Volver");
    int id = Ask(w, "Resolución", "Resolución actual: " + ResName(w) + "\n\nElegí la resolución de salida.", buttons);
    if (id < 0 || id == int(buttons.size()) - 1) return;
    if (id < int(sizes.size())) { s_cfg.width = sizes[id].first; s_cfg.height = sizes[id].second; }
    else s_cfg.width = s_cfg.height = 0;
    Apply(w);
    Save();
}
}

#ifdef __ANDROID__
#include <jni.h>
#include <SDL3/SDL_system.h>
// com.bo3recomp.VirtualPad.show(on): shows / hides the on-screen controller
static void ShowVirtualPad(bool on)
{
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    if (!env) return;
    jclass cls = env->FindClass("com/bo3recomp/VirtualPad");
    if (!cls) { env->ExceptionClear(); LogWarn("options: no VirtualPad class"); return; }
    jmethodID mid = env->GetStaticMethodID(cls, "show", "(Z)V");
    if (mid) env->CallStaticVoidMethod(cls, mid, jboolean(on));
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    Log("options: on-screen controller %s", on ? "on" : "off");
}
#endif

void VideoOptionsInit(SDL_Window* w)
{
    Load();
    if (const char* e = getenv("BO3_RENDER_SCALE")) s_cfg.renderScale = std::clamp(float(atof(e)), 0.5f, 3.0f);
    RendererSetScale(s_cfg.renderScale);
    if (const char* e = getenv("BO3_FPS")) s_cfg.fpsOverlay = std::clamp(atoi(e), 0, 2);
    RendererSetOverlay(s_cfg.fpsOverlay);
    if (const char* e = getenv("BO3_FPS_LIMIT")) s_cfg.fpsLimit = std::clamp(atoi(e), 30, 480);
    GpuSetFrameLimit(s_cfg.fpsLimit);
    if (const char* e = getenv("BO3_FOV")) s_cfg.fov = std::clamp(atoi(e), 0, 120);
    s_fovLive = s_cfg.fov;
    if (const char* e = getenv("BO3_MOUSE_SENS")) s_cfg.mouseSens = std::clamp(float(atof(e)), 0.05f, 10.0f);
    HostInputSetMouseSens(s_cfg.mouseSens);
    HostInputSetMouseRatio(s_cfg.mouseRatio);
    if (getenv("BO3_FULLSCREEN")) s_cfg.fullscreen = atoi(getenv("BO3_FULLSCREEN")) != 0 || !*getenv("BO3_FULLSCREEN");
    Apply(w);
#ifdef __ANDROID__
    if (s_cfg.virtualPad) ShowVirtualPad(true);
#endif
}

bool VideoOptionsFullscreen() { return s_cfg.fullscreen; }

void VideoOptionsCycleOverlay()
{
    s_cfg.fpsOverlay = (s_cfg.fpsOverlay + 1) % 3;
    RendererSetOverlay(s_cfg.fpsOverlay);
    Save();
}

namespace
{
void FovDialog(SDL_Window* w)
{
    static const int kFovs[] = { 0, 75, 80, 90, 100, 110 };
    std::vector<std::string> buttons;
    for (int v : kFovs) buttons.push_back(v == 0 ? std::string("juego") : std::to_string(v));
    buttons.push_back("Volver");
    int id = Ask(w, "Campo de visión (FOV)",
                 "FOV actual: " + (s_cfg.fov ? std::to_string(s_cfg.fov) : std::string("juego")) +
                     "\n\nSe aplica al instante (puede tardar un cuadro en notarse).",
                 buttons);
    if (id < 0 || id >= int(std::size(kFovs))) return;
    s_cfg.fov = kFovs[id];
    s_fovLive = s_cfg.fov;
    Save();
}

void FpsLimitDialog(SDL_Window* w)
{
    // Two pages: the system message box does not open with more than ~8 buttons.
    static const int kPage1[] = { 30, 60, 120, 144, 165 };
    static const int kPage2[] = { 240, 360, 480 };
    const std::string text = "Límite actual: " + std::to_string(s_cfg.fpsLimit) +
                             " FPS\n\nLa velocidad del juego no cambia (la simulación usa tiempo real). Por encima de 60 los FPS"
                             "\ndependen también de la CPU: las escenas pesadas pueden no llegar al límite.";
    std::vector<std::string> buttons;
    for (int v : kPage1) buttons.push_back(std::to_string(v) + (v == 60 ? " (consola)" : ""));
    buttons.push_back("Más...");
    buttons.push_back("Volver");
    int id = Ask(w, "Límite de FPS", text, buttons);
    int limit;
    if (id >= 0 && id < int(std::size(kPage1))) limit = kPage1[id];
    else if (id == int(std::size(kPage1)))
    {
        buttons.clear();
        for (int v : kPage2) buttons.push_back(std::to_string(v));
        buttons.push_back("Volver");
        id = Ask(w, "Límite de FPS (alto)", text, buttons);
        if (id < 0 || id >= int(std::size(kPage2))) return;
        limit = kPage2[id];
    }
    else return;
    s_cfg.fpsLimit = limit;
    GpuSetFrameLimit(s_cfg.fpsLimit);
    Save();
}

void MouseDialog(SDL_Window* w)
{
    static const float kSens[] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };
    std::vector<std::string> buttons;
    for (float v : kSens) { char b[16]; snprintf(b, sizeof(b), "%.2g", v); buttons.push_back(b); }
    buttons.push_back("Volver");
    char cur[64];
    snprintf(cur, sizeof(cur), "%.2g", s_cfg.mouseSens);
    int id = Ask(w, "Sensibilidad del mouse",
                 std::string("Sensibilidad actual: ") + cur + "\n\nEl mouse mueve la cámara directamente (1.0 ~ 0.055 grados por punto;"
                 "\napuntando con clic derecho se reduce a la mitad). También: mouse_sens en bo3recomp.ini.",
                 buttons);
    if (id < 0 || id >= int(std::size(kSens))) return;
    s_cfg.mouseSens = kSens[id];
    HostInputSetMouseSens(s_cfg.mouseSens);
    Save();
}
}

void VideoOptionsSetFullscreen(SDL_Window* w, bool on)
{
    s_cfg.fullscreen = on;
    Apply(w);
    Save();
}

void VideoOptionsDialog(SDL_Window* w)
{
    HostInputReleaseMouse();
    for (;;)
    {
        std::string text = "Resolución interna: " + ScaleName(s_cfg.renderScale) + "\nVentana / pantalla: " + ResName(w) +
                           "\nPantalla completa: " + (s_cfg.fullscreen ? "ON" : "OFF") +
                           "\nContador de FPS: " + kOverlayNames[s_cfg.fpsOverlay] +
                           "\nLímite de FPS: " + std::to_string(s_cfg.fpsLimit) +

                           "\n\nLos cambios se guardan en bo3recomp.ini.\nAtajos: F1 opciones, F2 contador de FPS, F11 o Alt+Enter pantalla completa.";
#ifdef __ANDROID__
        // always full screen, gamepad only: just the options that apply on a phone
        text = "Resolución interna: " + ScaleName(s_cfg.renderScale) + "\nContador de FPS: " + kOverlayNames[s_cfg.fpsOverlay] +
               "\nLímite de FPS: " + std::to_string(s_cfg.fpsLimit) + "\n\nLos cambios se guardan en bo3recomp.ini.";
        int a = Ask(w, "Opciones - BO3 Recomp", text,
                    { "Resolución interna...", std::string("Contador de FPS: ") + kOverlayNames[s_cfg.fpsOverlay], "Límite de FPS...",
                      std::string("Mando virtual: ") + (s_cfg.virtualPad ? "ON" : "OFF"), "Cerrar" });
        if (a == 3)
        {
            s_cfg.virtualPad = !s_cfg.virtualPad;
            ShowVirtualPad(s_cfg.virtualPad);
            Save();
            continue;
        }
        static const int kMap[] = { 1, 3, 4, 6, 6 };
        int id = a >= 0 && a < 5 ? kMap[a] : -1;
#else
        int id = Ask(w, "Opciones - BO3 Recomp", text,
                     { std::string("Pantalla completa: ") + (s_cfg.fullscreen ? "ON" : "OFF"), "Resolución interna...", "Tamaño de ventana...",
                       std::string("Contador de FPS: ") + kOverlayNames[s_cfg.fpsOverlay], "Límite de FPS...", "Mouse...", "Cerrar" });
#endif
        if (id == 0) VideoOptionsSetFullscreen(w, !s_cfg.fullscreen);
        else if (id == 1) RenderScaleDialog(w);
        else if (id == 2) ResolutionDialog(w);
        else if (id == 3) VideoOptionsCycleOverlay();
        else if (id == 4) FpsLimitDialog(w);
        else if (id == 5) MouseDialog(w);
        else break;
    }
}

int VideoOptionsFov() { return s_fovLive.load(); }
