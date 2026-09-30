#pragma once
// Playback transport driven by the audio device clock.
//
// A producer thread renders mixer blocks ahead of the device into a queue;
// the device callback consumes them. The timeline position reported to the
// video preview is the position of the audio currently leaving the speakers,
// so picture and sound stay locked. Supports JKL shuttle (-8x .. 8x),
// looping over a work area and short scrub snippets while paused.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "audio/mixer.h"
#include "audio/output.h"

namespace avc::audio {

class PlaybackEngine {
public:
    // Returns the project snapshot and the sequence to play (called from the producer thread).
    using SnapshotFn = std::function<std::pair<ProjectPtr, SequenceId>()>;

    PlaybackEngine(SnapshotFn snapshot, std::unique_ptr<IAudioOutput> output);
    ~PlaybackEngine();

    void play(double speed = 1.0);
    void pause();
    void togglePlay() { playing() ? pause() : play(1.0); }
    void seek(Time t);
    // Plays a short snippet at `t` without moving the transport (scrubbing).
    void scrub(Time t);
    void setLoop(std::optional<TimeRange> range);
    void setScrubAudio(bool on) { scrubAudio_ = on; }

    [[nodiscard]] Time position() const;
    [[nodiscard]] bool playing() const { return playing_.load(); }
    [[nodiscard]] double speed() const { return speed_.load(); }
    [[nodiscard]] const IAudioOutput& output() const { return *output_; }
    [[nodiscard]] Meter masterMeter() const { return mixer_.masterMeter(); }
    [[nodiscard]] std::map<TrackId, Meter> trackMeters() const { return mixer_.trackMeters(); }
    [[nodiscard]] uint64_t underruns() const { return underruns_.load() + output_->underruns(); }
    // Drops cached readers/effect state (after relink or project change).
    void invalidate();
    // Optional end-of-timeline handler (stop at end when not looping).
    void setEndTime(Time t) { endTicks_ = t.ticks; }

private:
    struct Block {
        uint64_t generation = 0;
        double startSample = 0;  // timeline sample position of the first frame
        double speed = 1;        // timeline samples advanced per output frame
        std::vector<float> data;
        size_t consumed = 0;     // frames
    };
    void producerLoop();
    void callback(float* out, int frames);
    void restartProducer(double samplePos);

    SnapshotFn snapshot_;
    std::unique_ptr<IAudioOutput> output_;
    AudioSourcePool sources_;
    SequenceMixer mixer_;
    int sampleRate_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Block> queue_;
    size_t queuedFrames_ = 0;
    uint64_t generation_ = 1;
    double renderPos_ = 0;       // producer position (timeline samples)
    std::atomic<double> playPos_{0};  // position of audio at the device
    std::atomic<bool> playing_{false};
    std::atomic<double> speed_{1.0};
    std::optional<TimeRange> loop_;
    std::atomic<int64_t> endTicks_{0};
    std::atomic<bool> scrubAudio_{true};
    bool scrubPending_ = false;
    double scrubPos_ = 0;
    bool stop_ = false;
    bool resetMixer_ = false;
    std::atomic<uint64_t> underruns_{0};
    std::thread producer_;
};

}  // namespace avc::audio
