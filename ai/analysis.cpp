#include "ai/analysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include "core/log.h"
#include "decode/audio_reader.h"
#include "decode/video_decoder.h"
#include "timeline/edit_ops.h"

namespace avc::ai {

namespace {

constexpr int kAnalysisRate = 16000;

float toDb(double meanSquare) { return meanSquare <= 1e-12 ? -120.0f : static_cast<float>(10.0 * std::log10(meanSquare)); }

// Streams a file as mono floats at `rate`, calling fn(chunk, frames) in order.
template <typename Fn>
Status streamMono(const std::string& path, int rate, const CancelToken& cancel, JobContext* progress, Fn&& fn) {
    auto reader = openAudioReader(path, -1, AudioFormat{rate, 1});
    if (!reader) return reader.status();
    const int64_t total = (*reader)->lengthFrames();
    const int64_t chunk = rate * 2;
    std::vector<float> buf(static_cast<size_t>(chunk));
    for (int64_t pos = 0; pos < total; pos += chunk) {
        if (cancel.cancelled() || (progress && progress->cancelled())) return Status::error("cancelled");
        const int64_t n = std::min(chunk, total - pos);
        (*reader)->read(pos, n, buf.data(), cancel);
        fn(buf.data(), static_cast<size_t>(n));
        if (progress) progress->setProgress(static_cast<float>(pos + n) / static_cast<float>(std::max<int64_t>(1, total)));
    }
    return Status::ok();
}

// RMS envelope in dB, one value per `hop` samples.
std::vector<float> envelopeDb(const float* x, size_t n, size_t hop) {
    std::vector<float> env;
    env.reserve(n / hop + 1);
    for (size_t i = 0; i < n; i += hop) {
        const size_t e = std::min(n, i + hop);
        double acc = 0;
        for (size_t k = i; k < e; ++k) acc += static_cast<double>(x[k]) * x[k];
        env.push_back(toDb(acc / static_cast<double>(std::max<size_t>(1, e - i))));
    }
    return env;
}

float percentile(std::vector<float> v, float p) {
    if (v.empty()) return -120.0f;
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<float>(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return v[k];
}

SilenceResult silenceFromEnvelope(const std::vector<float>& env, double hopSec, double durationSec, const SilenceOptions& opt) {
    SilenceResult r;
    r.durationSec = durationSec;
    r.noiseFloorDb = percentile(env, 0.10f);
    if (opt.autoThreshold) {
        // Between the noise floor and the speech level, bounded to sane values.
        const float speech = percentile(env, 0.90f);
        float t = std::max(r.noiseFloorDb + 12.0f, speech - 30.0f);
        r.thresholdDb = std::clamp(t, -60.0f, -20.0f);
        if (speech - r.noiseFloorDb < 10.0f) r.thresholdDb = std::min(r.thresholdDb, r.noiseFloorDb + 3.0f);  // flat signal
    } else {
        r.thresholdDb = opt.thresholdDb;
    }
    size_t i = 0;
    while (i < env.size()) {
        if (env[i] >= r.thresholdDb) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < env.size() && env[j] < r.thresholdDb) ++j;
        const double s = static_cast<double>(i) * hopSec, e = std::min(durationSec, static_cast<double>(j) * hopSec);
        if (e - s >= opt.minSilenceSec) {
            // Keep a little audio around speech; silences at the very start/end keep padding only on the inner side.
            const double ps = s <= 0.0 ? s : s + opt.paddingSec;
            const double pe = e >= durationSec ? e : e - opt.paddingSec;
            if (pe - ps > 0.02) r.silences.push_back(TimeRange::fromStartEnd(Time::fromSeconds(ps), Time::fromSeconds(pe)));
        }
        i = j;
    }
    return r;
}

}  // namespace

// ------------------------------------------------------------------ silence

SilenceResult detectSilenceInSamples(const float* mono, size_t frames, int sampleRate, const SilenceOptions& opt) {
    const size_t hop = static_cast<size_t>(std::max(1, sampleRate / 100));  // 10 ms
    const auto env = envelopeDb(mono, frames, hop);
    return silenceFromEnvelope(env, static_cast<double>(hop) / sampleRate, static_cast<double>(frames) / sampleRate, opt);
}

Result<SilenceResult> detectSilence(const std::string& path, const SilenceOptions& opt, const CancelToken& cancel, JobContext* progress) {
    const size_t hop = kAnalysisRate / 100;
    std::vector<float> env;
    std::vector<float> carry;
    size_t total = 0;
    Status st = streamMono(path, kAnalysisRate, cancel, progress, [&](const float* x, size_t n) {
        total += n;
        carry.insert(carry.end(), x, x + n);
        const size_t whole = carry.size() / hop * hop;
        auto part = envelopeDb(carry.data(), whole, hop);
        env.insert(env.end(), part.begin(), part.end());
        carry.erase(carry.begin(), carry.begin() + static_cast<long>(whole));
    });
    if (!st) return st;
    if (!carry.empty()) {
        auto part = envelopeDb(carry.data(), carry.size(), hop);
        env.insert(env.end(), part.begin(), part.end());
    }
    return silenceFromEnvelope(env, static_cast<double>(hop) / kAnalysisRate, static_cast<double>(total) / kAnalysisRate, opt);
}

// ------------------------------------------------------------------ beats

BeatResult detectBeatsInSamples(const float* x, size_t n, int sampleRate) {
    BeatResult r;
    // Onset envelope: positive log-energy flux on ~11.6 ms hops.
    const size_t hop = static_cast<size_t>(std::max(1, sampleRate / 86));
    const double hopSec = static_cast<double>(hop) / sampleRate;
    std::vector<float> energy;
    for (size_t i = 0; i + hop <= n; i += hop) {
        double acc = 0;
        for (size_t k = i; k < i + hop; ++k) acc += static_cast<double>(x[k]) * x[k];
        energy.push_back(static_cast<float>(std::log10(1e-9 + acc / static_cast<double>(hop))));
    }
    if (energy.size() < 64) return r;
    std::vector<float> onset(energy.size(), 0.0f);
    for (size_t i = 1; i < energy.size(); ++i) onset[i] = std::max(0.0f, energy[i] - energy[i - 1]);
    // Remove the local mean so sustained loudness does not count as onsets.
    {
        std::vector<float> smooth(onset.size());
        const int w = 16;
        for (size_t i = 0; i < onset.size(); ++i) {
            double s = 0;
            int c = 0;
            for (int k = -w; k <= w; ++k) {
                const long j = static_cast<long>(i) + k;
                if (j < 0 || j >= static_cast<long>(onset.size())) continue;
                s += onset[static_cast<size_t>(j)];
                ++c;
            }
            smooth[i] = static_cast<float>(s / std::max(1, c));
        }
        for (size_t i = 0; i < onset.size(); ++i) onset[i] = std::max(0.0f, onset[i] - smooth[i]);
    }
    // Tempo: autocorrelation over 60..200 BPM with a mild preference around 120 BPM.
    const int minLag = static_cast<int>(std::floor(60.0 / 200.0 / hopSec));
    const int maxLag = static_cast<int>(std::ceil(60.0 / 60.0 / hopSec));
    double best = -1, energy0 = 0;
    int bestLag = 0;
    for (float v : onset) energy0 += static_cast<double>(v) * v;
    for (int lag = std::max(1, minLag); lag <= maxLag && lag < static_cast<int>(onset.size()) / 2; ++lag) {
        double ac = 0;
        for (size_t i = static_cast<size_t>(lag); i < onset.size(); ++i) ac += static_cast<double>(onset[i]) * onset[i - static_cast<size_t>(lag)];
        const double bpm = 60.0 / (lag * hopSec);
        const double weight = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 1.0, 2.0));
        const double score = ac * (0.6 + 0.4 * weight);
        if (score > best) {
            best = score;
            bestLag = lag;
        }
    }
    if (bestLag <= 0 || energy0 <= 0) return r;
    // Refine the period with parabolic interpolation of the autocorrelation peak.
    auto acAt = [&](int lag) {
        double ac = 0;
        for (size_t i = static_cast<size_t>(lag); i < onset.size(); ++i) ac += static_cast<double>(onset[i]) * onset[i - static_cast<size_t>(lag)];
        return ac;
    };
    double period = bestLag;
    {
        const double a = acAt(bestLag - 1), b = acAt(bestLag), c = acAt(bestLag + 1);
        const double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) period += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    r.bpm = 60.0 / (period * hopSec);
    r.confidence = static_cast<float>(std::clamp(best / energy0, 0.0, 1.0));
    // Phase: the offset whose comb sums the most onset energy.
    double bestPhase = 0, bestSum = -1;
    for (int ph = 0; ph < static_cast<int>(std::ceil(period)); ++ph) {
        double s = 0;
        for (double p = ph; p < static_cast<double>(onset.size()); p += period) s += onset[static_cast<size_t>(p)];
        if (s > bestSum) {
            bestSum = s;
            bestPhase = ph;
        }
    }
    // Place beats and snap each to the strongest onset nearby.
    const int win = std::max(1, static_cast<int>(period * 0.15));
    std::vector<std::pair<double, float>> placed;  // time, onset strength
    for (double p = bestPhase; p + win < static_cast<double>(onset.size()); p += period) {
        const long c = static_cast<long>(std::lround(p));
        long arg = c;
        float mv = -1;
        for (long k = c - win; k <= c + win; ++k) {
            if (k < 0 || k >= static_cast<long>(onset.size())) continue;
            if (onset[static_cast<size_t>(k)] > mv) {
                mv = onset[static_cast<size_t>(k)];
                arg = k;
            }
        }
        // Onset flux peaks one hop after the attack starts.
        const double t = std::max(0.0, (static_cast<double>(arg) - 1.0) * hopSec);
        placed.emplace_back(t, mv);
    }
    // Drop comb positions without a real onset (fade-in/out, trailing silence).
    std::vector<float> strengths;
    for (const auto& b : placed) strengths.push_back(b.second);
    const float median = percentile(strengths, 0.5f);
    for (const auto& [t, strength] : placed)
        if (strength >= median * 0.15f) r.beats.push_back(Time::fromSeconds(t));
    return r;
}

Result<BeatResult> detectBeats(const std::string& path, const CancelToken& cancel, JobContext* progress) {
    const int rate = 11025;
    std::vector<float> all;
    Status st = streamMono(path, rate, cancel, progress, [&](const float* x, size_t n) { all.insert(all.end(), x, x + n); });
    if (!st) return st;
    return detectBeatsInSamples(all.data(), all.size(), rate);
}

// ------------------------------------------------------------------ scenes

Result<SceneResult> detectScenes(const std::string& path, const SceneOptions& opt, const CancelToken& cancel, JobContext* progress) {
    VideoDecoderOptions vo;
    vo.maxWidth = opt.analysisWidth;
    vo.maxHeight = opt.analysisWidth;
    vo.fastDecode = true;
    auto dec = openVideoDecoder(path, vo);
    if (!dec) return dec.status();
    const double durSec = std::max(0.001, (*dec)->duration().seconds());
    SceneResult r;
    constexpr int kBins = 16;
    std::array<float, kBins * 3> prev{};
    bool havePrev = false;
    Time lastCut = Time{0};
    std::vector<float> recent;  // recent distances (adaptive threshold)
    for (;;) {
        if (cancel.cancelled() || (progress && progress->cancelled())) return Status::error("cancelled");
        auto f = (*dec)->nextFrame(cancel);
        if (!f) break;
        const VideoFrame& fr = **f;
        std::vector<uint8_t> rgba(static_cast<size_t>(fr.width) * static_cast<size_t>(fr.height) * 4);
        if (fr.format == PixelFormat::RGBA8) {
            for (int y = 0; y < fr.height; ++y)
                std::copy_n(fr.planes[0] + static_cast<ptrdiff_t>(y) * fr.strides[0], fr.width * 4,
                            rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(fr.width) * 4);
        } else {
            convertToRgba8(fr, rgba.data(), fr.width * 4);
        }
        std::array<float, kBins * 3> hist{};
        const size_t px = static_cast<size_t>(fr.width) * static_cast<size_t>(fr.height);
        for (size_t i = 0; i < px; ++i)
            for (int c = 0; c < 3; ++c) hist[static_cast<size_t>(c * kBins + rgba[i * 4 + static_cast<size_t>(c)] * kBins / 256)] += 1.0f;
        for (float& h : hist) h /= static_cast<float>(std::max<size_t>(1, px)) * 3.0f;
        if (havePrev) {
            float d = 0;
            for (size_t i = 0; i < hist.size(); ++i) d += std::fabs(hist[i] - prev[i]);
            d *= 0.5f;  // 0..1
            float avg = 0;
            for (float v : recent) avg += v;
            avg = recent.empty() ? 0.0f : avg / static_cast<float>(recent.size());
            const bool cut = d > opt.threshold && d > avg * 3.0f + 0.05f && (fr.pts - lastCut).seconds() >= opt.minSceneSec;
            if (cut) {
                r.cuts.push_back(fr.pts);
                r.scores.push_back(d);
                lastCut = fr.pts;
                recent.clear();
            } else {
                recent.push_back(d);
                if (recent.size() > 30) recent.erase(recent.begin());
            }
        }
        prev = hist;
        havePrev = true;
        if (progress) progress->setProgress(static_cast<float>(std::min(1.0, fr.pts.seconds() / durSec)));
    }
    return r;
}

// ------------------------------------------------------------------ highlights

std::vector<Highlight> findHighlightsInSamples(const float* x, size_t n, int sampleRate, int maxCount, double windowSec) {
    std::vector<Highlight> out;
    const size_t hop = static_cast<size_t>(std::max(1, sampleRate / 10));  // 100 ms
    const auto env = envelopeDb(x, n, hop);
    if (env.empty()) return out;
    const float median = percentile(env, 0.5f);
    const size_t win = std::max<size_t>(1, static_cast<size_t>(windowSec * 10.0));
    // Sliding mean loudness; pick non-overlapping maxima.
    std::vector<std::pair<float, size_t>> cand;
    double acc = 0;
    for (size_t i = 0; i < env.size(); ++i) {
        acc += env[i];
        if (i >= win) acc -= env[i - win];
        if (i + 1 >= win) cand.emplace_back(static_cast<float>(acc / static_cast<double>(win)) - median, i + 1 - win);
    }
    std::vector<float> windowScore(env.size(), -1e9f);
    for (const auto& [score, start] : cand) windowScore[start] = score;
    std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::pair<size_t, size_t>> taken;
    for (const auto& [score, start0] : cand) {
        if (static_cast<int>(out.size()) >= maxCount || score < 3.0f) break;
        size_t start = start0, end = start0 + win;
        bool overlaps = false;
        for (const auto& [a, b] : taken)
            if (start < b && a < end) overlaps = true;
        if (overlaps) continue;
        // Grow the moment while the neighbouring windows stay about as loud,
        // so one long cheer becomes one highlight instead of several.
        const size_t maxLen = win * 6;
        while (end - start < maxLen) {
            const bool canLeft = start > 0 && windowScore[start - 1] >= score - 3.0f;
            const bool canRight = end < env.size() && end + 1 >= win && end + 1 - win < windowScore.size() &&
                                  windowScore[end + 1 - win] >= score - 3.0f;
            if (!canLeft && !canRight) break;
            if (canLeft) --start;
            if (canRight) ++end;
        }
        for (const auto& [a, b] : taken)
            if (start < b && a < end) overlaps = true;
        if (overlaps) continue;
        taken.emplace_back(start, end);
        Highlight h;
        h.range = TimeRange::fromStartEnd(Time::fromSeconds(static_cast<double>(start) * 0.1),
                                          Time::fromSeconds(std::min(static_cast<double>(n) / sampleRate, static_cast<double>(end) * 0.1)));
        h.score = score;
        out.push_back(h);
    }
    std::sort(out.begin(), out.end(), [](const Highlight& a, const Highlight& b) { return a.range.start < b.range.start; });
    return out;
}

Result<std::vector<Highlight>> findHighlights(const std::string& path, int maxCount, double windowSec, const CancelToken& cancel,
                                              JobContext* progress) {
    const int rate = 8000;
    std::vector<float> all;
    Status st = streamMono(path, rate, cancel, progress, [&](const float* x, size_t n) { all.insert(all.end(), x, x + n); });
    if (!st) return st;
    return findHighlightsInSamples(all.data(), all.size(), rate, maxCount, windowSec);
}

// ------------------------------------------------------------------ timeline helpers

std::optional<Time> sourceTimeToTimeline(const Clip& c, Time source) {
    if (c.freezeFrame || !c.speed.valid()) return std::nullopt;
    const Time srcLen = c.sourceDuration();
    const Time rel = source - c.sourceIn;
    if (rel.ticks < 0 || rel > srcLen) return std::nullopt;
    const Time local = c.reverse ? (srcLen - rel).scaled(c.speed.inverse(), Rounding::Nearest) : rel.scaled(c.speed.inverse(), Rounding::Nearest);
    return c.start + minTime(local, c.duration);
}

std::vector<TimeRange> sourceRangesToTimeline(const Clip& c, const std::vector<TimeRange>& sourceRanges) {
    std::vector<TimeRange> out;
    const TimeRange src{c.sourceIn, c.sourceDuration()};
    for (const TimeRange& r : sourceRanges) {
        const TimeRange in = r.intersection(src);
        if (in.empty()) continue;
        auto a = sourceTimeToTimeline(c, in.start), b = sourceTimeToTimeline(c, in.end());
        if (!a || !b) continue;
        if (*b < *a) std::swap(a, b);
        if (*b > *a) out.push_back(TimeRange::fromStartEnd(*a, *b));
    }
    return out;
}

std::vector<TimeRange> normalizeRanges(std::vector<TimeRange> ranges, Time minLength) {
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> out;
    for (const TimeRange& r : ranges) {
        if (r.empty()) continue;
        if (!out.empty() && r.start <= out.back().end()) {
            const Time end = maxTime(out.back().end(), r.end());
            out.back() = TimeRange::fromStartEnd(out.back().start, end);
        } else {
            out.push_back(r);
        }
    }
    std::erase_if(out, [&](const TimeRange& r) { return r.duration < minLength; });
    return out;
}

Status rippleRemoveRanges(SequenceEditor& e, const std::vector<TimeRange>& ranges) {
    const auto merged = normalizeRanges(ranges);
    std::vector<int> all;
    for (int i = 0; i < e.trackCount(); ++i) all.push_back(i);
    for (auto it = merged.rbegin(); it != merged.rend(); ++it)
        if (auto st = edit::extract(e, *it, all); !st) return st;
    // Markers inside removed ranges go away; later ones move with the content.
    Sequence& s = e.props();
    std::vector<Marker> kept;
    for (Marker m : s.markers) {
        Time shift{0};
        bool inside = false;
        for (const TimeRange& r : merged) {
            if (r.contains(m.time)) inside = true;
            else if (r.end() <= m.time) shift += r.duration;
        }
        if (inside) continue;
        m.time -= shift;
        kept.push_back(m);
    }
    s.markers = std::move(kept);
    return Status::ok();
}

}  // namespace avc::ai
