#include "http.h"
#include "log.h"

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <mutex>

// com.bo3recomp.Http.request(String method, String url, String[] headers (k, v, k, v...), byte[] body,
// int timeoutMs) -> byte[]: 4 bytes big-endian status, then the response body (status 0 = failure)
HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, int timeoutMs)
{
    static std::mutex lock;
    static jclass cls = nullptr;
    static jmethodID mid = nullptr;
    HttpResponse out;
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    if (!env) { out.error = "no JNI"; return out; }
    {
        std::lock_guard g(lock);
        if (!cls)
        {
            // FindClass sees the app's classes only from a thread Java started: the first call
            // comes from SDL's main thread (startup sync), later threads reuse the global ref
            jclass c = env->FindClass("com/bo3recomp/Http");
            if (!c) { env->ExceptionClear(); out.error = "no Http class"; return out; }
            cls = (jclass)env->NewGlobalRef(c);
            env->DeleteLocalRef(c);
            mid = env->GetStaticMethodID(cls, "request", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BI)[B");
        }
    }
    if (!mid) { out.error = "no Http.request"; return out; }
    jstring jm = env->NewStringUTF(method.c_str()), ju = env->NewStringUTF(url.c_str());
    jobjectArray jh = env->NewObjectArray(jsize(headers.size() * 2), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < headers.size(); i++)
    {
        jstring k = env->NewStringUTF(headers[i].first.c_str()), v = env->NewStringUTF(headers[i].second.c_str());
        env->SetObjectArrayElement(jh, jsize(i * 2), k);
        env->SetObjectArrayElement(jh, jsize(i * 2 + 1), v);
        env->DeleteLocalRef(k);
        env->DeleteLocalRef(v);
    }
    jbyteArray jb = env->NewByteArray(jsize(body.size()));
    env->SetByteArrayRegion(jb, 0, jsize(body.size()), (const jbyte*)body.data());
    jbyteArray r = (jbyteArray)env->CallStaticObjectMethod(cls, mid, jm, ju, jh, jb, jint(timeoutMs));
    if (env->ExceptionCheck()) { env->ExceptionClear(); out.error = "java exception"; }
    else if (r)
    {
        jsize n = env->GetArrayLength(r);
        std::string all(size_t(n), '\0');
        env->GetByteArrayRegion(r, 0, n, (jbyte*)all.data());
        if (n >= 4)
        {
            out.status = (uint8_t(all[0]) << 24) | (uint8_t(all[1]) << 16) | (uint8_t(all[2]) << 8) | uint8_t(all[3]);
            out.body = all.substr(4);
            if (!out.status) out.error = out.body;
        }
        env->DeleteLocalRef(r);
    }
    env->DeleteLocalRef(jm); env->DeleteLocalRef(ju); env->DeleteLocalRef(jh); env->DeleteLocalRef(jb);
    return out;
}

#else
#include <dlfcn.h>
#include <mutex>

// The few libcurl entry points used (stable ABI since 7.x); constants from curl.h.
namespace
{
typedef void CURL;
struct curl_slist;
enum { CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015, CURLOPT_HTTPHEADER = 10023, CURLOPT_CUSTOMREQUEST = 10036,
       CURLOPT_WRITEFUNCTION = 20011, CURLOPT_WRITEDATA = 10001, CURLOPT_POSTFIELDSIZE = 60, CURLOPT_TIMEOUT_MS = 155,
       CURLOPT_CONNECTTIMEOUT_MS = 156, CURLOPT_NOSIGNAL = 99, CURLOPT_HTTPGET = 80, CURLINFO_RESPONSE_CODE = 0x200002 };
struct Curl
{
    CURL* (*easy_init)();
    int (*easy_setopt)(CURL*, int, ...);
    int (*easy_perform)(CURL*);
    int (*easy_getinfo)(CURL*, int, ...);
    void (*easy_cleanup)(CURL*);
    const char* (*easy_strerror)(int);
    curl_slist* (*slist_append)(curl_slist*, const char*);
    void (*slist_free_all)(curl_slist*);
    int (*global_init)(long);
    bool ok = false;
};
Curl& Lib()
{
    static Curl c;
    static std::once_flag once;
    std::call_once(once, [] {
        void* h = dlopen("libcurl.so.4", RTLD_NOW);
        if (!h) h = dlopen("libcurl.so", RTLD_NOW);
        if (!h) { LogWarn("http: libcurl not found - cloud sync off"); return; }
        c.easy_init = (CURL * (*)()) dlsym(h, "curl_easy_init");
        c.easy_setopt = (int (*)(CURL*, int, ...))dlsym(h, "curl_easy_setopt");
        c.easy_perform = (int (*)(CURL*))dlsym(h, "curl_easy_perform");
        c.easy_getinfo = (int (*)(CURL*, int, ...))dlsym(h, "curl_easy_getinfo");
        c.easy_cleanup = (void (*)(CURL*))dlsym(h, "curl_easy_cleanup");
        c.easy_strerror = (const char* (*)(int))dlsym(h, "curl_easy_strerror");
        c.slist_append = (curl_slist * (*)(curl_slist*, const char*)) dlsym(h, "curl_slist_append");
        c.slist_free_all = (void (*)(curl_slist*))dlsym(h, "curl_slist_free_all");
        c.global_init = (int (*)(long))dlsym(h, "curl_global_init");
        c.ok = c.easy_init && c.easy_setopt && c.easy_perform && c.easy_getinfo && c.easy_cleanup && c.slist_append && c.slist_free_all;
        if (c.ok && c.global_init) c.global_init(3);   // CURL_GLOBAL_ALL
    });
    return c;
}
size_t Write(char* p, size_t s, size_t n, void* user)
{
    static_cast<std::string*>(user)->append(p, s * n);
    return s * n;
}
}

HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, int timeoutMs)
{
    HttpResponse out;
    Curl& c = Lib();
    if (!c.ok) { out.error = "libcurl missing"; return out; }
    CURL* h = c.easy_init();
    if (!h) { out.error = "curl init"; return out; }
    curl_slist* hl = nullptr;
    for (auto& [k, v] : headers) hl = c.slist_append(hl, (k + ": " + v).c_str());
    c.easy_setopt(h, CURLOPT_URL, url.c_str());
    c.easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    c.easy_setopt(h, CURLOPT_TIMEOUT_MS, long(timeoutMs));
    c.easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, long(timeoutMs));
    c.easy_setopt(h, CURLOPT_HTTPHEADER, hl);
    c.easy_setopt(h, CURLOPT_WRITEFUNCTION, (size_t (*)(char*, size_t, size_t, void*))Write);
    c.easy_setopt(h, CURLOPT_WRITEDATA, &out.body);
    if (method == "GET") c.easy_setopt(h, CURLOPT_HTTPGET, 1L);
    else
    {
        c.easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
        c.easy_setopt(h, CURLOPT_POSTFIELDSIZE, long(body.size()));
        c.easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
    }
    int rc = c.easy_perform(h);
    if (rc == 0) { long code = 0; c.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code); out.status = int(code); }
    else out.error = c.easy_strerror ? c.easy_strerror(rc) : "curl error";
    c.slist_free_all(hl);
    c.easy_cleanup(h);
    return out;
}
#endif
