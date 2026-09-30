#include "timeline/document.h"

#include <stdexcept>

#include "core/log.h"

namespace avc {

Document::Document(ProjectPtr project) : current_(std::move(project)) {
    if (!current_) current_ = std::make_shared<Project>(makeProject("Untitled"));
    savedRoot_ = current_;
}

ProjectPtr Document::snapshot() const {
    std::lock_guard lock(snapshotMutex_);
    return current_;
}

Status Document::edit(const std::string& label, const std::function<Status(ProjectEditor&)>& fn, const EditOptions& opt) {
    ProjectEditor editor(current_);
    Status st;
    try {
        st = fn(editor);
    } catch (const std::exception& e) {
        st = Status::error(std::string("Edit failed: ") + e.what());
    }
    if (!st) {
        AVC_DEBUG("document", "Edit '{}' rejected: {}", label, st.message());
        return st;
    }
    if (!editor.changed()) return Status::ok();
    ProjectPtr next = editor.finish();
    if (validate_) {
        const std::string err = validateProject(*next);
        if (!err.empty()) {
            AVC_ERROR("document", "Edit '{}' produced an invalid project and was discarded: {}", label, err);
            return Status::error("Internal consistency check failed: " + err);
        }
    }
    ChangeEvent ev;
    ev.before = current_;
    ev.after = next;
    ev.label = label;
    ev.kind = ChangeEvent::Kind::Edit;

    if (opt.undoable && groupDepth_ == 0) {
        const bool canMerge = !opt.mergeKey.empty() && opt.mergeKey == lastMergeKey_ && !undo_.empty() &&
                              undo_.back().after == current_ && lastEditRevision_ == revision_;
        if (canMerge) {
            undo_.back().after = next;
            ev.merged = true;
        } else {
            undo_.push_back(UndoEntry{label, current_, next, opt.mergeKey, revision_ + 1});
            while (undo_.size() > undoLimit_) undo_.pop_front();
        }
        redo_.clear();
        lastMergeKey_ = opt.mergeKey;
    } else {
        // Inside a group (recorded at endGroup) or explicitly non-undoable.
        lastMergeKey_.clear();
    }
    publish(std::move(next), std::move(ev));
    lastEditRevision_ = revision_;
    return Status::ok();
}

Status Document::editSequence(const std::string& label, SequenceId seq, const std::function<Status(SequenceEditor&)>& fn,
                              const EditOptions& opt) {
    return edit(label, [&](ProjectEditor& pe) -> Status {
        if (!pe.current().findSequence(seq)) return Status::error("Sequence not found");
        SequenceEditor se = pe.sequence(seq);
        return fn(se);
    }, opt);
}

bool Document::undo() {
    if (!canUndo()) return false;
    UndoEntry e = std::move(undo_.back());
    undo_.pop_back();
    ChangeEvent ev;
    ev.before = current_;
    ev.after = e.before;
    ev.label = e.label;
    ev.kind = ChangeEvent::Kind::Undo;
    ProjectPtr target = e.before;
    redo_.push_back(std::move(e));
    lastMergeKey_.clear();
    publish(std::move(target), std::move(ev));
    return true;
}

bool Document::redo() {
    if (!canRedo()) return false;
    UndoEntry e = std::move(redo_.back());
    redo_.pop_back();
    ChangeEvent ev;
    ev.before = current_;
    ev.after = e.after;
    ev.label = e.label;
    ev.kind = ChangeEvent::Kind::Redo;
    ProjectPtr target = e.after;
    undo_.push_back(std::move(e));
    lastMergeKey_.clear();
    publish(std::move(target), std::move(ev));
    return true;
}

void Document::beginGroup(const std::string& label) {
    if (groupDepth_++ == 0) {
        groupLabel_ = label;
        groupBefore_ = current_;
    }
}

void Document::endGroup() {
    if (groupDepth_ == 0) return;
    if (--groupDepth_ > 0) return;
    if (groupBefore_ && groupBefore_ != current_) {
        undo_.push_back(UndoEntry{groupLabel_, groupBefore_, current_, {}, revision_});
        while (undo_.size() > undoLimit_) undo_.pop_front();
        redo_.clear();
    }
    groupBefore_.reset();
    lastMergeKey_.clear();
}

void Document::abortGroup() {
    if (groupDepth_ == 0) return;
    groupDepth_ = 0;
    if (groupBefore_ && groupBefore_ != current_) {
        ChangeEvent ev;
        ev.before = current_;
        ev.after = groupBefore_;
        ev.label = groupLabel_;
        ev.kind = ChangeEvent::Kind::Undo;
        ProjectPtr target = groupBefore_;
        publish(std::move(target), std::move(ev));
    }
    groupBefore_.reset();
    lastMergeKey_.clear();
}

void Document::reset(ProjectPtr project, bool markClean) {
    undo_.clear();
    redo_.clear();
    groupDepth_ = 0;
    groupBefore_.reset();
    lastMergeKey_.clear();
    ChangeEvent ev;
    ev.before = current_;
    ev.after = project;
    ev.kind = ChangeEvent::Kind::Reset;
    publish(std::move(project), std::move(ev));
    if (markClean) savedRoot_ = current_;
    else savedRoot_ = nullptr;
}

void Document::publish(ProjectPtr next, ChangeEvent ev) {
    {
        std::lock_guard lock(snapshotMutex_);
        current_ = std::move(next);
    }
    ev.revision = ++revision_;
    ev.after = current_;
    // Copy listeners: a listener may add/remove listeners.
    auto listeners = listeners_;
    for (auto& [id, fn] : listeners) {
        try {
            fn(ev);
        } catch (const std::exception& e) {
            AVC_ERROR("document", "Change listener threw: {}", e.what());
        }
    }
}

int Document::addListener(std::function<void(const ChangeEvent&)> fn) {
    const int id = nextListener_++;
    listeners_[id] = std::move(fn);
    return id;
}

void Document::removeListener(int id) { listeners_.erase(id); }

}  // namespace avc
