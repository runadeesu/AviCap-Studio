#pragma once
// Sequence audio mixer.
//
// Renders a block of the timeline mix at the sequence sample rate (stereo,
// interleaved float): per clip source read (sample accurate, varispeed and
// reverse aware) -> clip effects -> clip volume/gain/fades -> pan -> track
// effects -> track volume/pan -> master effects -> master volume. Peak/RMS
// meters are updated per track and for the master bus.

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/jobs.h"
#include "decode/audio_reader.h"
#include "effects/effects.h"
#include "timeline/evaluate.h"

namespace avc::audio {

struct Meter {
    float peak[2] = {0, 0};  // linear, with decay
    float rms[2] = {0, 0};
    bool clipped = false;    // any sample >= 1.0 since last reset
    void update(const float* interleaved, int frames, int channels, float decay);
};

// Shared pool of audio readers keyed by (file, stream, slot).
class AudioSourcePool {
public:
    explicit AudioSourcePool(int sampleRate = 48000, size_t maxReaders = 48);
    AudioReader* reader(const MediaItem& media, int streamIndex, uint64_t slot);
    void clear();
    [[nodiscard]] int sampleRate() const { return sampleRate_; }

private:
    struct Entry {
        std::unique_ptr<AudioReader> reader;
        uint64_t lastUse = 0;
        bool failed = false;
    };
    int sampleRate_;
    size_t maxReaders_;
    uint64_t counter_ = 0;
    std::unordered_map<std::string, Entry> readers_;
};

class SequenceMixer {
public:
    explicit SequenceMixer(AudioSourcePool& sources);
    ~SequenceMixer();

    // Renders `frames` stereo frames starting at timeline sample `start`
    // (sample index at the sequence rate). `out` receives frames*2 floats.
    void render(const ProjectPtr& project, const Sequence& seq, int64_t start, int frames, float* out,
                const CancelToken& cancel = {});
    // Clears effect tails and filter state (call after seeks).
    void reset();

    [[nodiscard]] Meter masterMeter() const;
    [[nodiscard]] std::map<TrackId, Meter> trackMeters() const;
    void resetClipIndicators();

private:
    struct EffectChain {
        std::vector<std::pair<Id, std::unique_ptr<fx::IAudioEffect>>> effects;
        void process(const std::vector<EffectInstance>& instances, float* buf, int frames, Time local, int sampleRate);
    };
    void renderClip(const ProjectPtr& project, const Sequence& seq, const Track& track, const Clip& clip, int64_t start,
                    int frames, float* dst, int depth, const CancelToken& cancel);
    void readSource(const ProjectPtr& project, const Clip& clip, Time t0, int frames, float* buf, int depth,
                    const CancelToken& cancel);

    AudioSourcePool& sources_;
    int sampleRate_ = 48000;
    std::unordered_map<Id, EffectChain> clipChains_;
    std::unordered_map<Id, EffectChain> trackChains_;
    EffectChain masterChain_;
    std::unordered_map<Id, std::unique_ptr<SequenceMixer>> nested_;
    mutable std::mutex meterMutex_;
    Meter master_;
    std::map<TrackId, Meter> tracks_;
    std::vector<float> clipBuf_, trackBuf_, tmp_;
};

// Offline mixdown of a time range (export, analysis). Returns interleaved stereo.
std::vector<float> mixdown(const ProjectPtr& project, const Sequence& seq, TimeRange range, int sampleRate = 48000,
                           const CancelToken& cancel = {});

}  // namespace avc::audio
