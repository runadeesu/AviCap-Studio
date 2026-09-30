#include "core/time.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace avc {

namespace {

#if defined(__SIZEOF_INT128__)
using i128 = __int128;

int64_t mulDivImpl(int64_t a, int64_t b, int64_t c, Rounding r) noexcept {
    i128 p = static_cast<i128>(a) * static_cast<i128>(b);
    i128 q = p / c;
    i128 rem = p % c;
    if (rem != 0) {
        const bool negative = (rem < 0) != (c < 0);  // sign of the exact quotient
        switch (r) {
        case Rounding::Down:
            if (negative) q -= 1;
            break;
        case Rounding::Up:
            if (!negative) q += 1;
            break;
        case Rounding::Nearest: {
            i128 ar = rem < 0 ? -rem : rem;
            i128 ac = c < 0 ? -static_cast<i128>(c) : static_cast<i128>(c);
            if (ar * 2 >= ac) q += negative ? -1 : 1;
            break;
        }
        }
    }
    constexpr i128 kMax = std::numeric_limits<int64_t>::max();
    constexpr i128 kMin = std::numeric_limits<int64_t>::min();
    if (q > kMax) return std::numeric_limits<int64_t>::max();
    if (q < kMin) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(q);
}
#else
// Portable path (MSVC): unsigned 128-bit arithmetic on two 64-bit halves.
struct U128 {
    uint64_t hi = 0, lo = 0;
};

U128 mul64(uint64_t a, uint64_t b) {
    const uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    U128 r;
    r.lo = (mid << 32) | (p00 & 0xffffffffu);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

// Divides n by d, returns quotient (saturated to 2^64-1) and remainder.
uint64_t div128(U128 n, uint64_t d, uint64_t& rem, bool& overflow) {
    overflow = n.hi >= d;
    uint64_t q = 0, r = 0;
    for (int i = 127; i >= 0; --i) {
        const bool carry = (r >> 63) != 0;
        r <<= 1;
        const uint64_t bit = i >= 64 ? (n.hi >> (i - 64)) & 1u : (n.lo >> i) & 1u;
        r |= bit;
        if (carry || r >= d) {
            r -= d;
            if (i < 64) q |= (uint64_t{1} << i);
        }
    }
    rem = r;
    return q;
}

int64_t mulDivImpl(int64_t a, int64_t b, int64_t c, Rounding rnd) noexcept {
    const bool negative = ((a < 0) != (b < 0)) != (c < 0);
    auto uabs = [](int64_t v) { return v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v); };
    U128 p = mul64(uabs(a), uabs(b));
    const uint64_t ac = uabs(c);
    uint64_t rem = 0;
    bool overflow = false;
    uint64_t q = div128(p, ac, rem, overflow);
    if ((p.hi == 0 && p.lo == 0) || (!overflow && rem == 0)) {
        // exact
    } else if (!overflow) {
        switch (rnd) {
        case Rounding::Down:
            if (negative) q += 1;  // magnitude grows toward -inf
            break;
        case Rounding::Up:
            if (!negative) q += 1;
            break;
        case Rounding::Nearest:
            if (rem >= ac - rem) q += 1;
            break;
        }
    }
    constexpr uint64_t kMaxPos = uint64_t(std::numeric_limits<int64_t>::max());
    if (overflow || q > kMaxPos + (negative ? 1u : 0u)) {
        return negative ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    }
    return negative ? static_cast<int64_t>(uint64_t(0) - q) : static_cast<int64_t>(q);
}
#endif

}  // namespace

int64_t mulDiv(int64_t a, int64_t b, int64_t c, Rounding r) noexcept {
    if (c == 0) return 0;
    return mulDivImpl(a, b, c, r);
}

int64_t gcd64(int64_t a, int64_t b) noexcept {
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b != 0) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

Rational Rational::reduced() const noexcept {
    if (den == 0) return *this;
    int64_t g = gcd64(num, den);
    if (g == 0) return {0, 1};
    Rational r{num / g, den / g};
    if (r.den < 0) {
        r.num = -r.num;
        r.den = -r.den;
    }
    return r;
}

std::string Rational::toString() const {
    Rational r = reduced();
    if (r.den == 1) return std::to_string(r.num);
    return std::to_string(r.num) + "/" + std::to_string(r.den);
}

Rational Rational::fromFrameRate(double fps) noexcept {
    if (!(fps > 0.0) || !std::isfinite(fps)) return {30, 1};
    // NTSC family: N * 1000 / 1001.
    for (int base : {24, 30, 48, 60, 120, 240}) {
        const double ntsc = base * 1000.0 / 1001.0;
        if (std::fabs(fps - ntsc) < 0.005) return {base * 1000LL, 1001};
    }
    const double rounded = std::round(fps);
    if (std::fabs(fps - rounded) < 0.001) return {static_cast<int64_t>(rounded), 1};
    // Generic: keep three decimals.
    return Rational{static_cast<int64_t>(std::llround(fps * 1000.0)), 1000}.reduced();
}

std::optional<Rational> Rational::parse(std::string_view s) noexcept {
    std::string str(s);
    const auto slash = str.find('/');
    char* end = nullptr;
    if (slash == std::string::npos) {
        const double v = std::strtod(str.c_str(), &end);
        if (end == str.c_str() || !(v > 0)) return std::nullopt;
        return fromFrameRate(v);
    }
    const long long n = std::strtoll(str.substr(0, slash).c_str(), &end, 10);
    const long long d = std::strtoll(str.substr(slash + 1).c_str(), &end, 10);
    if (d == 0) return std::nullopt;
    return Rational{n, d}.reduced();
}

Time Time::fromSeconds(double s) noexcept {
    return Time{static_cast<int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))};
}

Time Time::fromFrames(int64_t frame, Rational fps) noexcept {
    if (fps.num == 0) return Time{0};
    // frame / fps seconds == frame * den / num seconds
    const int64_t perFrameNum = mulDiv(kTicksPerSecond, fps.den, 1, Rounding::Down);  // ticks * den
    return Time{mulDiv(frame, perFrameNum, fps.num, Rounding::Nearest)};
}

Time Time::fromSamples(int64_t samples, int sampleRate) noexcept {
    if (sampleRate <= 0) return Time{0};
    return Time{mulDiv(samples, kTicksPerSecond, sampleRate, Rounding::Nearest)};
}

Time Time::fromTimebase(int64_t value, Rational tb) noexcept {
    if (tb.den == 0) return Time{0};
    const int64_t scaled = mulDiv(kTicksPerSecond, tb.num, 1, Rounding::Down);
    return Time{mulDiv(value, scaled, tb.den, Rounding::Nearest)};
}

int64_t Time::toFrames(Rational fps, Rounding r) const noexcept {
    if (fps.den == 0) return 0;
    // ticks * num / (den * kTicks)
    const int64_t denom = mulDiv(fps.den, kTicksPerSecond, 1, Rounding::Down);
    return mulDiv(ticks, fps.num, denom, r);
}

int64_t Time::toSamples(int sampleRate, Rounding r) const noexcept {
    return mulDiv(ticks, sampleRate, kTicksPerSecond, r);
}

int64_t Time::toTimebase(Rational tb, Rounding r) const noexcept {
    if (tb.num == 0) return 0;
    const int64_t denom = mulDiv(tb.num, kTicksPerSecond, 1, Rounding::Down);
    return mulDiv(ticks, tb.den, denom, r);
}

bool isDropFrameRate(Rational fps) noexcept {
    Rational r = fps.reduced();
    return r.den == 1001 && (r.num == 30000 || r.num == 60000);
}

namespace {

int nominalFps(Rational fps) {
    return static_cast<int>(std::lround(fps.toDouble()));
}

}  // namespace

std::string formatTimecode(Time t, Rational fps, bool dropFrame) {
    const bool negative = t.ticks < 0;
    if (negative) t = -t;
    int64_t frame = t.toFrames(fps, Rounding::Down);
    const int nominal = std::max(1, nominalFps(fps));
    const bool df = dropFrame && isDropFrameRate(fps);
    if (df) {
        const int64_t dropFrames = nominal / 15;  // 2 for 29.97, 4 for 59.94
        const int64_t framesPerMinute = nominal * 60 - dropFrames;
        const int64_t framesPer10Minutes = framesPerMinute * 10 + dropFrames;
        const int64_t d = frame / framesPer10Minutes;
        const int64_t m = frame % framesPer10Minutes;
        if (m > dropFrames)
            frame += dropFrames * 9 * d + dropFrames * ((m - dropFrames) / framesPerMinute);
        else
            frame += dropFrames * 9 * d;
    }
    const int64_t ff = frame % nominal;
    const int64_t totalSeconds = frame / nominal;
    const int64_t ss = totalSeconds % 60;
    const int64_t mm = (totalSeconds / 60) % 60;
    const int64_t hh = totalSeconds / 3600;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld%c%02lld", negative ? "-" : "", static_cast<long long>(hh),
                  static_cast<long long>(mm), static_cast<long long>(ss), df ? ';' : ':', static_cast<long long>(ff));
    return buf;
}

std::string formatTime(Time t, Rational fps, TimecodeStyle style, int sampleRate) {
    switch (style) {
    case TimecodeStyle::Timecode:
        return formatTimecode(t, fps, true);
    case TimecodeStyle::Frames:
        return std::to_string(t.toFrames(fps, Rounding::Down));
    case TimecodeStyle::Samples:
        return std::to_string(t.toSamples(sampleRate, Rounding::Down));
    case TimecodeStyle::Seconds: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", t.seconds());
        return buf;
    }
    }
    return {};
}

std::optional<Time> parseTimecode(std::string_view input, Rational fps, bool dropFrame) {
    std::string s;
    for (char c : input)
        if (c != ' ') s.push_back(c);
    if (s.empty()) return std::nullopt;
    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        s.erase(0, 1);
    } else if (s[0] == '+') {
        s.erase(0, 1);
    }
    std::vector<int64_t> parts;
    bool sawSemicolon = false;
    std::string cur;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            cur.push_back(c);
        } else if (c == ':' || c == ';' || c == '.') {
            if (c == ';') sawSemicolon = true;
            parts.push_back(cur.empty() ? 0 : std::stoll(cur));
            cur.clear();
        } else {
            return std::nullopt;
        }
    }
    parts.push_back(cur.empty() ? 0 : std::stoll(cur));
    if (parts.size() > 4) return std::nullopt;
    const int nominal = std::max(1, nominalFps(fps));
    int64_t frame = 0;
    if (parts.size() == 1) {
        frame = parts[0];
    } else {
        while (parts.size() < 4) parts.insert(parts.begin(), 0);
        const int64_t hh = parts[0], mm = parts[1], ss = parts[2], ff = parts[3];
        frame = ((hh * 60 + mm) * 60 + ss) * nominal + ff;
        if ((dropFrame || sawSemicolon) && isDropFrameRate(fps)) {
            const int64_t dropFrames = nominal / 15;
            const int64_t totalMinutes = hh * 60 + mm;
            frame -= dropFrames * (totalMinutes - totalMinutes / 10);
        }
    }
    Time t = Time::fromFrames(frame, fps);
    return negative ? -t : t;
}

std::string formatDuration(Time t) {
    const bool negative = t.ticks < 0;
    if (negative) t = -t;
    const double secs = t.seconds();
    const int64_t whole = static_cast<int64_t>(secs);
    const int64_t h = whole / 3600, m = (whole / 60) % 60;
    const double s = secs - static_cast<double>(h * 3600 + m * 60);
    char buf[48];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%s%lld:%02lld:%04.1f", negative ? "-" : "", static_cast<long long>(h),
                      static_cast<long long>(m), s);
    else
        std::snprintf(buf, sizeof(buf), "%s%lld:%04.1f", negative ? "-" : "", static_cast<long long>(m), s);
    return buf;
}

}  // namespace avc
