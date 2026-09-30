#include "core/strings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace avc {

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    std::u8string u8(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(u8);
}

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

#if defined(_WIN32)
std::wstring utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
#endif

std::string toLower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool startsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool endsWith(std::string_view s, std::string_view x) {
    return s.size() >= x.size() && s.substr(s.size() - x.size()) == x;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool icontains(std::string_view h, std::string_view n) {
    if (n.empty()) return true;
    auto it = std::search(h.begin(), h.end(), n.begin(), n.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != h.end();
}

std::vector<std::string> split(std::string_view s, char sep, bool skipEmpty) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) pos = s.size();
        std::string_view part = s.substr(start, pos - start);
        if (!skipEmpty || !part.empty()) out.emplace_back(part);
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string r;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) r += sep;
        r += parts[i];
    }
    return r;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string formatBytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool parseHex64(std::string_view s, uint64_t& out) {
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    out = v;
    return true;
}

std::u32string utf8ToUtf32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            len = 4;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string utf32ToUtf8(std::u32string_view s) {
    std::string out;
    for (char32_t cp : s) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

size_t utf8Length(std::string_view s) {
    size_t n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

std::string sanitizeFileName(std::string_view name) {
    std::string r;
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
            c == '?' || c == '*')
            r.push_back('_');
        else
            r.push_back(c);
    }
    while (!r.empty() && (r.back() == '.' || r.back() == ' ')) r.pop_back();
    if (r.empty()) r = "untitled";
    return r;
}

}  // namespace avc
