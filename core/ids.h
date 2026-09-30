#pragma once
// Stable 64-bit identifiers for project entities (clips, tracks, media, ...).

#include <cstdint>
#include <string>

namespace avc {

using Id = uint64_t;
inline constexpr Id kInvalidId = 0;

// Random, non-zero, thread-safe.
Id newId();
std::string idToString(Id id);   // 16 hex digits
Id idFromString(const std::string& s);  // 0 on error

}  // namespace avc
