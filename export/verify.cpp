#include "export/verify.h"

#include <cmath>
#include <cstdio>

#include "core/file_io.h"
#include "core/strings.h"
#include "decode/audio_reader.h"
#include "decode/video_decoder.h"
#include "media/ffmpeg_util.h"
#include "media/probe.h"

namespace avc::exp {

std::string VerifyReport::summary() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s: %dx%d @ %.3f fps, %.2f s, video %s, audio %s (%d ch, %d Hz), %s, %lld packets",
                  ok ? "OK" : "FAILED", width, height, frameRate.toDouble(), durationSec, videoCodec.c_str(),
                  audioCodec.empty() ? "none" : audioCodec.c_str(), audioChannels, sampleRate, formatBytes(fileSize).c_str(),
                  static_cast<long long>(packets));
    std::string s = buf;
    for (const auto& p : problems) s += "\n  - " + p;
    return s;
}

VerifyReport verifyExport(const std::string& path, const VerifyExpectations& ex, const CancelToken& cancel) {
    VerifyReport r;
    const FileIdentity id = fileIdentity(pathFromUtf8(path));
    if (!id.exists) {
        r.problems.push_back("Output file does not exist");
        return r;
    }
    r.fileSize = id.size;
    if (id.size < 1024) r.problems.push_back("Output file is suspiciously small");
    auto info = probeMedia(path);
    if (!info) {
        r.problems.push_back("Output cannot be opened: " + info.errorMessage());
        return r;
    }
    r.durationSec = info->duration.seconds();
    if (const VideoStreamInfo* v = info->primaryVideo()) {
        r.width = v->width;
        r.height = v->height;
        r.frameRate = v->frameRate;
        r.videoCodec = v->codec;
    } else {
        r.problems.push_back("No video stream");
    }
    if (const AudioStreamInfo* a = info->primaryAudio()) {
        r.audioCodec = a->codec;
        r.audioChannels = a->channels;
        r.sampleRate = a->sampleRate;
    }
    if (ex.width && (r.width != ex.width || r.height != ex.height))
        r.problems.push_back("Resolution " + std::to_string(r.width) + "x" + std::to_string(r.height) + " != expected " +
                             std::to_string(ex.width) + "x" + std::to_string(ex.height));
    if (ex.frameRate.num > 0 && std::fabs(r.frameRate.toDouble() - ex.frameRate.toDouble()) > 0.01)
        r.problems.push_back("Frame rate " + r.frameRate.toString() + " != expected " + ex.frameRate.toString());
    if (ex.durationSec > 0) {
        const double tol = std::max(0.1, 2.0 / std::max(1.0, ex.frameRate.toDouble()));
        if (std::fabs(r.durationSec - ex.durationSec) > tol + 0.05)
            r.problems.push_back("Duration " + std::to_string(r.durationSec) + " s != expected " + std::to_string(ex.durationSec) + " s");
    }
    if (ex.audio && r.audioCodec.empty()) r.problems.push_back("Audio track missing");

    // Demux every packet: detects truncation / corruption in the container.
    std::string err;
    ff::FormatCtxPtr fmt = ff::openInput(path, &err);
    if (!fmt) {
        r.problems.push_back("Cannot reopen output: " + err);
    } else {
        ff::PacketPtr pkt = ff::makePacket();
        int rr;
        while ((rr = av_read_frame(fmt.get(), pkt.get())) >= 0) {
            ++r.packets;
            av_packet_unref(pkt.get());
            if ((r.packets & 1023) == 0 && cancel.cancelled()) break;
        }
        if (rr != AVERROR_EOF && !cancel.cancelled()) r.problems.push_back("Read error while scanning: " + ff::errorString(rr));
        if (r.packets == 0) r.problems.push_back("File contains no packets");
    }
    // Decode first / middle / last frames.
    if (!r.videoCodec.empty()) {
        auto dec = openVideoDecoder(path);
        if (!dec) {
            r.problems.push_back("Video cannot be decoded: " + dec.errorMessage());
        } else {
            for (double f : {0.0, 0.5, 0.97}) {
                auto frame = (*dec)->frameAt(Time::fromSeconds(r.durationSec * f), cancel);
                if (frame) ++r.decodedFrames;
                else r.problems.push_back("Frame at " + std::to_string(r.durationSec * f) + " s failed to decode");
            }
        }
    }
    if (!r.audioCodec.empty()) {
        auto rd = openAudioReader(path);
        if (!rd) {
            r.problems.push_back("Audio cannot be decoded: " + rd.errorMessage());
        } else {
            std::vector<float> buf(4800 * 2);
            if (!(*rd)->read(static_cast<int64_t>(r.durationSec * 0.5 * 48000), 4800, buf.data(), cancel))
                r.problems.push_back("Audio decode error");
        }
    }
    r.ok = r.problems.empty();
    return r;
}

}  // namespace avc::exp
