#pragma once
// Document: owns the current project snapshot, applies edits transactionally
// and maintains the undo/redo history (Command pattern over snapshots).
//
// Every edit runs against a ProjectEditor; if the edit function fails or the
// result violates model invariants, nothing is published. Undo entries store
// the before/after roots — thanks to structural sharing each entry only costs
// the nodes that changed, so thousands of steps are cheap.

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "core/result.h"
#include "timeline/editor.h"

namespace avc {

struct ChangeEvent {
    ProjectPtr before;
    ProjectPtr after;
    std::string label;
    enum class Kind { Edit, Undo, Redo, Reset } kind = Kind::Edit;
    uint64_t revision = 0;
    bool merged = false;  // coalesced into the previous undo entry
};

struct EditOptions {
    // Consecutive edits sharing a non-empty key (e.g. dragging a slider) are
    // merged into one undo step.
    std::string mergeKey;
    bool undoable = true;
};

struct UndoEntry {
    std::string label;
    ProjectPtr before;
    ProjectPtr after;
    std::string mergeKey;
    uint64_t revision = 0;
};

class Document {
public:
    explicit Document(ProjectPtr project);

    // Thread-safe snapshot for render / audio / export / autosave threads.
    [[nodiscard]] ProjectPtr snapshot() const;
    // UI-thread access to the current state.
    [[nodiscard]] const Project& project() const noexcept { return *current_; }
    [[nodiscard]] const ProjectPtr& current() const noexcept { return current_; }

    Status edit(const std::string& label, const std::function<Status(ProjectEditor&)>& fn, const EditOptions& opt = {});
    Status editSequence(const std::string& label, SequenceId seq, const std::function<Status(SequenceEditor&)>& fn,
                        const EditOptions& opt = {});

    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty() && groupDepth_ == 0; }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty() && groupDepth_ == 0; }
    [[nodiscard]] std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    [[nodiscard]] std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    bool undo();
    bool redo();
    [[nodiscard]] const std::deque<UndoEntry>& undoHistory() const noexcept { return undo_; }
    [[nodiscard]] const std::deque<UndoEntry>& redoHistory() const noexcept { return redo_; }

    // Groups several edits into a single undo step (e.g. an AI plan).
    void beginGroup(const std::string& label);
    void endGroup();
    // Reverts every edit made since beginGroup and closes the group.
    void abortGroup();
    [[nodiscard]] bool inGroup() const noexcept { return groupDepth_ > 0; }

    void setUndoLimit(size_t entries) { undoLimit_ = entries == 0 ? 1 : entries; }
    [[nodiscard]] size_t undoLimit() const noexcept { return undoLimit_; }

    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool dirty() const noexcept { return current_ != savedRoot_; }
    void markSaved() { savedRoot_ = current_; }
    void markDirty() { savedRoot_ = nullptr; }

    // Replaces the project (open/new/recover); clears history.
    void reset(ProjectPtr project, bool markClean = true);

    int addListener(std::function<void(const ChangeEvent&)> fn);
    void removeListener(int id);

    // When enabled, every published snapshot is validated (always on in tests
    // and debug builds; cheap even for thousands of clips).
    void setValidation(bool on) { validate_ = on; }

private:
    void publish(ProjectPtr next, ChangeEvent ev);

    mutable std::mutex snapshotMutex_;
    ProjectPtr current_;
    ProjectPtr savedRoot_;
    std::deque<UndoEntry> undo_;
    std::deque<UndoEntry> redo_;
    size_t undoLimit_ = 1000;
    uint64_t revision_ = 0;
    int groupDepth_ = 0;
    std::string groupLabel_;
    ProjectPtr groupBefore_;
    std::map<int, std::function<void(const ChangeEvent&)>> listeners_;
    int nextListener_ = 1;
    bool validate_ = true;
    std::string lastMergeKey_;
    uint64_t lastEditRevision_ = 0;
};

}  // namespace avc
