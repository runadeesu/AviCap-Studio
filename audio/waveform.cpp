#include "audio/waveform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "decode/audio_reader.h"

namespace avc::audio {

namespace {
int8_t q(float v) { return static_cast<int8_t>(std::clamp(std::lround(v * 127.0f), -127L, 127L)); }
}  // namespace

WaveformPeaks WaveformPeaks::fromSamples(const float* s, int64_t frames, int channels, int sampleRate) {
    WaveformPeaks w;
    w.sampleRate = sampleRate;
    w.channels = channels;
    w.frames = frames;
    const int64_t bins = (frames + kBaseBin - 1) / kBaseBin;
    std::vector<std::vector<MinMax>> base(static_cast<size_t>(channels), std::vector<MinMax>(static_cast<size_t>(bins)));
    for (int64_t b = 0; b < bins; ++b) {
        const int64_t a = b * kBaseBin, e = std::min(frames, a + kBaseBin);
        for (int c = 0; c < channels; ++c) {
            float mn = 0, mx = 0;
            for (int64_t i = a; i < e; ++i) {
                const float v = s[i * channels + c];
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
            base[static_cast<size_t>(c)][static_cast<size_t>(b)] = {q(mn), q(mx)};
        }
    }
    w.levels.push_back(std::move(base));
    while (w.levels.back()[0].size() > 1) {
        const auto& prev = w.levels.back();
        const size_t n = (prev[0].size() + kLevelFactor - 1) / kLevelFactor;
        std::vector<std::vector<MinMax>> lvl(static_cast<size_t>(channels), std::vector<MinMax>(n));
        for (int c = 0; c < channels; ++c)
            for (size_t i = 0; i < n; ++i) {
                MinMax m{127, -127};
                for (size_t k = i * kLevelFactor; k < std::min(prev[0].size(), (i + 1) * kLevelFactor); ++k) {
                    m.min = std::min(m.min, prev[static_cast<size_t>(c)][k].min);
                    m.max = std::max(m.max, prev[static_cast<size_t>(c)][k].max);
                }
                lvl[static_cast<size_t>(c)][i] = m;
            }
        w.levels.push_back(std::move(lvl));
    }
    return w;
}

std::vector<std::pair<float, float>> WaveformPeaks::query(double startSec, double endSec, int columns, int channel) const {
    std::vector<std::pair<float, float>> out(static_cast<size_t>(std::max(0, columns)), {0.0f, 0.0f});
    if (levels.empty() || columns <= 0 || endSec <= startSec) return out;
    const double samplesPerCol = (endSec - startSec) * sampleRate / columns;
    // Choose the coarsest level that still has several bins per column.
    size_t level = 0;
    double binSamples = kBaseBin;
    while (level + 1 < levels.size() && binSamples * kLevelFactor * 4 <= samplesPerCol) {
        binSamples *= kLevelFactor;
        ++level;
    }
    const auto& lv = levels[level];
    const int64_t nbins = static_cast<int64_t>(lv[0].size());
    for (int col = 0; col < columns; ++col) {
        const double s0 = startSec * sampleRate + col * samplesPerCol;
        const double s1 = s0 + samplesPerCol;
        int64_t b0 = static_cast<int64_t>(std::floor(s0 / binSamples));
        int64_t b1 = std::max(b0 + 1, static_cast<int64_t>(std::ceil(s1 / binSamples)));
        b0 = std::max<int64_t>(b0, 0);
        b1 = std::min(b1, nbins);
        int mn = 127, mx = -127;
        bool any = false;
        for (int64_t b = b0; b < b1; ++b)
            for (int c = 0; c < channels; ++c) {
                if (channel >= 0 && c != channel) continue;
                mn = std::min<int>(mn, lv[static_cast<size_t>(c)][static_cast<size_t>(b)].min);
                mx = std::max<int>(mx, lv[static_cast<size_t>(c)][static_cast<size_t>(b)].max);
                any = true;
            }
        if (any) out[static_cast<size_t>(col)] = {static_cast<float>(mn) / 127.0f, static_cast<float>(mx) / 127.0f};
    }
    return out;
}

std::string WaveformPeaks::serialize() const {
    std::string out("AVWF", 4);
    auto put32 = [&](int32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    auto put64 = [&](int64_t v) { out.append(reinterpret_cast<const char*>(&v), 8); };
    put32(1);
    put32(sampleRate);
    put32(channels);
    put64(frames);
    // Only level 0 is stored; the pyramid is rebuilt on load (cheap).
    const auto& base = levels.empty() ? std::vector<std::vector<MinMax>>{} : levels[0];
    put64(base.empty() ? 0 : static_cast<int64_t>(base[0].size()));
    for (const auto& ch : base) out.append(reinterpret_cast<const char*>(ch.data()), ch.size() * sizeof(MinMax));
    return out;
}

std::optional<WaveformPeaks> WaveformPeaks::deserialize(const std::string& d) {
    if (d.size() < 32 || d.compare(0, 4, "AVWF") != 0) return std::nullopt;
    size_t p = 4;
    auto get32 = [&]() {
        int32_t v;
        std::memcpy(&v, d.data() + p, 4);
        p += 4;
        return v;
    };
    auto get64 = [&]() {
        int64_t v;
        std::memcpy(&v, d.data() + p, 8);
        p += 8;
        return v;
    };
    if (get32() != 1) return std::nullopt;
    WaveformPeaks w;
    w.sampleRate = get32();
    w.channels = get32();
    w.frames = get64();
    const int64_t bins = get64();
    if (w.channels <= 0 || w.channels > 8 || bins < 0 ||
        d.size() != p + static_cast<size_t>(bins) * static_cast<size_t>(w.channels) * sizeof(MinMax))
        return std::nullopt;
    std::vector<std::vector<MinMax>> base(static_cast<size_t>(w.channels), std::vector<MinMax>(static_cast<size_t>(bins)));
    for (auto& ch : base) {
        std::memcpy(ch.data(), d.data() + p, ch.size() * sizeof(MinMax));
        p += ch.size() * sizeof(MinMax);
    }
    w.levels.push_back(std::move(base));
    while (w.levels.back()[0].size() > 1) {
        const auto& prev = w.levels.back();
        const size_t n = (prev[0].size() + kLevelFactor - 1) / kLevelFactor;
        std::vector<std::vector<MinMax>> lvl(static_cast<size_t>(w.channels), std::vector<MinMax>(n));
        for (int c = 0; c < w.channels; ++c)
            for (size_t i = 0; i < n; ++i) {
                MinMax m{127, -127};
                for (size_t k = i * kLevelFactor; k < std::min(prev[0].size(), (i + 1) * kLevelFactor); ++k) {
                    m.min = std::min(m.min, prev[static_cast<size_t>(c)][k].min);
                    m.max = std::max(m.max, prev[static_cast<size_t>(c)][k].max);
                }
                lvl[static_cast<size_t>(c)][i] = m;
            }
        w.levels.push_back(std::move(lvl));
    }
    return w;
}

Result<WaveformPeaks> analyzeWaveform(const std::string& path, int streamIndex, const CancelToken& cancel, JobContext* progress) {
    // Peaks at 24 kHz are visually identical and halve the decode cost.
    AudioFormat fmt{24000, 2};
    auto reader = openAudioReader(path, streamIndex, fmt);
    if (!reader) return reader.status();
    const int64_t len = (*reader)->lengthFrames();
    std::vector<float> all(static_cast<size_t>(std::max<int64_t>(0, len)) * 2);
    const int64_t chunk = fmt.sampleRate * 10;
    for (int64_t pos = 0; pos < len; pos += chunk) {
        if (cancel.cancelled()) return Result<WaveformPeaks>::error("cancelled");
        const int64_t n = std::min(chunk, len - pos);
        (*reader)->read(pos, n, all.data() + pos * 2, cancel);
        if (progress) progress->setProgress(static_cast<float>(pos + n) / static_cast<float>(std::max<int64_t>(1, len)));
    }
    return WaveformPeaks::fromSamples(all.data(), len, 2, fmt.sampleRate);
}

}  // namespace avc::audio
