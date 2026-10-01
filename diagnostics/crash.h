#pragma once
// Crash handling: on an unhandled exception (or std::terminate / abort) a
// minidump is written to the dump folder and the log is flushed. Dumps stay
// on this PC; nothing is uploaded. A session marker file detects unclean
// shutdowns so the next start can tell the user (autosave recovery is
// handled separately by the project journal).

#include <filesystem>
#include <string>
#include <vector>

namespace avc::diag {

// Installs the process-wide handlers. `writeDumps` false keeps logging only.
void installCrashHandler(const std::filesystem::path& dumpDir, bool writeDumps = true);
// Writes a dump of the current process now (diagnostics / tests). Returns the file or "".
std::filesystem::path writeDumpNow(const std::string& reason);

struct SessionState {
    bool previousSessionCrashed = false;
    std::string previousStartUtc;
};
// Creates the "session running" marker; reports whether the previous one was
// left behind (= the app did not exit cleanly).
SessionState beginSession(const std::filesystem::path& dataDir);
void endSession(const std::filesystem::path& dataDir);

std::vector<std::filesystem::path> listCrashDumps(const std::filesystem::path& dumpDir);

}  // namespace avc::diag
