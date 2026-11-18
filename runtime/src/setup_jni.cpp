// Android installer library (libbo3_setup.so, loaded by com.bo3recomp.InstallerActivity): the first-run
// installation of setup.cpp without the game. The player picks the disc image, the Title Update and the
// two Compatibility Packs with Android's file picker; Java passes them as /proc/self/fd/N paths (open
// file descriptors of the chosen documents, no storage permission needed) plus their display names.
//   Installer.installed(dataDir) -> everything in place?
//   Installer.install(dataDir, paths[], names[]) -> true if installed; status via Installer.progress(text, percent)
// Not part of the game libraries (they have their own log/UI); see CMakeLists.txt (bo3_setup).
#include "log.h"
#include "setup.h"
#include "ui.h"

#include <android/log.h>
#include <jni.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

bool g_logImports = false;

void LogImpl(const char* level, const char* fmt, ...)
{
    static std::mutex m;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard lock(m);
    __android_log_print(level[0] == 'E' || level[0] == 'F' ? ANDROID_LOG_ERROR : level[0] == 'W' ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "BO3Setup", "%s", buf);
    static FILE* f = fopen("install.log", "a");
    if (f) { fprintf(f, "[%s] %s\n", level, buf); fflush(f); }
}

void QuickExit(int code) { _exit(code); }

namespace
{
JNIEnv* t_env;     // the installing thread's env (install() runs on one Java thread)
jclass t_cls;
jmethodID t_progress;

void ReportToJava(const char* text, int percent)
{
    if (!t_env || !t_progress) return;
    jstring s = t_env->NewStringUTF(text);
    t_env->CallStaticVoidMethod(t_cls, t_progress, s, jint(percent));
    if (t_env->ExceptionCheck()) t_env->ExceptionClear();
    t_env->DeleteLocalRef(s);
}

std::string Str(JNIEnv* env, jstring js)
{
    if (!js) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string s = c ? c : "";
    if (c) env->ReleaseStringUTFChars(js, c);
    return s;
}
}

// setup.cpp only shows dialogs when no report hook is set (never here)
int UiChoice(const std::string& title, const std::string& text, const std::vector<std::string>&) { ReportToJava(text.c_str(), -1); return 0; }
void UiMessage(const std::string& title, const std::string& text) { ReportToJava(text.c_str(), -1); }
// setup.cpp's storage-permission helper asks SDL for the env; the installer never needs that path
extern "C" void* SDL_GetAndroidJNIEnv(void) { return t_env; }

extern "C" JNIEXPORT jboolean JNICALL Java_com_bo3recomp_Installer_installed(JNIEnv* env, jclass, jstring dir)
{
    if (chdir(Str(env, dir).c_str()) != 0) return JNI_FALSE;
    return SetupInstalled() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_bo3recomp_Installer_install(JNIEnv* env, jclass cls, jstring dir, jobjectArray paths, jobjectArray names)
{
    std::string d = Str(env, dir);
    if (chdir(d.c_str()) != 0) return JNI_FALSE;
    t_env = env;
    t_cls = cls;
    t_progress = env->GetStaticMethodID(cls, "progress", "(Ljava/lang/String;I)V");
    std::vector<std::pair<std::string, std::string>> files;
    jsize n = env->GetArrayLength(paths);
    for (jsize i = 0; i < n; i++)
    {
        jstring p = (jstring)env->GetObjectArrayElement(paths, i), nm = (jstring)env->GetObjectArrayElement(names, i);
        files.emplace_back(Str(env, p), Str(env, nm));
        env->DeleteLocalRef(p);
        env->DeleteLocalRef(nm);
    }
    Log("setup: installing into %s from %d chosen files", d.c_str(), int(n));
    bool ok = SetupGameDataFrom(files, ReportToJava);
    t_env = nullptr;
    return ok ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------- .zip extraction
// The installer's zips (Title Update, Compatibility Packs, DLC) are extracted here, from the file
// descriptor of the picked file, with zlib: not through Java streams - Husk (Android apps on iOS) kept
// every buffer its Java I/O passed through and ran out of memory after ~3 GB. Stored and deflated
// entries; zip64 sizes / offsets; names with ".." or absolute paths are skipped.
#include <zlib.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>

namespace
{
std::atomic<long long> s_unzipBytes{ 0 };

bool ReadAt(int fd, void* p, size_t n, uint64_t off)
{
    uint8_t* b = static_cast<uint8_t*>(p);
    while (n)
    {
        ssize_t r = pread(fd, b, n, off_t(off));
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r; n -= size_t(r); off += uint64_t(r);
    }
    return true;
}
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t U64(const uint8_t* p) { return uint64_t(U32(p)) | (uint64_t(U32(p + 4)) << 32); }

bool MakeDirs(const std::string& path)   // every directory of `path` (not the last component)
{
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') { std::string d = path.substr(0, i); if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false; }
    return true;
}

bool WriteAll(int fd, const uint8_t* p, size_t n)
{
    while (n)
    {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return false; }
        p += w; n -= size_t(w);
    }
    return true;
}

std::string Unzip(int fd, const std::string& dst)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 22) return "no es un .zip";
    const uint64_t size = uint64_t(st.st_size);
    // end of central directory: in the last 64 KiB + 22 bytes
    size_t tailN = size_t(std::min<uint64_t>(size, 65557));
    std::vector<uint8_t> tail(tailN);
    if (!ReadAt(fd, tail.data(), tailN, size - tailN)) return "no se pudo leer el .zip";
    long eocd = -1;
    for (long i = long(tailN) - 22; i >= 0; i--) if (U32(&tail[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return "no es un .zip (sin directorio central)";
    uint64_t count = U16(&tail[eocd + 10]), cdSize = U32(&tail[eocd + 12]), cdOff = U32(&tail[eocd + 16]);
    if (eocd >= 20 && U32(&tail[eocd - 20]) == 0x07064b50)   // zip64 locator
    {
        uint8_t z[56];
        if (!ReadAt(fd, z, sizeof(z), U64(&tail[eocd - 20 + 8])) || U32(z) != 0x06064b50) return "zip64 inválido";
        count = U64(z + 32); cdSize = U64(z + 40); cdOff = U64(z + 48);
    }
    if (cdOff + cdSize > size) return "directorio central fuera del archivo";
    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    if (!ReadAt(fd, cd.data(), cd.size(), cdOff)) return "no se pudo leer el directorio central";
    std::vector<uint8_t> in(1 << 20), out(1 << 20);
    size_t p = 0;
    for (uint64_t e = 0; e < count; e++)
    {
        if (p + 46 > cd.size() || U32(&cd[p]) != 0x02014b50) return "directorio central dañado";
        uint16_t method = U16(&cd[p + 10]), nameLen = U16(&cd[p + 28]), extraLen = U16(&cd[p + 30]), commentLen = U16(&cd[p + 32]);
        uint64_t csize = U32(&cd[p + 20]), usize = U32(&cd[p + 24]), lho = U32(&cd[p + 42]);
        std::string name(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
        // zip64 extra field: the 0xFFFFFFFF values, in order
        for (size_t x = p + 46 + nameLen; x + 4 <= p + 46 + nameLen + extraLen;)
        {
            uint16_t id = U16(&cd[x]), len = U16(&cd[x + 2]);
            if (id == 1)
            {
                size_t q = x + 4;
                if (usize == 0xFFFFFFFFu) { usize = U64(&cd[q]); q += 8; }
                if (csize == 0xFFFFFFFFu) { csize = U64(&cd[q]); q += 8; }
                if (lho == 0xFFFFFFFFu) { lho = U64(&cd[q]); }
            }
            x += 4 + len;
        }
        p += 46 + nameLen + extraLen + commentLen;
        for (char& c : name) if (c == '\\') c = '/';
        if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos) continue;
        std::string path = dst + "/" + name;
        if (name.back() == '/') { MakeDirs(path); continue; }
        uint8_t lh[30];
        if (!ReadAt(fd, lh, 30, lho) || U32(lh) != 0x04034b50) return "entrada dañada: " + name;
        uint64_t data = lho + 30 + U16(lh + 26) + U16(lh + 28);
        if (data + csize > size) return "entrada fuera del archivo: " + name;
        if (!MakeDirs(path)) return "no se pudo crear la carpeta de " + name;
        int o = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (o < 0) return "no se pudo crear " + name + ": " + strerror(errno);
        bool ok = true;
        if (method == 0)
        {
            for (uint64_t done = 0; ok && done < csize;)
            {
                size_t n = size_t(std::min<uint64_t>(in.size(), csize - done));
                ok = ReadAt(fd, in.data(), n, data + done) && WriteAll(o, in.data(), n);
                done += n; s_unzipBytes += (long long)n;
            }
        }
        else if (method == 8)
        {
            z_stream z{};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK) { close(o); return "zlib"; }
            uint64_t done = 0;
            int r = Z_OK;
            while (ok && r != Z_STREAM_END)
            {
                if (z.avail_in == 0 && done < csize)
                {
                    size_t n = size_t(std::min<uint64_t>(in.size(), csize - done));
                    if (!ReadAt(fd, in.data(), n, data + done)) { ok = false; break; }
                    done += n;
                    z.next_in = in.data(); z.avail_in = uInt(n);
                }
                z.next_out = out.data(); z.avail_out = uInt(out.size());
                r = inflate(&z, Z_NO_FLUSH);
                if (r != Z_OK && r != Z_STREAM_END) { ok = false; break; }
                size_t produced = out.size() - z.avail_out;
                ok = WriteAll(o, out.data(), produced);
                s_unzipBytes += (long long)produced;
                if (r == Z_OK && produced == 0 && z.avail_in == 0 && done >= csize) { ok = false; break; }   // truncated
            }
            inflateEnd(&z);
        }
        else { close(o); return "método de compresión no soportado en " + name; }
        if (close(o) != 0) ok = false;
        if (!ok) return "error al extraer " + name + " (¿espacio libre?)";
        (void)usize;
    }
    return {};
}
}

// Installer.unzipFd(fd, dst): "" when done, otherwise the error
extern "C" JNIEXPORT jstring JNICALL Java_com_bo3recomp_Installer_unzipFd(JNIEnv* env, jclass, jint fd, jstring dst)
{
    std::string d = Str(env, dst);
    mkdir(d.c_str(), 0755);
    std::string err = Unzip(int(fd), d);
    if (!err.empty()) LogImpl("W", "unzip into %s: %s", d.c_str(), err.c_str());
    return env->NewStringUTF(err.c_str());
}

extern "C" JNIEXPORT jlong JNICALL Java_com_bo3recomp_Installer_unzipBytes(JNIEnv*, jclass) { return jlong(s_unzipBytes.load()); }
