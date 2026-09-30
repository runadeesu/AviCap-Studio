#pragma once
// Audio waveform peaks with a multi-resolution pyramid (LOD) so the timeline
// can draw any zoom level without re-reading audio.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/result.h"

namespace avc::audio {

class WaveformPeaks {
public:
    static constexpr int kBaseBin = 256;  // samples per bin at level 0
    static constexpr int kLevelFactor = 4;

    struct MinMax {
        int8_t min = 0;
        int8_t max = 0;
    };

    int sampleRate = 48000;
    int channels = 2;
    int64_t frames = 0;
    // levels[l][ch] = bins (kBaseBin * 4^l samples each)
    std::vector<std::vector<std::vector<MinMax>>> levels;

    [[nodiscard]] double durationSeconds() const { return sampleRate > 0 ? static_cast<double>(frames) / sampleRate : 0.0; }
    // Min/max for `columns` equal slices of [startSec, endSec) for a channel
    // (-1 = all channels combined), values in [-1, 1].
    [[nodiscard]] std::vector<std::pair<float, float>> query(double startSec, double endSec, int columns, int channel = -1) const;
    [[nodiscard]] std::string serialize() const;
    static std::optional<WaveformPeaks> deserialize(const std::string& data);
    // Builds from interleaved samples.
    static WaveformPeaks fromSamples(const float* interleaved, int64_t frames, int channels, int sampleRate);
};

// Decodes the audio stream of a file and builds its peaks (cancellable).
Result<WaveformPeaks> analyzeWaveform(const std::string& utf8Path, int streamIndex = -1, const CancelToken& cancel = {},
                                      JobContext* progress = nullptr);

}  // namespace avc::audio
