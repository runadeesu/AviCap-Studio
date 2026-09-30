#pragma once
// Forward migration of .avicap documents between schema versions.
//
// Each step upgrades the JSON document from version N to N+1 in place. Steps
// are pure JSON transforms so old files never need old code paths.

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/result.h"

namespace avc {

using MigrationStep = std::function<Status(nlohmann::json& doc, std::vector<std::string>& warnings)>;

// Upgrades `doc` from `fromVersion` to kProjectSchemaVersion.
Status migrateProjectJson(nlohmann::json& doc, int fromVersion, std::vector<std::string>& warnings);

// Registered steps; index i upgrades version i -> i+1.
const std::vector<MigrationStep>& migrationSteps();

}  // namespace avc
