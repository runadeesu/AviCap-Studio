#pragma once
// Offline media detection and relinking.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/result.h"
#include "timeline/editor.h"

namespace avc {

struct MissingMedia {
    MediaId id = kInvalidId;
    std::string name;
    std::string path;
    uint64_t fileSize = 0;
};

std::vector<MissingMedia> findMissingMedia(const Project& project);
bool mediaOnline(const MediaItem& m);

// Recursively searches `folder` for files whose name matches a missing item
// (and whose size matches, when the original size is known).
std::map<MediaId, std::string> searchFolderForMedia(const std::vector<MissingMedia>& missing,
                                                    const std::filesystem::path& folder, int maxDepth = 8,
                                                    const CancelToken& cancel = {});

// Given that `located` was found at `newPath`, infers new locations for other
// missing items that shared its old directory structure.
std::map<MediaId, std::string> inferRelinks(const Project& project, MediaId located, const std::string& newPath);

// Points a media item at a new file (identity refreshed from disk).
Status relinkMedia(ProjectEditor& pe, MediaId id, const std::string& newPath);

}  // namespace avc
