#include "media/ffmpeg_util.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "core/log.h"

namespace avc::ff {

std::string errorString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

namespace {
int interruptCb(void* opaque) {
    const bool* flag = static_cast<const bool*>(opaque);
    return flag && *flag ? 1 : 0;
}
}  // namespace

FormatCtxPtr openInput(const std::string& path, std::string* error, const bool* abortFlag) {
    installLogCallback();
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        if (error) *error = "out of memory";
        return nullptr;
    }
    if (abortFlag) {
        fmt->interrupt_callback.callback = interruptCb;
        fmt->interrupt_callback.opaque = const_cast<bool*>(abortFlag);
    }
    int r = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (r < 0) {
        if (error) *error = errorString(r);
        return nullptr;  // fmt freed by avformat_open_input on failure
    }
    FormatCtxPtr ptr(fmt);
    r = avformat_find_stream_info(fmt, nullptr);
    if (r < 0) {
        if (error) *error = errorString(r);
        return nullptr;
    }
    return ptr;
}

int64_t containerOriginUs(const AVFormatContext* fmt) {
    return fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
}

int displayRotation(const AVStream* st) {
    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * 4) return 0;
    double theta = -av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
    theta -= 360.0 * static_cast<int>(theta / 360.0 + 0.9 / 360.0);
    int deg = static_cast<int>(std::lround(theta)) % 360;
    if (deg < 0) deg += 360;
    return (deg + 45) / 90 * 90 % 360;
}

bool isKeyFrame(const AVFrame* f) { return (f->flags & AV_FRAME_FLAG_KEY) != 0; }

int64_t frameDurationTs(const AVFrame* f) { return f->duration; }

int bitDepthOf(AVPixelFormat fmt) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    return d ? d->comp[0].depth : 8;
}

bool hasAlpha(AVPixelFormat fmt) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    return d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
}

namespace {
void logCallback(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char line[1024];
    static thread_local int printPrefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &printPrefix);
    std::string msg(line);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    if (msg.empty()) return;
    if (level <= AV_LOG_ERROR) AVC_DEBUG("ffmpeg", "{}", msg);  // decoder errors are handled by callers
    else AVC_TRACE("ffmpeg", "{}", msg);
}
}  // namespace

void installLogCallback() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(logCallback);
    });
}

}  // namespace avc::ff
