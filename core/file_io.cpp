#include "core/file_io.h"

#include <cstdio>
#include <random>

#include "core/platform.h"
#include "core/strings.h"

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace avc {

std::optional<std::string> readFileBytes(const std::filesystem::path& p) {
    std::FILE* f = openFileUtf8(p, "rb");
    if (!f) return std::nullopt;
    std::string data;
    char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) return std::nullopt;
    return data;
}

namespace {

bool flushFile(std::FILE* f) {
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
    return h != INVALID_HANDLE_VALUE && FlushFileBuffers(h) != 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

std::string randomSuffix() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    return std::to_string(currentProcessId()) + "-" + std::to_string(rng() & 0xffffff);
}

}  // namespace

Status writeFileAtomic(const std::filesystem::path& p, std::string_view data, const AtomicWriteOptions& opt) {
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::filesystem::path tmp = p;
    tmp += pathFromUtf8(".tmp-" + randomSuffix());

    std::FILE* f = openFileUtf8(tmp, "wb");
    if (!f) return Status::error("Cannot create temporary file: " + pathToUtf8(tmp));
    const size_t written = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    bool ok = written == data.size();
    if (ok && opt.flushToDisk) ok = flushFile(f);
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return Status::error("Write failed (disk full?): " + pathToUtf8(tmp));
    }

    if (opt.keepBackup && std::filesystem::exists(p, ec)) {
        std::filesystem::path bak = p;
        bak += ".bak";
        std::filesystem::copy_file(p, bak, std::filesystem::copy_options::overwrite_existing, ec);
    }

#if defined(_WIN32)
    const std::wstring dst = p.wstring();
    const std::wstring src = tmp.wstring();
    BOOL moved = MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!moved) {
        // Destination may be momentarily locked (antivirus / indexer); retry briefly.
        for (int i = 0; i < 10 && !moved; ++i) {
            Sleep(50);
            moved = MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }
    if (!moved) {
        const DWORD err = GetLastError();
        std::filesystem::remove(tmp, ec);
        return Status::error("Atomic replace failed (error " + std::to_string(err) + "): " + pathToUtf8(p));
    }
#else
    if (std::rename(tmp.c_str(), p.c_str()) != 0) {
        std::filesystem::remove(tmp, ec);
        return Status::error("Atomic rename failed: " + pathToUtf8(p));
    }
    if (opt.flushToDisk && p.has_parent_path()) {
        int dfd = ::open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
#endif
    return Status::ok();
}

FileIdentity fileIdentity(const std::filesystem::path& p) {
    FileIdentity id;
    std::error_code ec;
    auto st = std::filesystem::status(p, ec);
    if (ec || !std::filesystem::is_regular_file(st)) return id;
    id.exists = true;
    id.size = std::filesystem::file_size(p, ec);
    auto t = std::filesystem::last_write_time(p, ec);
    if (!ec) id.modifiedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    return id;
}

uint64_t fnv1a64(std::string_view data, uint64_t h) {
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::optional<uint64_t> quickFileHash(const std::filesystem::path& p, size_t sampleBytes) {
    std::FILE* f = openFileUtf8(p, "rb");
    if (!f) return std::nullopt;
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(p, ec);
    std::string buf(sampleBytes, '\0');
    size_t n = std::fread(buf.data(), 1, sampleBytes, f);
    uint64_t h = fnv1a64(std::string_view(buf.data(), n));
    if (size > sampleBytes * 2) {
#if defined(_WIN32)
        _fseeki64(f, static_cast<int64_t>(size - sampleBytes), SEEK_SET);
#else
        fseeko(f, static_cast<off_t>(size - sampleBytes), SEEK_SET);
#endif
        n = std::fread(buf.data(), 1, sampleBytes, f);
        h = fnv1a64(std::string_view(buf.data(), n), h);
    }
    std::fclose(f);
    h ^= size * 0x9E3779B97F4A7C15ull;
    return h;
}

}  // namespace avc
