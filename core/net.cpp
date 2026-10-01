#include "core/net.h"

#include <cstring>
#include <system_error>

#include "core/file_io.h"
#include "core/log.h"
#include "core/strings.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#include <winhttp.h>
#endif

namespace avc {

#if defined(_WIN32)

namespace {

struct HInternet {
    HINTERNET h = nullptr;
    HInternet() = default;
    explicit HInternet(HINTERNET x) : h(x) {}
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

std::string lastErrorText(const char* what) {
    const DWORD e = GetLastError();
    return std::string(what) + " failed (error " + std::to_string(e) + ")";
}

}  // namespace

bool httpAvailable() { return true; }

HttpResponse httpRequest(const HttpRequest& req, const CancelToken& cancel) {
    HttpResponse out;
    const std::wstring url = utf8ToWide(req.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        out.error = "invalid URL (https only): " + req.url;
        return out;
    }
    HInternet session(WinHttpOpen(L"AviCapStudio/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        // Older Windows without automatic proxy support.
        session.h = WinHttpOpen(L"AviCapStudio/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        out.error = lastErrorText("WinHttpOpen");
        return out;
    }
    WinHttpSetTimeouts(session.h, 10000, 15000, req.timeoutMs, req.timeoutMs);
    HInternet connect(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connect) {
        out.error = lastErrorText("WinHttpConnect");
        return out;
    }
    HInternet request(WinHttpOpenRequest(connect.h, utf8ToWide(req.method).c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        out.error = lastErrorText("WinHttpOpenRequest");
        return out;
    }
    std::wstring headers;
    for (const auto& [k, v] : req.headers) headers += utf8ToWide(k) + L": " + utf8ToWide(v) + L"\r\n";
    if (cancel.cancelled()) {
        out.error = "cancelled";
        return out;
    }
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), const_cast<char*>(req.body.data()),
                            static_cast<DWORD>(req.body.size()), static_cast<DWORD>(req.body.size()), 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        out.error = lastErrorText("HTTPS request");
        return out;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);
    for (;;) {
        if (cancel.cancelled()) {
            out.error = "cancelled";
            out.status = 0;
            return out;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) {
            out.error = lastErrorText("WinHttpQueryDataAvailable");
            return out;
        }
        if (avail == 0) break;
        const size_t old = out.body.size();
        out.body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, out.body.data() + old, avail, &read)) {
            out.error = lastErrorText("WinHttpReadData");
            return out;
        }
        out.body.resize(old + read);
        if (out.body.size() > (64u << 20)) {
            out.error = "response too large";
            return out;
        }
    }
    return out;
}

bool secretStoreAvailable() { return true; }

Status saveUserSecret(const std::filesystem::path& file, const std::string& secret) {
    DATA_BLOB in{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"AviCap Studio", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return Status::error(lastErrorText("CryptProtectData"));
    std::string blob(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, blob);
}

Result<std::string> loadUserSecret(const std::filesystem::path& file) {
    auto blob = readFileBytes(file);
    if (!blob) return Result<std::string>::error("no stored secret");
    DATA_BLOB in{static_cast<DWORD>(blob->size()), reinterpret_cast<BYTE*>(blob->data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return Result<std::string>::error(lastErrorText("CryptUnprotectData"));
    std::string s(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

#else  // !_WIN32

bool httpAvailable() { return false; }

HttpResponse httpRequest(const HttpRequest&, const CancelToken&) {
    HttpResponse r;
    r.error = "HTTPS is only available in the Windows build";
    return r;
}

bool secretStoreAvailable() { return false; }

Status saveUserSecret(const std::filesystem::path&, const std::string&) {
    return Status::error("secure key storage is only available on Windows");
}

Result<std::string> loadUserSecret(const std::filesystem::path&) { return Result<std::string>::error("no stored secret"); }

#endif

}  // namespace avc
