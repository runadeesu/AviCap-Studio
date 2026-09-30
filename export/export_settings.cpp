#include "export/export_settings.h"

namespace avc::exp {

namespace {
ExportSettings mk(enc::VideoCodec vc, const char* container, int w, int h, int kbps, int audioKbps,
                  enc::RateControl rc = enc::RateControl::VBR) {
    ExportSettings s;
    s.videoCodec = vc;
    s.container = container;
    s.width = w;
    s.height = h;
    s.bitrateKbps = kbps;
    s.audioBitrateKbps = audioKbps;
    s.rateControl = rc;
    return s;
}
}  // namespace

const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = [] {
        std::vector<Preset> v;
        v.push_back({"youtube-1080p", "YouTube 1080p", "H.264 1920x1080, 16 Mbps, AAC 320 kbps",
                     mk(enc::VideoCodec::H264, "mp4", 1920, 1080, 16000, 320)});
        v.push_back({"youtube-4k", "YouTube 4K", "HEVC 3840x2160, 45 Mbps, AAC 320 kbps",
                     mk(enc::VideoCodec::HEVC, "mp4", 3840, 2160, 45000, 320)});
        v.push_back({"tiktok", "TikTok 1080x1920", "H.264 vertical 1080x1920, 12 Mbps",
                     mk(enc::VideoCodec::H264, "mp4", 1080, 1920, 12000, 256)});
        v.push_back({"youtube-shorts", "YouTube Shorts", "H.264 vertical 1080x1920, 14 Mbps",
                     mk(enc::VideoCodec::H264, "mp4", 1080, 1920, 14000, 320)});
        v.push_back({"instagram-reels", "Instagram Reels", "H.264 vertical 1080x1920, 10 Mbps",
                     mk(enc::VideoCodec::H264, "mp4", 1080, 1920, 10000, 256)});
        ExportSettings master = mk(enc::VideoCodec::ProRes, "mov", 0, 0, 0, 0);
        master.profile = "hq";
        master.bitDepth = 10;
        master.audioCodec = enc::AudioCodec::PCM24;
        v.push_back({"master-prores", "High Quality Master (ProRes 422 HQ)", "Apple ProRes 422 HQ 10-bit, PCM 24-bit", master});
        ExportSettings dnx = mk(enc::VideoCodec::DNxHR, "mov", 0, 0, 0, 0);
        dnx.audioCodec = enc::AudioCodec::PCM24;
        v.push_back({"master-dnxhr", "High Quality Master (DNxHR HQ)", "Avid DNxHR HQ, PCM 24-bit", dnx});
        ExportSettings av1 = mk(enc::VideoCodec::AV1, "mp4", 0, 0, 8000, 256, enc::RateControl::VBR);
        v.push_back({"av1-web", "AV1 Web", "AV1 at sequence resolution, 8 Mbps", av1});
        ExportSettings custom = mk(enc::VideoCodec::H264, "mp4", 0, 0, 16000, 320);
        v.push_back({"custom", "Custom", "Sequence settings", custom});
        for (auto& p : v) p.settings.presetId = p.id;
        return v;
    }();
    return list;
}

const Preset* findPreset(const std::string& id) {
    for (auto& p : presets())
        if (p.id == id) return &p;
    return nullptr;
}

ExportSettings applyPreset(const std::string& id, const ExportSettings& base) {
    const Preset* p = findPreset(id);
    if (!p) return base;
    ExportSettings s = p->settings;
    s.outputPath = base.outputPath;
    s.range = base.range;
    s.verify = base.verify;
    s.preferHardware = base.preferHardware;
    return s;
}

std::string defaultExtension(const std::string& container) {
    if (container == "mov") return ".mov";
    if (container == "matroska" || container == "mkv") return ".mkv";
    if (container == "webm") return ".webm";
    return ".mp4";
}

uint64_t estimateOutputBytes(const ExportSettings& s, int w, int h, double fps, double seconds) {
    double videoKbps = s.bitrateKbps;
    if (s.rateControl == enc::RateControl::Quality || s.videoCodec == enc::VideoCodec::ProRes ||
        s.videoCodec == enc::VideoCodec::DNxHR || videoKbps <= 0) {
        double bpp = 0.1;
        switch (s.videoCodec) {
        case enc::VideoCodec::HEVC: bpp = 0.07; break;
        case enc::VideoCodec::AV1: bpp = 0.06; break;
        case enc::VideoCodec::ProRes: bpp = 3.5; break;
        case enc::VideoCodec::DNxHR: bpp = 3.0; break;
        case enc::VideoCodec::VP9: bpp = 0.07; break;
        case enc::VideoCodec::MPEG4: bpp = 0.15; break;
        default: break;
        }
        videoKbps = static_cast<double>(w) * h * fps * bpp / 1000.0;
    }
    double audioKbps = s.includeAudio ? s.audioBitrateKbps : 0;
    if (s.audioCodec == enc::AudioCodec::PCM24) audioKbps = s.sampleRate * 2 * 24 / 1000.0;
    if (s.audioCodec == enc::AudioCodec::PCM16) audioKbps = s.sampleRate * 2 * 16 / 1000.0;
    return static_cast<uint64_t>((videoKbps + audioKbps) * 1000.0 / 8.0 * seconds * 1.05) + (1u << 20);
}

}  // namespace avc::exp
