// Minimal HTTPS client for the cloud sync (Supabase REST): libcurl loaded at run time on the
// desktop (no build dependency), the Java HttpURLConnection through JNI on Android.
#pragma once
#include <string>
#include <utility>
#include <vector>

struct HttpResponse
{
    int status = 0;        // HTTP status, 0 = no connection / error
    std::string body;
    std::string error;
};

HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, int timeoutMs = 8000);
