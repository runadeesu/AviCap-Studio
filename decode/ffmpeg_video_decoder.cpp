#include <algorithm>
#include <chrono>
#include <deque>

#include "core/log.h"
#include "core/platform.h"
#include "decode/video_decoder.h"
#include "media/ffmpeg_util.h"

// d3d11.h must be included outside extern "C": the Windows SDK version
// declares C++ operator overloads (MSVC rejects them with C linkage).
#if defined(_WIN32)
#include <d3d11.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#endif
}

namespace avc {

namespace {

PixelFormat mapFormat(AVPixelFormat f, bool& fullRangeHint) {
    fullRangeHint = false;
    switch (f) {
    case AV_PIX_FMT_YUVJ420P: fullRangeHint = true; [[fallthrough]];
    case AV_PIX_FMT_YUV420P: return PixelFormat::YUV420P;
    case AV_PIX_FMT_YUVJ422P: fullRangeHint = true; [[fallthrough]];
    case AV_PIX_FMT_YUV422P: return PixelFormat::YUV422P;
    case AV_PIX_FMT_YUVJ444P: fullRangeHint = true; [[fallthrough]];
    case AV_PIX_FMT_YUV444P: return PixelFormat::YUV444P;
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUV420P12LE: return PixelFormat::YUV420P10;
    case AV_PIX_FMT_YUV422P10LE:
    case AV_PIX_FMT_YUV422P12LE: return PixelFormat::YUV422P10;
    case AV_PIX_FMT_YUV444P10LE:
    case AV_PIX_FMT_YUV444P12LE: return PixelFormat::YUV444P10;
    case AV_PIX_FMT_NV12: return PixelFormat::NV12;
    case AV_PIX_FMT_P010LE: return PixelFormat::P010;
    case AV_PIX_FMT_RGBA: return PixelFormat::RGBA8;
    case AV_PIX_FMT_BGRA: return PixelFormat::BGRA8;
    case AV_PIX_FMT_RGBA64LE: return PixelFormat::RGBA16;
    case AV_PIX_FMT_GRAY8: return PixelFormat::GRAY8;
    case AV_PIX_FMT_YUVA420P: return PixelFormat::YUVA420P;
    default: return PixelFormat::None;
    }
}

ColorMatrix mapMatrix(AVColorSpace cs, int height) {
    switch (cs) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return ColorMatrix::BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return ColorMatrix::BT2020;
    case AVCOL_SPC_BT709: return ColorMatrix::BT709;
    default: return height < 720 ? ColorMatrix::BT601 : ColorMatrix::BT709;
    }
}

ColorTransfer mapTransfer(AVColorTransferCharacteristic t) {
    switch (t) {
    case AVCOL_TRC_SMPTE2084: return ColorTransfer::PQ;
    case AVCOL_TRC_ARIB_STD_B67: return ColorTransfer::HLG;
    case AVCOL_TRC_LINEAR: return ColorTransfer::Linear;
    case AVCOL_TRC_IEC61966_2_1: return ColorTransfer::SRGB;
    default: return ColorTransfer::SDR;
    }
}

ColorPrimaries mapPrimaries(AVColorPrimaries p) {
    switch (p) {
    case AVCOL_PRI_BT2020: return ColorPrimaries::BT2020;
    case AVCOL_PRI_SMPTE432: return ColorPrimaries::DisplayP3;
    case AVCOL_PRI_BT470BG:
    case AVCOL_PRI_SMPTE170M: return ColorPrimaries::BT601;
    default: return ColorPrimaries::BT709;
    }
}

std::shared_ptr<void> ownFrame(AVFrame* ref) {
    return std::shared_ptr<AVFrame>(ref, [](AVFrame* f) { av_frame_free(&f); });
}

class FFmpegVideoDecoder final : public IVideoDecoder {
public:
    ~FFmpegVideoDecoder() override {
        codec_.reset();
        if (hwDevice_) av_buffer_unref(&hwDevice_);
    }

    Status open(const std::string& path, const VideoDecoderOptions& opt) {
        opt_ = opt;
        std::string err;
        fmt_ = ff::openInput(path, &err);
        if (!fmt_) return Status::error("Cannot open '" + path + "': " + err);
        origin_ = ff::containerOriginUs(fmt_.get());
        const AVCodec* codec = nullptr;
        int idx = opt.streamIndex;
        if (idx < 0 || idx >= static_cast<int>(fmt_->nb_streams) ||
            fmt_->streams[idx]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            idx = av_find_best_stream(fmt_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
            if (idx < 0) return Status::error("No video stream");
        }
        streamIndex_ = idx;
        st_ = fmt_->streams[idx];
        for (unsigned i = 0; i < fmt_->nb_streams; ++i)
            if (static_cast<int>(i) != idx) fmt_->streams[i]->discard = AVDISCARD_ALL;
        if (!codec) codec = avcodec_find_decoder(st_->codecpar->codec_id);
        // Prefer libdav1d for AV1 software decoding when available.
        if (st_->codecpar->codec_id == AV_CODEC_ID_AV1 && opt.hardware == HwDecodeMode::Off) {
            if (const AVCodec* d = avcodec_find_decoder_by_name("libdav1d")) codec = d;
        }
        if (!codec) return Status::error(std::string("Unsupported video codec: ") + avcodec_get_name(st_->codecpar->codec_id));
        codec_.reset(avcodec_alloc_context3(codec));
        if (!codec_) return Status::error("Out of memory");
        avcodec_parameters_to_context(codec_.get(), st_->codecpar);
        codec_->pkt_timebase = st_->time_base;
        if (opt.fastDecode) {
            codec_->skip_loop_filter = AVDISCARD_ALL;
            codec_->flags2 |= AV_CODEC_FLAG2_FAST;
        }
        const bool hwOk = opt.hardware != HwDecodeMode::Off && setupHardware(codec);
        const int threads = opt.threads > 0 ? opt.threads : static_cast<int>(std::min(8u, hardwareThreads()));
        codec_->thread_count = hwOk ? 1 : threads;
        codec_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        int r = avcodec_open2(codec_.get(), codec, nullptr);
        if (r < 0 && hwOk) {
            AVC_WARN("decode", "Hardware decoder open failed ({}); falling back to software", ff::errorString(r));
            codec_.reset(avcodec_alloc_context3(codec));
            avcodec_parameters_to_context(codec_.get(), st_->codecpar);
            codec_->pkt_timebase = st_->time_base;
            codec_->thread_count = threads;
            hw_ = false;
            r = avcodec_open2(codec_.get(), codec, nullptr);
        }
        if (r < 0) return Status::error("Cannot open decoder: " + ff::errorString(r));
        decoderName_ = std::string(codec->name) + (hw_ ? " (d3d11va)" : "");

        info_.index = idx;
        info_.width = st_->codecpar->width;
        info_.height = st_->codecpar->height;
        AVRational fr = av_guess_frame_rate(fmt_.get(), st_, nullptr);
        if (fr.num <= 0 || fr.den <= 0) fr = AVRational{25, 1};
        info_.frameRate = Rational{fr.num, fr.den}.reduced();
        info_.codec = avcodec_get_name(st_->codecpar->codec_id);
        info_.rotation = ff::displayRotation(st_);
        frameDur_ = Time::frameDuration(info_.frameRate);
        if (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0)
            duration_ = Time::fromTimebase(fmt_->duration, Rational{1, AV_TIME_BASE});
        else if (st_->duration != AV_NOPTS_VALUE && st_->duration > 0)
            duration_ = Time::fromTimebase(st_->duration, ff::toRational(st_->time_base));
        const std::string demux = fmt_->iformat ? fmt_->iformat->name : "";
        isImage_ = duration_.ticks <= 0 || demux == "image2" || demux.find("_pipe") != std::string::npos ||
                   st_->nb_frames == 1;
        pkt_ = ff::makePacket();
        frame_ = ff::makeFrame();
        swFrame_ = ff::makeFrame();
        return Status::ok();
    }

    Result<VideoFramePtr> frameAt(Time t, const CancelToken& cancel) override {
        const auto t0 = std::chrono::steady_clock::now();
        auto result = frameAtImpl(t, cancel);
        stats_.lastDecodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return result;
    }

    Result<VideoFramePtr> nextFrame(const CancelToken& cancel) override {
        if (pending_) {
            VideoFramePtr f = std::move(pending_);
            pending_.reset();
            remember(f);
            return f;
        }
        auto f = decodeNext(cancel);
        if (!f) return f;
        remember(*f);
        return f;
    }

    const VideoStreamInfo& stream() const override { return info_; }
    Time duration() const override { return duration_; }
    bool hardwareAccelerated() const override { return hw_; }
    std::string decoderName() const override { return decoderName_; }
    const DecoderStats& stats() const override { return stats_; }

private:
    bool setupHardware(const AVCodec* codec) {
#if defined(_WIN32)
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                hwPixFmt_ = cfg->pix_fmt;
                break;
            }
        }
        if (hwPixFmt_ == AV_PIX_FMT_NONE) return false;
        int r;
        if (opt_.d3d11Device) {
            hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            auto* dctx = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
            auto* d3d = static_cast<AVD3D11VADeviceContext*>(dctx->hwctx);
            d3d->device = static_cast<ID3D11Device*>(opt_.d3d11Device);
            d3d->device->AddRef();
            if (opt_.d3d11Lock && opt_.d3d11Unlock) {
                d3d->lock = opt_.d3d11Lock;
                d3d->unlock = opt_.d3d11Unlock;
                d3d->lock_ctx = opt_.d3d11LockCtx;
            }
            r = av_hwdevice_ctx_init(hwDevice_);
        } else {
            r = av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        }
        if (r < 0) {
            AVC_INFO("decode", "D3D11VA unavailable: {}", ff::errorString(r));
            if (hwDevice_) av_buffer_unref(&hwDevice_);
            return false;
        }
        codec_->hw_device_ctx = av_buffer_ref(hwDevice_);
        codec_->opaque = this;
        codec_->extra_hw_frames = 6;
        codec_->get_format = [](AVCodecContext* ctx, const AVPixelFormat* fmts) -> AVPixelFormat {
            auto* self = static_cast<FFmpegVideoDecoder*>(ctx->opaque);
            for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
                if (*p == self->hwPixFmt_) return *p;
            self->hw_ = false;  // profile not supported by the GPU: software path
            for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
                const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
                if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
            }
            return AV_PIX_FMT_NONE;
        };
        hw_ = true;
        return true;
#else
        (void)codec;
        return false;
#endif
    }

    void remember(const VideoFramePtr& f) {
        recent_.push_back(f);
        while (recent_.size() > 4) recent_.pop_front();
        lastPts_ = f->pts;
        havePosition_ = true;
    }

    Status seekTo(Time t) {
        ++stats_.seeks;
        const int64_t ts = ff::timeToTs(maxTime(t, Time{0}), st_->time_base, origin_);
        int r = av_seek_frame(fmt_.get(), streamIndex_, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) r = avformat_seek_file(fmt_.get(), streamIndex_, INT64_MIN, ts, ts, 0);
        if (r < 0) r = av_seek_frame(fmt_.get(), streamIndex_, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        avcodec_flush_buffers(codec_.get());
        demuxEof_ = false;
        sentFlush_ = false;
        pending_.reset();
        havePosition_ = false;
        if (r < 0) return Status::error("Seek failed: " + ff::errorString(r));
        return Status::ok();
    }

    Result<VideoFramePtr> decodeNext(const CancelToken& cancel) {
        for (;;) {
            if (cancel.cancelled()) return Result<VideoFramePtr>::error("cancelled");
            int r = avcodec_receive_frame(codec_.get(), frame_.get());
            if (r == 0) {
                ++stats_.framesDecoded;
                auto f = convert(frame_.get());
                av_frame_unref(frame_.get());
                if (f) return f;
                continue;  // conversion failed: skip frame
            }
            if (r == AVERROR_EOF) return Result<VideoFramePtr>::error("eof");
            if (r != AVERROR(EAGAIN)) {
                AVC_DEBUG("decode", "receive_frame: {}", ff::errorString(r));
                if (sentFlush_) return Result<VideoFramePtr>::error("eof");
            }
            if (demuxEof_) {
                if (sentFlush_) return Result<VideoFramePtr>::error("eof");
                avcodec_send_packet(codec_.get(), nullptr);
                sentFlush_ = true;
                continue;
            }
            r = av_read_frame(fmt_.get(), pkt_.get());
            if (r == AVERROR_EOF || r == AVERROR(EIO)) {
                demuxEof_ = true;
                continue;
            }
            if (r < 0) return Result<VideoFramePtr>::error("read error: " + ff::errorString(r));
            ++stats_.packetsRead;
            if (pkt_->stream_index == streamIndex_) {
                r = avcodec_send_packet(codec_.get(), pkt_.get());
                if (r < 0 && r != AVERROR(EAGAIN)) AVC_DEBUG("decode", "send_packet: {}", ff::errorString(r));
            }
            av_packet_unref(pkt_.get());
        }
    }

    Result<VideoFramePtr> frameAtImpl(Time t, const CancelToken& cancel) {
        if (isImage_) {
            if (!still_) {
                auto f = decodeNext(cancel);
                if (!f) return f;
                auto copy = std::make_shared<VideoFrame>(**f);
                copy->pts = Time{0};
                copy->duration = Time::max();
                still_ = copy;
            }
            return still_;
        }
        if (t.ticks < 0) t = Time{0};
        if (duration_.ticks > 0 && t >= duration_) t = duration_ - Time{1};
        for (auto it = recent_.rbegin(); it != recent_.rend(); ++it)
            if ((*it)->covers(t)) return *it;
        if (pending_ && pending_->covers(t)) {
            VideoFramePtr f = pending_;
            pending_.reset();
            remember(f);
            return f;
        }

        VideoFramePtr prev;
        const bool forward = havePosition_ && t >= lastPts_ && (t - lastPts_) < Time::fromSeconds(2.5);
        if (forward) {
            prev = recent_.empty() ? nullptr : recent_.back();
        } else {
            if (auto st = seekTo(t); !st) AVC_DEBUG("decode", "{}", st.message());
        }
        int retries = 0;
        for (;;) {
            VideoFramePtr f;
            if (pending_) {
                f = std::move(pending_);
                pending_.reset();
            } else {
                auto r = decodeNext(cancel);
                if (!r) {
                    if (r.errorMessage() == "cancelled") return r;
                    if (prev) return prev;  // past the last frame: hold it
                    if (!recent_.empty()) return recent_.back();
                    return Result<VideoFramePtr>::error("No frame at " + std::to_string(t.seconds()) + "s");
                }
                f = *r;
            }
            if (f->pts <= t) {
                remember(f);
                if (t < f->pts + f->duration) return f;
                prev = f;
                continue;
            }
            // f starts after t.
            if (prev) {
                pending_ = f;
                return prev;
            }
            if (retries < 4 && t.ticks > 0) {
                // The demuxer landed after the target (bad index): seek earlier.
                ++retries;
                seekTo(t - Time::fromSeconds(1.0 * retries * retries));
                continue;
            }
            remember(f);  // t precedes the first frame
            return f;
        }
    }

    VideoFramePtr convert(AVFrame* src) {
        auto out = std::make_shared<VideoFrame>();
        out->serial = nextFrameSerial();
        out->rotation = info_.rotation;
        const int64_t bets = src->best_effort_timestamp != AV_NOPTS_VALUE ? src->best_effort_timestamp : src->pts;
        out->pts = ff::tsToTime(bets, st_->time_base, origin_);
        const int64_t dur = ff::frameDurationTs(src);
        out->duration = dur > 0 ? Time::fromTimebase(dur, ff::toRational(st_->time_base)) : frameDur_;
        if (out->duration.ticks <= 0) out->duration = frameDur_;
        out->matrix = mapMatrix(src->colorspace, src->height);
        out->transfer = mapTransfer(src->color_trc);
        out->primaries = mapPrimaries(src->color_primaries);
        out->fullRange = src->color_range == AVCOL_RANGE_JPEG;
        if (src->sample_aspect_ratio.num > 0) out->sampleAspect = ff::toRational(src->sample_aspect_ratio).reduced();

        AVFrame* sw = src;
        if (src->hw_frames_ctx) {
#if defined(_WIN32)
            if (opt_.hardware == HwDecodeMode::Keep && src->format == AV_PIX_FMT_D3D11 && opt_.maxWidth <= 0) {
                auto* frames = reinterpret_cast<AVHWFramesContext*>(src->hw_frames_ctx->data);
                out->width = src->width;
                out->height = src->height;
                out->hw = HwFrameKind::D3D11;
                out->hwTexture = src->data[0];
                out->hwSubresource = static_cast<int>(reinterpret_cast<intptr_t>(src->data[1]));
                out->hwFormat = frames->sw_format == AV_PIX_FMT_P010LE ? PixelFormat::P010 : PixelFormat::NV12;
                out->format = out->hwFormat;
                out->bitDepth = out->hwFormat == PixelFormat::P010 ? 10 : 8;
                AVFrame* ref = av_frame_clone(src);
                out->owner = ownFrame(ref);
                return out;
            }
#endif
            av_frame_unref(swFrame_.get());
            if (av_hwframe_transfer_data(swFrame_.get(), src, 0) < 0) return nullptr;
            av_frame_copy_props(swFrame_.get(), src);
            sw = swFrame_.get();
        }

        const AVPixelFormat pf = static_cast<AVPixelFormat>(sw->format);
        bool fullHint = false;
        PixelFormat mapped = mapFormat(pf, fullHint);
        if (fullHint) out->fullRange = true;
        const bool downscale = opt_.maxWidth > 0 || opt_.maxHeight > 0;
        if (mapped != PixelFormat::None && !downscale) {
            out->width = sw->width;
            out->height = sw->height;
            out->format = mapped;
            out->bitDepth = ff::bitDepthOf(pf);
            for (int p = 0; p < planeCount(mapped); ++p) {
                out->planes[static_cast<size_t>(p)] = sw->data[p];
                out->strides[static_cast<size_t>(p)] = sw->linesize[p];
            }
            AVFrame* ref = av_frame_clone(sw);
            out->owner = ownFrame(ref);
            return out;
        }
        // Convert with swscale (unsupported layouts or downscaled output).
        int dw = sw->width, dh = sw->height;
        if (downscale) {
            const double sx = opt_.maxWidth > 0 ? double(opt_.maxWidth) / dw : 1e9;
            const double sy = opt_.maxHeight > 0 ? double(opt_.maxHeight) / dh : 1e9;
            const double s = std::min({sx, sy, 1.0});
            dw = std::max(2, static_cast<int>(dw * s) & ~1);
            dh = std::max(2, static_cast<int>(dh * s) & ~1);
        }
        const bool deep = ff::bitDepthOf(pf) > 8 && !downscale;
        const AVPixelFormat dstFmt = deep ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
        auto frame = allocateFrame(dw, dh, deep ? PixelFormat::RGBA16 : PixelFormat::RGBA8);
        SwsContext* ctx = sws_getCachedContext(sws_.release(), sw->width, sw->height, pf, dw, dh, dstFmt,
                                               downscale ? SWS_AREA : SWS_BICUBIC, nullptr, nullptr, nullptr);
        sws_.reset(ctx);
        if (!ctx) return nullptr;
        // Tell swscale the source colorimetry.
        const int* coeffs = sws_getCoefficients(out->matrix == ColorMatrix::BT601 ? SWS_CS_ITU601
                                                : out->matrix == ColorMatrix::BT2020 ? SWS_CS_BT2020
                                                                                      : SWS_CS_ITU709);
        sws_setColorspaceDetails(ctx, coeffs, out->fullRange ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16,
                                 1 << 16);
        uint8_t* dst[4] = {const_cast<uint8_t*>(frame->planes[0]), nullptr, nullptr, nullptr};
        int dstStride[4] = {frame->strides[0], 0, 0, 0};
        sws_scale(ctx, sw->data, sw->linesize, 0, sw->height, dst, dstStride);
        frame->pts = out->pts;
        frame->duration = out->duration;
        frame->rotation = out->rotation;
        frame->sampleAspect = out->sampleAspect;
        frame->fullRange = true;
        frame->bitDepth = deep ? 16 : 8;
        return frame;
    }

    VideoDecoderOptions opt_;
    ff::FormatCtxPtr fmt_;
    ff::CodecCtxPtr codec_;
    AVStream* st_ = nullptr;
    int streamIndex_ = -1;
    int64_t origin_ = 0;
    VideoStreamInfo info_;
    Time duration_;
    Time frameDur_;
    bool isImage_ = false;
    ff::PacketPtr pkt_;
    ff::FramePtr frame_;
    ff::FramePtr swFrame_;
    ff::SwsPtr sws_;
    bool demuxEof_ = false;
    bool sentFlush_ = false;
    std::deque<VideoFramePtr> recent_;
    VideoFramePtr pending_;
    VideoFramePtr still_;
    Time lastPts_;
    bool havePosition_ = false;
    AVBufferRef* hwDevice_ = nullptr;
    AVPixelFormat hwPixFmt_ = AV_PIX_FMT_NONE;
    bool hw_ = false;
    std::string decoderName_;
    DecoderStats stats_;
};

}  // namespace

Result<std::unique_ptr<IVideoDecoder>> openVideoDecoder(const std::string& path, const VideoDecoderOptions& opt) {
    auto dec = std::make_unique<FFmpegVideoDecoder>();
    if (auto st = dec->open(path, opt); !st) return st;
    return std::unique_ptr<IVideoDecoder>(std::move(dec));
}

std::string hardwareDecodeSummary() {
#if defined(_WIN32)
    AVBufferRef* dev = nullptr;
    const int r = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (r < 0) return "D3D11VA: unavailable (" + ff::errorString(r) + ")";
    av_buffer_unref(&dev);
    std::string s = "D3D11VA: available";
    for (const char* name : {"h264_cuvid", "hevc_cuvid"})
        if (avcodec_find_decoder_by_name(name)) s += std::string(", ") + name + " built in";
    return s;
#else
    return "Hardware decode: not available on this platform build";
#endif
}

}  // namespace avc
