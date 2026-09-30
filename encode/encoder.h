#pragma once
// Encoding layer (libavcodec / libavformat).
//
// EncoderCatalog discovers which encoders really work on this machine by
// opening each candidate once (hardware encoders fail without the matching
// GPU/driver), in preference order: NVIDIA NVENC, AMD AMF, Intel QSV,
// Windows Media Foundation, then software encoders.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/time.h"

struct AVPacket;
struct AVCodecContext;
struct AVFormatContext;

namespace avc::enc {

enum class VideoCodec { H264, HEVC, AV1, ProRes, DNxHR, VP9, MPEG4 };
enum class AudioCodec { AAC, Opus, PCM16, PCM24, FLAC, MP3 };
enum class RateControl { Quality, VBR, CBR };
enum class EncoderKind { Nvidia, Amd, Intel, MediaFoundation, Software };

const char* videoCodecName(VideoCodec c);
const char* audioCodecName(AudioCodec c);
const char* encoderKindName(EncoderKind k);

struct EncoderInfo {
    std::string name;         // FFmpeg encoder name ("h264_nvenc")
    std::string displayName;  // "NVIDIA NVENC H.264"
    VideoCodec codec = VideoCodec::H264;
    EncoderKind kind = EncoderKind::Software;
    bool hardware = false;
    bool available = false;
    std::string reason;  // why unavailable
};

class EncoderCatalog {
public:
    static EncoderCatalog& instance();
    // Candidates for a codec in preference order (probes on first call).
    const std::vector<EncoderInfo>& encoders(VideoCodec codec);
    // Best available encoder for a codec; preferHardware=false skips HW.
    const EncoderInfo* best(VideoCodec codec, bool preferHardware = true);
    const EncoderInfo* find(const std::string& name);
    std::vector<VideoCodec> availableCodecs();
    std::string summary();

private:
    EncoderCatalog() = default;
    std::vector<std::pair<VideoCodec, std::vector<EncoderInfo>>> cache_;
};

// Pixel layouts the export pipeline can produce on the GPU.
enum class PlaneLayout { NV12, YUV420P, YUV422P, YUV422P10, YUV420P10, P010 };
PlaneLayout layoutFor(const std::string& encoderName, VideoCodec codec, int bitDepth);

struct VideoEncodeSettings {
    std::string encoder;  // FFmpeg encoder name
    VideoCodec codec = VideoCodec::H264;
    int width = 1920;
    int height = 1080;
    Rational frameRate{30, 1};
    RateControl rateControl = RateControl::VBR;
    int bitrateKbps = 16000;
    int maxBitrateKbps = 0;  // 0 = 1.5x bitrate
    int quality = 20;        // CRF / CQ (lower is better) for RateControl::Quality
    int gopFrames = 0;       // 0 = 2 seconds
    std::string profile;     // "high", "main", "4444", "hq", ...
    int bitDepth = 8;
    bool bt709 = true;       // colour metadata (BT.709 limited range)
};

struct AudioEncodeSettings {
    AudioCodec codec = AudioCodec::AAC;
    int sampleRate = 48000;
    int channels = 2;
    int bitrateKbps = 320;
};

// Planar YUV frame produced by the export conversion.
struct EncodeFrame {
    const uint8_t* planes[3] = {};
    int strides[3] = {};
    int64_t index = 0;  // frame number (pts in 1/fps)
};

class Muxer {
public:
    ~Muxer();
    static Result<std::unique_ptr<Muxer>> create(const std::string& utf8Path, const std::string& container);
    // Adds streams; returns stream index.
    Result<int> addVideo(const VideoEncodeSettings& s, std::string* usedEncoder = nullptr);
    Result<int> addAudio(const AudioEncodeSettings& s);
    Status start();
    // Encodes one video frame (nullptr flushes).
    Status writeVideo(const EncodeFrame* frame);
    // Encodes interleaved float samples (nullptr flushes).
    Status writeAudio(const float* interleaved, int frames);
    Status finish();
    void abort();  // closes without trailer (partial file)
    [[nodiscard]] PlaneLayout videoLayout() const { return layout_; }
    [[nodiscard]] uint64_t bytesWritten() const;
    [[nodiscard]] std::string videoEncoderName() const { return videoEncoderName_; }

private:
    Muxer() = default;
    Status drain(AVCodecContext* ctx, int streamIndex);
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* vctx_ = nullptr;
    AVCodecContext* actx_ = nullptr;
    int vstream_ = -1, astream_ = -1;
    PlaneLayout layout_ = PlaneLayout::YUV420P;
    std::string path_;
    std::string videoEncoderName_;
    bool started_ = false;
    bool finished_ = false;
    // Audio frame assembly (encoder frame_size).
    std::vector<float> audioPending_;
    int64_t audioPts_ = 0;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace avc::enc
