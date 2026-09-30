#pragma once
// Shared FFmpeg helpers: RAII wrappers, error strings, time conversion and
// API compatibility across FFmpeg 6.1 - 8.x.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

#include "core/time.h"

namespace avc::ff {

std::string errorString(int err);

struct FormatCtxDeleter {
    void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
struct CodecCtxDeleter {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* s) const { sws_freeContext(s); }
};
struct SwrDeleter {
    void operator()(SwrContext* s) const { swr_free(&s); }
};

using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }

// Opens an input file (UTF-8 path) and reads stream info. Interrupt callback
// aborts blocking IO when `abortFlag` becomes true.
FormatCtxPtr openInput(const std::string& utf8Path, std::string* error = nullptr, const bool* abortFlag = nullptr);

inline Rational toRational(AVRational r) { return Rational{r.num, r.den}; }
inline AVRational toAv(Rational r) { return AVRational{static_cast<int>(r.num), static_cast<int>(r.den)}; }

// Converts a stream timestamp to media Time relative to `origin` (container start).
inline Time tsToTime(int64_t ts, AVRational tb, int64_t originUs) {
    if (ts == AV_NOPTS_VALUE) return Time{0};
    return Time::fromTimebase(ts, toRational(tb)) - Time::fromTimebase(originUs, Rational{1, AV_TIME_BASE});
}
inline int64_t timeToTs(Time t, AVRational tb, int64_t originUs) {
    return (t + Time::fromTimebase(originUs, Rational{1, AV_TIME_BASE})).toTimebase(toRational(tb), Rounding::Down);
}

// Container origin used to make media time start at 0 (keeps A/V offsets).
int64_t containerOriginUs(const AVFormatContext* fmt);

int displayRotation(const AVStream* st);  // 0, 90, 180, 270
bool isKeyFrame(const AVFrame* f);
int64_t frameDurationTs(const AVFrame* f);
int bitDepthOf(AVPixelFormat fmt);
bool hasAlpha(AVPixelFormat fmt);

// Silences FFmpeg console output and routes warnings/errors to our log.
void installLogCallback();

}  // namespace avc::ff
