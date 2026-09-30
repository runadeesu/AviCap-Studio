#include "decode/audio_reader.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "media/ffmpeg_util.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace avc {

namespace {

class FFmpegAudioReader final : public AudioReader {
public:
    Status open(const std::string& path, int streamIndex, AudioFormat out) {
        out_ = out;
        std::string err;
        fmt_ = ff::openInput(path, &err);
        if (!fmt_) return Status::error("Cannot open '" + path + "': " + err);
        origin_ = ff::containerOriginUs(fmt_.get());
        const AVCodec* codec = nullptr;
        int idx = streamIndex;
        if (idx < 0 || idx >= static_cast<int>(fmt_->nb_streams) ||
            fmt_->streams[idx]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            idx = av_find_best_stream(fmt_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
            if (idx < 0) return Status::error("No audio stream");
        }
        streamIndex_ = idx;
        st_ = fmt_->streams[idx];
        for (unsigned i = 0; i < fmt_->nb_streams; ++i)
            if (static_cast<int>(i) != idx) fmt_->streams[i]->discard = AVDISCARD_ALL;
        if (!codec) codec = avcodec_find_decoder(st_->codecpar->codec_id);
        if (!codec) return Status::error(std::string("Unsupported audio codec: ") + avcodec_get_name(st_->codecpar->codec_id));
        codec_.reset(avcodec_alloc_context3(codec));
        avcodec_parameters_to_context(codec_.get(), st_->codecpar);
        codec_->pkt_timebase = st_->time_base;
        int r = avcodec_open2(codec_.get(), codec, nullptr);
        if (r < 0) return Status::error("Cannot open audio decoder: " + ff::errorString(r));
        srcRate_ = codec_->sample_rate;
        srcChannels_ = codec_->ch_layout.nb_channels;
        Time dur;
        if (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0)
            dur = Time::fromTimebase(fmt_->duration, Rational{1, AV_TIME_BASE});
        else if (st_->duration != AV_NOPTS_VALUE)
            dur = Time::fromTimebase(st_->duration, ff::toRational(st_->time_base));
        length_ = dur.toSamples(out_.sampleRate, Rounding::Nearest);
        pkt_ = ff::makePacket();
        frame_ = ff::makeFrame();
        return Status::ok();
    }

    bool read(int64_t start, int64_t count, float* dst, const CancelToken& cancel) override {
        const int ch = out_.channels;
        std::fill(dst, dst + count * ch, 0.0f);
        int64_t a = std::max<int64_t>(start, 0);
        const int64_t b = std::min<int64_t>(start + count, length_ > 0 ? length_ : start + count);
        if (a >= b) return true;
        const int64_t kFarAhead = static_cast<int64_t>(out_.sampleRate) * 4;
        if (bufFrames() == 0 || a < bufStart_ || a > bufEnd() + kFarAhead) {
            if (!(eof_ && bufFrames() > 0 && a >= bufStart_)) seek(a);
        }
        while (bufEnd() < b && !eof_) {
            if (cancel.cancelled()) return false;
            if (!decodeMore()) break;
        }
        const int64_t s = std::max(a, bufStart_);
        const int64_t e = std::min(b, bufEnd());
        if (e > s) {
            std::memcpy(dst + (s - start) * ch, buf_.data() + (s - bufStart_) * ch,
                        static_cast<size_t>(e - s) * static_cast<size_t>(ch) * sizeof(float));
        }
        // Keep ~0.5 s of history for overlapping reads (crossfades, scrubbing).
        const int64_t keepFrom = a - out_.sampleRate / 2;
        if (keepFrom > bufStart_) {
            const int64_t drop = std::min(keepFrom - bufStart_, bufFrames());
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop * ch));
            bufStart_ += drop;
        }
        return !error_;
    }

    int64_t lengthFrames() const override { return length_; }
    const AudioFormat& format() const override { return out_; }
    int sourceSampleRate() const override { return srcRate_; }
    int sourceChannels() const override { return srcChannels_; }

private:
    int64_t bufFrames() const { return static_cast<int64_t>(buf_.size()) / out_.channels; }
    int64_t bufEnd() const { return bufStart_ + bufFrames(); }

    void seek(int64_t targetFrame) {
        const Time t = Time::fromSamples(targetFrame, out_.sampleRate) - Time::fromSeconds(0.2);  // decoder pre-roll
        const int64_t ts = ff::timeToTs(maxTime(t, Time{0}), st_->time_base, origin_);
        if (targetFrame <= 0 || ts <= 0) {
            av_seek_frame(fmt_.get(), streamIndex_, INT64_MIN, AVSEEK_FLAG_BACKWARD);
            avformat_seek_file(fmt_.get(), streamIndex_, INT64_MIN, 0, 0, 0);
        } else if (av_seek_frame(fmt_.get(), streamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            avformat_seek_file(fmt_.get(), streamIndex_, INT64_MIN, ts, ts, 0);
        }
        avcodec_flush_buffers(codec_.get());
        swr_.reset();
        buf_.clear();
        bufStart_ = 0;
        needPosition_ = true;
        eof_ = false;
        demuxEof_ = false;
        sentFlush_ = false;
        target_ = targetFrame;
    }

    bool ensureResampler(const AVFrame* f) {
        if (swr_) return true;
        SwrContext* s = nullptr;
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, out_.channels);
        AVChannelLayout inLayout;
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels <= 0)
            av_channel_layout_default(&inLayout, std::max(1, f->ch_layout.nb_channels));
        else
            av_channel_layout_copy(&inLayout, &f->ch_layout);
        int r = swr_alloc_set_opts2(&s, &outLayout, AV_SAMPLE_FMT_FLT, out_.sampleRate, &inLayout,
                                    static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
        const bool monoIn = inLayout.nb_channels == 1;
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        if (r < 0 || !s) return false;
        // Mono sources play at unity in both channels (not the -3 dB centre mix).
        if (monoIn && out_.channels == 2) {
            const double matrix[2] = {1.0, 1.0};  // out[ch][in], stride 1
            swr_set_matrix(s, matrix, 1);
        }
        if (swr_init(s) < 0) {
            swr_free(&s);
            return false;
        }
        swr_.reset(s);
        return true;
    }

    // Appends converted samples of one decoded frame.
    void append(AVFrame* f) {
        if (!ensureResampler(f)) {
            error_ = true;
            return;
        }
        if (needPosition_) {
            const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            const Time t = ff::tsToTime(pts == AV_NOPTS_VALUE ? 0 : pts, st_->time_base, origin_);
            bufStart_ = t.toSamples(out_.sampleRate, Rounding::Nearest);
            needPosition_ = false;
        }
        const int maxOut = swr_get_out_samples(swr_.get(), f->nb_samples) + 32;
        const size_t old = buf_.size();
        buf_.resize(old + static_cast<size_t>(maxOut) * static_cast<size_t>(out_.channels));
        uint8_t* outPtr = reinterpret_cast<uint8_t*>(buf_.data() + old);
        const int got = swr_convert(swr_.get(), &outPtr, maxOut, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        buf_.resize(old + static_cast<size_t>(std::max(0, got)) * static_cast<size_t>(out_.channels));
        // Drop samples that precede the seek target by more than the history window.
        const int64_t keepFrom = target_ - out_.sampleRate / 2;
        if (keepFrom > bufStart_) {
            const int64_t drop = std::min(keepFrom - bufStart_, bufFrames());
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop * out_.channels));
            bufStart_ += drop;
        }
    }

    bool decodeMore() {
        for (;;) {
            int r = avcodec_receive_frame(codec_.get(), frame_.get());
            if (r == 0) {
                append(frame_.get());
                av_frame_unref(frame_.get());
                return true;
            }
            if (r == AVERROR_EOF) {
                eof_ = true;
                if (length_ <= 0 || bufEnd() > length_) length_ = std::max(length_, bufEnd());
                return false;
            }
            if (demuxEof_) {
                if (sentFlush_) {
                    eof_ = true;
                    return false;
                }
                avcodec_send_packet(codec_.get(), nullptr);
                sentFlush_ = true;
                continue;
            }
            r = av_read_frame(fmt_.get(), pkt_.get());
            if (r < 0) {
                demuxEof_ = true;
                continue;
            }
            if (pkt_->stream_index == streamIndex_) avcodec_send_packet(codec_.get(), pkt_.get());
            av_packet_unref(pkt_.get());
        }
    }

    AudioFormat out_;
    ff::FormatCtxPtr fmt_;
    ff::CodecCtxPtr codec_;
    AVStream* st_ = nullptr;
    int streamIndex_ = -1;
    int64_t origin_ = 0;
    ff::PacketPtr pkt_;
    ff::FramePtr frame_;
    ff::SwrPtr swr_;
    std::vector<float> buf_;
    int64_t bufStart_ = 0;
    int64_t length_ = 0;
    int64_t target_ = 0;
    bool needPosition_ = true;
    bool eof_ = false;
    bool demuxEof_ = false;
    bool sentFlush_ = false;
    bool error_ = false;
    int srcRate_ = 0;
    int srcChannels_ = 0;
};

}  // namespace

Result<std::unique_ptr<AudioReader>> openAudioReader(const std::string& path, int streamIndex, AudioFormat out) {
    auto r = std::make_unique<FFmpegAudioReader>();
    if (auto st = r->open(path, streamIndex, out); !st) return st;
    return std::unique_ptr<AudioReader>(std::move(r));
}

Result<std::vector<float>> decodeAllAudio(const std::string& path, AudioFormat out, int streamIndex,
                                          const CancelToken& cancel) {
    auto reader = openAudioReader(path, streamIndex, out);
    if (!reader) return reader.status();
    const int64_t len = (*reader)->lengthFrames();
    std::vector<float> samples(static_cast<size_t>(std::max<int64_t>(0, len)) * static_cast<size_t>(out.channels));
    const int64_t chunk = out.sampleRate * 4;
    for (int64_t pos = 0; pos < len; pos += chunk) {
        if (cancel.cancelled()) return Result<std::vector<float>>::error("cancelled");
        const int64_t n = std::min(chunk, len - pos);
        (*reader)->read(pos, n, samples.data() + pos * out.channels, cancel);
    }
    return samples;
}

}  // namespace avc
