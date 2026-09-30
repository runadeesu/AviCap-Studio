#include "encode/encoder.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "media/ffmpeg_util.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace avc::enc {

const char* videoCodecName(VideoCodec c) {
    switch (c) {
    case VideoCodec::H264: return "H.264";
    case VideoCodec::HEVC: return "HEVC (H.265)";
    case VideoCodec::AV1: return "AV1";
    case VideoCodec::ProRes: return "Apple ProRes";
    case VideoCodec::DNxHR: return "Avid DNxHR";
    case VideoCodec::VP9: return "VP9";
    case VideoCodec::MPEG4: return "MPEG-4 Part 2";
    }
    return "?";
}

const char* audioCodecName(AudioCodec c) {
    switch (c) {
    case AudioCodec::AAC: return "AAC";
    case AudioCodec::Opus: return "Opus";
    case AudioCodec::PCM16: return "PCM 16-bit";
    case AudioCodec::PCM24: return "PCM 24-bit";
    case AudioCodec::FLAC: return "FLAC";
    case AudioCodec::MP3: return "MP3";
    }
    return "?";
}

const char* encoderKindName(EncoderKind k) {
    switch (k) {
    case EncoderKind::Nvidia: return "NVIDIA NVENC";
    case EncoderKind::Amd: return "AMD AMF";
    case EncoderKind::Intel: return "Intel Quick Sync";
    case EncoderKind::MediaFoundation: return "Windows Media Foundation";
    case EncoderKind::Software: return "Software";
    }
    return "?";
}

namespace {

struct Candidate {
    const char* name;
    EncoderKind kind;
};

std::vector<Candidate> candidatesFor(VideoCodec c) {
    switch (c) {
    case VideoCodec::H264:
        return {{"h264_nvenc", EncoderKind::Nvidia}, {"h264_amf", EncoderKind::Amd}, {"h264_qsv", EncoderKind::Intel},
                {"h264_mf", EncoderKind::MediaFoundation}, {"libx264", EncoderKind::Software}, {"libopenh264", EncoderKind::Software}};
    case VideoCodec::HEVC:
        return {{"hevc_nvenc", EncoderKind::Nvidia}, {"hevc_amf", EncoderKind::Amd}, {"hevc_qsv", EncoderKind::Intel},
                {"hevc_mf", EncoderKind::MediaFoundation}, {"libx265", EncoderKind::Software}};
    case VideoCodec::AV1:
        return {{"av1_nvenc", EncoderKind::Nvidia}, {"av1_amf", EncoderKind::Amd}, {"av1_qsv", EncoderKind::Intel},
                {"av1_mf", EncoderKind::MediaFoundation}, {"libsvtav1", EncoderKind::Software}, {"libaom-av1", EncoderKind::Software},
                {"librav1e", EncoderKind::Software}};
    case VideoCodec::ProRes: return {{"prores_ks", EncoderKind::Software}, {"prores_aw", EncoderKind::Software}};
    case VideoCodec::DNxHR: return {{"dnxhd", EncoderKind::Software}};
    case VideoCodec::VP9: return {{"vp9_qsv", EncoderKind::Intel}, {"libvpx-vp9", EncoderKind::Software}};
    case VideoCodec::MPEG4: return {{"mpeg4", EncoderKind::Software}};
    }
    return {};
}

const AVPixelFormat* supportedPixFmts(const AVCodec* codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* out = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &out, &n) >= 0 && out)
        return static_cast<const AVPixelFormat*>(out);
    return nullptr;
#else
    return codec->pix_fmts;
#endif
}

bool supports(const AVCodec* codec, AVPixelFormat f) {
    const AVPixelFormat* fmts = supportedPixFmts(codec);
    if (!fmts) return f == AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == f) return true;
    return false;
}

AVPixelFormat choosePixFmt(const AVCodec* codec, VideoCodec vc, int bitDepth) {
    if (vc == VideoCodec::ProRes) return supports(codec, AV_PIX_FMT_YUV422P10LE) ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV444P10LE;
    if (vc == VideoCodec::DNxHR) return bitDepth > 8 && supports(codec, AV_PIX_FMT_YUV422P10LE) ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
    if (bitDepth > 8) {
        if (supports(codec, AV_PIX_FMT_P010LE)) return AV_PIX_FMT_P010LE;
        if (supports(codec, AV_PIX_FMT_YUV420P10LE)) return AV_PIX_FMT_YUV420P10LE;
    }
    if (supports(codec, AV_PIX_FMT_NV12) && !supports(codec, AV_PIX_FMT_YUV420P)) return AV_PIX_FMT_NV12;
    const std::string n = codec->name;
    if (supports(codec, AV_PIX_FMT_NV12) && (n.find("_nvenc") != std::string::npos || n.find("_amf") != std::string::npos ||
                                             n.find("_qsv") != std::string::npos || n.find("_mf") != std::string::npos))
        return AV_PIX_FMT_NV12;
    if (supports(codec, AV_PIX_FMT_YUV420P)) return AV_PIX_FMT_YUV420P;
    const AVPixelFormat* fmts = supportedPixFmts(codec);
    return fmts ? fmts[0] : AV_PIX_FMT_YUV420P;
}

PlaneLayout layoutOf(AVPixelFormat f) {
    switch (f) {
    case AV_PIX_FMT_NV12: return PlaneLayout::NV12;
    case AV_PIX_FMT_P010LE: return PlaneLayout::P010;
    case AV_PIX_FMT_YUV422P: return PlaneLayout::YUV422P;
    case AV_PIX_FMT_YUV422P10LE: return PlaneLayout::YUV422P10;
    case AV_PIX_FMT_YUV420P10LE: return PlaneLayout::YUV420P10;
    default: return PlaneLayout::YUV420P;
    }
}

void applyRateControl(AVCodecContext* ctx, const std::string& name, const VideoEncodeSettings& s) {
    const int64_t br = static_cast<int64_t>(s.bitrateKbps) * 1000;
    const int64_t maxbr = static_cast<int64_t>(s.maxBitrateKbps > 0 ? s.maxBitrateKbps : s.bitrateKbps * 3 / 2) * 1000;
    const std::string q = std::to_string(s.quality);
    auto opt = [&](const char* k, const std::string& v) { av_opt_set(ctx->priv_data, k, v.c_str(), 0); };
    if (s.codec == VideoCodec::ProRes) {
        opt("profile", s.profile.empty() ? "hq" : s.profile);
        return;
    }
    if (s.codec == VideoCodec::DNxHR) {
        opt("profile", s.profile.empty() ? (s.bitDepth > 8 ? "dnxhr_hqx" : "dnxhr_hq") : s.profile);
        return;
    }
    const bool nvenc = name.find("_nvenc") != std::string::npos;
    const bool amf = name.find("_amf") != std::string::npos;
    const bool qsv = name.find("_qsv") != std::string::npos;
    const bool mf = name.find("_mf") != std::string::npos;
    switch (s.rateControl) {
    case RateControl::Quality:
        if (nvenc) {
            opt("rc", "vbr");
            opt("cq", q);
            ctx->bit_rate = 0;
        } else if (amf) {
            opt("rc", "qvbr");
            opt("qvbr_quality_level", std::to_string(std::clamp(51 - s.quality, 1, 51)));
            ctx->bit_rate = br;
        } else if (qsv) {
            ctx->global_quality = s.quality;
        } else if (mf) {
            opt("rate_control", "quality");
            opt("quality", std::to_string(std::clamp(100 - s.quality * 2, 1, 100)));
        } else if (name == "mpeg4") {
            ctx->flags |= AV_CODEC_FLAG_QSCALE;
            ctx->global_quality = FF_QP2LAMBDA * std::clamp(s.quality / 6, 2, 31);
        } else {
            opt("crf", q);
            if (name == "libvpx-vp9" || name == "libaom-av1") ctx->bit_rate = 0;
        }
        break;
    case RateControl::VBR:
        ctx->bit_rate = br;
        ctx->rc_max_rate = maxbr;
        ctx->rc_buffer_size = static_cast<int>(maxbr * 2);
        if (nvenc) opt("rc", "vbr");
        if (amf) opt("rc", "vbr_peak");
        if (mf) opt("rate_control", "pc_vbr");
        break;
    case RateControl::CBR:
        ctx->bit_rate = br;
        ctx->rc_max_rate = br;
        ctx->rc_min_rate = br;
        ctx->rc_buffer_size = static_cast<int>(br);
        if (nvenc) opt("rc", "cbr");
        if (amf) opt("rc", "cbr");
        if (mf) opt("rate_control", "cbr");
        if (name == "libx264" || name == "libx265") opt("nal-hrd", "cbr");
        break;
    }
    if (nvenc) opt("preset", "p5");
    if (name == "libx264" || name == "libx265") opt("preset", "medium");
    if (name == "libsvtav1") opt("preset", "8");
    if (name == "libaom-av1") opt("cpu-used", "6");
    if (name == "libvpx-vp9") {
        opt("deadline", "good");
        opt("cpu-used", "4");
        opt("row-mt", "1");
    }
    if (mf) {
        opt("hw_encoding", "1");
        opt("scenario", "archive");
    }
    if (!s.profile.empty() && (name == "libx264" || nvenc || amf)) opt("profile", s.profile);
}

// Opens a small test instance of an encoder to see if it works here.
std::string probeEncoder(const AVCodec* codec, VideoCodec vc) {
    ff::installLogCallback();
    ff::CodecCtxPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) return "out of memory";
    ctx->width = 640;
    ctx->height = 360;
    ctx->time_base = AVRational{1, 30};
    ctx->framerate = AVRational{30, 1};
    ctx->pix_fmt = choosePixFmt(codec, vc, 8);
    ctx->bit_rate = 2000000;
    ctx->gop_size = 30;
    if (vc == VideoCodec::DNxHR) av_opt_set(ctx->priv_data, "profile", "dnxhr_hq", 0);
    if (std::string(codec->name).find("_mf") != std::string::npos) av_opt_set(ctx->priv_data, "hw_encoding", "0", 0);
    const int r = avcodec_open2(ctx.get(), codec, nullptr);
    if (r < 0) return ff::errorString(r);
    return {};
}

}  // namespace

PlaneLayout layoutFor(const std::string& encoderName, VideoCodec codec, int bitDepth) {
    const AVCodec* c = avcodec_find_encoder_by_name(encoderName.c_str());
    if (!c) return PlaneLayout::YUV420P;
    return layoutOf(choosePixFmt(c, codec, bitDepth));
}

EncoderCatalog& EncoderCatalog::instance() {
    static EncoderCatalog c;
    return c;
}

const std::vector<EncoderInfo>& EncoderCatalog::encoders(VideoCodec codec) {
    static std::mutex m;
    std::lock_guard lock(m);
    for (auto& [c, list] : cache_)
        if (c == codec) return list;
    std::vector<EncoderInfo> list;
    for (const auto& cand : candidatesFor(codec)) {
        EncoderInfo info;
        info.name = cand.name;
        info.codec = codec;
        info.kind = cand.kind;
        info.hardware = cand.kind != EncoderKind::Software;
        info.displayName = std::string(encoderKindName(cand.kind)) + " " + videoCodecName(codec) + " (" + cand.name + ")";
        const AVCodec* c = avcodec_find_encoder_by_name(cand.name);
        if (!c) {
            info.reason = "not included in this build";
        } else {
            info.reason = probeEncoder(c, codec);
            info.available = info.reason.empty();
        }
        AVC_INFO("encode", "Encoder {}: {}", cand.name, info.available ? "available" : info.reason);
        list.push_back(std::move(info));
    }
    cache_.emplace_back(codec, std::move(list));
    return cache_.back().second;
}

const EncoderInfo* EncoderCatalog::best(VideoCodec codec, bool preferHardware) {
    const auto& list = encoders(codec);
    for (const auto& e : list)
        if (e.available && (preferHardware || !e.hardware)) return &e;
    for (const auto& e : list)
        if (e.available) return &e;
    return nullptr;
}

const EncoderInfo* EncoderCatalog::find(const std::string& name) {
    for (VideoCodec c : {VideoCodec::H264, VideoCodec::HEVC, VideoCodec::AV1, VideoCodec::ProRes, VideoCodec::DNxHR,
                         VideoCodec::VP9, VideoCodec::MPEG4})
        for (const auto& e : encoders(c))
            if (e.name == name) return &e;
    return nullptr;
}

std::vector<VideoCodec> EncoderCatalog::availableCodecs() {
    std::vector<VideoCodec> out;
    for (VideoCodec c : {VideoCodec::H264, VideoCodec::HEVC, VideoCodec::AV1, VideoCodec::ProRes, VideoCodec::DNxHR,
                         VideoCodec::VP9, VideoCodec::MPEG4})
        if (best(c)) out.push_back(c);
    return out;
}

std::string EncoderCatalog::summary() {
    std::string s;
    for (VideoCodec c : {VideoCodec::H264, VideoCodec::HEVC, VideoCodec::AV1, VideoCodec::ProRes, VideoCodec::DNxHR})
        for (const auto& e : encoders(c))
            if (e.available) s += e.name + " ";
    return s.empty() ? "none" : s;
}

// ============================================================ Muxer

struct Muxer::Impl {
    ff::FramePtr vframe;
    ff::FramePtr aframe;
    ff::PacketPtr pkt;
};

Muxer::~Muxer() {
    if (!finished_) abort();
}

Result<std::unique_ptr<Muxer>> Muxer::create(const std::string& path, const std::string& container) {
    ff::installLogCallback();
    std::unique_ptr<Muxer> m(new Muxer());
    m->path_ = path;
    m->impl_ = std::make_shared<Impl>();
    m->impl_->pkt = ff::makePacket();
    const char* fmtName = container.empty() ? nullptr : container.c_str();
    int r = avformat_alloc_output_context2(&m->fmt_, nullptr, fmtName, path.c_str());
    if (r < 0 || !m->fmt_) return Result<std::unique_ptr<Muxer>>::error("Unsupported container: " + ff::errorString(r));
    return m;
}

Result<int> Muxer::addVideo(const VideoEncodeSettings& s, std::string* usedEncoder) {
    const AVCodec* codec = avcodec_find_encoder_by_name(s.encoder.c_str());
    if (!codec) return Result<int>::error("Encoder not available: " + s.encoder);
    vctx_ = avcodec_alloc_context3(codec);
    vctx_->width = s.width & ~1;
    vctx_->height = s.height & ~1;
    const Rational fps = s.frameRate.reduced();
    vctx_->time_base = AVRational{static_cast<int>(fps.den), static_cast<int>(fps.num)};
    vctx_->framerate = AVRational{static_cast<int>(fps.num), static_cast<int>(fps.den)};
    vctx_->sample_aspect_ratio = AVRational{1, 1};
    vctx_->pix_fmt = choosePixFmt(codec, s.codec, s.bitDepth);
    layout_ = layoutOf(vctx_->pix_fmt);
    vctx_->gop_size = s.gopFrames > 0 ? s.gopFrames : static_cast<int>(std::max<int64_t>(1, fps.num * 2 / std::max<int64_t>(1, fps.den)));
    vctx_->color_primaries = AVCOL_PRI_BT709;
    vctx_->color_trc = AVCOL_TRC_BT709;
    vctx_->colorspace = AVCOL_SPC_BT709;
    vctx_->color_range = AVCOL_RANGE_MPEG;
    vctx_->thread_count = 0;
    if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) vctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    applyRateControl(vctx_, codec->name, s);
    int r = avcodec_open2(vctx_, codec, nullptr);
    if (r < 0 && std::string(codec->name).find("_mf") != std::string::npos) {
        // Hardware MFT unavailable: retry with the Windows software encoder.
        avcodec_free_context(&vctx_);

        vctx_ = avcodec_alloc_context3(codec);
        vctx_->width = s.width & ~1;
        vctx_->height = s.height & ~1;
        vctx_->time_base = AVRational{static_cast<int>(fps.den), static_cast<int>(fps.num)};
        vctx_->framerate = AVRational{static_cast<int>(fps.num), static_cast<int>(fps.den)};
        vctx_->pix_fmt = choosePixFmt(codec, s.codec, 8);
        layout_ = layoutOf(vctx_->pix_fmt);
        vctx_->gop_size = std::max(1, static_cast<int>(fps.num * 2 / std::max<int64_t>(1, fps.den)));
        vctx_->bit_rate = static_cast<int64_t>(s.bitrateKbps) * 1000;
        if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) vctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set(vctx_->priv_data, "hw_encoding", "0", 0);
        r = avcodec_open2(vctx_, codec, nullptr);
    }
    if (r < 0) return Result<int>::error("Cannot open encoder " + s.encoder + ": " + ff::errorString(r));
    AVStream* st = avformat_new_stream(fmt_, nullptr);
    avcodec_parameters_from_context(st->codecpar, vctx_);
    st->time_base = vctx_->time_base;
    st->avg_frame_rate = vctx_->framerate;
    vstream_ = st->index;
    videoEncoderName_ = codec->name;
    if (usedEncoder) *usedEncoder = codec->name;
    impl_->vframe = ff::makeFrame();
    return vstream_;
}

Result<int> Muxer::addAudio(const AudioEncodeSettings& s) {
    const char* name = "aac";
    switch (s.codec) {
    case AudioCodec::AAC: name = "aac"; break;
    case AudioCodec::Opus: name = avcodec_find_encoder_by_name("libopus") ? "libopus" : "opus"; break;
    case AudioCodec::PCM16: name = "pcm_s16le"; break;
    case AudioCodec::PCM24: name = "pcm_s24le"; break;
    case AudioCodec::FLAC: name = "flac"; break;
    case AudioCodec::MP3: name = "libmp3lame"; break;
    }
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) return Result<int>::error(std::string("Audio encoder not available: ") + name);
    actx_ = avcodec_alloc_context3(codec);
    actx_->sample_rate = s.sampleRate;
    av_channel_layout_default(&actx_->ch_layout, s.channels);
    // Pick the first supported sample format.
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* fmts = nullptr;
    int nf = 0;
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &fmts, &nf);
    actx_->sample_fmt = fmts ? static_cast<const AVSampleFormat*>(fmts)[0] : AV_SAMPLE_FMT_FLTP;
#else
    actx_->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
#endif
    if (s.codec == AudioCodec::PCM24) actx_->bits_per_raw_sample = 24;
    actx_->bit_rate = static_cast<int64_t>(s.bitrateKbps) * 1000;
    actx_->time_base = AVRational{1, s.sampleRate};
    if (std::string(name) == "opus") actx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) actx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    const int r = avcodec_open2(actx_, codec, nullptr);
    if (r < 0) return Result<int>::error(std::string("Cannot open audio encoder ") + name + ": " + ff::errorString(r));
    AVStream* st = avformat_new_stream(fmt_, nullptr);
    avcodec_parameters_from_context(st->codecpar, actx_);
    st->time_base = actx_->time_base;
    astream_ = st->index;
    impl_->aframe = ff::makeFrame();
    return astream_;
}

Status Muxer::start() {
    if (!(fmt_->oformat->flags & AVFMT_NOFILE)) {
        const int r = avio_open(&fmt_->pb, path_.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) return Status::error("Cannot create output file: " + ff::errorString(r));
    }
    AVDictionary* opts = nullptr;
    const std::string fmtName = fmt_->oformat->name;
    if (fmtName.find("mp4") != std::string::npos || fmtName.find("mov") != std::string::npos)
        av_dict_set(&opts, "movflags", "+faststart", 0);
    const int r = avformat_write_header(fmt_, &opts);
    av_dict_free(&opts);
    if (r < 0) return Status::error("Cannot write file header: " + ff::errorString(r));
    started_ = true;
    return Status::ok();
}

Status Muxer::drain(AVCodecContext* ctx, int streamIndex) {
    AVPacket* pkt = impl_->pkt.get();
    for (;;) {
        const int r = avcodec_receive_packet(ctx, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return Status::ok();
        if (r < 0) return Status::error("Encoding failed: " + ff::errorString(r));
        pkt->stream_index = streamIndex;
        av_packet_rescale_ts(pkt, ctx->time_base, fmt_->streams[streamIndex]->time_base);
        const int w = av_interleaved_write_frame(fmt_, pkt);
        if (w < 0) return Status::error("Write failed (disk full?): " + ff::errorString(w));
    }
}

Status Muxer::writeVideo(const EncodeFrame* f) {
    if (!vctx_) return Status::error("No video stream");
    if (!f) {
        avcodec_send_frame(vctx_, nullptr);
        return drain(vctx_, vstream_);
    }
    AVFrame* frame = impl_->vframe.get();
    av_frame_unref(frame);
    frame->format = vctx_->pix_fmt;
    frame->width = vctx_->width;
    frame->height = vctx_->height;
    frame->color_primaries = vctx_->color_primaries;
    frame->color_trc = vctx_->color_trc;
    frame->colorspace = vctx_->colorspace;
    frame->color_range = vctx_->color_range;
    if (av_frame_get_buffer(frame, 32) < 0) return Status::error("Out of memory");
    const int planes = (layout_ == PlaneLayout::NV12 || layout_ == PlaneLayout::P010) ? 2 : 3;
    const bool wide = layout_ == PlaneLayout::P010 || layout_ == PlaneLayout::YUV422P10 || layout_ == PlaneLayout::YUV420P10;
    const int bpc = wide ? 2 : 1;
    for (int p = 0; p < planes; ++p) {
        const int cw = p == 0 ? frame->width : (layout_ == PlaneLayout::NV12 || layout_ == PlaneLayout::P010 ? frame->width : (frame->width + 1) / 2);
        const int ch = p == 0 ? frame->height
                              : (layout_ == PlaneLayout::YUV422P || layout_ == PlaneLayout::YUV422P10 ? frame->height : (frame->height + 1) / 2);
        const size_t rowBytes = static_cast<size_t>(cw) * static_cast<size_t>(bpc);
        for (int y = 0; y < ch; ++y)
            std::memcpy(frame->data[p] + static_cast<ptrdiff_t>(y) * frame->linesize[p], f->planes[p] + static_cast<ptrdiff_t>(y) * f->strides[p], rowBytes);
    }
    frame->pts = f->index;
    const int r = avcodec_send_frame(vctx_, frame);
    if (r < 0 && r != AVERROR(EAGAIN)) return Status::error("Encoder rejected frame: " + ff::errorString(r));
    return drain(vctx_, vstream_);
}

Status Muxer::writeAudio(const float* s, int frames) {
    if (!actx_) return Status::ok();
    const int ch = actx_->ch_layout.nb_channels;
    if (s) audioPending_.insert(audioPending_.end(), s, s + static_cast<size_t>(frames) * static_cast<size_t>(ch));
    const int fsz = actx_->frame_size > 0 ? actx_->frame_size : 1024;
    AVFrame* frame = impl_->aframe.get();
    auto sendChunk = [&](int n) -> Status {
        av_frame_unref(frame);
        frame->nb_samples = n;
        frame->format = actx_->sample_fmt;
        frame->sample_rate = actx_->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &actx_->ch_layout);
        if (av_frame_get_buffer(frame, 0) < 0) return Status::error("Out of memory");
        const float* src = audioPending_.data();
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < ch; ++c) {
                const float v = std::clamp(src[i * ch + c], -1.0f, 1.0f);
                switch (actx_->sample_fmt) {
                case AV_SAMPLE_FMT_FLTP: reinterpret_cast<float*>(frame->data[c])[i] = v; break;
                case AV_SAMPLE_FMT_FLT: reinterpret_cast<float*>(frame->data[0])[i * ch + c] = v; break;
                case AV_SAMPLE_FMT_S16: reinterpret_cast<int16_t*>(frame->data[0])[i * ch + c] = static_cast<int16_t>(v * 32767.0f); break;
                case AV_SAMPLE_FMT_S16P: reinterpret_cast<int16_t*>(frame->data[c])[i] = static_cast<int16_t>(v * 32767.0f); break;
                case AV_SAMPLE_FMT_S32: reinterpret_cast<int32_t*>(frame->data[0])[i * ch + c] = static_cast<int32_t>(v * 2147483647.0); break;
                case AV_SAMPLE_FMT_S32P: reinterpret_cast<int32_t*>(frame->data[c])[i] = static_cast<int32_t>(v * 2147483647.0); break;
                default: break;
                }
            }
        frame->pts = audioPts_;
        audioPts_ += n;
        audioPending_.erase(audioPending_.begin(), audioPending_.begin() + static_cast<std::ptrdiff_t>(n) * ch);
        const int r = avcodec_send_frame(actx_, frame);
        if (r < 0 && r != AVERROR(EAGAIN)) return Status::error("Audio encoder error: " + ff::errorString(r));
        return drain(actx_, astream_);
    };
    while (static_cast<int>(audioPending_.size()) / ch >= fsz)
        if (auto st = sendChunk(fsz); !st) return st;
    if (!s) {
        const int rest = static_cast<int>(audioPending_.size()) / ch;
        if (rest > 0) {
            if (actx_->frame_size > 0 && !(actx_->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) {
                audioPending_.resize(static_cast<size_t>(fsz) * static_cast<size_t>(ch), 0.0f);  // pad final frame
                if (auto st = sendChunk(fsz); !st) return st;
            } else if (auto st = sendChunk(rest); !st) {
                return st;
            }
        }
        avcodec_send_frame(actx_, nullptr);
        return drain(actx_, astream_);
    }
    return Status::ok();
}

Status Muxer::finish() {
    if (!started_) return Status::error("Muxer not started");
    Status st = Status::ok();
    if (vctx_) st = writeVideo(nullptr);
    if (st && actx_) st = writeAudio(nullptr, 0);
    const int r = av_write_trailer(fmt_);
    if (st && r < 0) st = Status::error("Cannot finalize file: " + ff::errorString(r));
    if (fmt_->pb && !(fmt_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt_->pb);
    avcodec_free_context(&vctx_);
    avcodec_free_context(&actx_);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    finished_ = true;
    return st;
}

void Muxer::abort() {
    if (fmt_) {
        if (fmt_->pb && !(fmt_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
    }
    avcodec_free_context(&vctx_);
    avcodec_free_context(&actx_);
    finished_ = true;
}

uint64_t Muxer::bytesWritten() const {
    if (fmt_ && fmt_->pb) return static_cast<uint64_t>(std::max<int64_t>(0, avio_tell(fmt_->pb)));
    return 0;
}

}  // namespace avc::enc
