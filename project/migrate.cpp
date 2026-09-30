#include "project/migrate.h"

#include "core/log.h"
#include "timeline/model.h"

namespace avc {

namespace {

// v0 -> v1: pre-release prototype documents stored times as floating seconds
// ("start": 1.5) and the frame rate as a number. Convert to integer ticks and
// rational strings.
Status migrate0to1(nlohmann::json& doc, std::vector<std::string>& warnings) {
    auto toTicks = [](nlohmann::json& obj, const char* key) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_number_float()) *it = Time::fromSeconds(it->get<double>()).ticks;
    };
    auto rate = [](nlohmann::json& obj, const char* key) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_number()) *it = Rational::fromFrameRate(it->get<double>()).toString();
    };
    auto& p = doc["project"];
    if (!p.is_object()) return Status::error("Project data missing");
    if (p.contains("settings")) rate(p["settings"], "frameRate");
    if (p.contains("sequences"))
        for (auto& s : p["sequences"]) {
            rate(s, "frameRate");
            if (s.contains("markers"))
                for (auto& m : s["markers"]) {
                    toTicks(m, "time");
                    toTicks(m, "duration");
                }
            if (s.contains("tracks"))
                for (auto& t : s["tracks"])
                    if (t.contains("clips"))
                        for (auto& c : t["clips"]) {
                            toTicks(c, "start");
                            toTicks(c, "duration");
                            toTicks(c, "sourceIn");
                        }
        }
    if (p.contains("media"))
        for (auto& m : p["media"])
            if (m.contains("info")) toTicks(m["info"], "duration");
    warnings.push_back("Project upgraded from prototype format (schema 0).");
    return Status::ok();
}

}  // namespace

const std::vector<MigrationStep>& migrationSteps() {
    static const std::vector<MigrationStep> steps = {migrate0to1};
    return steps;
}

Status migrateProjectJson(nlohmann::json& doc, int fromVersion, std::vector<std::string>& warnings) {
    const auto& steps = migrationSteps();
    if (fromVersion < 0) return Status::error("Invalid schema version");
    for (int v = fromVersion; v < kProjectSchemaVersion; ++v) {
        if (static_cast<size_t>(v) >= steps.size()) return Status::error("No migration from schema " + std::to_string(v));
        if (auto st = steps[static_cast<size_t>(v)](doc, warnings); !st) return st;
        doc["schemaVersion"] = v + 1;
        AVC_INFO("project", "Migrated project schema {} -> {}", v, v + 1);
    }
    return Status::ok();
}

}  // namespace avc
