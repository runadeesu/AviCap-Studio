#pragma once
// Child process execution with captured stdout (used to run crash-isolated
// worker jobs such as proxy generation through avicap-cli).

#include <functional>
#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/result.h"

namespace avc {

struct ProcessResult {
    int exitCode = -1;
    bool cancelled = false;
};

// Runs `exe args...`; `onLine` receives each stdout line. The process is killed
// when `cancel` fires.
Result<ProcessResult> runProcess(const std::string& exeUtf8, const std::vector<std::string>& args,
                                 const std::function<void(const std::string&)>& onLine, const CancelToken& cancel = {});

}  // namespace avc
