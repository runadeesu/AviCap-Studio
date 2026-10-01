#pragma once
// Minimal HTTPS client and per-user secret storage for opt-in cloud features.
//
// Nothing in the editor uses the network unless the user enables a cloud
// feature. Requests go only to the URL the caller passes (TLS verified by the
// OS); secrets are encrypted for the current Windows user with DPAPI.

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/jobs.h"
#include "core/result.h"

namespace avc {

struct HttpRequest {
    std::string method = "POST";
    std::string url;  // https://host/path
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeoutMs = 120000;
};

struct HttpResponse {
    int status = 0;     // 0 = no response (see error)
    std::string body;
    std::string error;  // transport error (DNS, TLS, timeout, cancelled)
};

using HttpTransport = std::function<HttpResponse(const HttpRequest&, const CancelToken&)>;

// WinHTTP on Windows (system proxy settings honoured). On other platforms
// it returns an error response: cloud features are Windows-only.
HttpResponse httpRequest(const HttpRequest& req, const CancelToken& cancel = {});
bool httpAvailable();

// Encrypts `secret` for the current user (DPAPI) and writes it atomically.
Status saveUserSecret(const std::filesystem::path& file, const std::string& secret);
Result<std::string> loadUserSecret(const std::filesystem::path& file);
bool secretStoreAvailable();

}  // namespace avc
