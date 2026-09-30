#pragma once
// Incremental project journal (command journal) and crash recovery.
//
// Autosave never rewrites the whole project on every edit. Each published
// change is converted into a compact diff (only the clips/tracks/media that
// changed, found by pointer comparison of the persistent model) and appended
// to "<id>.journal" with a flush to disk. Periodically a full snapshot
// "<id>.base.avicap" is written atomically and the journal restarts at a new
// generation. After a crash: recovered = base + replay(journal).

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/result.h"
#include "timeline/document.h"

namespace avc {

// Diff ops between two snapshots (JSON array). Empty array when identical.
nlohmann::json diffProjects(const Project& before, const Project& after);
// Applies ops from diffProjects. The result is validated.
Result<ProjectPtr> applyProjectDiff(const ProjectPtr& base, const nlohmann::json& ops);

struct RecoveryCandidate {
    Id projectId = kInvalidId;
    std::string projectName;
    std::string projectPath;  // original .avicap location ("" if never saved)
    std::string lastUpdateUtc;
    std::filesystem::path metaFile;
    std::filesystem::path baseFile;
    std::filesystem::path journalFile;
    size_t journalEntries = 0;
};

struct RecoveredProject {
    ProjectPtr project;
    std::string projectPath;
    size_t replayedEdits = 0;
    std::vector<std::string> warnings;
};

class AutosaveManager {
public:
    struct Config {
        std::filesystem::path directory;
        int snapshotIntervalSec = 120;
        size_t maxJournalBytes = 8u << 20;
    };

    explicit AutosaveManager(Config cfg);
    ~AutosaveManager();
    AutosaveManager(const AutosaveManager&) = delete;
    AutosaveManager& operator=(const AutosaveManager&) = delete;

    // Starts journaling `doc`. Writes an initial base snapshot.
    void attach(Document& doc, const std::string& projectPath);
    // Stops journaling. `clean` (normal close/exit) deletes the autosave files.
    void detach(bool clean);
    void setProjectPath(const std::string& path);
    // Call after an explicit save: refreshes the base snapshot.
    void onSaved();
    // Blocks until all queued journal writes are on disk.
    void flush();
    [[nodiscard]] size_t journalEntries() const;
    [[nodiscard]] std::string lastError() const;

    // Leftover sessions from processes that are no longer running.
    // `includeCurrentProcess` is for tests that simulate a crash in-process.
    static std::vector<RecoveryCandidate> findRecoverable(const std::filesystem::path& directory,
                                                          bool includeCurrentProcess = false);
    static Result<RecoveredProject> recover(const RecoveryCandidate& c);
    static void discard(const RecoveryCandidate& c);

private:
    struct Task {
        enum class Kind { Journal, Snapshot, Meta } kind;
        std::string text;     // journal line
        ProjectPtr snapshot;  // for Snapshot
        std::string name;     // project name / path captured on the UI thread
        std::string path;
        Id projectId = kInvalidId;
    };
    void enqueue(Task t);
    void writerLoop();
    void writeMeta(const Task& t);
    void onChange(const ChangeEvent& ev);
    std::filesystem::path metaPath(Id id) const;
    std::filesystem::path basePath(Id id) const;
    std::filesystem::path journalPath(Id id) const;

    Config cfg_;
    Document* doc_ = nullptr;
    int listenerId_ = 0;
    Id projectId_ = kInvalidId;
    std::string projectName_;
    std::string projectPath_;
    std::atomic<uint64_t> generation_{0};
    size_t journalBytes_ = 0;
    size_t journalEntries_ = 0;
    std::chrono::steady_clock::time_point lastSnapshot_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::deque<Task> queue_;
    bool busy_ = false;
    bool stop_ = false;
    std::string lastError_;
    std::thread thread_;
};

}  // namespace avc
