#pragma once
// Post-export verification: probes the written file and decodes samples of it
// to make sure the export is complete and playable.

#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/time.h"

namespace avc::exp {

struct VerifyExpectations {
    double durationSec = 0;
    int width = 0;
    int height = 0;
    Rational frameRate{0, 1};
    bool audio = false;
};

struct VerifyReport {
    bool ok = false;
    std::vector<std::string> problems;
    double durationSec = 0;
    int width = 0;
    int height = 0;
    Rational frameRate{0, 1};
    std::string videoCodec;
    std::string audioCodec;
    int audioChannels = 0;
    int sampleRate = 0;
    uint64_t fileSize = 0;
    int64_t packets = 0;
    int decodedFrames = 0;
    [[nodiscard]] std::string summary() const;
};

VerifyReport verifyExport(const std::string& utf8Path, const VerifyExpectations& expect, const CancelToken& cancel = {});

}  // namespace avc::exp
