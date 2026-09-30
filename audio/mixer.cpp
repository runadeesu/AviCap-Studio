#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/dsp.h"
#include "core/log.h"

namespace avc::audio {

// ---------------------------------------------------------------- Meter

void Meter::update(const float* s, int frames, int channels, float decay) {
    for (int c = 0; c < std::min(channels, 2); ++c) {
        float pk = 0;
        double sum = 0;
        for (int i = 0; i < frames; ++i) {
            const float v = std::fabs(s[i * channels + c]);
            pk = std::max(pk, v);
            sum += static_cast<double>(v) * v;
            if (v >= 1.0f) clipped = true;
        }
        peak[c] = std::max(pk, peak[c] * decay);
        const float blockRms = frames > 0 ? static_cast<float>(std::sqrt(sum / frames)) : 0.0f;
        rms[c] = std::max(blockRms, rms[c] * decay);
    }
    if (channels == 1) {
        peak[1] = peak[0];
        rms[1] = rms[0];
    }
}

// ---------------------------------------------------------------- AudioSourcePool

AudioSourcePool::AudioSourcePool(int sampleRate, size_t maxReaders) : sampleRate_(sampleRate), maxReaders_(maxReaders) {}

AudioReader* AudioSourcePool::reader(const MediaItem& media, int streamIndex, uint64_t slot) {
    const std::string key = media.path + "|" + std::to_string(streamIndex) + "|" + std::to_string(slot);
    auto it = readers_.find(key);
    if (it != readers_.end()) {
        it->second.lastUse = ++counter_;
        return it->second.failed ? nullptr : it->second.reader.get();
    }
    while (readers_.size() >= maxReaders_) {
        auto victim = std::min_element(readers_.begin(), readers_.end(),
                                       [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        readers_.erase(victim);
    }
    Entry e;
    e.lastUse = ++counter_;
    auto r = openAudioReader(media.path, streamIndex, AudioFormat{sampleRate_, 2});
    if (r) {
        e.reader = std::move(*r);
    } else {
        e.failed = true;
        AVC_WARN("audio", "Cannot open audio of '{}': {}", media.path, r.errorMessage());
    }
    AudioReader* ptr = e.reader.get();
    readers_[key] = std::move(e);
    return ptr;
}

void AudioSourcePool::clear() { readers_.clear(); }

// ---------------------------------------------------------------- EffectChain

void SequenceMixer::EffectChain::process(const std::vector<EffectInstance>& instances, float* buf, int frames, Time local,
                                         int sampleRate) {
    if (instances.empty() && effects.empty()) return;
    auto& reg = fx::EffectRegistry::instance();
    std::vector<std::pair<Id, std::unique_ptr<fx::IAudioEffect>>> next;
    for (const auto& inst : instances) {
        const fx::EffectDef* def = reg.find(inst.effectId);
        if (!def || def->kind != fx::EffectKind::Audio || !def->audio) continue;
        std::unique_ptr<fx::IAudioEffect> state;
        for (auto& e : effects)
            if (e.first == inst.id && e.second) {
                state = std::move(e.second);
                break;
            }
        if (!state) {
            state = def->audio();
            state->prepare(sampleRate, 2);
        }
        if (inst.enabled) state->process(buf, frames, inst, local);
        next.emplace_back(inst.id, std::move(state));
    }
    effects = std::move(next);
}

// ---------------------------------------------------------------- SequenceMixer

SequenceMixer::SequenceMixer(AudioSourcePool& sources) : sources_(sources), sampleRate_(sources.sampleRate()) {
    registerAudioEffects();
}

SequenceMixer::~SequenceMixer() = default;

void SequenceMixer::reset() {
    clipChains_.clear();
    trackChains_.clear();
    masterChain_.effects.clear();
    nested_.clear();
}

Meter SequenceMixer::masterMeter() const {
    std::lock_guard lock(meterMutex_);
    return master_;
}

std::map<TrackId, Meter> SequenceMixer::trackMeters() const {
    std::lock_guard lock(meterMutex_);
    return tracks_;
}

void SequenceMixer::resetClipIndicators() {
    std::lock_guard lock(meterMutex_);
    master_.clipped = false;
    for (auto& [id, m] : tracks_) m.clipped = false;
}

void SequenceMixer::readSource(const ProjectPtr& project, const Clip& clip, Time t0, int frames, float* buf, int depth,
                               const CancelToken& cancel) {
    std::fill(buf, buf + frames * 2, 0.0f);
    if (clip.freezeFrame) return;
    const int sr = sampleRate_;
    const bool unity = clip.speed == Rational{1, 1} && !clip.reverse;

    // Source block reader: fills `count` frames starting at source sample `pos`.
    auto readRaw = [&](int64_t pos, int count, float* dst) {
        if (clip.kind == ClipKind::Media) {
            const MediaItem* media = project->findMedia(clip.media);
            if (!media) return;
            AudioReader* r = sources_.reader(*media, clip.streamIndex, clip.id);
            if (r) r->read(pos, count, dst, cancel);
        } else if (clip.kind == ClipKind::Compound) {
            const Sequence* nested = project->findSequence(clip.nested);
            if (!nested || depth >= 8) return;
            auto& child = nested_[clip.id];
            if (!child) child = std::make_unique<SequenceMixer>(sources_);
            child->render(project, *nested, pos, count, dst, cancel);
        }
    };

    if (unity) {
        const Time src = clip.sourceIn + (t0 - clip.start);
        readRaw(src.toSamples(sr, Rounding::Nearest), frames, buf);
        return;
    }
    // Varispeed / reverse: resample the needed source span linearly.
    const double speed = clip.speed.toDouble();
    const double startSec = (t0 - clip.start).seconds();
    const double inSec = clip.sourceIn.seconds();
    const double durSec = clip.duration.seconds();
    auto srcPos = [&](int i) {
        const double local = startSec + static_cast<double>(i) / sr;
        const double s = clip.reverse ? inSec + (durSec - local) * speed : inSec + local * speed;
        return s * sr;
    };
    const double a = srcPos(0), b = srcPos(frames);
    const int64_t lo = static_cast<int64_t>(std::floor(std::min(a, b))) - 1;
    const int64_t hi = static_cast<int64_t>(std::ceil(std::max(a, b))) + 2;
    const int span = static_cast<int>(std::min<int64_t>(hi - lo, static_cast<int64_t>(frames) * 64 + 8));
    tmp_.assign(static_cast<size_t>(span) * 2, 0.0f);
    readRaw(lo, span, tmp_.data());
    for (int i = 0; i < frames; ++i) {
        const double p = srcPos(i) - static_cast<double>(lo);
        const int i0 = static_cast<int>(std::floor(p));
        if (i0 < 0 || i0 + 1 >= span) continue;
        const float f = static_cast<float>(p - i0);
        for (int c = 0; c < 2; ++c)
            buf[i * 2 + c] = tmp_[static_cast<size_t>(i0 * 2 + c)] * (1 - f) + tmp_[static_cast<size_t>((i0 + 1) * 2 + c)] * f;
    }
}

void SequenceMixer::renderClip(const ProjectPtr& project, const Sequence& seq, const Track& track, const Clip& clip,
                               int64_t start, int frames, float* dst, int depth, const CancelToken& cancel) {
    const int sr = sampleRate_;
    const Time t0 = Time::fromSamples(start, sr);
    clipBuf_.resize(static_cast<size_t>(frames) * 2);
    float* buf = clipBuf_.data();
    readSource(project, clip, t0, frames, buf, depth, cancel);
    const Time local = t0 - clip.start;
    clipChains_[clip.id].process(clip.effects, buf, frames, local, sr);
    // Volume envelope + fades, evaluated every 32 samples and interpolated.
    constexpr int kStep = 32;
    float prevGain = -1;
    const float pan = std::clamp(clip.audio.evaluate1("pan", local, 0.0f), -1.0f, 1.0f);
    const float panL = pan > 0 ? 1.0f - pan : 1.0f, panR = pan < 0 ? 1.0f + pan : 1.0f;
    for (int i0 = 0; i0 < frames; i0 += kStep) {
        const int n = std::min(kStep, frames - i0);
        const Time t = t0 + Time::fromSamples(i0 + n, sr);
        const float volDb = clip.audio.evaluate1("volume", t - clip.start, 0.0f) + clip.gainDb;
        const float g = dbToGain(volDb) * audioTransitionGain(track, clip, t - Time::fromSamples(1, sr));
        const float g0 = prevGain < 0 ? g : prevGain;
        for (int k = 0; k < n; ++k) {
            const float gk = g0 + (g - g0) * static_cast<float>(k + 1) / static_cast<float>(n);
            const int i = i0 + k;
            dst[i * 2] += buf[i * 2] * gk * panL;
            dst[i * 2 + 1] += buf[i * 2 + 1] * gk * panR;
        }
        prevGain = g;
    }
}

void SequenceMixer::render(const ProjectPtr& project, const Sequence& seq, int64_t start, int frames, float* out,
                           const CancelToken& cancel) {
    std::fill(out, out + frames * 2, 0.0f);
    const int sr = sampleRate_;
    const Time t0 = Time::fromSamples(start, sr);
    const TimeRange range{t0, Time::fromSamples(frames, sr)};
    const float decay = std::pow(10.0f, -20.0f / 20.0f * static_cast<float>(frames) / static_cast<float>(sr));  // 20 dB/s
    const auto active = activeAudioClips(seq, range);
    std::map<TrackId, Meter> meters;
    {
        std::lock_guard lock(meterMutex_);
        meters = tracks_;
    }
    trackBuf_.resize(static_cast<size_t>(frames) * 2);
    for (int ti : seq.audioTrackIndices()) {
        const Track& track = *seq.tracks[static_cast<size_t>(ti)];
        std::fill(trackBuf_.begin(), trackBuf_.end(), 0.0f);
        bool any = false;
        if (trackAudible(seq, ti)) {
            for (const auto& ref : active) {
                if (ref.trackIndex != ti) continue;
                if (cancel.cancelled()) return;
                renderClip(project, seq, track, *ref.clip, start, frames, trackBuf_.data(), 0, cancel);
                any = true;
            }
        }
        if (any || !track.effects.empty()) trackChains_[track.id].process(track.effects, trackBuf_.data(), frames, t0, sr);
        const float g = dbToGain(track.volumeDb);
        const float pan = std::clamp(track.pan, -1.0f, 1.0f);
        const float gl = g * (pan > 0 ? 1.0f - pan : 1.0f), gr = g * (pan < 0 ? 1.0f + pan : 1.0f);
        for (int i = 0; i < frames; ++i) {
            trackBuf_[static_cast<size_t>(i) * 2] *= gl;
            trackBuf_[static_cast<size_t>(i) * 2 + 1] *= gr;
            out[i * 2] += trackBuf_[static_cast<size_t>(i) * 2];
            out[i * 2 + 1] += trackBuf_[static_cast<size_t>(i) * 2 + 1];
        }
        meters[track.id].update(trackBuf_.data(), frames, 2, decay);
    }
    masterChain_.process(seq.masterEffects, out, frames, t0, sr);
    const float mg = dbToGain(seq.masterVolumeDb);
    if (mg != 1.0f)
        for (int i = 0; i < frames * 2; ++i) out[i] *= mg;
    std::lock_guard lock(meterMutex_);
    tracks_ = std::move(meters);
    master_.update(out, frames, 2, decay);
}

std::vector<float> mixdown(const ProjectPtr& project, const Sequence& seq, TimeRange range, int sampleRate,
                           const CancelToken& cancel) {
    AudioSourcePool pool(sampleRate);
    SequenceMixer mixer(pool);
    const int64_t start = range.start.toSamples(sampleRate, Rounding::Nearest);
    const int64_t total = range.duration.toSamples(sampleRate, Rounding::Nearest);
    std::vector<float> out(static_cast<size_t>(std::max<int64_t>(0, total)) * 2);
    constexpr int kBlock = 4096;
    for (int64_t pos = 0; pos < total; pos += kBlock) {
        if (cancel.cancelled()) break;
        const int n = static_cast<int>(std::min<int64_t>(kBlock, total - pos));
        mixer.render(project, seq, start + pos, n, out.data() + pos * 2, cancel);
    }
    return out;
}

}  // namespace avc::audio
