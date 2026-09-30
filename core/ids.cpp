#include "core/ids.h"

#include <chrono>
#include <mutex>
#include <random>

#include "core/platform.h"
#include "core/strings.h"

namespace avc {

Id newId() {
    static std::mutex m;
    static std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(),
                          static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()),
                          static_cast<unsigned>(currentProcessId())};
        return std::mt19937_64(seq);
    }();
    std::lock_guard lock(m);
    Id id = 0;
    while (id == 0) id = rng();
    return id;
}

std::string idToString(Id id) { return hex64(id); }

Id idFromString(const std::string& s) {
    uint64_t v = 0;
    return parseHex64(s, v) ? v : 0;
}

}  // namespace avc
