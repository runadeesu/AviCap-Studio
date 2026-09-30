#pragma once
// Sample-accurate audio reading: decodes one audio stream and delivers
// interleaved float samples at the mixer rate (default 48 kHz stereo).
// Positions are in output sample frames from media time 0.

#include <memory>
#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/result.h"

namespace avc {

struct AudioFormat {
    int sampleRate = 48000;
    int channels = 2;
};

class AudioReader {
public:
    virtual ~AudioReader() = default;
    // Fills `count` frames starting at `startFrame` into `dst` (count * channels
    // floats). Regions before 0 or after the end are silence. Returns false on
    // a hard error (the buffer is still fully written).
    virtual bool read(int64_t startFrame, int64_t count, float* dst, const CancelToken& cancel = {}) = 0;
    [[nodiscard]] virtual int64_t lengthFrames() const = 0;
    [[nodiscard]] virtual const AudioFormat& format() const = 0;
    [[nodiscard]] virtual int sourceSampleRate() const = 0;
    [[nodiscard]] virtual int sourceChannels() const = 0;
};

// streamIndex -1 = best audio stream.
Result<std::unique_ptr<AudioReader>> openAudioReader(const std::string& utf8Path, int streamIndex = -1,
                                                     AudioFormat out = {});

// Convenience: decodes a whole stream (analysis, tests). Mono mixdown if channels == 1.
Result<std::vector<float>> decodeAllAudio(const std::string& utf8Path, AudioFormat out = {}, int streamIndex = -1,
                                          const CancelToken& cancel = {});

}  // namespace avc
