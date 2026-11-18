// Linux first-run installer window: the AppImage's "Instalación" screen, the same look and steps as the
// Android one (InstallerActivity). The player picks the disc image, the Title Update and the
// Compatibility Packs 1 and 2 (files, .zip or folders, with the desktop's file dialogs) and optional
// DLC; "Convertir" installs them (setup.cpp, on a worker thread) with a progress bar.
// Drawn with SDL_Renderer + stb_truetype and a system font (fc-match); no font or renderer = the
// dialog wizard instead (setup.cpp RunWizard).
// Tests: BO3_UI_TEST="disc|tu|cp1|cp2[|dlc...]" picks those, BO3_UI_TEST_CONVERT=1 starts right away,
// BO3_UI_SHOT=<file.bmp> saves the window's own image (BO3_UI_SHOT_FRAME = after that many frames).
#ifndef __ANDROID__
#include "setup.h"
#include "log.h"

#include <SDL3/SDL.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct Rgb { uint8_t r, g, b, a = 255; };
const Rgb BG{ 11, 13, 16 }, CARD{ 24, 27, 32 }, CARD_DONE{ 22, 32, 26 }, CARD_DONE_LINE{ 46, 92, 62 }, LINE{ 44, 49, 57 },
    TEXT{ 236, 238, 241 }, SUB{ 150, 156, 165 }, ACCENT{ 242, 107, 29 }, OK{ 76, 196, 120 }, ERR{ 255, 120, 100 },
    BTN{ 34, 38, 45 }, TRACK{ 40, 45, 53 }, CHIP{ 32, 36, 42 }, WHITE{ 255, 255, 255 };

enum FontId { F_KICK, F_SMALL, F_STATUS, F_BODY, F_CARD, F_BTN, F_TITLE, F_BIG, F_COUNT };
const struct { float size; bool bold; } kSpecs[F_COUNT] = {
    { 12, true }, { 13, false }, { 14, false }, { 15, false }, { 17, true }, { 14, true }, { 28, true }, { 34, true } };

struct Font
{
    stbtt_packedchar latin[224];   // U+0020..U+00FF
    stbtt_packedchar punct[32];    // U+2010..U+202F (dashes, quotes, ellipsis)
    float ascent = 0, lineH = 0;
};

// ------------------------------------------------------------------------------ fonts
std::string Run(const std::string& cmd)
{
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r"))
    {
        char b[1024];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), p)) > 0) out.append(b, n);
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

bool ReadAll(const std::string& p, std::vector<uint8_t>& out)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// a sans-serif TrueType font of the system (regular or bold)
std::string FindFont(bool bold)
{
    std::string p = Run(std::string("fc-match -f '%{file}' 'sans-serif:style=") + (bold ? "Bold" : "Regular") + "' 2>/dev/null");
    std::error_code ec;
    if (!p.empty() && fs::exists(p, ec) && (p.ends_with(".ttf") || p.ends_with(".otf") || p.ends_with(".TTF"))) return p;
    const char* regular[] = { "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf" };
    const char* boldf[] = { "/usr/share/fonts/google-noto/NotoSans-Bold.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf" };
    for (const char* c : bold ? boldf : regular)
        if (fs::exists(c, ec)) return c;
    return bold ? FindFont(false) : std::string();
}

uint32_t NextCp(const std::string& s, size_t& i)
{
    uint8_t c = uint8_t(s[i++]);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i < s.size(); k++) cp = cp << 6 | (uint8_t(s[i++]) & 0x3F);
    return cp;
}

// ------------------------------------------------------------------------------ the window
struct Pick
{
    std::string path, name, err, via;   // via: chosen in another card (a folder with both packs)
    bool folder = false, zip = false, ok = false, installed = false;
};

const char* kTitles[4] = { "Imagen del disco", "Title Update", "Compatibility Pack 1", "Compatibility Pack 2" };
const char* kHints[4] = {
    "Tu disco de Black Ops III para Xbox 360: .iso, .zar o .zip.",
    "El TU 8 (archivo tu00000001_00000000), su .zip o la carpeta extraída.",
    "Paquete gratuito de Xbox Live: archivo, .zip o carpeta. La carpeta 00000002 instala todos los DLC que tenga.",
    "El otro paquete gratuito (o la misma carpeta 00000002)." };
const char* kWrong[4] = { "No es una imagen de disco de Black Ops III (.iso o .zar).", "No es el Title Update de Black Ops III.",
    "No es el Compatibility Pack 1.", "No es el Compatibility Pack 2." };

// file dialog results (SDL may call back from another thread)
std::mutex g_dlgMutex;
std::vector<std::pair<int, std::vector<std::string>>> g_dlgResults;
std::atomic<bool> g_dlgOpen{ false };

void SDLCALL OnDialog(void* user, const char* const* list, int)
{
    int slot = int(reinterpret_cast<intptr_t>(user));
    std::vector<std::string> got;
    if (!list) LogWarn("installer: file dialog failed: %s", SDL_GetError());
    else for (; *list; list++) got.push_back(*list);
    std::lock_guard<std::mutex> l(g_dlgMutex);
    g_dlgResults.push_back({ slot, std::move(got) });
    g_dlgOpen = false;
}

// installation progress (worker thread -> window)
std::mutex g_progMutex;
int g_pct = 0;
std::string g_status, g_message;
void Progress(const char* text, int pct)
{
    std::lock_guard<std::mutex> l(g_progMutex);
    if (pct < 0) g_message = text;
    else { g_pct = pct; g_status = text; }
}
void Status(const std::string& text)
{
    std::lock_guard<std::mutex> l(g_progMutex);
    g_status = text;
}

std::string Quote(const std::string& s)
{
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

// a .zip into dst (unzip, 7z or Python - one of them is on any desktop)
bool Unzip(const std::string& zip, const std::string& dst)
{
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (system("command -v unzip >/dev/null 2>&1") == 0) return system(("unzip -q -o " + Quote(zip) + " -d " + Quote(dst)).c_str()) == 0;
    if (system("command -v 7z >/dev/null 2>&1") == 0) return system(("7z x -y -bd " + Quote("-o" + dst) + " " + Quote(zip) + " >/dev/null").c_str()) == 0;
    return system(("python3 -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' " + Quote(zip) + " " + Quote(dst)).c_str()) == 0;
}

bool IsZip(const std::string& p)
{
    std::string e = fs::path(p).extension().string();
    for (char& c : e) c = char(tolower((unsigned char)c));
    return e == ".zip";
}

class InstallerWindow
{
public:
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* atlas = nullptr;
    Font f[F_COUNT];
    float k = 1;                 // pixels per design unit
    float mx = -1, my = -1;      // mouse, pixels
    bool click = false;
    float cx = 0, cy = 0;        // click position, pixels
    float scroll = 0, contentH = 0, outW = 0, outH = 0;
    bool zombies = false;

    Pick picks[4];
    std::vector<Pick> extra;
    std::string lastDir;
    enum { IDLE, RUNNING, DONE, FAILED } state = IDLE;
    std::thread worker;
    std::atomic<int> result{ -1 };
    uint64_t doneAt = 0;

    bool Init()
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { LogWarn("installer: SDL video: %s", SDL_GetError()); return false; }
        std::string reg = FindFont(false), bold = FindFont(true);
        if (reg.empty()) { LogWarn("installer: no system font found"); return false; }
        float cs = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        if (cs <= 0) cs = 1;
        SDL_Rect usable{};
        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
        int w = int(700 * cs), h = int(900 * cs);
        if (usable.h > 0) h = std::min(h, usable.h - int(60 * cs));
        win = SDL_CreateWindow("BO3 Recomp - Instalación", w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!win) { LogWarn("installer: window: %s", SDL_GetError()); return false; }
        ren = SDL_CreateRenderer(win, nullptr);
        if (!ren) { LogWarn("installer: renderer: %s", SDL_GetError()); return false; }
        SDL_SetRenderVSync(ren, 1);
        k = SDL_GetWindowDisplayScale(win);
        if (k <= 0) k = 1;
        if (!BakeFonts(reg, bold)) return false;
        Log("installer: window %dx%d, scale %.2f, fonts %s / %s, renderer %s", w, h, k, reg.c_str(), bold.c_str(), SDL_GetRendererName(ren));
        return true;
    }

    bool BakeFonts(const std::string& reg, const std::string& bold)
    {
        std::vector<uint8_t> data[2];
        if (!ReadAll(reg, data[0]) || !ReadAll(bold, data[1])) return false;
        const int W = 2048, H = 2048;
        std::vector<uint8_t> alpha(size_t(W) * H);
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, alpha.data(), W, H, 0, 1, nullptr)) return false;
        stbtt_PackSetOversampling(&pc, 2, 1);
        for (int i = 0; i < F_COUNT; i++)
        {
            const uint8_t* d = data[kSpecs[i].bold ? 1 : 0].data();
            float px = kSpecs[i].size * k * 1.15f;   // (Android sp ~ a bit larger than the em size)
            stbtt_pack_range r[2] = {};
            r[0].font_size = px; r[0].first_unicode_codepoint_in_range = 0x20; r[0].num_chars = 224; r[0].chardata_for_range = f[i].latin;
            r[1].font_size = px; r[1].first_unicode_codepoint_in_range = 0x2010; r[1].num_chars = 32; r[1].chardata_for_range = f[i].punct;
            if (!stbtt_PackFontRanges(&pc, d, 0, r, 2)) { LogWarn("installer: font atlas full"); stbtt_PackEnd(&pc); return false; }
            stbtt_fontinfo fi;
            stbtt_InitFont(&fi, d, stbtt_GetFontOffsetForIndex(d, 0));
            int asc, desc, gap;
            stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
            float sc = stbtt_ScaleForPixelHeight(&fi, px);
            f[i].ascent = asc * sc;
            f[i].lineH = (asc - desc + gap) * sc;
        }
        stbtt_PackEnd(&pc);
        std::vector<uint32_t> rgba(size_t(W) * H);
        for (size_t i = 0; i < rgba.size(); i++) rgba[i] = 0x00FFFFFFu | uint32_t(alpha[i]) << 24;
        atlas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
        if (!atlas) return false;
        SDL_UpdateTexture(atlas, nullptr, rgba.data(), W * 4);
        SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_BLEND);
        return true;
    }

    // ---------------------------------------------------------------- drawing
    static SDL_FColor FC(Rgb c) { return { c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f }; }

    void FillRound(float x, float y, float w, float h, float r, Rgb c)
    {
        r = std::min({ r, w / 2, h / 2 });
        std::vector<SDL_Vertex> v;
        std::vector<int> idx;
        SDL_FColor col = FC(c);
        v.push_back({ { x + w / 2, y + h / 2 }, col, { 0, 0 } });
        const int seg = 8;
        const float cxs[4] = { x + w - r, x + r, x + r, x + w - r }, cys[4] = { y + r, y + r, y + h - r, y + h - r };
        for (int q = 0; q < 4; q++)
            for (int s = 0; s <= seg; s++)
            {
                float a = (q * 90 + s * 90.f / seg) * float(M_PI) / 180;
                v.push_back({ { cxs[q] + std::cos(a) * r, cys[q] - std::sin(a) * r }, col, { 0, 0 } });
            }
        int n = int(v.size()) - 1;
        for (int i = 1; i <= n; i++) { idx.push_back(0); idx.push_back(i); idx.push_back(i % n + 1); }
        SDL_RenderGeometry(ren, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
    }

    void Panel(float x, float y, float w, float h, float r, Rgb fill, Rgb line)
    {
        FillRound(x, y, w, h, r, line);
        FillRound(x + k, y + k, w - 2 * k, h - 2 * k, r - k, fill);
    }

    const stbtt_packedchar* Glyph(FontId id, uint32_t cp)
    {
        if (cp >= 0x20 && cp < 0x100) return &f[id].latin[cp - 0x20];
        if (cp >= 0x2010 && cp < 0x2030) return &f[id].punct[cp - 0x2010];
        return &f[id].latin['?' - 0x20];
    }

    float Measure(FontId id, const std::string& s)
    {
        float w = 0;
        for (size_t i = 0; i < s.size();) w += Glyph(id, NextCp(s, i))->xadvance;
        return w;
    }

    void Text(FontId id, float x, float y, const std::string& s, Rgb c)
    {
        std::vector<SDL_Vertex> v;
        std::vector<int> idx;
        SDL_FColor col = FC(c);
        float px = x, py = y + f[id].ascent;
        for (size_t i = 0; i < s.size();)
        {
            stbtt_aligned_quad q;
            const stbtt_packedchar* g = Glyph(id, NextCp(s, i));
            stbtt_packedchar tmp = *g;
            float ox = px, oy = py;
            stbtt_GetPackedQuad(&tmp, 2048, 2048, 0, &ox, &oy, &q, 0);
            px += g->xadvance;
            int b = int(v.size());
            v.push_back({ { q.x0, q.y0 }, col, { q.s0, q.t0 } });
            v.push_back({ { q.x1, q.y0 }, col, { q.s1, q.t0 } });
            v.push_back({ { q.x1, q.y1 }, col, { q.s1, q.t1 } });
            v.push_back({ { q.x0, q.y1 }, col, { q.s0, q.t1 } });
            int t[6] = { b, b + 1, b + 2, b, b + 2, b + 3 };
            idx.insert(idx.end(), t, t + 6);
        }
        if (!v.empty()) SDL_RenderGeometry(ren, atlas, v.data(), int(v.size()), idx.data(), int(idx.size()));
    }

    std::vector<std::string> Wrap(FontId id, const std::string& s, float maxW)
    {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= s.size())
        {
            size_t nl = s.find('\n', start);
            std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            std::string line;
            size_t p = 0;
            while (p < para.size())
            {
                size_t sp = para.find(' ', p);
                std::string word = para.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
                std::string tryLine = line.empty() ? word : line + " " + word;
                if (!line.empty() && Measure(id, tryLine) > maxW) { lines.push_back(line); line = word; }
                else line = tryLine;
                if (sp == std::string::npos) break;
                p = sp + 1;
            }
            lines.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        return lines;
    }

    float TextBlock(FontId id, float x, float y, float maxW, const std::string& s, Rgb c, bool draw = true)
    {
        auto lines = Wrap(id, s, maxW);
        float lh = f[id].lineH * 1.12f;
        if (draw)
            for (size_t i = 0; i < lines.size(); i++) Text(id, x, y + i * lh, lines[i], c);
        return lines.size() * lh;
    }

    bool Inside(float x, float y, float w, float h, float px, float py) { return px >= x && px < x + w && py >= y && py < y + h; }

    // button: true when clicked this frame
    bool Button(float x, float y, float w, float h, const std::string& label, bool primary, bool enabled)
    {
        bool hover = enabled && Inside(x, y, w, h, mx, my);
        Rgb fill = primary ? ACCENT : BTN;
        if (!enabled) fill = primary ? Rgb{ 90, 60, 44 } : Rgb{ 28, 31, 36 };
        else if (hover) fill = primary ? Rgb{ 255, 130, 60 } : Rgb{ 46, 51, 60 };
        if (primary) FillRound(x, y, w, h, 14 * k, fill);
        else Panel(x, y, w, h, h / 2, fill, LINE);
        Rgb tc = !enabled ? Rgb{ 110, 114, 120 } : primary ? WHITE : TEXT;
        FontId fid = primary ? F_CARD : F_BTN;
        Text(fid, x + (w - Measure(fid, label)) / 2, y + (h - f[fid].lineH) / 2, label, tc);
        return enabled && click && Inside(x, y, w, h, cx, cy);
    }

    void Check(float x, float y, float s, Rgb c)
    {
        // a check mark from two thick strokes
        auto stroke = [&](float x0, float y0, float x1, float y1) {
            float dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy), t = s * 0.09f;
            float nx = -dy / l * t, ny = dx / l * t;
            SDL_FColor col = FC(c);
            SDL_Vertex v[4] = { { { x0 + nx, y0 + ny }, col, {} }, { { x1 + nx, y1 + ny }, col, {} },
                                { { x1 - nx, y1 - ny }, col, {} }, { { x0 - nx, y0 - ny }, col, {} } };
            int i[6] = { 0, 1, 2, 0, 2, 3 };
            SDL_RenderGeometry(ren, nullptr, v, 4, i, 6);
        };
        stroke(x + s * 0.25f, y + s * 0.52f, x + s * 0.43f, y + s * 0.70f);
        stroke(x + s * 0.43f, y + s * 0.70f, x + s * 0.76f, y + s * 0.32f);
    }

    // ---------------------------------------------------------------- picking
    void OpenDialog(int slot, bool folder, bool many)
    {
        if (g_dlgOpen || state == RUNNING) return;
        g_dlgOpen = true;
        const char* loc = lastDir.empty() ? getenv("HOME") : lastDir.c_str();
        static const SDL_DialogFileFilter discFilter[] = { { "Disco de Xbox 360 (.iso, .zar, .zip)", "iso;zar;zip" }, { "Todos los archivos", "*" } };
        if (folder) SDL_ShowOpenFolderDialog(OnDialog, reinterpret_cast<void*>(intptr_t(slot)), win, loc, false);
        else if (slot == 0) SDL_ShowOpenFileDialog(OnDialog, reinterpret_cast<void*>(intptr_t(slot)), win, discFilter, 2, loc, false);
        else SDL_ShowOpenFileDialog(OnDialog, reinterpret_cast<void*>(intptr_t(slot)), win, nullptr, 0, loc, many);
    }

    void Choose(int i, const std::string& path)
    {
        Pick p;
        p.path = path;
        p.folder = fs::is_directory(path);
        p.name = fs::path(path).filename().string() + (p.folder ? " (carpeta)" : "");
        lastDir = (p.folder ? fs::path(path).parent_path() : fs::path(path).parent_path()).string();
        if (IsZip(path)) { p.zip = true; p.ok = true; picks[i] = p; return; }
        SetupProbe pr = SetupProbePath(path);
        bool good = i == 0 ? pr.discs > 0 : i == 1 ? pr.updates > 0 : i == 2 ? pr.cp1 : pr.cp2;
        if (!good)
        {
            // the other pack (or the other card's kind): put it where it belongs
            if (i >= 2 && (pr.cp1 || pr.cp2)) { Choose(pr.cp1 ? 2 : 3, path); picks[i].err = kWrong[i]; picks[i].ok = false; return; }
            p.err = kWrong[i];
            picks[i] = p;
            return;
        }
        p.ok = true;
        picks[i] = p;
        // a folder (00000002) with both packs fills both cards
        int other = i == 2 ? 3 : i == 3 ? 2 : -1;
        if (other >= 0 && (other == 2 ? pr.cp1 : pr.cp2) && !picks[other].ok)
        {
            Pick o = p;
            o.via = p.name;
            picks[other] = o;
        }
        Log("installer: card %d <- %s (discs %d, updates %d, dlcs %d%s%s)", i, path.c_str(), pr.discs, pr.updates, pr.dlcs, pr.cp1 ? ", CP1" : "", pr.cp2 ? ", CP2" : "");
    }

    void AddExtra(const std::string& path)
    {
        Pick p;
        p.path = path;
        p.folder = fs::is_directory(path);
        p.name = fs::path(path).filename().string() + (p.folder ? " (carpeta)" : "");
        lastDir = fs::path(path).parent_path().string();
        if (IsZip(path)) p.zip = p.ok = true;
        else
        {
            SetupProbe pr = SetupProbePath(path);
            p.ok = pr.dlcs > 0;
            if (!p.ok) p.err = "\"" + p.name + "\" no es un paquete de contenido de Black Ops III.";
        }
        for (auto& e : extra) if (e.path == path) return;
        extra.push_back(p);
    }

    void HandleDialogs()
    {
        std::vector<std::pair<int, std::vector<std::string>>> res;
        {
            std::lock_guard<std::mutex> l(g_dlgMutex);
            res.swap(g_dlgResults);
        }
        for (auto& [slot, files] : res)
        {
            if (files.empty()) continue;
            if (slot >= 20) for (auto& p : files) AddExtra(p);
            else Choose(slot % 10, files[0]);
        }
    }

    bool AllChosen()
    {
        for (auto& p : picks) if (!p.ok && !p.installed) return false;
        for (auto& e : extra) if (!e.ok) return false;
        return true;
    }

    // ---------------------------------------------------------------- installing
    void Start()
    {
        if (state == RUNNING || !AllChosen()) return;
        state = RUNNING;
        {
            std::lock_guard<std::mutex> l(g_progMutex);
            g_pct = 0; g_status = "Preparando"; g_message.clear();
        }
        std::vector<Pick> todo;
        for (auto& p : picks) if (p.ok) todo.push_back(p);
        for (auto& e : extra) todo.push_back(e);
        if (worker.joinable()) worker.join();
        worker = std::thread([todo]() {
            std::vector<std::pair<std::string, std::string>> files;
            std::vector<std::string> seen;
            std::error_code ec;
            int n = 0;
            bool ok = true;
            for (auto& p : todo)
            {
                if (std::find(seen.begin(), seen.end(), p.path) != seen.end()) continue;   // (one folder for both packs)
                seen.push_back(p.path);
                std::string path = p.path;
                if (p.zip)
                {
                    // into "instalar-carpetas": the installer moves what is there instead of copying it
                    std::string dst = "instalar-carpetas/zip" + std::to_string(n++);
                    fs::remove_all(dst, ec);
                    Status("Descomprimiendo " + p.name + "...");
                    Log("installer: unzipping %s", p.path.c_str());
                    if (!Unzip(p.path, dst))
                    {
                        Progress(("No se pudo descomprimir \"" + p.name + "\" (¿espacio libre? ¿archivo dañado?).").c_str(), -1);
                        ok = false;
                        break;
                    }
                    path = dst;
                }
                files.push_back({ path, p.name });
            }
            if (ok) ok = SetupGameDataFrom(files, Progress);
            fs::remove_all("instalar-carpetas", ec);
            g_result.store(ok ? 1 : 0);
        });
    }
    static std::atomic<int> g_result;

    // ---------------------------------------------------------------- one frame
    void Frame()
    {
        int ow, oh;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        outW = float(ow); outH = float(oh);
        // background: dark, warm at the top
        {
            SDL_FColor top = FC({ 28, 20, 16 }), bot = FC(BG);
            float gh = outH * 0.45f;
            SDL_Vertex v[4] = { { { 0, 0 }, top, {} }, { { outW, 0 }, top, {} }, { { outW, gh }, bot, {} }, { { 0, gh }, bot, {} } };
            int i[6] = { 0, 1, 2, 0, 2, 3 };
            SDL_SetRenderDrawColor(ren, BG.r, BG.g, BG.b, 255);
            SDL_RenderClear(ren);
            SDL_RenderGeometry(ren, nullptr, v, 4, i, 6);
        }
        float pad = 20 * k;
        float colW = std::min(outW - 2 * pad, 640 * k);
        float x0 = (outW - colW) / 2;
        float y = 28 * k - scroll;
        bool busy = state == RUNNING || state == DONE;

        // header
        Text(F_KICK, x0, y, zombies ? "BO3 RECOMP · ZOMBIES" : "BO3 RECOMP · MULTIJUGADOR", ACCENT);
        y += f[F_KICK].lineH + 2 * k;
        Text(F_TITLE, x0, y, "Instalación", TEXT);
        y += f[F_TITLE].lineH + 12 * k;
        y += TextBlock(F_BODY, x0, y, colW,
                       "Elegí los archivos de tu copia de Call of Duty: Black Ops III para Xbox 360. Con los cuatro elegidos, "
                       "tocá Convertir y se instalan en la carpeta del juego.", SUB);
        y += 8 * k;
        {
            float cxp = x0;
            for (const char* c : { "Unos minutos", "8 GB libres" })
            {
                float w = Measure(F_SMALL, c) + 24 * k, h = f[F_SMALL].lineH + 12 * k;
                FillRound(cxp, y, w, h, h / 2, CHIP);
                Text(F_SMALL, cxp + 12 * k, y + 6 * k, c, TEXT);
                cxp += w + 8 * k;
            }
            y += f[F_SMALL].lineH + 12 * k;
        }

        // the four steps + optional DLC
        for (int i = 0; i < 5; i++)
        {
            y += 12 * k;
            bool isExtra = i == 4;
            Pick* p = isExtra ? nullptr : &picks[i];
            bool done = isExtra ? (!extra.empty() && std::all_of(extra.begin(), extra.end(), [](const Pick& e) { return e.ok; }))
                                : (p->ok || p->installed);
            std::string title = isExtra ? "Otros DLC (opcional)" : kTitles[i];
            std::string hint = isExtra ? "Packs de mapas y demás contenido: varios archivos (o .zip) a la vez, o una carpeta." : kHints[i];
            std::string label;
            Rgb lc = SUB;
            if (isExtra)
            {
                int bad = 0;
                std::string err;
                for (auto& e : extra) if (!e.ok) { bad++; err = e.err; }
                if (extra.empty()) label = "Ninguno";
                else if (bad) { label = err + (bad > 1 ? " (y " + std::to_string(bad - 1) + " más)" : ""); lc = ERR; }
                else { label = std::to_string(extra.size()) + " elegido(s)"; lc = OK; }
            }
            else if (!p->err.empty()) { label = (p->name.empty() ? "" : p->name + ": ") + p->err; lc = ERR; }
            else if (!p->via.empty()) { label = "Incluido en " + p->via; lc = OK; }
            else if (p->ok) { label = p->name + (p->zip ? " (se revisa al convertir)" : ""); lc = OK; }
            else if (p->installed) { label = "Ya instalado"; lc = OK; }
            else label = "Sin elegir";

            float ip = 16 * k, badge = 32 * k;
            float tx = x0 + ip + badge + 12 * k, tw = colW - (tx - x0) - ip;
            // label on the left of the buttons' row
            const char* b1 = isExtra ? "Archivos" : i == 0 ? "Elegir" : "Archivo";
            float w2 = Measure(F_BTN, "Carpeta") + 36 * k, w1 = Measure(F_BTN, b1) + 36 * k, w3 = Measure(F_BTN, "Quitar") + 36 * k;
            float bw = w1 + (i > 0 ? w2 + 8 * k : 0) + (isExtra && !extra.empty() ? w3 + 8 * k : 0);
            float lw = std::max(colW - (tx - x0) - ip - bw - 16 * k, 120 * k);
            float th = f[F_CARD].lineH, hh = TextBlock(F_SMALL, 0, 0, tw, hint, SUB, false), lh = TextBlock(F_SMALL, 0, 0, lw, label, lc, false);
            float bh = 36 * k;
            float row = std::max(lh, bh);
            float h = ip + th + 4 * k + hh + 12 * k + row + ip;
            Panel(x0, y, colW, h, 16 * k, done ? CARD_DONE : CARD, done ? CARD_DONE_LINE : LINE);
            // badge
            float bx = x0 + ip, by = y + ip;
            FillRound(bx, by, badge, badge, badge / 2, done ? OK : TRACK);
            if (done) Check(bx, by, badge, Rgb{ 10, 30, 16 });
            else
            {
                std::string n = isExtra ? "+" : std::to_string(i + 1);
                Text(F_CARD, bx + (badge - Measure(F_CARD, n)) / 2, by + (badge - f[F_CARD].lineH) / 2, n, TEXT);
            }
            float ty = y + ip;
            Text(F_CARD, tx, ty, title, TEXT);
            ty += th + 4 * k;
            ty += TextBlock(F_SMALL, tx, ty, tw, hint, SUB);
            ty += 12 * k;
            TextBlock(F_SMALL, tx, ty + (row - lh) / 2, lw, label, lc);
            // buttons, right-aligned
            float by2 = ty + (row - bh) / 2, bxr = x0 + colW - ip;
            bool can = !busy && !g_dlgOpen;
            if (i > 0)
            {
                if (Button(bxr - w2, by2, w2, bh, "Carpeta", false, can)) OpenDialog(isExtra ? 21 : 10 + i, true, false);
                bxr -= w2 + 8 * k;
            }
            if (Button(bxr - w1, by2, w1, bh, b1, false, can)) OpenDialog(isExtra ? 20 : i, false, isExtra);
            if (isExtra && !extra.empty())
            {
                if (Button(bxr - w1 - 8 * k - w3, by2, w3, bh, "Quitar", false, can)) extra.clear();
            }
            y += h;
        }

        // convert / progress
        y += 22 * k;
        if (state == IDLE)
        {
            if (Button(x0, y, colW, 52 * k, "Convertir", true, AllChosen() && !g_dlgOpen)) Start();
            y += 52 * k;
        }
        else
        {
            int pct;
            std::string status, msg;
            {
                std::lock_guard<std::mutex> l(g_progMutex);
                pct = g_pct; status = g_status; msg = g_message;
            }
            float ip = 18 * k;
            float iw = colW - 2 * ip;
            std::string big = state == DONE ? "Listo" : state == FAILED ? "No se pudo instalar" : std::to_string(pct) + "%";
            Rgb bc = state == DONE ? OK : state == FAILED ? ERR : TEXT;
            std::string line = state == DONE ? "Arrancando el juego..." : state == FAILED ? (msg.empty() ? status : msg) : status;
            FontId bf = state == FAILED ? F_TITLE : F_BIG;
            float lh = TextBlock(F_STATUS, 0, 0, iw, line, SUB, false);
            float h = ip + f[bf].lineH + 8 * k + 10 * k + 12 * k + lh + ip + (state == FAILED ? 52 * k : 0);
            Panel(x0, y, colW, h, 16 * k, CARD, LINE);
            float yy = y + ip;
            if (state == DONE) { Check(x0 + ip - 6 * k, yy + 2 * k, f[bf].lineH * 0.9f, OK); Text(bf, x0 + ip + f[bf].lineH * 0.8f, yy, big, bc); }
            else Text(bf, x0 + ip, yy, big, bc);
            yy += f[bf].lineH + 8 * k;
            FillRound(x0 + ip, yy, iw, 10 * k, 5 * k, TRACK);
            float fw = iw * std::clamp(state == DONE ? 100 : pct, 0, 100) / 100.f;
            if (fw > 1) FillRound(x0 + ip, yy, std::max(fw, 10 * k), 10 * k, 5 * k, state == FAILED ? ERR : state == DONE ? OK : ACCENT);
            yy += 10 * k + 12 * k;
            yy += TextBlock(F_STATUS, x0 + ip, yy, iw, line, state == FAILED ? TEXT : SUB);
            if (state == FAILED)
            {
                yy += 12 * k;
                if (Button(x0 + ip, yy, iw, 40 * k, "Volver a intentar", true, true)) state = IDLE;
            }
            y += h;
        }
        y += 28 * k;
        contentH = y + scroll;
        float maxScroll = std::max(0.f, contentH - outH);
        scroll = std::clamp(scroll, 0.f, maxScroll);
        // scroll bar
        if (maxScroll > 0)
        {
            float bh = outH * outH / contentH, by = (outH - bh) * scroll / maxScroll;
            FillRound(outW - 8 * k, by + 2 * k, 4 * k, bh - 4 * k, 2 * k, Rgb{ 70, 76, 86 });
        }
        click = false;
    }

    void Shot(const char* path)
    {
        if (SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr))
        {
            SDL_SaveBMP(s, path);
            SDL_DestroySurface(s);
            Log("installer: window image saved to %s", path);
        }
    }

    int Loop()
    {
        bool part[4];
        SetupInstalledParts(part);
        for (int i = 0; i < 4; i++) picks[i].installed = part[i];
        if (const char* t = getenv("BO3_UI_TEST"))
        {
            std::string s = t;
            int i = 0;
            size_t pos = 0;
            while (pos <= s.size())
            {
                size_t e = s.find('|', pos);
                std::string p = s.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                if (!p.empty()) { if (i < 4) Choose(i, p); else AddExtra(p); }
                i++;
                if (e == std::string::npos) break;
                pos = e + 1;
            }
            if (getenv("BO3_UI_TEST_CONVERT")) Start();
        }
        const char* shot = getenv("BO3_UI_SHOT");
        int shotFrame = getenv("BO3_UI_SHOT_FRAME") ? atoi(getenv("BO3_UI_SHOT_FRAME")) : 10;
        int frames = 0;
        for (;;)
        {
            SDL_Event e;
            bool any = SDL_WaitEventTimeout(&e, state == RUNNING || shot ? 50 : 250);
            while (any)
            {
                float dens = SDL_GetWindowPixelDensity(win);
                switch (e.type)
                {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (state != RUNNING) { Close(); return 0; }
                    break;
                case SDL_EVENT_MOUSE_MOTION: mx = e.motion.x * dens; my = e.motion.y * dens; break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (e.button.button == SDL_BUTTON_LEFT) { click = true; cx = e.button.x * dens; cy = e.button.y * dens; }
                    break;
                case SDL_EVENT_MOUSE_WHEEL: scroll -= e.wheel.y * 60 * k; break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.key == SDLK_DOWN || e.key.key == SDLK_PAGEDOWN) scroll += (e.key.key == SDLK_DOWN ? 60 : 400) * k;
                    if (e.key.key == SDLK_UP || e.key.key == SDLK_PAGEUP) scroll -= (e.key.key == SDLK_UP ? 60 : 400) * k;
                    break;
                default: break;
                }
                any = SDL_PollEvent(&e);
            }
            HandleDialogs();
            if (state == RUNNING && g_result.load() >= 0)
            {
                if (worker.joinable()) worker.join();
                bool ok = g_result.exchange(-1) == 1;
                state = ok ? DONE : FAILED;
                doneAt = SDL_GetTicks();
                Log("installer: %s", ok ? "installed" : "failed");
                if (!ok) { bool p2[4]; SetupInstalledParts(p2); for (int i = 0; i < 4; i++) picks[i].installed = p2[i]; }
            }
            Frame();
            if (shot && ++frames == shotFrame) Shot(shot);   // (before presenting: the back buffer is undefined after)
            SDL_RenderPresent(ren);
            if (state == DONE && SDL_GetTicks() - doneAt > 1500) { Close(); return 1; }
        }
    }

    void Close()
    {
        if (atlas) SDL_DestroyTexture(atlas);
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        atlas = nullptr; ren = nullptr; win = nullptr;
    }
};
std::atomic<int> InstallerWindow::g_result{ -1 };
}

int InstallerUiRun(bool zombies)
{
    static InstallerWindow w;   // (the worker thread outlives nothing: joined before returning)
    w.zombies = zombies;
    if (!w.Init()) { w.Close(); return -1; }
    return w.Loop();
}
#endif
