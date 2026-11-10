#include "account.h"
#include "http.h"
#include "json_mini.h"
#include "log.h"
#include "ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

using namespace json_mini;

namespace
{
const char* kFile = "account.txt";
std::recursive_mutex s_lock;
std::string s_url, s_key, s_username, s_refresh, s_token;
std::chrono::steady_clock::time_point s_tokenTime;
bool s_skip = false;

void Load()
{
    FILE* f = fopen(kFile, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f))
    {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (!l.compare(0, 9, "username=")) s_username = l.substr(9);
        else if (!l.compare(0, 8, "refresh=")) s_refresh = l.substr(8);
        else if (l == "skip=1") s_skip = true;
    }
    fclose(f);
}

void Save()
{
    FILE* f = fopen(kFile, "w");
    if (!f) { LogWarn("account: can't write %s", kFile); return; }
    fprintf(f, "# BO3 Recomp online account (delete this file to sign out / be asked again)\n");
    if (s_skip) fprintf(f, "skip=1\n");
    else fprintf(f, "username=%s\nrefresh=%s\n", s_username.c_str(), s_refresh.c_str());
    fclose(f);
}

std::vector<std::pair<std::string, std::string>> H(const std::string& bearer = "")
{
    std::vector<std::pair<std::string, std::string>> h = { { "apikey", s_key }, { "Content-Type", "application/json" } };
    if (!bearer.empty()) h.push_back({ "Authorization", "Bearer " + bearer });
    return h;
}

bool TakeSession(const HttpResponse& r)
{
    std::string t = JsonField(r.body, "access_token"), rf = JsonField(r.body, "refresh_token");
    if (r.status != 200 || t.empty()) return false;
    s_token = t;
    s_refresh = rf;
    s_tokenTime = std::chrono::steady_clock::now();
    return true;
}

// Supabase error text of a reply (for the dialogs)
std::string ErrorText(const HttpResponse& r)
{
    if (!r.status) return "Sin conexión: " + r.error;
    for (const char* k : { "msg", "error_description", "message", "error" })
    {
        std::string v = JsonField(r.body, k);
        if (!v.empty()) return v;
    }
    return "Error " + std::to_string(r.status);
}

bool PasswordSignIn(const std::string& email, const std::string& password)
{
    HttpResponse r = HttpRequest("POST", s_url + "/auth/v1/token?grant_type=password", H(),
                                 "{\"email\":\"" + JsonEscape(email) + "\",\"password\":\"" + JsonEscape(password) + "\"}");
    return TakeSession(r);
}

bool ValidName(const std::string& n)
{
    if (n.size() < 3 || n.size() > 15) return false;
    return std::all_of(n.begin(), n.end(), [](char c) { return isalnum((unsigned char)c) || c == '_'; });
}

std::string Trim(std::string s)
{
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

bool CreateAccount()
{
    std::string err;
    for (;;)
    {
        std::vector<UiField> f = { { "Nombre de usuario (3-15: letras, números, _)" }, { "Contraseña (mínimo 6)", true },
                                   { "Repetir contraseña", true }, { "Email (para recuperar la contraseña)" } };
        if (!UiForm("Crear cuenta - BO3 Online", (err.empty() ? std::string() : err + "\n\n") +
                    "El nombre de usuario es tu nombre en el juego y no se puede repetir.", f)) return false;
        std::string name = Trim(f[0].value), pass = f[1].value, email = Trim(f[3].value);
        if (!ValidName(name)) { err = "Nombre inválido: entre 3 y 15 caracteres, solo letras, números y _."; continue; }
        if (pass.size() < 6) { err = "La contraseña tiene que tener al menos 6 caracteres."; continue; }
        if (pass != f[2].value) { err = "Las contraseñas no coinciden."; continue; }
        if (email.find('@') == std::string::npos || email.find('.') == std::string::npos) { err = "Email inválido."; continue; }
        HttpResponse av = HttpRequest("POST", s_url + "/rest/v1/rpc/username_available", H(), "{\"p_username\":\"" + JsonEscape(name) + "\"}");
        if (av.status != 200) { err = ErrorText(av); continue; }
        if (av.body.find("true") == std::string::npos) { err = "El nombre \"" + name + "\" ya está en uso."; continue; }
        HttpResponse su = HttpRequest("POST", s_url + "/auth/v1/signup", H(),
                                      "{\"email\":\"" + JsonEscape(email) + "\",\"password\":\"" + JsonEscape(pass) + "\",\"data\":{\"username\":\"" + JsonEscape(name) + "\"}}");
        if (!TakeSession(su))
        {
            err = su.status == 200 ? std::string("La cuenta necesita confirmar el email (desactivá \"Confirm email\" en Supabase).") : ErrorText(su);
            if (err.find("already") != std::string::npos) err = "Ese email ya tiene una cuenta: usá \"Ya tengo cuenta\".";
            continue;
        }
        HttpResponse pr = HttpRequest("POST", s_url + "/rest/v1/profiles", H(s_token), "{\"username\":\"" + JsonEscape(name) + "\"}");
        if (pr.status < 200 || pr.status >= 300)
        {
            err = pr.status == 409 ? "El nombre \"" + name + "\" ya está en uso." : ErrorText(pr);
            s_token.clear();
            continue;
        }
        s_username = name;
        Log("account: created \"%s\"", name.c_str());
        UiMessage("BO3 Online", "Cuenta creada. ¡Bienvenido, " + name + "!");
        return true;
    }
}

bool SignIn()
{
    std::string err;
    for (;;)
    {
        std::vector<UiField> f = { { "Nombre de usuario" }, { "Contraseña", true } };
        if (!UiForm("Iniciar sesión - BO3 Online", err.empty() ? "Tu nombre de usuario y contraseña." : err, f)) return false;
        std::string name = Trim(f[0].value);
        HttpResponse r = HttpRequest("POST", s_url + "/rest/v1/rpc/resolve_login", H(),
                                     "{\"p_username\":\"" + JsonEscape(name) + "\",\"p_password\":\"" + JsonEscape(f[1].value) + "\"}");
        if (r.status != 200) { err = ErrorText(r); continue; }
        std::string email = r.body;
        email.erase(std::remove(email.begin(), email.end(), '"'), email.end());
        email = Trim(email);
        if (email.empty() || email == "null") { err = "Usuario o contraseña incorrectos."; continue; }
        if (!PasswordSignIn(email, f[1].value)) { err = "No se pudo iniciar sesión, probá de nuevo."; continue; }
        // the exact spelling of the name comes from the profile
        HttpResponse pr = HttpRequest("GET", s_url + "/rest/v1/profiles?select=username", H(s_token), "");
        std::string n = JsonField(pr.body, "username");
        s_username = n.empty() ? name : n;
        Log("account: signed in as \"%s\"", s_username.c_str());
        return true;
    }
}

// Recovery by code: Supabase mails a code (the "Reset Password" template must contain {{ .Token }}),
// which signs the user in so the new password can be set here.
bool Recover()
{
    std::vector<UiField> f = { { "Email de la cuenta" } };
    if (!UiForm("Recuperar contraseña", "Te vamos a mandar un código por email.", f)) return false;
    std::string email = Trim(f[0].value);
    HttpResponse r = HttpRequest("POST", s_url + "/auth/v1/recover", H(), "{\"email\":\"" + JsonEscape(email) + "\"}");
    if (r.status != 200) { UiMessage("Recuperar contraseña", "No se pudo enviar el email: " + ErrorText(r)); return false; }
    std::string err;
    for (;;)
    {
        std::vector<UiField> g = { { "Código del email" }, { "Contraseña nueva (mínimo 6)", true }, { "Repetir contraseña", true } };
        if (!UiForm("Recuperar contraseña", (err.empty() ? std::string() : err + "\n\n") + "Revisá tu email (también la carpeta de spam).", g)) return false;
        if (g[1].value.size() < 6) { err = "La contraseña tiene que tener al menos 6 caracteres."; continue; }
        if (g[1].value != g[2].value) { err = "Las contraseñas no coinciden."; continue; }
        HttpResponse v = HttpRequest("POST", s_url + "/auth/v1/verify", H(),
                                     "{\"type\":\"recovery\",\"email\":\"" + JsonEscape(email) + "\",\"token\":\"" + JsonEscape(Trim(g[0].value)) + "\"}");
        if (!TakeSession(v)) { err = "Código incorrecto o vencido."; continue; }
        HttpResponse u = HttpRequest("PUT", s_url + "/auth/v1/user", H(s_token), "{\"password\":\"" + JsonEscape(g[1].value) + "\"}");
        if (u.status != 200) { err = ErrorText(u); continue; }
        HttpResponse pr = HttpRequest("GET", s_url + "/rest/v1/profiles?select=username", H(s_token), "");
        s_username = JsonField(pr.body, "username");
        UiMessage("Recuperar contraseña", "Listo, contraseña cambiada." + (s_username.empty() ? std::string() : " Usuario: " + s_username));
        return true;
    }
}

bool Refresh()
{
    if (s_refresh.empty()) return false;
    HttpResponse r = HttpRequest("POST", s_url + "/auth/v1/token?grant_type=refresh_token", H(), "{\"refresh_token\":\"" + JsonEscape(s_refresh) + "\"}");
    if (!TakeSession(r)) { LogWarn("account: session refresh failed (%d) %s", r.status, ErrorText(r).c_str()); return false; }
    Save();   // refresh tokens rotate
    return true;
}
}

bool AccountInit(const std::string& url, const std::string& key)
{
    std::lock_guard g(s_lock);
    s_url = url;
    s_key = key;
    Load();
    if (s_skip) return false;
    if (Refresh()) { Log("account: \"%s\" signed in", s_username.c_str()); return true; }
    for (;;)
    {
        int c = UiChoice("BO3 Online",
                         "Para el modo online y guardar tu progreso en la nube necesitás una cuenta.\n\n"
                         "El nombre de usuario es tu nombre en el juego.",
                         { "Crear cuenta", "Ya tengo cuenta", "Olvidé mi contraseña", "Ahora no" });
        bool ok = c == 0 ? CreateAccount() : c == 1 ? SignIn() : c == 2 ? Recover() : false;
        if (ok) { s_skip = false; Save(); return true; }
        if (c == 3) { s_skip = true; Save(); Log("account: skipped (delete account.txt to be asked again)"); return false; }
        if (c < 0) { Log("account: dialog closed - no account this time"); return false; }   // back: ask again next launch
    }
}

std::string AccountToken()
{
    std::lock_guard g(s_lock);
    if (s_token.empty() && s_refresh.empty()) return {};
    if (s_token.empty() || std::chrono::steady_clock::now() - s_tokenTime > std::chrono::minutes(50)) Refresh();
    return s_token;
}

std::string AccountUsername()
{
    std::lock_guard g(s_lock);
    return s_username;
}
