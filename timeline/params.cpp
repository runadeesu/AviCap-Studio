#include "timeline/params.h"

#include <algorithm>
#include <cmath>

namespace avc {

const char* interpName(Interp i) {
    switch (i) {
    case Interp::Linear: return "linear";
    case Interp::Hold: return "hold";
    case Interp::Bezier: return "bezier";
    case Interp::EaseIn: return "easeIn";
    case Interp::EaseOut: return "easeOut";
    case Interp::EaseInOut: return "easeInOut";
    }
    return "linear";
}

Interp interpFromName(std::string_view s) {
    if (s == "hold") return Interp::Hold;
    if (s == "bezier") return Interp::Bezier;
    if (s == "easeIn") return Interp::EaseIn;
    if (s == "easeOut") return Interp::EaseOut;
    if (s == "easeInOut") return Interp::EaseInOut;
    return Interp::Linear;
}

float cubicBezierEase(float x, float x1, float y1, float x2, float y2) {
    x = std::clamp(x, 0.0f, 1.0f);
    x1 = std::clamp(x1, 0.0f, 1.0f);
    x2 = std::clamp(x2, 0.0f, 1.0f);
    auto bez = [](float t, float p1, float p2) {
        const float u = 1.0f - t;
        return 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t;
    };
    auto dbez = [](float t, float p1, float p2) {
        const float u = 1.0f - t;
        return 3.0f * u * u * p1 + 6.0f * u * t * (p2 - p1) + 3.0f * t * t * (1.0f - p2);
    };
    // Newton-Raphson, then bisection fallback.
    float t = x;
    for (int i = 0; i < 8; ++i) {
        const float err = bez(t, x1, x2) - x;
        if (std::fabs(err) < 1e-6f) return bez(t, y1, y2);
        const float d = dbez(t, x1, x2);
        if (std::fabs(d) < 1e-6f) break;
        t = std::clamp(t - err / d, 0.0f, 1.0f);
    }
    float lo = 0.0f, hi = 1.0f;
    t = x;
    for (int i = 0; i < 40; ++i) {
        const float v = bez(t, x1, x2);
        if (std::fabs(v - x) < 1e-6f) break;
        if (v < x) lo = t;
        else hi = t;
        t = 0.5f * (lo + hi);
    }
    return bez(t, y1, y2);
}

namespace {

float easeFactor(const Keyframe& k, float f) {
    switch (k.interp) {
    case Interp::Linear: return f;
    case Interp::Hold: return 0.0f;
    case Interp::Bezier: return cubicBezierEase(f, k.x1, k.y1, k.x2, k.y2);
    case Interp::EaseIn: return cubicBezierEase(f, 0.42f, 0.0f, 1.0f, 1.0f);
    case Interp::EaseOut: return cubicBezierEase(f, 0.0f, 0.0f, 0.58f, 1.0f);
    case Interp::EaseInOut: return cubicBezierEase(f, 0.42f, 0.0f, 0.58f, 1.0f);
    }
    return f;
}

}  // namespace

ParamValue AnimatedParam::evaluate(Time t) const {
    if (keys_.empty()) return value_;
    if (t <= keys_.front().time) return keys_.front().value;
    if (t >= keys_.back().time) return keys_.back().value;
    auto it = std::upper_bound(keys_.begin(), keys_.end(), t, [](Time v, const Keyframe& k) { return v < k.time; });
    const Keyframe& b = *it;
    const Keyframe& a = *(it - 1);
    const double span = static_cast<double>((b.time - a.time).ticks);
    const float f = span > 0 ? static_cast<float>(static_cast<double>((t - a.time).ticks) / span) : 1.0f;
    const float e = easeFactor(a, f);
    ParamValue r;
    for (int i = 0; i < 4; ++i) r[i] = a.value[i] + (b.value[i] - a.value[i]) * e;
    return r;
}

void AnimatedParam::setValueAt(Time t, ParamValue v) {
    if (keys_.empty()) {
        value_ = v;
        return;
    }
    Keyframe k;
    k.time = t;
    k.value = v;
    const int idx = keyIndexAt(t);
    if (idx >= 0) {
        keys_[static_cast<size_t>(idx)].value = v;
    } else {
        // New key inherits the interpolation of the segment it lands in.
        auto it = std::upper_bound(keys_.begin(), keys_.end(), t, [](Time v2, const Keyframe& kk) { return v2 < kk.time; });
        if (it != keys_.begin()) k.interp = (it - 1)->interp;
        keys_.insert(it, k);
    }
}

void AnimatedParam::addKey(Keyframe k) {
    const int idx = keyIndexAt(k.time);
    if (idx >= 0) {
        keys_[static_cast<size_t>(idx)] = k;
        return;
    }
    auto it = std::upper_bound(keys_.begin(), keys_.end(), k.time, [](Time v, const Keyframe& kk) { return v < kk.time; });
    keys_.insert(it, k);
}

bool AnimatedParam::removeKeyAt(Time t, Time tolerance) {
    const int idx = keyIndexAt(t, tolerance);
    if (idx < 0) return false;
    if (keys_.size() == 1) value_ = keys_[0].value;
    keys_.erase(keys_.begin() + idx);
    return true;
}

void AnimatedParam::clearKeys() {
    if (!keys_.empty()) value_ = keys_.front().value;
    keys_.clear();
}

void AnimatedParam::setKeys(std::vector<Keyframe> keys) {
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    // Drop duplicates (keep last).
    std::vector<Keyframe> out;
    for (auto& k : keys) {
        if (!out.empty() && out.back().time == k.time) out.back() = k;
        else out.push_back(k);
    }
    keys_ = std::move(out);
    if (!keys_.empty()) value_ = keys_.front().value;
}

void AnimatedParam::shiftKeys(Time delta) {
    for (auto& k : keys_) k.time += delta;
}

int AnimatedParam::keyIndexAt(Time t, Time tolerance) const {
    for (size_t i = 0; i < keys_.size(); ++i) {
        const int64_t d = std::llabs((keys_[i].time - t).ticks);
        if (d <= tolerance.ticks) return static_cast<int>(i);
    }
    return -1;
}

const AnimatedParam* ParamSet::find(std::string_view id) const {
    for (auto& [k, v] : items_)
        if (k == id) return &v;
    return nullptr;
}

AnimatedParam* ParamSet::find(std::string_view id) {
    for (auto& [k, v] : items_)
        if (k == id) return &v;
    return nullptr;
}

AnimatedParam& ParamSet::getOrAdd(std::string_view id, ParamValue def) {
    if (auto* p = find(id)) return *p;
    items_.emplace_back(std::string(id), AnimatedParam(def));
    return items_.back().second;
}

void ParamSet::set(std::string_view id, AnimatedParam p) { getOrAdd(id) = std::move(p); }

ParamValue ParamSet::evaluate(std::string_view id, Time t, ParamValue def) const {
    const AnimatedParam* p = find(id);
    return p ? p->evaluate(t) : def;
}

bool ParamSet::remove(std::string_view id) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->first == id) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

void ParamSet::shiftAllKeys(Time delta) {
    for (auto& [k, v] : items_) v.shiftKeys(delta);
}

bool ParamSet::anyAnimated() const {
    for (auto& [k, v] : items_)
        if (v.animated()) return true;
    return false;
}

}  // namespace avc
