#pragma once
// .avicap project serialization (UTF-8 JSON).
//
// Top level:
//   { "format": "avicap-project", "schemaVersion": N, "appVersion": "...",
//     "savedUtc": "...", "project": { ... } }
//
// Times are integer ticks (see core/time.h), rationals are "num/den" strings,
// ids are 16-digit hex strings. Unknown fields are ignored so newer minor
// versions can be read; files with a newer schemaVersion are migrated forward
// by project/migrate.h or rejected with a clear message.

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/result.h"
#include "timeline/model.h"

namespace avc {

using Json = nlohmann::json;

// ---- element converters (also used by the journal and clipboard) ----------
Json paramSetToJson(const ParamSet& p);
ParamSet paramSetFromJson(const Json& j);
Json clipToJson(const Clip& c);
Clip clipFromJson(const Json& j);
Json trackPropsToJson(const Track& t);          // without clips
Track trackPropsFromJson(const Json& j);
Json trackToJson(const Track& t);               // with clips
Track trackFromJson(const Json& j);
Json sequencePropsToJson(const Sequence& s);    // without tracks
Sequence sequencePropsFromJson(const Json& j);
Json sequenceToJson(const Sequence& s);
Sequence sequenceFromJson(const Json& j);
Json mediaToJson(const MediaItem& m);
MediaItem mediaFromJson(const Json& j);
Json mediaInfoToJson(const MediaInfo& m);
MediaInfo mediaInfoFromJson(const Json& j);
Json projectPropsToJson(const Project& p);      // name, settings, bins, active sequence
void projectPropsFromJson(const Json& j, Project& p);

std::string timeToString(Time t);  // ticks as decimal string (for display/debug)

struct SaveOptions {
    std::filesystem::path projectDir;  // for relative media paths (empty: none)
    bool pretty = true;
};

Json projectToJson(const Project& p, const SaveOptions& opt = {});
std::string projectToString(const Project& p, const SaveOptions& opt = {});

struct LoadResult {
    ProjectPtr project;
    int loadedSchemaVersion = 0;
    bool migrated = false;
    std::vector<std::string> warnings;
};

Result<LoadResult> projectFromJson(Json j, const std::filesystem::path& projectDir = {});
Result<LoadResult> projectFromString(const std::string& text, const std::filesystem::path& projectDir = {});

// Saves atomically (temp file + flush + replace) and keeps "<file>.bak".
Status saveProjectFile(const Project& p, const std::filesystem::path& file);
Result<LoadResult> loadProjectFile(const std::filesystem::path& file);

}  // namespace avc
