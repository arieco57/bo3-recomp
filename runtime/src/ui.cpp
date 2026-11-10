#include "ui.h"
#include "gpu/vk.h"
#include "log.h"

#include <SDL3/SDL.h>

int UiChoice(const std::string& title, const std::string& text, const std::vector<std::string>& buttons)
{
    std::vector<SDL_MessageBoxButtonData> bd;
    for (size_t i = 0; i < buttons.size(); i++) bd.push_back({ 0, int(i), buttons[i].c_str() });
    SDL_MessageBoxData md{};
    md.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    md.window = g_vk.window;
    md.title = title.c_str();
    md.message = text.c_str();
    md.numbuttons = int(bd.size());
    md.buttons = bd.data();
    int id = -1;
    if (!SDL_ShowMessageBox(&md, &id)) { LogWarn("ui: message box failed: %s", SDL_GetError()); return -1; }
    return id;
}

void UiMessage(const std::string& title, const std::string& text) { UiChoice(title, text, { "OK" }); }

#ifdef __ANDROID__
#include <jni.h>
#include <SDL3/SDL_system.h>

// com.bo3recomp.Dialogs.form(title, text, labels[], secret[]) -> String[] values, or null if cancelled
bool UiForm(const std::string& title, const std::string& text, std::vector<UiField>& fields)
{
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    if (!env) return false;
    jclass cls = env->FindClass("com/bo3recomp/Dialogs");
    if (!cls) { env->ExceptionClear(); LogWarn("ui: no Dialogs class"); return false; }
    jmethodID mid = env->GetStaticMethodID(cls, "form", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[Z)[Ljava/lang/String;");
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray labels = env->NewObjectArray(jsize(fields.size()), strCls, nullptr);
    jbooleanArray secret = env->NewBooleanArray(jsize(fields.size()));
    for (size_t i = 0; i < fields.size(); i++)
    {
        jstring l = env->NewStringUTF(fields[i].label.c_str());
        env->SetObjectArrayElement(labels, jsize(i), l);
        env->DeleteLocalRef(l);
        jboolean b = fields[i].secret;
        env->SetBooleanArrayRegion(secret, jsize(i), 1, &b);
    }
    jstring jt = env->NewStringUTF(title.c_str()), jx = env->NewStringUTF(text.c_str());
    jobjectArray r = (jobjectArray)env->CallStaticObjectMethod(cls, mid, jt, jx, labels, secret);
    bool ok = false;
    if (env->ExceptionCheck()) env->ExceptionClear();
    else if (r)
    {
        for (size_t i = 0; i < fields.size() && i < size_t(env->GetArrayLength(r)); i++)
        {
            jstring v = (jstring)env->GetObjectArrayElement(r, jsize(i));
            const char* c = v ? env->GetStringUTFChars(v, nullptr) : nullptr;
            fields[i].value = c ? c : "";
            if (c) env->ReleaseStringUTFChars(v, c);
            if (v) env->DeleteLocalRef(v);
        }
        ok = true;
        env->DeleteLocalRef(r);
    }
    env->DeleteLocalRef(jt); env->DeleteLocalRef(jx); env->DeleteLocalRef(labels); env->DeleteLocalRef(secret); env->DeleteLocalRef(cls);
    return ok;
}

#else
#include <cstdio>
#include <sys/wait.h>

namespace
{
std::string Quote(const std::string& s)
{
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}
}

// zenity --forms: the values come back on one line, separated by the separator
bool UiForm(const std::string& title, const std::string& text, std::vector<UiField>& fields)
{
    const std::string sep = "\x1f";
    std::string cmd = "zenity --forms --width=420 --title=" + Quote(title) + " --text=" + Quote(text) + " --separator=" + Quote(sep);
    for (auto& f : fields) cmd += (f.secret ? " --add-password=" : " --add-entry=") + Quote(f.label);
    cmd += " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    int rc = pclose(p);
    if (rc != 0) return false;   // cancelled (or no zenity)
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    size_t pos = 0;
    for (auto& f : fields)
    {
        size_t e = out.find(sep, pos);
        f.value = out.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? out.size() : e + sep.size();
    }
    return true;
}
#endif
