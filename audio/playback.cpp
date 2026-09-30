#include "audio/playback.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/platform.h"

namespace avc::audio {

namespace {
constexpr int kBlockFrames = 1024;
constexpr size_t kTargetQueued = 48000 / 5;  // ~200 ms ahead
}  // namespace

PlaybackEngine::PlaybackEngine(SnapshotFn snapshot, std::unique_ptr<IAudioOutput> output)
    : snapshot_(std::move(snapshot)),
      output_(output ? std::move(output) : createNullOutput()),
      sources_(output_->sampleRate()),
      mixer_(sources_),
      sampleRate_(output_->sampleRate()) {
    producer_ = std::thread([this] { producerLoop(); });
    output_->start([this](float* out, int frames) { callback(out, frames); });
}

PlaybackEngine::~PlaybackEngine() {
    output_->stop();
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (producer_.joinable()) producer_.join();
}

Time PlaybackEngine::position() const {
    double pos = playPos_.load();
    if (playing_) pos -= static_cast<double>(output_->latencyFrames()) * speed_.load();
    if (pos < 0) pos = 0;
    return Time::fromSamples(static_cast<int64_t>(std::llround(pos)), sampleRate_);
}

void PlaybackEngine::restartProducer(double samplePos) {
    // Caller holds mutex_.
    ++generation_;
    queue_.clear();
    queuedFrames_ = 0;
    renderPos_ = samplePos;
    playPos_ = samplePos;
    resetMixer_ = true;
}

void PlaybackEngine::play(double speed) {
    if (speed == 0.0) {
        pause();
        return;
    }
    std::lock_guard lock(mutex_);
    const double pos = playPos_.load();
    speed_ = std::clamp(speed, -8.0, 8.0);
    playing_ = true;
    restartProducer(pos);
    cv_.notify_all();
}

void PlaybackEngine::pause() {
    std::lock_guard lock(mutex_);
    if (!playing_) return;
    // Keep the position the listener actually heard.
    const double pos = std::max(0.0, playPos_.load() - static_cast<double>(output_->latencyFrames()) * speed_.load());
    playing_ = false;
    restartProducer(pos);
}

void PlaybackEngine::seek(Time t) {
    std::lock_guard lock(mutex_);
    restartProducer(static_cast<double>(std::max<int64_t>(0, t.toSamples(sampleRate_, Rounding::Nearest))));
    cv_.notify_all();
}

void PlaybackEngine::scrub(Time t) {
    std::lock_guard lock(mutex_);
    const double pos = static_cast<double>(std::max<int64_t>(0, t.toSamples(sampleRate_, Rounding::Nearest)));
    if (!playing_) {
        ++generation_;
        queue_.clear();
        queuedFrames_ = 0;
        playPos_ = pos;
        if (scrubAudio_) {
            scrubPending_ = true;
            scrubPos_ = pos;
        }
    } else {
        restartProducer(pos);
    }
    cv_.notify_all();
}

void PlaybackEngine::setLoop(std::optional<TimeRange> range) {
    std::lock_guard lock(mutex_);
    loop_ = range;
}

void PlaybackEngine::invalidate() {
    std::lock_guard lock(mutex_);
    resetMixer_ = true;
    sources_.clear();
}

void PlaybackEngine::producerLoop() {
    setCurrentThreadName("audio-mixer");
    std::vector<float> tmp;
    for (;;) {
        double pos = 0, spd = 1;
        uint64_t gen = 0;
        bool scrub = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(5), [this] {
                return stop_ || scrubPending_ || (playing_ && queuedFrames_ < kTargetQueued);
            });
            if (stop_) return;
            if (resetMixer_) {
                mixer_.reset();
                resetMixer_ = false;
            }
            if (scrubPending_) {
                scrub = true;
                scrubPending_ = false;
                pos = scrubPos_;
                spd = 1.0;
            } else if (playing_ && queuedFrames_ < kTargetQueued) {
                pos = renderPos_;
                spd = speed_.load();
                if (loop_ && spd > 0) {
                    const double ls = static_cast<double>(loop_->start.toSamples(sampleRate_));
                    const double le = static_cast<double>(loop_->end().toSamples(sampleRate_));
                    if (le > ls && pos >= le) pos = ls + std::fmod(pos - ls, le - ls);
                }
            } else {
                continue;
            }
            gen = generation_;
        }
        auto [project, seqId] = snapshot_();
        const Sequence* seq = project ? project->findSequence(seqId) : nullptr;
        Block block;
        block.generation = gen;
        block.startSample = pos;
        block.speed = spd;
        const int frames = scrub ? sampleRate_ * 70 / 1000 : kBlockFrames;
        block.data.assign(static_cast<size_t>(frames) * 2, 0.0f);
        const double aspd = std::fabs(spd);
        if (seq && aspd <= 4.0) {
            if (aspd == 1.0 && spd > 0) {
                mixer_.render(project, *seq, static_cast<int64_t>(std::llround(pos)), frames, block.data.data());
            } else {
                // Varispeed shuttle: render the covered span and resample.
                const int span = static_cast<int>(std::ceil(frames * aspd)) + 2;
                tmp.assign(static_cast<size_t>(span) * 2, 0.0f);
                const double first = spd > 0 ? pos : pos - frames * aspd;
                mixer_.render(project, *seq, static_cast<int64_t>(std::floor(first)), span, tmp.data());
                for (int i = 0; i < frames; ++i) {
                    const double p = spd > 0 ? i * aspd : (frames - i) * aspd;
                    const int i0 = std::clamp(static_cast<int>(p), 0, span - 2);
                    const float f = static_cast<float>(p - i0);
                    for (int c = 0; c < 2; ++c)
                        block.data[static_cast<size_t>(i * 2 + c)] =
                            tmp[static_cast<size_t>(i0 * 2 + c)] * (1 - f) + tmp[static_cast<size_t>((i0 + 1) * 2 + c)] * f;
                }
            }
            if (scrub) {
                // Short fades to avoid clicks.
                const int fade = std::min(frames / 4, 240);
                for (int i = 0; i < fade; ++i) {
                    const float g = static_cast<float>(i) / static_cast<float>(fade);
                    for (int c = 0; c < 2; ++c) {
                        block.data[static_cast<size_t>(i * 2 + c)] *= g;
                        block.data[static_cast<size_t>((frames - 1 - i) * 2 + c)] *= g;
                    }
                }
                block.speed = 0;  // scrub snippets do not advance the transport
            }
        }
        std::lock_guard lock(mutex_);
        if (gen != generation_) continue;  // seeked meanwhile
        if (!scrub) renderPos_ = pos + frames * spd;
        queuedFrames_ += static_cast<size_t>(frames);
        queue_.push_back(std::move(block));
    }
}

void PlaybackEngine::callback(float* out, int frames) {
    std::fill(out, out + frames * 2, 0.0f);
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    int written = 0;
    const bool wasPlaying = playing_;
    while (written < frames && !queue_.empty()) {
        Block& b = queue_.front();
        if (b.generation != generation_) {
            queuedFrames_ -= (b.data.size() / 2 - b.consumed);
            queue_.pop_front();
            continue;
        }
        const size_t avail = b.data.size() / 2 - b.consumed;
        const size_t n = std::min<size_t>(avail, static_cast<size_t>(frames - written));
        std::memcpy(out + written * 2, b.data.data() + b.consumed * 2, n * 2 * sizeof(float));
        b.consumed += n;
        written += static_cast<int>(n);
        queuedFrames_ -= n;
        if (b.speed != 0) playPos_ = b.startSample + static_cast<double>(b.consumed) * b.speed;
        if (b.consumed * 2 >= b.data.size()) queue_.pop_front();
    }
    if (wasPlaying && written < frames) {
        underruns_.fetch_add(1);
        // Keep the clock moving so video does not freeze during a starved block.
        playPos_ = playPos_.load() + static_cast<double>(frames - written) * speed_.load();
    }
    if (wasPlaying) {
        const double end = static_cast<double>(Time{endTicks_.load()}.toSamples(sampleRate_));
        if (!loop_ && end > 0 && speed_ > 0 && playPos_.load() >= end) {
            playing_ = false;
            restartProducer(end);
        }
        if (speed_ < 0 && playPos_.load() <= 0) {
            playing_ = false;
            restartProducer(0);
        }
    }
    cv_.notify_all();
}

}  // namespace avc::audio
