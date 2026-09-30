#include "media/probe.h"

#include <algorithm>

#include "core/file_io.h"
#include "core/log.h"
#include "core/strings.h"
#include "media/ffmpeg_util.h"

namespace avc {

const std::vector<std::string>& supportedVideoExtensions() {
    static const std::vector<std::string> v = {".mp4", ".mov", ".mkv", ".avi", ".webm", ".mxf", ".mpeg", ".mpg",
                                               ".m4v", ".ts",  ".mts", ".m2ts", ".wmv", ".flv", ".3gp", ".ogv",
                                               ".vob", ".dv",  ".y4m", ".gif"};
    return v;
}

const std::vector<std::string>& supportedAudioExtensions() {
    static const std::vector<std::string> v = {".wav", ".mp3", ".aac", ".flac", ".ogg", ".oga", ".m4a",
                                               ".aiff", ".aif", ".opus", ".wma", ".ac3", ".caf", ".w64"};
    return v;
}

const std::vector<std::string>& supportedImageExtensions() {
    static const std::vector<std::string> v = {".png", ".jpg", ".jpeg", ".webp", ".tif", ".tiff", ".bmp",
                                               ".psd", ".tga", ".exr", ".dpx", ".jxl", ".heic"};
    return v;
}

ImportKind classifyExtension(std::string_view ext) {
    const std::string e = toLower(ext);
    auto has = [&](const std::vector<std::string>& v) { return std::find(v.begin(), v.end(), e) != v.end(); };
    if (e == ".avicap") return ImportKind::Project;
    if (e == ".srt" || e == ".vtt" || e == ".ass") return ImportKind::Subtitle;
    if (has(supportedVideoExtensions())) return ImportKind::Video;
    if (has(supportedAudioExtensions())) return ImportKind::Audio;
    if (has(supportedImageExtensions())) return ImportKind::Image;
    return ImportKind::Unsupported;
}

bool isImportableFile(const std::filesystem::path& p) {
    const auto k = classifyExtension(pathToUtf8(p.extension()));
    return k == ImportKind::Video || k == ImportKind::Audio || k == ImportKind::Image;
}

namespace {

const char* nameOr(const char* s, const char* def) { return s ? s : def; }

bool isStillImageDemuxer(const AVFormatContext* fmt) {
    const std::string n = fmt->iformat ? fmt->iformat->name : "";
    return n == "image2" || endsWith(n, "_pipe") || n == "png" || n == "psd";
}

}  // namespace

Result<MediaInfo> probeMedia(const std::string& path) {
    std::string err;
    ff::FormatCtxPtr fmt = ff::openInput(path, &err);
    if (!fmt) return Result<MediaInfo>::error("Cannot open media (" + err + ")");
    MediaInfo info;
    info.container = fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "";
    info.bitRate = fmt->bit_rate;
    const int64_t origin = ff::containerOriginUs(fmt.get());
    info.startTime = Time::fromTimebase(origin, Rational{1, AV_TIME_BASE});
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        info.duration = Time::fromTimebase(fmt->duration, Rational{1, AV_TIME_BASE});

    if (AVDictionaryEntry* tc = av_dict_get(fmt->metadata, "timecode", nullptr, 0)) info.timecode = tc->value;

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        AVCodecParameters* par = st->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;  // cover art
            VideoStreamInfo v;
            v.index = static_cast<int>(i);
            v.width = par->width;
            v.height = par->height;
            AVRational fr = av_guess_frame_rate(fmt.get(), st, nullptr);
            if (fr.num <= 0 || fr.den <= 0) fr = st->avg_frame_rate;
            if (fr.num <= 0 || fr.den <= 0) fr = AVRational{30, 1};
            v.frameRate = Rational::fromFrameRate(av_q2d(fr));
            // Keep exact rationals (e.g. 30000/1001) when FFmpeg reports them.
            if ((fr.den == 1001 || fr.den == 1) && fr.num > 0) v.frameRate = Rational{fr.num, fr.den}.reduced();
            if (par->sample_aspect_ratio.num > 0) v.sampleAspect = ff::toRational(par->sample_aspect_ratio).reduced();
            v.codec = avcodec_get_name(par->codec_id);
            const AVPixelFormat pf = static_cast<AVPixelFormat>(par->format);
            v.pixelFormat = nameOr(av_get_pix_fmt_name(pf), "unknown");
            v.bitDepth = pf == AV_PIX_FMT_NONE ? 8 : ff::bitDepthOf(pf);
            v.hasAlpha = pf != AV_PIX_FMT_NONE && ff::hasAlpha(pf);
            v.rotation = ff::displayRotation(st);
            v.colorSpace = nameOr(av_color_space_name(par->color_space), "");
            v.colorTransfer = nameOr(av_color_transfer_name(par->color_trc), "");
            v.colorPrimaries = nameOr(av_color_primaries_name(par->color_primaries), "");
            v.fullRange = par->color_range == AVCOL_RANGE_JPEG;
            v.frameCount = st->nb_frames;
            if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
                const Time d = Time::fromTimebase(st->duration, ff::toRational(st->time_base));
                if (info.duration.ticks <= 0) info.duration = d;
            }
            if (AVDictionaryEntry* tc = av_dict_get(st->metadata, "timecode", nullptr, 0); tc && info.timecode.empty())
                info.timecode = tc->value;
            info.video.push_back(std::move(v));
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            AudioStreamInfo a;
            a.index = static_cast<int>(i);
            a.sampleRate = par->sample_rate;
            a.channels = par->ch_layout.nb_channels;
            a.codec = avcodec_get_name(par->codec_id);
            char layout[128] = {};
            av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
            a.channelLayout = layout;
            if (AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0)) a.language = lang->value;
            if (st->duration != AV_NOPTS_VALUE && st->duration > 0 && info.duration.ticks <= 0)
                info.duration = Time::fromTimebase(st->duration, ff::toRational(st->time_base));
            info.audio.push_back(std::move(a));
        }
    }
    if (info.video.empty() && info.audio.empty()) return Result<MediaInfo>::error("No audio or video streams found");

    const bool still = !info.video.empty() && info.audio.empty() &&
                       (isStillImageDemuxer(fmt.get()) || info.video[0].frameCount == 1 ||
                        (info.video[0].frameCount == 0 && info.duration.ticks <= 0));
    const bool animatedGif = info.container == "gif" && info.video[0].frameCount != 1 && info.duration.ticks > 0;
    if (still && !animatedGif) {
        info.kind = MediaKind::Image;
        info.duration = Time{0};
    } else if (!info.video.empty()) {
        info.kind = MediaKind::Video;
    } else {
        info.kind = MediaKind::Audio;
    }
    if (info.kind != MediaKind::Image && info.duration.ticks <= 0)
        return Result<MediaInfo>::error("Media duration is unknown");
    return info;
}

Result<MediaItem> createMediaItem(const std::string& path) {
    auto info = probeMedia(path);
    if (!info) {
        AVC_WARN("media", "Probe failed for '{}': {}", path, info.errorMessage());
        return info.status();
    }
    const FileIdentity id = fileIdentity(pathFromUtf8(path));
    MediaItem m;
    m.id = newId();
    m.path = path;
    m.name = pathToUtf8(pathFromUtf8(path).filename());
    m.fileSize = id.size;
    m.fileModifiedNs = id.modifiedNs;
    m.info = std::move(*info);
    AVC_INFO("media", "Imported '{}' ({}, {} video / {} audio streams, {:.2f}s)", m.name, m.info.container,
             m.info.video.size(), m.info.audio.size(), m.info.duration.seconds());
    return m;
}

}  // namespace avc
