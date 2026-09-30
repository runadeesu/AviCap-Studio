#pragma once
// Exact time model for editing.
//
// All timeline positions are integer "ticks". One second is 254,016,000,000
// ticks, a number chosen so that every common frame duration (23.976, 24, 25,
// 29.97, 30, 48, 50, 59.94, 60, 120 fps) and every common audio sample period
// (8k..192k, 44.1k family) is an exact integer number of ticks. Editing math is
// therefore exact; floating point seconds are only used for display.

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace avc {

enum class Rounding { Down, Up, Nearest };

// Computes a * b / c without intermediate overflow (128-bit), with rounding.
// c must be non-zero. Result saturates at int64 limits.
int64_t mulDiv(int64_t a, int64_t b, int64_t c, Rounding r = Rounding::Nearest) noexcept;

int64_t gcd64(int64_t a, int64_t b) noexcept;

struct Rational {
    int64_t num = 0;
    int64_t den = 1;

    constexpr Rational() = default;
    constexpr Rational(int64_t n, int64_t d = 1) : num(n), den(d) {}

    [[nodiscard]] Rational reduced() const noexcept;
    [[nodiscard]] double toDouble() const noexcept { return den ? double(num) / double(den) : 0.0; }
    [[nodiscard]] bool valid() const noexcept { return den != 0 && num != 0; }
    [[nodiscard]] Rational inverse() const noexcept { return {den, num}; }
    [[nodiscard]] std::string toString() const;  // "30000/1001"

    // Snaps approximate rates such as 29.97 to the exact NTSC rational.
    static Rational fromFrameRate(double fps) noexcept;
    static std::optional<Rational> parse(std::string_view s) noexcept;  // "30000/1001" or "25"

    friend bool operator==(const Rational& a, const Rational& b) noexcept {
        // Compare by value, not representation.
        return mulDiv(a.num, b.den, 1, Rounding::Down) == mulDiv(b.num, a.den, 1, Rounding::Down);
    }
};

inline constexpr int64_t kTicksPerSecond = 254016000000LL;

struct Time {
    int64_t ticks = 0;

    constexpr Time() = default;
    constexpr explicit Time(int64_t t) : ticks(t) {}

    static constexpr Time zero() { return Time{0}; }
    static constexpr Time max() { return Time{std::numeric_limits<int64_t>::max() / 4}; }
    static Time fromSeconds(double s) noexcept;
    static Time fromMilliseconds(int64_t ms) noexcept { return Time{ms * (kTicksPerSecond / 1000)}; }
    // Start time of frame index `frame` at rate `fps` (frames per second).
    static Time fromFrames(int64_t frame, Rational fps) noexcept;
    static Time fromSamples(int64_t samples, int sampleRate) noexcept;
    // Converts a timestamp expressed in `timebase` units (e.g. AVStream::time_base).
    static Time fromTimebase(int64_t value, Rational timebase) noexcept;
    // Duration of a single frame at `fps`.
    static Time frameDuration(Rational fps) noexcept { return fromFrames(1, fps); }

    [[nodiscard]] double seconds() const noexcept { return double(ticks) / double(kTicksPerSecond); }
    [[nodiscard]] int64_t milliseconds() const noexcept { return ticks / (kTicksPerSecond / 1000); }
    [[nodiscard]] int64_t toFrames(Rational fps, Rounding r = Rounding::Down) const noexcept;
    [[nodiscard]] int64_t toSamples(int sampleRate, Rounding r = Rounding::Down) const noexcept;
    [[nodiscard]] int64_t toTimebase(Rational timebase, Rounding r = Rounding::Nearest) const noexcept;
    // Snaps to the nearest (or floor) frame boundary.
    [[nodiscard]] Time snappedToFrame(Rational fps, Rounding r = Rounding::Nearest) const noexcept {
        return fromFrames(toFrames(fps, r), fps);
    }

    constexpr Time operator+(Time o) const noexcept { return Time{ticks + o.ticks}; }
    constexpr Time operator-(Time o) const noexcept { return Time{ticks - o.ticks}; }
    constexpr Time operator-() const noexcept { return Time{-ticks}; }
    constexpr Time& operator+=(Time o) noexcept { ticks += o.ticks; return *this; }
    constexpr Time& operator-=(Time o) noexcept { ticks -= o.ticks; return *this; }
    constexpr Time operator*(int64_t k) const noexcept { return Time{ticks * k}; }
    [[nodiscard]] Time scaled(Rational r, Rounding rd = Rounding::Nearest) const noexcept {
        return Time{mulDiv(ticks, r.num, r.den, rd)};
    }
    constexpr auto operator<=>(const Time&) const = default;
};

inline Time minTime(Time a, Time b) { return a < b ? a : b; }
inline Time maxTime(Time a, Time b) { return a < b ? b : a; }

// Half-open interval [start, start + duration).
struct TimeRange {
    Time start;
    Time duration;

    constexpr TimeRange() = default;
    constexpr TimeRange(Time s, Time d) : start(s), duration(d) {}
    static constexpr TimeRange fromStartEnd(Time s, Time e) { return {s, e - s}; }

    [[nodiscard]] constexpr Time end() const noexcept { return start + duration; }
    [[nodiscard]] constexpr bool empty() const noexcept { return duration.ticks <= 0; }
    [[nodiscard]] constexpr bool contains(Time t) const noexcept { return t >= start && t < end(); }
    [[nodiscard]] constexpr bool overlaps(const TimeRange& o) const noexcept {
        return start < o.end() && o.start < end();
    }
    [[nodiscard]] TimeRange intersection(const TimeRange& o) const noexcept {
        Time s = maxTime(start, o.start), e = minTime(end(), o.end());
        return e > s ? fromStartEnd(s, e) : TimeRange{s, Time{0}};
    }
    constexpr bool operator==(const TimeRange&) const = default;
};

// ---- Timecode --------------------------------------------------------------

enum class TimecodeStyle { Timecode, Frames, Seconds, Samples };

// SMPTE timecode. Uses drop-frame notation (HH:MM:SS;FF) for 29.97 and 59.94.
std::string formatTimecode(Time t, Rational fps, bool dropFrame = true);
std::string formatTime(Time t, Rational fps, TimecodeStyle style, int sampleRate = 48000);
// Parses "HH:MM:SS:FF", "HH:MM:SS;FF", "MM:SS", "123" (frames), "+10" (relative frames).
std::optional<Time> parseTimecode(std::string_view s, Rational fps, bool dropFrame = true);
// Human friendly duration: "1:02:03.5"
std::string formatDuration(Time t);

bool isDropFrameRate(Rational fps) noexcept;

}  // namespace avc
