#pragma once
// Animatable parameters and keyframes.
//
// Every keyframable value is stored as up to four floats (scalar, 2D vector or
// RGBA colour). Keyframe times are relative to the clip's content origin (the
// timeline position of the clip's source in-point), so moving a clip keeps its
// animation and trimming the head keeps keys aligned with the footage.

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/time.h"

namespace avc {

using ParamValue = std::array<float, 4>;

inline ParamValue pv(float x, float y = 0.f, float z = 0.f, float w = 0.f) { return {x, y, z, w}; }

enum class Interp : uint8_t {
    Linear = 0,
    Hold,
    Bezier,     // uses the cubic-bezier handles below
    EaseIn,
    EaseOut,
    EaseInOut,
};

const char* interpName(Interp i);
Interp interpFromName(std::string_view s);

struct Keyframe {
    Time time;  // relative to clip content origin
    ParamValue value{};
    Interp interp = Interp::Linear;  // interpolation towards the next key
    // Cubic-bezier handles in normalized segment space (like CSS cubic-bezier).
    float x1 = 0.333f, y1 = 0.0f, x2 = 0.667f, y2 = 1.0f;

    bool operator==(const Keyframe&) const = default;
};

class AnimatedParam {
public:
    AnimatedParam() = default;
    explicit AnimatedParam(ParamValue v) : value_(v) {}

    [[nodiscard]] bool animated() const noexcept { return !keys_.empty(); }
    [[nodiscard]] const ParamValue& staticValue() const noexcept { return value_; }
    [[nodiscard]] const std::vector<Keyframe>& keys() const noexcept { return keys_; }
    [[nodiscard]] ParamValue evaluate(Time t) const;

    // Sets the value at time t: updates the static value when not animated,
    // otherwise adds/replaces a key at t.
    void setValueAt(Time t, ParamValue v);
    void setStatic(ParamValue v) {
        value_ = v;
        keys_.clear();
    }
    void addKey(Keyframe k);  // replaces a key at the same time
    bool removeKeyAt(Time t, Time tolerance = Time{0});
    void clearKeys();  // keeps the current static value
    void setKeys(std::vector<Keyframe> keys);
    void shiftKeys(Time delta);
    // Index of the key at time t (within tolerance) or -1.
    int keyIndexAt(Time t, Time tolerance = Time{0}) const;

    bool operator==(const AnimatedParam&) const = default;

private:
    ParamValue value_{};
    std::vector<Keyframe> keys_;  // sorted by time
};

// Evaluates cubic-bezier easing y(x) for x in [0,1].
float cubicBezierEase(float x, float x1, float y1, float x2, float y2);

// Ordered, string-keyed set of parameters (small; linear lookup).
class ParamSet {
public:
    [[nodiscard]] const AnimatedParam* find(std::string_view id) const;
    AnimatedParam* find(std::string_view id);
    AnimatedParam& getOrAdd(std::string_view id, ParamValue def = {});
    void set(std::string_view id, AnimatedParam p);
    void setStatic(std::string_view id, ParamValue v) { getOrAdd(id).setStatic(v); }
    [[nodiscard]] ParamValue evaluate(std::string_view id, Time t, ParamValue def = {}) const;
    [[nodiscard]] float evaluate1(std::string_view id, Time t, float def = 0.f) const {
        return evaluate(id, t, pv(def))[0];
    }
    bool remove(std::string_view id);
    void shiftAllKeys(Time delta);
    [[nodiscard]] bool anyAnimated() const;
    [[nodiscard]] const std::vector<std::pair<std::string, AnimatedParam>>& items() const noexcept { return items_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] size_t size() const noexcept { return items_.size(); }

    // Order-insensitive comparison.
    bool operator==(const ParamSet& o) const;

private:
    std::vector<std::pair<std::string, AnimatedParam>> items_;
};

// ---- Parameter definitions (UI metadata + defaults) ------------------------

enum class ParamType : uint8_t { Float, Vec2, Color, Angle, Percent, Decibel, Bool, Int, Enum };

struct ParamDef {
    std::string id;
    std::string label;  // English (translated by UI)
    ParamType type = ParamType::Float;
    ParamValue def{};
    float minValue = 0.f, maxValue = 1.f;   // soft UI range
    float step = 0.01f;
    bool keyframable = true;
    std::vector<std::string> enumLabels;   // for Enum
    std::string unit;                      // "px", "%", "°", "dB"
    std::string group;                     // UI grouping
};

}  // namespace avc
