#pragma once
// Media probing and import.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "timeline/model.h"

namespace avc {

enum class ImportKind { Video, Audio, Image, Subtitle, Project, Unsupported };

ImportKind classifyExtension(std::string_view extensionLower);  // ".mp4"
const std::vector<std::string>& supportedVideoExtensions();
const std::vector<std::string>& supportedAudioExtensions();
const std::vector<std::string>& supportedImageExtensions();
bool isImportableFile(const std::filesystem::path& p);

// Probes a file with libavformat (no external processes).
Result<MediaInfo> probeMedia(const std::string& utf8Path);

// Probe + file identity -> a MediaItem ready to add to a project.
Result<MediaItem> createMediaItem(const std::string& utf8Path);

}  // namespace avc
