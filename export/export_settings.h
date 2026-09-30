#pragma once
// Export settings, presets and size estimation.

#include <optional>
#include <string>
#include <vector>

#include "encode/encoder.h"
#include "timeline/model.h"

namespace avc::exp {

struct ExportSettings {
    std::string presetId = "custom";
    std::string outputPath;       // UTF-8, final file
    std::string container = "mp4";  // mp4 | mov | mkv | webm
    enc::VideoCodec videoCodec = enc::VideoCodec::H264;
    std::string encoder = "auto";   // "auto" or an FFmpeg encoder name
    bool preferHardware = true;
    int width = 0;   // 0 = sequence size
    int height = 0;
    Rational frameRate{0, 1};  // 0 = sequence rate
    enc::RateControl rateControl = enc::RateControl::VBR;
    int bitrateKbps = 16000;
    int maxBitrateKbps = 0;
    int quality = 20;
    int bitDepth = 8;
    std::string profile;
    int gopFrames = 0;
    bool includeAudio = true;
    enc::AudioCodec audioCodec = enc::AudioCodec::AAC;
    int audioBitrateKbps = 320;
    int sampleRate = 48000;
    std::optional<TimeRange> range;  // nullopt = work area or whole sequence
    bool verify = true;
    bool burnInSubtitles = true;     // subtitle tracks are part of the picture
};

struct Preset {
    std::string id;
    std::string name;
    std::string description;
    ExportSettings settings;
};

const std::vector<Preset>& presets();
const Preset* findPreset(const std::string& id);
// Applies a preset on top of `base` keeping the output path.
ExportSettings applyPreset(const std::string& id, const ExportSettings& base);
std::string defaultExtension(const std::string& container);
// Rough output size in bytes (used to check free disk space before starting).
uint64_t estimateOutputBytes(const ExportSettings& s, int width, int height, double fps, double seconds);

}  // namespace avc::exp
