#include "project/journal.h"

#include <algorithm>
#include <map>
#include <unordered_map>

#include "core/file_io.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "project/serialize.h"

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace avc {

using nlohmann::json;

// ============================================================ diff

namespace {

std::vector<Id> ids(const std::vector<MediaItemPtr>& v) {
    std::vector<Id> r;
    for (auto& m : v) r.push_back(m->id);
    return r;
}
std::vector<Id> ids(const std::vector<SequencePtr>& v) {
    std::vector<Id> r;
    for (auto& s : v) r.push_back(s->id);
    return r;
}
std::vector<Id> ids(const std::vector<TrackPtr>& v) {
    std::vector<Id> r;
    for (auto& t : v) r.push_back(t->id);
    return r;
}
json idList(const std::vector<Id>& v) {
    json a = json::array();
    for (Id i : v) a.push_back(idToString(i));
    return a;
}

void diffSequence(const Sequence& a, const Sequence& b, json& ops) {
    const std::string sid = idToString(b.id);
    json pa = sequencePropsToJson(a), pb = sequencePropsToJson(b);
    if (pa != pb) ops.push_back({{"op", "seqProps"}, {"seq", sid}, {"v", std::move(pb)}});
    std::unordered_map<Id, const TrackPtr*> at;
    for (auto& t : a.tracks) at[t->id] = &t;
    for (auto& t : b.tracks) {
        auto it = at.find(t->id);
        if (it == at.end()) {
            ops.push_back({{"op", "track"}, {"seq", sid}, {"v", trackToJson(*t)}});
            continue;
        }
        const Track& ta = **it->second;
        if (it->second->get() == t.get()) continue;
        const std::string tid = idToString(t->id);
        json tpa = trackPropsToJson(ta), tpb = trackPropsToJson(*t);
        if (tpa != tpb) ops.push_back({{"op", "trackProps"}, {"seq", sid}, {"v", std::move(tpb)}});
        std::unordered_map<Id, const Clip*> ac;
        for (auto& c : ta.clips) ac[c->id] = c.get();
        std::unordered_map<Id, bool> seen;
        for (auto& c : t->clips) {
            seen[c->id] = true;
            auto ci = ac.find(c->id);
            if (ci == ac.end() || (ci->second != c.get() && !(*ci->second == *c)))
                ops.push_back({{"op", "clip"}, {"seq", sid}, {"track", tid}, {"v", clipToJson(*c)}});
        }
        for (auto& c : ta.clips)
            if (!seen.count(c->id)) ops.push_back({{"op", "clipDel"}, {"seq", sid}, {"track", tid}, {"id", idToString(c->id)}});
    }
    std::unordered_map<Id, bool> bt;
    for (auto& t : b.tracks) bt[t->id] = true;
    for (auto& t : a.tracks)
        if (!bt.count(t->id)) ops.push_back({{"op", "trackDel"}, {"seq", sid}, {"id", idToString(t->id)}});
    if (ids(a.tracks) != ids(b.tracks)) ops.push_back({{"op", "trackOrder"}, {"seq", sid}, {"ids", idList(ids(b.tracks))}});
}

}  // namespace

json diffProjects(const Project& a, const Project& b) {
    json ops = json::array();
    if (&a == &b) return ops;
    json pa = projectPropsToJson(a), pb = projectPropsToJson(b);
    if (pa != pb) ops.push_back({{"op", "props"}, {"v", std::move(pb)}});

    std::unordered_map<Id, const MediaItem*> am;
    for (auto& m : a.media) am[m->id] = m.get();
    std::unordered_map<Id, bool> bm;
    for (auto& m : b.media) {
        bm[m->id] = true;
        auto it = am.find(m->id);
        if (it == am.end() || (it->second != m.get() && !(*it->second == *m)))
            ops.push_back({{"op", "media"}, {"v", mediaToJson(*m)}});
    }
    for (auto& m : a.media)
        if (!bm.count(m->id)) ops.push_back({{"op", "mediaDel"}, {"id", idToString(m->id)}});
    if (ids(a.media) != ids(b.media)) ops.push_back({{"op", "mediaOrder"}, {"ids", idList(ids(b.media))}});

    std::unordered_map<Id, const Sequence*> as;
    for (auto& s : a.sequences) as[s->id] = s.get();
    std::unordered_map<Id, bool> bs;
    for (auto& s : b.sequences) {
        bs[s->id] = true;
        auto it = as.find(s->id);
        if (it == as.end()) ops.push_back({{"op", "seq"}, {"v", sequenceToJson(*s)}});
        else if (it->second != s.get()) diffSequence(*it->second, *s, ops);
    }
    for (auto& s : a.sequences)
        if (!bs.count(s->id)) ops.push_back({{"op", "seqDel"}, {"id", idToString(s->id)}});
    if (ids(a.sequences) != ids(b.sequences)) ops.push_back({{"op", "seqOrder"}, {"ids", idList(ids(b.sequences))}});
    return ops;
}

namespace {

template <typename P>
void reorder(std::vector<P>& v, const json& list) {
    std::vector<P> out;
    for (auto& jid : list) {
        const Id id = idFromString(jid.get<std::string>());
        auto it = std::find_if(v.begin(), v.end(), [id](const P& p) { return p->id == id; });
        if (it != v.end()) out.push_back(*it);
    }
    for (auto& p : v)
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    v = std::move(out);
}

}  // namespace

Result<ProjectPtr> applyProjectDiff(const ProjectPtr& base, const json& ops) {
    if (!ops.is_array()) return Result<ProjectPtr>::error("Invalid journal entry");
    auto p = std::make_shared<Project>(*base);
    auto seqMut = [&](Id sid) -> Sequence* {
        const int i = p->sequenceIndex(sid);
        if (i < 0) return nullptr;
        auto s = std::make_shared<Sequence>(*p->sequences[static_cast<size_t>(i)]);
        p->sequences[static_cast<size_t>(i)] = s;
        return s.get();
    };
    auto trackMut = [&](Sequence* s, Id tid) -> Track* {
        const int i = s->trackIndex(tid);
        if (i < 0) return nullptr;
        auto t = std::make_shared<Track>(*s->tracks[static_cast<size_t>(i)]);
        s->tracks[static_cast<size_t>(i)] = t;
        return t.get();
    };
    try {
        for (auto& op : ops) {
            const std::string kind = op.at("op").get<std::string>();
            if (kind == "props") {
                projectPropsFromJson(op.at("v"), *p);
            } else if (kind == "media") {
                auto m = std::make_shared<MediaItem>(mediaFromJson(op.at("v")));
                const int i = p->mediaIndex(m->id);
                if (i >= 0) p->media[static_cast<size_t>(i)] = m;
                else p->media.push_back(m);
            } else if (kind == "mediaDel") {
                const int i = p->mediaIndex(idFromString(op.at("id").get<std::string>()));
                if (i >= 0) p->media.erase(p->media.begin() + i);
            } else if (kind == "mediaOrder") {
                reorder(p->media, op.at("ids"));
            } else if (kind == "seq") {
                auto s = std::make_shared<Sequence>(sequenceFromJson(op.at("v")));
                const int i = p->sequenceIndex(s->id);
                if (i >= 0) p->sequences[static_cast<size_t>(i)] = s;
                else p->sequences.push_back(s);
            } else if (kind == "seqDel") {
                const int i = p->sequenceIndex(idFromString(op.at("id").get<std::string>()));
                if (i >= 0) p->sequences.erase(p->sequences.begin() + i);
            } else if (kind == "seqOrder") {
                reorder(p->sequences, op.at("ids"));
            } else if (kind == "seqProps") {
                Sequence* s = seqMut(idFromString(op.at("seq").get<std::string>()));
                if (!s) return Result<ProjectPtr>::error("Journal references unknown sequence");
                Sequence props = sequencePropsFromJson(op.at("v"));
                props.tracks = std::move(s->tracks);
                *s = std::move(props);
            } else if (kind == "track" || kind == "trackProps" || kind == "trackDel" || kind == "trackOrder" ||
                       kind == "clip" || kind == "clipDel") {
                Sequence* s = seqMut(idFromString(op.at("seq").get<std::string>()));
                if (!s) return Result<ProjectPtr>::error("Journal references unknown sequence");
                if (kind == "track") {
                    auto t = std::make_shared<Track>(trackFromJson(op.at("v")));
                    const int i = s->trackIndex(t->id);
                    if (i >= 0) s->tracks[static_cast<size_t>(i)] = t;
                    else s->tracks.push_back(t);
                } else if (kind == "trackProps") {
                    Track props = trackPropsFromJson(op.at("v"));
                    Track* t = trackMut(s, props.id);
                    if (!t) return Result<ProjectPtr>::error("Journal references unknown track");
                    props.clips = std::move(t->clips);
                    *t = std::move(props);
                } else if (kind == "trackDel") {
                    const int i = s->trackIndex(idFromString(op.at("id").get<std::string>()));
                    if (i >= 0) s->tracks.erase(s->tracks.begin() + i);
                } else if (kind == "trackOrder") {
                    reorder(s->tracks, op.at("ids"));
                } else {
                    Track* t = trackMut(s, idFromString(op.at("track").get<std::string>()));
                    if (!t) return Result<ProjectPtr>::error("Journal references unknown track");
                    const Id cid = kind == "clip" ? idFromString(op.at("v").at("id").get<std::string>())
                                                  : idFromString(op.at("id").get<std::string>());
                    t->clips.erase(std::remove_if(t->clips.begin(), t->clips.end(),
                                                  [cid](const ClipPtr& c) { return c->id == cid; }),
                                   t->clips.end());
                    if (kind == "clip") t->clips.push_back(std::make_shared<Clip>(clipFromJson(op.at("v"))));
                }
            }
        }
    } catch (const std::exception& e) {
        return Result<ProjectPtr>::error(std::string("Journal entry is damaged: ") + e.what());
    }
    // Upserted clips were appended; restore start order where needed.
    for (auto& sp : p->sequences) {
        bool needs = false;
        for (auto& t : sp->tracks)
            for (size_t i = 1; i < t->clips.size() && !needs; ++i)
                if (t->clips[i]->start < t->clips[i - 1]->start) needs = true;
        if (!needs) continue;
        auto s = std::make_shared<Sequence>(*sp);
        for (auto& tp : s->tracks) {
            auto t = std::make_shared<Track>(*tp);
            std::stable_sort(t->clips.begin(), t->clips.end(), [](const ClipPtr& a, const ClipPtr& b) { return a->start < b->start; });
            tp = t;
        }
        sp = s;
    }
    const std::string err = validateProject(*p);
    if (!err.empty()) return Result<ProjectPtr>::error("Replayed journal is inconsistent: " + err);
    return ProjectPtr(p);
}

// ============================================================ AutosaveManager

namespace {

bool processAlive(uint32_t pid) {
    if (pid == 0) return false;
    if (pid == currentProcessId()) return true;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

bool appendDurable(const std::filesystem::path& file, const std::string& text) {
    std::FILE* f = openFileUtf8(file, "ab");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = ok && std::fflush(f) == 0;
#if defined(_WIN32)
    if (ok) {
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
        ok = h != INVALID_HANDLE_VALUE && FlushFileBuffers(h) != 0;
    }
#else
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    std::fclose(f);
    return ok;
}


}  // namespace

AutosaveManager::AutosaveManager(Config cfg) : cfg_(std::move(cfg)) {
    std::error_code ec;
    std::filesystem::create_directories(cfg_.directory, ec);
    thread_ = std::thread([this] { writerLoop(); });
}

AutosaveManager::~AutosaveManager() {
    if (doc_) detach(false);
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::filesystem::path AutosaveManager::metaPath(Id id) const { return cfg_.directory / (idToString(id) + ".meta.json"); }
std::filesystem::path AutosaveManager::basePath(Id id) const { return cfg_.directory / (idToString(id) + ".base.avicap"); }
std::filesystem::path AutosaveManager::journalPath(Id id) const { return cfg_.directory / (idToString(id) + ".journal"); }

void AutosaveManager::attach(Document& doc, const std::string& projectPath) {
    if (doc_) detach(false);
    doc_ = &doc;
    projectId_ = doc.project().id;
    projectName_ = doc.project().name;
    projectPath_ = projectPath;
    generation_ = 0;
    journalBytes_ = 0;
    journalEntries_ = 0;
    listenerId_ = doc.addListener([this](const ChangeEvent& ev) { onChange(ev); });
    enqueue({Task::Kind::Snapshot, {}, doc.current()});
    lastSnapshot_ = std::chrono::steady_clock::now();
}

void AutosaveManager::detach(bool clean) {
    if (!doc_) return;
    doc_->removeListener(listenerId_);
    doc_ = nullptr;
    flush();
    if (clean) {
        std::error_code ec;
        std::filesystem::remove(journalPath(projectId_), ec);
        std::filesystem::remove(basePath(projectId_), ec);
        std::filesystem::remove(metaPath(projectId_), ec);
    }
}

void AutosaveManager::setProjectPath(const std::string& path) {
    projectPath_ = path;
    if (doc_) projectName_ = doc_->project().name;
    enqueue({Task::Kind::Meta, {}, nullptr});
}

void AutosaveManager::onSaved() {
    if (!doc_) return;
    enqueue({Task::Kind::Snapshot, {}, doc_->current()});
    lastSnapshot_ = std::chrono::steady_clock::now();
}

void AutosaveManager::onChange(const ChangeEvent& ev) {
    if (!ev.after) return;
    if (ev.after->id != projectId_ || ev.kind == ChangeEvent::Kind::Reset) {
        projectId_ = ev.after->id;
        projectName_ = ev.after->name;
        enqueue({Task::Kind::Snapshot, {}, ev.after});
        lastSnapshot_ = std::chrono::steady_clock::now();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSnapshot_ > std::chrono::seconds(cfg_.snapshotIntervalSec) || journalBytes_ > cfg_.maxJournalBytes) {
        enqueue({Task::Kind::Snapshot, {}, ev.after});
        lastSnapshot_ = now;
        return;
    }
    if (!ev.before) return;
    json ops = diffProjects(*ev.before, *ev.after);
    if (ops.empty()) return;
    const char* kind = ev.kind == ChangeEvent::Kind::Undo ? "undo" : ev.kind == ChangeEvent::Kind::Redo ? "redo" : "edit";
    json rec{{"rev", ev.revision}, {"label", ev.label}, {"kind", kind}, {"ops", std::move(ops)}};
    std::string line = rec.dump(-1, ' ', false, json::error_handler_t::replace);
    line.push_back('\n');
    journalBytes_ += line.size();
    ++journalEntries_;
    enqueue({Task::Kind::Journal, std::move(line), nullptr});
}

size_t AutosaveManager::journalEntries() const { return journalEntries_; }

std::string AutosaveManager::lastError() const {
    std::lock_guard lock(mutex_);
    return lastError_;
}

void AutosaveManager::enqueue(Task t) {
    t.name = projectName_;
    t.path = projectPath_;
    t.projectId = projectId_;
    if (t.kind == Task::Kind::Snapshot) journalBytes_ = 0;
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(t));
    }
    cv_.notify_one();
}

void AutosaveManager::flush() {
    std::unique_lock lock(mutex_);
    idleCv_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void AutosaveManager::writeMeta(const Task& t) {
    json meta{{"projectId", idToString(t.projectId)},
              {"projectName", t.name},
              {"projectPath", t.path},
              {"pid", currentProcessId()},
              {"generation", generation_.load()},
              {"updatedUtc", utcNowIso8601()}};
    writeFileAtomic(metaPath(t.projectId), meta.dump(2));
}

void AutosaveManager::writerLoop() {
    setCurrentThreadName("autosave");
    for (;;) {
        std::deque<Task> batch;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty() && stop_) return;
            batch.swap(queue_);
            busy_ = true;
        }
        std::string pendingJournal;
        Id pendingId = kInvalidId;
        auto flushJournal = [&]() {
            if (pendingJournal.empty()) return;
            if (!appendDurable(journalPath(pendingId), pendingJournal)) {
                std::lock_guard lock(mutex_);
                lastError_ = "Autosave journal write failed (disk full?)";
                AVC_ERROR("autosave", "{}", lastError_);
            }
            pendingJournal.clear();
        };
        for (auto& task : batch) {
            switch (task.kind) {
            case Task::Kind::Journal:
                if (pendingId != task.projectId) flushJournal();
                pendingId = task.projectId;
                pendingJournal += task.text;
                break;
            case Task::Kind::Meta:
                flushJournal();
                writeMeta(task);
                break;
            case Task::Kind::Snapshot: {
                flushJournal();
                ++generation_;
                SaveOptions so;
                if (!task.path.empty()) so.projectDir = pathFromUtf8(task.path).parent_path();
                so.pretty = false;
                json doc = projectToJson(*task.snapshot, so);
                doc["autosaveGeneration"] = generation_.load();
                Status st = writeFileAtomic(basePath(task.projectId), doc.dump(-1, ' ', false, json::error_handler_t::replace));
                if (st) {
                    json header{{"generation", generation_.load()}, {"projectId", idToString(task.projectId)}};
                    st = writeFileAtomic(journalPath(task.projectId), header.dump() + "\n");
                }
                if (!st) {
                    std::lock_guard lock(mutex_);
                    lastError_ = "Autosave snapshot failed: " + st.message();
                    AVC_ERROR("autosave", "{}", lastError_);
                }
                writeMeta(task);
                break;
            }
            }
        }
        flushJournal();
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
        }
        idleCv_.notify_all();
    }
}

std::vector<RecoveryCandidate> AutosaveManager::findRecoverable(const std::filesystem::path& directory,
                                                               bool includeCurrentProcess) {
    std::vector<RecoveryCandidate> out;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = pathToUtf8(e.path().filename());
        if (!endsWith(name, ".meta.json")) continue;
        auto data = readFileBytes(e.path());
        if (!data) continue;
        json meta = json::parse(*data, nullptr, false);
        if (meta.is_discarded()) continue;
        const uint32_t pid = meta.value("pid", 0u);
        if (!(includeCurrentProcess && pid == currentProcessId()) && processAlive(pid)) continue;  // running instance
        RecoveryCandidate c;
        c.projectId = idFromString(meta.value("projectId", std::string()));
        c.projectName = meta.value("projectName", std::string());
        c.projectPath = meta.value("projectPath", std::string());
        c.lastUpdateUtc = meta.value("updatedUtc", std::string());
        c.metaFile = e.path();
        c.baseFile = directory / (idToString(c.projectId) + ".base.avicap");
        c.journalFile = directory / (idToString(c.projectId) + ".journal");
        if (!std::filesystem::exists(c.baseFile, ec)) continue;
        if (auto j = readFileBytes(c.journalFile)) c.journalEntries = static_cast<size_t>(std::count(j->begin(), j->end(), '\n'));
        if (c.journalEntries > 0) --c.journalEntries;  // header line
        out.push_back(std::move(c));
    }
    return out;
}

Result<RecoveredProject> AutosaveManager::recover(const RecoveryCandidate& c) {
    auto baseText = readFileBytes(c.baseFile);
    if (!baseText) return Result<RecoveredProject>::error("Autosave snapshot is missing");
    json baseJson = json::parse(*baseText, nullptr, false);
    if (baseJson.is_discarded()) return Result<RecoveredProject>::error("Autosave snapshot is damaged");
    const uint64_t generation = baseJson.value("autosaveGeneration", uint64_t{0});
    std::filesystem::path projectDir;
    if (!c.projectPath.empty()) projectDir = pathFromUtf8(c.projectPath).parent_path();
    auto loaded = projectFromJson(std::move(baseJson), projectDir);
    if (!loaded) return loaded.status();
    RecoveredProject r;
    r.project = loaded->project;
    r.projectPath = c.projectPath;
    r.warnings = loaded->warnings;
    if (auto jtext = readFileBytes(c.journalFile)) {
        auto lines = split(*jtext, '\n');
        bool first = true;
        for (auto& line : lines) {
            json rec = json::parse(line, nullptr, false);
            if (first) {
                first = false;
                if (rec.is_discarded() || rec.value("generation", uint64_t{0}) != generation) {
                    r.warnings.push_back("Autosave journal belongs to an older snapshot; using snapshot only.");
                    break;
                }
                continue;
            }
            if (rec.is_discarded()) {
                r.warnings.push_back("The last autosave entry was incomplete and was skipped.");
                break;
            }
            auto next = applyProjectDiff(r.project, rec.value("ops", json::array()));
            if (!next) {
                r.warnings.push_back("Stopped replaying autosave journal: " + next.errorMessage());
                break;
            }
            r.project = *next;
            ++r.replayedEdits;
        }
    }
    AVC_INFO("autosave", "Recovered project '{}' ({} journaled edits)", c.projectName, r.replayedEdits);
    return r;
}

void AutosaveManager::discard(const RecoveryCandidate& c) {
    std::error_code ec;
    std::filesystem::remove(c.journalFile, ec);
    std::filesystem::remove(c.baseFile, ec);
    std::filesystem::remove(c.metaFile, ec);
}

}  // namespace avc
