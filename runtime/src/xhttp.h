// Local HTTP endpoint behind the emulated XHTTP API (see xhttp.cpp).
#pragma once
#include <cstdint>
#include <string>

struct HttpLocalRequest { std::string method, host, path, headers, body; uint16_t port = 0; bool secure = false; };
struct HttpLocalResponse { int status = 404; std::string contentType = "application/json", body; };

HttpLocalResponse HttpLocalHandle(const HttpLocalRequest& req);   // dw_server.cpp
