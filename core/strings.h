#pragma once
// String helpers. All std::string values in AviCap are UTF-8.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace avc {

// Paths must never be built from UTF-8 std::string directly (on Windows that
// would use the ANSI code page); use these helpers.
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& p);

#if defined(_WIN32)
std::wstring utf8ToWide(std::string_view s);
std::string wideToUtf8(std::wstring_view s);
#endif

std::string toLower(std::string_view s);  // ASCII only
std::string trim(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool iequals(std::string_view a, std::string_view b);  // ASCII case-insensitive
bool icontains(std::string_view haystack, std::string_view needle);
std::vector<std::string> split(std::string_view s, char sep, bool skipEmpty = true);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);
std::string formatBytes(uint64_t bytes);
std::string hex64(uint64_t v);
bool parseHex64(std::string_view s, uint64_t& out);
// Decodes UTF-8 into code points (invalid bytes become U+FFFD).
std::u32string utf8ToUtf32(std::string_view s);
std::string utf32ToUtf8(std::u32string_view s);
// Number of code points.
size_t utf8Length(std::string_view s);
// Sanitizes a string for use as a file name component.
std::string sanitizeFileName(std::string_view name);

}  // namespace avc
