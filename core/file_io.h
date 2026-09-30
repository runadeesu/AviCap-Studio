#pragma once
// Crash-safe file primitives.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.h"

namespace avc {

std::optional<std::string> readFileBytes(const std::filesystem::path& p);

struct AtomicWriteOptions {
    bool keepBackup = false;  // keep previous version as "<name>.bak"
    bool flushToDisk = true;  // FlushFileBuffers / fsync before replacing
};

// Writes to a temporary file in the same directory, flushes it to disk and then
// atomically replaces the destination. A crash at any point leaves either the
// old or the new complete file, never a truncated one.
Status writeFileAtomic(const std::filesystem::path& p, std::string_view data, const AtomicWriteOptions& opt = {});

// Identity used by caches to detect changed sources.
struct FileIdentity {
    uint64_t size = 0;
    int64_t modifiedNs = 0;  // last write time, ns since the file clock epoch
    bool exists = false;
    bool operator==(const FileIdentity&) const = default;
};
FileIdentity fileIdentity(const std::filesystem::path& p);

// FNV-1a 64-bit (fast, non-cryptographic) for cache keys.
uint64_t fnv1a64(std::string_view data, uint64_t seed = 1469598103934665603ull);
// Hash of the first and last `sampleBytes` of a file plus its size (quick content fingerprint).
std::optional<uint64_t> quickFileHash(const std::filesystem::path& p, size_t sampleBytes = 64 * 1024);

}  // namespace avc
