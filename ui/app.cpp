#include "ui/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "core/file_io.h"
#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "effects/effects.h"
#include "media/probe.h"
#include "project/relink.h"
#include "project/serialize.h"
#include "text/text_raster.h"

namespace avc::ui {

namespace fs = std::filesystem;

const char* toolLabel(Tool t) {
    switch (t) {
    case Tool::Select: return "Selection Tool";
    case Tool::Razor: return "Razor Tool";
    case Tool::Ripple: return "Ripple Edit Tool";
    case Tool::Roll: return "Rolling Edit Tool";
    case Tool::Slip: return "Slip Tool";
    case Tool::Slide: return "Slide Tool";
    }
    return "";
}

namespace {

class NullDialogs final : public IDialogs {
public:
    std::vector<std::string> openFiles(const std::string&, const std::vector<FileFilter>&, bool) override { return {}; }
    std::optional<std::string> saveFile(const std::string&, const std::vector<FileFilter>&, const std::string&,
                                        const std::string&) override {
        return std::nullopt;
    }
    std::optional<std::string> pickFolder(const std::string&) override { return std::nullopt; }
};

double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr uint64_t kMB = 1024ull * 1024ull;

int64_t absTicks(Time t) { return t.ticks < 0 ? -t.ticks : t.ticks; }

}  // namespace

std::string fpsText(Rational r) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", r.toDouble());
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

namespace {

}  // namespace

// Posted work outlives the App safely: jobs hold the queue, not the App.
struct PostQueue {
    std::mutex mutex;
    std::vector<std::function<void()>> items;
    std::function<void()> wake;
    bool closed = false;
    void post(std::function<void()> fn) {
        std::function<void()> w;
        {
            std::lock_guard lk(mutex);
            if (closed) return;
            items.push_back(std::move(fn));
            w = wake;
        }
        if (w) w();
    }
    void close() {
        std::lock_guard lk(mutex);
        closed = true;
        wake = nullptr;
        items.clear();
    }
};

// ------------------------------------------------------------------ lifecycle

App::App(AppOptions opt) : posts_(std::make_shared<PostQueue>()) {
    headless_ = opt.headless;
    startSteady_ = steadySeconds();
    dataDir_ = opt.dataDir.empty() ? appDataDir() : opt.dataDir;
    std::error_code ec;
    fs::create_directories(dataDir_, ec);
    if (opt.loadSettings) settings_ = AppSettings::load(dataDir_ / "settings.json");
    setLanguage(languageFromSetting(settings_.general.language));
    snapping = settings_.ui.snapping;
    loopPlayback = settings_.playback.loop;
    {
        const std::string& ws = settings_.ui.workspace;
        ui_.workspace = (ws == "color" || ws == "audio" || ws == "export") ? ws : "edit";
    }

    dialogs_ = opt.dialogs ? std::move(opt.dialogs) : std::make_unique<NullDialogs>();
    device_ = opt.device ? std::move(opt.device) : gpu::createCpuDevice();
    AVC_INFO("app", "render device: {} ({})", device_->info().name, device_->info().adapter);

    const fs::path cacheRoot = settings_.cache.location.empty() ? dataDir_ / "Cache" : pathFromUtf8(settings_.cache.location);
    cacheStore_ = std::make_unique<cache::CacheStore>(cacheRoot);
    assets_ = std::make_unique<AssetService>(*cacheStore_, *device_, [this] { wake(); });
    ai_ = std::make_unique<AiService>(*this, cacheStore_.get());
    sounds_ = std::make_unique<SoundLibrary>(*this);
    const fs::path proxyDir = settings_.proxy.location.empty() ? cacheRoot / "proxies" : pathFromUtf8(settings_.proxy.location);
    proxies_ = std::make_unique<proxy::ProxyManager>(proxyDir);
    std::string worker = opt.workerExecutable;
    if (worker.empty()) {
#if defined(_WIN32)
        const fs::path cand = executableDir() / "avicap-cli.exe";
#else
        const fs::path cand = executableDir() / "avicap-cli";
#endif
        if (fs::exists(cand, ec)) worker = pathToUtf8(cand);
    }
    proxies_->setWorkerExecutable(worker);

    FrameProviderOptions fp;
    fp.cacheBytes = static_cast<size_t>(std::max(128, settings_.performance.frameCacheMB)) * kMB;
    fp.maxOpenDecoders = std::max(4, settings_.performance.maxOpenDecoders);
    if (device_->info().kind == gpu::BackendKind::D3D11 && settings_.performance.hardwareDecode && !device_->info().software) {
        fp.hardware = HwDecodeMode::Keep;
        fp.d3d11Device = device_->nativeDevice();
        fp.d3d11Lock = &gpu::deviceLockThunk;
        fp.d3d11Unlock = &gpu::deviceUnlockThunk;
        fp.d3d11LockCtx = device_.get();
    }
    preview_ = std::make_unique<PreviewRenderer>(*device_, text::createTextRasterizer(), fp);
    preview_->frames().setProxyResolver([this](const MediaItem& m) { return proxyFor(m); });
    preview_->setOnFrame([this] { wake(); });

    doc_ = std::make_unique<Document>(makeLocalizedProject(tr("Untitled"), ProjectSettings{}));
    doc_->setUndoLimit(1000);
    docListener_ = doc_->addListener([this](const ChangeEvent& ev) { onDocumentChanged(ev); });

    AutosaveManager::Config ac;
    ac.directory = dataDir_ / "Autosave";
    ac.snapshotIntervalSec = std::max(30, settings_.general.autosaveIntervalSec * 2);
    recovery_ = AutosaveManager::findRecoverable(ac.directory);
    autosave_ = std::make_unique<AutosaveManager>(ac);
    autosave_->attach(*doc_, "");
    if (!recovery_.empty()) {
        ui_.showRecovery = true;
        AVC_INFO("app", "{} recoverable project(s) found", recovery_.size());
    }

    std::unique_ptr<audio::IAudioOutput> out =
        headless_ ? audio::createNullOutput(48000) : audio::createDefaultOutput(settings_.audio.outputDevice, settings_.audio.bufferMs);
    playback_ = std::make_unique<audio::PlaybackEngine>(
        [this] {
            ProjectPtr p = planPreview();
            if (!p) p = doc_->snapshot();
            return std::make_pair(p, p ? p->activeSequence : kInvalidId);
        },
        std::move(out));

    exports_ = std::make_unique<exp::ExportQueue>();
    auto q = posts_;
    exports_->setOnFinished([q, this](const exp::ExportJob& job) {
        const auto prog = job.progress();
        const std::string name = job.name();
        const std::string summary = job.report().summary();
        const std::string path = job.settings().outputPath;
        q->post([this, prog, name, summary, path] {
            if (prog.state == exp::ExportState::Done)
                notify(LogLevel::Info, std::string(tr("Export finished")) + ": " + name, tr("Show in Folder"),
                       [path] { revealInFileManager(pathFromUtf8(path)); });
            else if (prog.state == exp::ExportState::Failed)
                notify(LogLevel::Error, std::string(tr("Export failed")) + ": " + name + " - " + prog.message);
        });
    });

    registerCommands();
    applySettings();

    if (settings_.cache.autoCleanup) {
        cache::CacheStore* store = cacheStore_.get();
        const uint64_t maxBytes = static_cast<uint64_t>(std::max(1.0, settings_.cache.maxGB) * 1024.0) * kMB;
        const int days = settings_.cache.maxAgeDays;
        Jobs::io().submit("Cache cleanup", JobPriority::Background, [store, maxBytes, days](JobContext&) {
            store->cleanup(maxBytes, days);
        });
    }
    playback_->setEndTime(sequenceEnd());
    AVC_INFO("app", "AviCap Studio ready (data: {})", pathToUtf8(dataDir_));
}

App::~App() {
    prepareExit();
    posts_->close();
    ai_.reset();
    sounds_.reset();
    // Background jobs reference engines owned here: let them finish first.
    Jobs::io().waitIdle();
    playback_.reset();
    exports_.reset();
    preview_.reset();
    assets_.reset();
    proxies_.reset();
    if (autosave_) autosave_->detach(true);
    autosave_.reset();
    if (doc_) doc_->removeListener(docListener_);
    doc_.reset();
    device_.reset();
}

void App::prepareExit() {
    if (playback_) playback_->pause();
    if (exports_) exports_->cancelAll();
    if (proxies_) proxies_->cancelAll();
    if (assets_) assets_->cancelAll();
    if (autosave_) autosave_->flush();
}

void App::setWakeCallback(std::function<void()> fn) {
    std::lock_guard lk(posts_->mutex);
    if (!posts_->closed) posts_->wake = std::move(fn);
}

void App::wake() {
    std::function<void()> w;
    {
        std::lock_guard lk(posts_->mutex);
        w = posts_->wake;
    }
    if (w) w();
}

void App::post(std::function<void()> fn) { posts_->post(std::move(fn)); }

double App::timeSec() const { return steadySeconds() - startSteady_; }

bool App::animating() const {
    if (playing() || importing_ > 0 || saving_ > 0 || loading_) return true;
    if (exports_ && exports_->busy()) return true;
    if (assets_ && assets_->pendingJobs() > 0) return true;
    if (ai_ && ai_->busy()) return true;
    if (sounds_ && (sounds_->scanning() || sounds_->previewer().playing())) return true;
    return !proxyRequested_.empty();
}

void App::tick() {
    const double now = timeSec();
    if (lastTick_ > 0) {
        stats.uiFrameMs = (now - lastTick_) * 1000.0;
        stats.uiFps = stats.uiFps * 0.9 + (stats.uiFrameMs > 0 ? 1000.0 / stats.uiFrameMs : 0) * 0.1;
    }
    lastTick_ = now;
    ++stats.frames;

    std::vector<std::function<void()>> items;
    {
        std::lock_guard lk(posts_->mutex);
        items.swap(posts_->items);
    }
    for (auto& fn : items) {
        try {
            fn();
        } catch (const std::exception& e) {
            AVC_ERROR("app", "posted task failed: {}", e.what());
        }
    }

    sounds_->previewer().tick();

    const bool isPlaying = playback_->playing();
    if (isPlaying) playhead_ = playback_->position();
    else if (wasPlaying_) playhead_ = snapToFrame(playback_->position());
    wasPlaying_ = isPlaying;

    // Proxy completion.
    if (!proxyRequested_.empty()) {
        bool changed = false;
        for (auto it = proxyRequested_.begin(); it != proxyRequested_.end();) {
            const auto st = proxies_->status(*it);
            if (st.state == proxy::ProxyState::Ready) {
                std::lock_guard lk(mediaMutex_);
                proxyPaths_[*it] = st.path;
                changed = true;
                it = proxyRequested_.erase(it);
            } else if (st.state == proxy::ProxyState::Failed) {
                const MediaItem* m = project().findMedia(*it);
                notify(LogLevel::Warning, std::string(tr("Proxy creation failed")) + ": " + (m ? m->name : "") + " " + st.error);
                it = proxyRequested_.erase(it);
            } else {
                ++it;
            }
        }
        if (changed && settings_.proxy.useProxies) {
            preview_->frames().closeAllDecoders();
            preview_->invalidate();
        }
        if (changed && proxyRequested_.empty()) notify(LogLevel::Info, tr("Proxies are ready"));
    }

    // Notifications expire (errors and actionable ones stay longer).
    while (!notes_.empty()) {
        const Notification& n = notes_.front();
        const double life = n.action ? 15.0 : n.level >= LogLevel::Error ? 12.0 : 5.0;
        if (now - n.createdSec < life) break;
        notes_.pop_front();
    }

    if (now - lastResourceCheck_ > 2.0) {
        lastResourceCheck_ = now;
        checkResources();
    }
    assets_->endFrame();
    updatePreviewRequest();
}

void App::updatePreviewRequest() {
    PreviewRequest r;
    r.project = planPreview();
    if (!r.project) r.project = doc_->current();
    r.sequence = sequenceId();
    Time t = playhead();
    const bool isPlaying = playing();
    if (isPlaying) {
        // Ask for the frame that will be audible once rendering finishes.
        const double latency = std::min(0.25, preview_->stats().avgRenderMs / 1000.0);
        t = t + Time::fromSeconds(latency * playSpeed());
        if (t < Time{0}) t = Time{0};
    }
    const Sequence* seq = sequence();
    r.time = seq ? t.snappedToFrame(seq->frameRate, Rounding::Down) : t;
    r.viewportWidth = viewerWidth;
    r.viewportHeight = viewerHeight;
    r.quality = previewQuality;
    r.playing = isPlaying;
    r.wantScopes = scopesVisible;
    PreviewRequest& last = lastRequest_;
    if (last.project == r.project && last.sequence == r.sequence && last.time == r.time && last.viewportWidth == r.viewportWidth &&
        last.viewportHeight == r.viewportHeight && last.quality == r.quality && last.playing == r.playing &&
        last.wantScopes == r.wantScopes)
        return;
    last = r;
    preview_->request(std::move(r));
}

void App::checkResources() {
    const MemoryStatus ms = queryMemoryStatus();
    const bool low = ms.totalPhysical > 0 && (ms.availablePhysical < 768 * kMB || ms.memoryLoadPercent >= 92);
    const size_t normalBudget = static_cast<size_t>(std::max(128, settings_.performance.frameCacheMB)) * kMB;
    if (low && !stats.lowMemory) {
        preview_->trimMemory();
        preview_->frames().cache().setBudget(std::min<size_t>(normalBudget, 256 * kMB));
        notify(LogLevel::Warning, tr("System memory is low: preview caches were reduced"));
        AVC_WARN("app", "low memory: {} MB available, load {}%", ms.availablePhysical / kMB, ms.memoryLoadPercent);
    } else if (!low && stats.lowMemory) {
        preview_->frames().cache().setBudget(normalBudget);
    }
    stats.lowMemory = low;

    if (auto ds = queryDiskSpace(cacheStore_->root())) {
        const bool lowDisk = ds->available < 2048 * kMB;
        if (lowDisk && !stats.lowDisk) {
            notify(LogLevel::Warning, tr("Disk space is low on the cache drive: cleaning the cache"));
            cache::CacheStore* store = cacheStore_.get();
            Jobs::io().submit("Cache cleanup", JobPriority::Background, [store](JobContext&) {
                store->cleanup(store->totalBytes() / 2, 0);
            });
        }
        stats.lowDisk = lowDisk;
    }
}

// ------------------------------------------------------------------ settings

void App::applySettings() {
    setLanguage(languageFromSetting(settings_.general.language));
    previewQuality = previewQualityFromName(settings_.playback.previewQuality);
    if (!settings_.playback.adaptiveQuality && previewQuality == PreviewQuality::Auto) previewQuality = PreviewQuality::Full;
    preview_->frames().setUseProxies(settings_.proxy.useProxies);
    preview_->frames().cache().setBudget(static_cast<size_t>(std::max(128, settings_.performance.frameCacheMB)) * kMB);
    playback_->setScrubAudio(settings_.playback.audioScrubbing);
    commands_.setPreset(settings_.keyboard.preset);
    commands_.loadOverrides(settings_.keyboard.overrides);
    setLoop(loopPlayback);
    preview_->invalidate();
}

void App::saveSettings() {
    settings_.keyboard.overrides = commands_.saveOverrides();
    settings_.keyboard.preset = commands_.preset();
    settings_.ui.snapping = snapping;
    settings_.playback.loop = loopPlayback;
    if (auto st = settings_.save(dataDir_ / "settings.json"); !st) AVC_WARN("app", "settings not saved: {}", st.message());
}

std::string App::windowTitle() const {
    std::string t = project().name;
    if (doc_->dirty()) t += " *";
    t += " - AviCap Studio";
    return t;
}

// ------------------------------------------------------------------ notifications

void App::notify(LogLevel level, std::string text, std::string actionLabel, std::function<void()> action) {
    logf(level, "ui", "{}", text);
    Notification n;
    n.id = nextNote_++;
    n.level = level;
    n.text = std::move(text);
    n.createdSec = timeSec();
    n.actionLabel = std::move(actionLabel);
    n.action = std::move(action);
    notes_.push_back(std::move(n));
    while (notes_.size() > 6) notes_.pop_front();
    wake();
}

std::string App::statusText() const {
    if (loading_) return tr("Loading project...");
    if (saving_) return tr("Saving...");
    if (importing_) return tr("Importing media...");
    return {};
}

// ------------------------------------------------------------------ project

void App::attachDocument(ProjectPtr p, const std::string& path, bool clean) {
    playback_->pause();
    assets_->cancelAll();
    preview_->frames().closeAllDecoders();
    autosave_->detach(true);
    doc_->reset(std::move(p), clean);
    projectPath_ = path;
    autosave_->attach(*doc_, path);
    selection.clear();
    playhead_ = Time{0};
    playback_->invalidate();
    playback_->seek(Time{0});
    playback_->setEndTime(sequenceEnd());
    targetVideoTrack = 0;
    targetAudioTrack = 0;
    {
        std::lock_guard lk(mediaMutex_);
        offline_.clear();
        proxyPaths_.clear();
    }
    proxyRequested_.clear();
    if (!path.empty()) {
        settings_.addRecentProject(path);
        saveSettings();
    }
    refreshMediaStatus();
    setLoop(loopPlayback);
    preview_->invalidate();
}

ProjectPtr App::makeLocalizedProject(const std::string& name, const ProjectSettings& s) {
    Project p = makeProject(name.empty() ? tr("Untitled") : name, s);
    for (auto& sp : p.sequences) {
        auto seq = std::make_shared<Sequence>(*sp);
        if (seq->name == "Sequence 1") seq->name = std::string(tr("Sequence")) + " 1";
        sp = seq;
    }
    return std::make_shared<Project>(std::move(p));
}

void App::newProject(const std::string& name, const ProjectSettings& s) {
    attachDocument(makeLocalizedProject(name, s), "", true);
    AVC_INFO("app", "new project '{}' {}x{} @ {}", project().name, s.width, s.height, s.frameRate.toString());
}

void App::openProject(const std::string& path) {
    if (loading_) return;
    loading_ = true;
    auto q = posts_;
    Jobs::io().submit("Open project", JobPriority::High, [q, this, path](JobContext&) {
        auto r = loadProjectFile(pathFromUtf8(path));
        q->post([this, path, r = std::move(r)]() mutable {
            loading_ = false;
            if (!r) {
                notify(LogLevel::Error, std::string(tr("Could not open project")) + ": " + r.errorMessage());
                return;
            }
            attachDocument(r->project, path, !r->migrated);
            if (r->migrated)
                notify(LogLevel::Info, std::string(tr("Project was upgraded from an older format")) + " (v" +
                                           std::to_string(r->loadedSchemaVersion) + ")");
            for (const auto& w : r->warnings) notify(LogLevel::Warning, w);
            AVC_INFO("app", "opened {}", path);
        });
    });
}

void App::saveProjectAs(const std::string& path, std::function<void(bool)> done) {
    std::string target = path;
    if (pathFromUtf8(target).extension() != ".avicap") target += ".avicap";
    // Name an untitled project after its file.
    if (project().name == tr("Untitled") || project().name == "Untitled") {
        const std::string stem = pathToUtf8(pathFromUtf8(target).stem());
        doc_->edit("Rename Project", [&](ProjectEditor& pe) {
            pe.root().name = stem;
            return Status::ok();
        }, EditOptions{"", false});
    }
    ++saving_;
    ProjectPtr snap = doc_->current();
    auto q = posts_;
    Jobs::io().submit("Save project", JobPriority::Highest, [q, this, snap, target, done](JobContext&) {
        Status st = saveProjectFile(*snap, pathFromUtf8(target));
        q->post([this, snap, target, done, st] {
            --saving_;
            if (st) {
                doc_->markSaved(snap);
                projectPath_ = target;
                autosave_->setProjectPath(target);
                autosave_->onSaved();
                settings_.addRecentProject(target);
                saveSettings();
                notify(LogLevel::Info, std::string(tr("Saved")) + ": " + pathToUtf8(pathFromUtf8(target).filename()));
            } else {
                notify(LogLevel::Error, std::string(tr("Save failed")) + ": " + st.message());
            }
            if (done) done(st.isOk());
        });
    });
}

void App::saveProject(std::function<void(bool)> done) {
    if (projectPath_.empty()) {
        saveProjectAsDialog(std::move(done));
        return;
    }
    saveProjectAs(projectPath_, std::move(done));
}

void App::saveProjectAsDialog(std::function<void(bool)> done) {
    auto p = dialogs_->saveFile(tr("Save Project As"), {{"AviCap Project", "*.avicap"}}, sanitizeFileName(project().name) + ".avicap",
                                "avicap");
    if (!p) {
        if (done) done(false);
        return;
    }
    saveProjectAs(*p, std::move(done));
}

void App::openProjectDialog() {
    guardUnsaved([this] {
        auto files = dialogs_->openFiles(tr("Open Project"), {{"AviCap Project", "*.avicap"}}, false);
        if (!files.empty()) openProject(files.front());
    });
}

void App::importDialog() {
    std::string patterns;
    for (const auto* list : {&supportedVideoExtensions(), &supportedAudioExtensions(), &supportedImageExtensions()})
        for (const auto& e : *list) patterns += (patterns.empty() ? "*" : ";*") + e;
    auto files = dialogs_->openFiles(tr("Import Media"), {{tr("Media files"), patterns}, {tr("All files"), "*.*"}}, true);
    if (!files.empty()) importFiles(std::move(files));
}

void App::addAudioDialog(bool music) {
    std::string patterns;
    for (const auto& e : supportedAudioExtensions()) patterns += (patterns.empty() ? "*" : ";*") + e;
    auto files = dialogs_->openFiles(music ? tr("Add Background Music") : tr("Add Sound Effect"),
                                     {{tr("Audio files"), patterns}, {tr("All files"), "*.*"}}, !music);
    addSoundFiles(std::move(files), music);
}

void App::addSoundFiles(std::vector<std::string> files, bool music, std::optional<Time> when) {
    if (files.empty()) return;
    const Time at = snapToFrame(when ? *when : playhead());
    importFiles(std::move(files), [this, music, at](const std::vector<MediaId>& ids) {
        Time t = at;
        for (MediaId id : ids) {
            auto r = placeAudio(id, t, music ? 1 : 2);
            if (!r) continue;
            if (const Clip* c = sequence()->clip(*r)) t = music ? c->end() : t;  // music plays back to back
        }
    });
}

Result<ClipId> App::placeAudio(MediaId id, Time at, int firstAudioTrack) {
    const MediaItem* m = project().findMedia(id);
    if (!m || !m->info.hasAudio()) return Result<ClipId>::error(tr("The file has no audio"));
    const MediaItem media = *m;
    ClipId created = kInvalidId;
    const Status st = editSequence(firstAudioTrack <= 1 ? "Add Background Music" : "Add Sound Effect", [&](SequenceEditor& e) {
        const Time dur = media.info.duration.ticks > 0 ? media.info.duration : Time::fromSeconds(1.0);
        const TimeRange range{at, dur};
        int track = -1;
        auto fam = e.seq().audioTrackIndices();
        for (size_t i = static_cast<size_t>(std::max(0, firstAudioTrack)); i < fam.size(); ++i) {
            const Track& t = e.trackAt(fam[i]);
            if (!t.locked && edit::rangeIsEmpty(t, range)) {
                track = fam[i];
                break;
            }
        }
        while (track < 0) {
            auto r = edit::addTrack(e, TrackKind::Audio);
            if (!r) return r.status();
            fam = e.seq().audioTrackIndices();
            if (static_cast<int>(fam.size()) > firstAudioTrack) {
                const int idx = e.seq().trackIndex(*r);
                if (edit::rangeIsEmpty(e.trackAt(idx), range)) track = idx;
            }
        }
        auto r = edit::addMediaClip(e, media, at, -1, track, edit::PlaceMode::Overwrite);
        if (!r) return r.status();
        if (!r->empty()) created = r->front();
        return Status::ok();
    });
    if (!st) return st;
    selection.clips = {created};
    return created;
}

void App::setWorkspace(const std::string& name) {
    ui_.workspace = name;
    ui_.applyWorkspace = name;
    settings_.ui.workspace = name;
}

void App::importFolderDialog() {
    if (auto dir = dialogs_->pickFolder(tr("Import Folder"))) importFiles({*dir});
}

void App::guardUnsaved(std::function<void()> fn) {
    if (!doc_->dirty()) {
        fn();
        return;
    }
    ui_.afterUnsavedCheck = std::move(fn);
    ui_.unsavedPrompt = true;
    wake();
}

void App::requestExit() {
    guardUnsaved([this] { ui_.exitConfirmed = true; });
}

void App::recoverProject(const RecoveryCandidate& c) {
    auto q = posts_;
    loading_ = true;
    Jobs::io().submit("Recover project", JobPriority::Highest, [q, this, c](JobContext&) {
        auto r = AutosaveManager::recover(c);
        q->post([this, c, r = std::move(r)]() mutable {
            loading_ = false;
            if (!r) {
                notify(LogLevel::Error, std::string(tr("Recovery failed")) + ": " + r.errorMessage());
                return;
            }
            attachDocument(r->project, r->projectPath, false);
            AutosaveManager::discard(c);
            std::erase_if(recovery_, [&](const RecoveryCandidate& x) { return x.metaFile == c.metaFile; });
            for (const auto& w : r->warnings) notify(LogLevel::Warning, w);
            notify(LogLevel::Info, std::string(tr("Project recovered")) + " (" + std::to_string(r->replayedEdits) + " " +
                                       tr("edits restored") + ")");
        });
    });
}

void App::discardRecovery(const RecoveryCandidate& c) {
    AutosaveManager::discard(c);
    std::erase_if(recovery_, [&](const RecoveryCandidate& x) { return x.metaFile == c.metaFile; });
}

// ------------------------------------------------------------------ media

void App::importFiles(std::vector<std::string> paths, std::function<void(const std::vector<MediaId>&)> done) {
    if (paths.empty()) return;
    ++importing_;
    std::set<std::string> existing;
    for (const auto& m : project().media) existing.insert(m->path);
    auto q = posts_;
    Jobs::io().submit("Import media", JobPriority::High, [q, this, paths, existing, done](JobContext& ctx) {
        // Expand folders (recursively) into importable files.
        std::vector<std::string> files;
        for (const auto& p : paths) {
            const fs::path fp = pathFromUtf8(p);
            std::error_code ec;
            if (fs::is_directory(fp, ec)) {
                std::vector<std::string> found;
                for (fs::recursive_directory_iterator it(fp, fs::directory_options::skip_permission_denied, ec), end;
                     !ec && it != end && found.size() < 5000; it.increment(ec))
                    if (it->is_regular_file(ec) && isImportableFile(it->path())) found.push_back(pathToUtf8(it->path()));
                std::sort(found.begin(), found.end());
                files.insert(files.end(), found.begin(), found.end());
            } else {
                files.push_back(p);
            }
        }
        std::vector<MediaItem> items;
        std::vector<std::string> errors;
        std::vector<std::string> duplicates;
        for (size_t i = 0; i < files.size(); ++i) {
            if (ctx.cancelled()) break;
            ctx.setProgress(static_cast<float>(i) / static_cast<float>(std::max<size_t>(1, files.size())));
            if (existing.count(files[i])) {
                duplicates.push_back(files[i]);
                continue;
            }
            auto r = createMediaItem(files[i]);
            if (r) items.push_back(std::move(*r));
            else errors.push_back(pathToUtf8(pathFromUtf8(files[i]).filename()) + ": " + r.errorMessage());
        }
        q->post([this, items = std::move(items), errors, duplicates, done, files]() mutable {
            --importing_;
            std::vector<MediaId> ids;
            std::string matched;
            if (!items.empty()) {
                editProject(items.size() == 1 ? "Import " + items[0].name : "Import " + std::to_string(items.size()) + " files",
                            [&](ProjectEditor& pe) {
                                // An empty first sequence adopts the first video's format.
                                const Sequence* seq = pe.current().active();
                                if (seq && seq->clipCount() == 0) {
                                    for (const auto& m : items) {
                                        const auto* v = m.info.primaryVideo();
                                        if (!v || m.info.kind != MediaKind::Video || v->width <= 0) continue;
                                        auto se = pe.sequence(seq->id);
                                        Sequence& s = se.props();
                                        const bool rotated = v->rotation == 90 || v->rotation == 270;
                                        s.width = rotated ? v->height : v->width;
                                        s.height = rotated ? v->width : v->height;
                                        if (v->frameRate.valid()) s.frameRate = v->frameRate;
                                        pe.root().settings.width = s.width;
                                        pe.root().settings.height = s.height;
                                        pe.root().settings.frameRate = s.frameRate;
                                        matched = m.name + " (" + std::to_string(s.width) + "x" + std::to_string(s.height) + ", " +
                                                  fpsText(s.frameRate) + " fps)";
                                        break;
                                    }
                                }
                                for (auto& m : items) {
                                    ids.push_back(m.id);
                                    pe.addMedia(m);
                                }
                                return Status::ok();
                            });
                selection.media = std::set<MediaId>(ids.begin(), ids.end());
                notify(LogLevel::Info, std::string(tr("Imported")) + " " + std::to_string(items.size()) + " " + tr("file(s)"));
                if (!matched.empty()) notify(LogLevel::Info, std::string(tr("Sequence settings matched to")) + " " + matched);
                if (settings_.proxy.autoCreateAboveHeight > 0) {
                    std::set<MediaId> big;
                    for (const auto& m : items)
                        if (const auto* v = m.info.primaryVideo(); v && v->height > settings_.proxy.autoCreateAboveHeight) big.insert(m.id);
                    if (!big.empty()) createProxies(big);
                }
            }
            if (!duplicates.empty())
                notify(LogLevel::Info, std::to_string(duplicates.size()) + " " + tr("file(s) were already in the project"));
            for (const auto& e : errors) notify(LogLevel::Warning, std::string(tr("Import failed")) + ": " + e);
            refreshMediaStatus();
            if (done) {
                // Report every requested file that is now in the project (new or already
                // imported), in the order given, so callers can place them.
                std::vector<MediaId> ordered;
                for (const auto& f : files)
                    for (const auto& m : project().media)
                        if (m->path == f) {
                            ordered.push_back(m->id);
                            break;
                        }
                done(ordered);
            }
        });
    });
}

void App::removeMedia(const std::set<MediaId>& ids) {
    if (ids.empty()) return;
    editProject("Remove Media", [&](ProjectEditor& pe) {
        for (const auto& sp : pe.current().sequences) {
            std::set<ClipId> clips;
            for (const auto& t : sp->tracks)
                for (const auto& c : t->clips)
                    if (c->kind == ClipKind::Media && ids.count(c->media)) clips.insert(c->id);
            if (!clips.empty()) {
                auto se = pe.sequence(sp->id);
                if (auto st = edit::deleteClips(se, clips); !st) return st;
            }
        }
        for (MediaId id : ids) pe.removeMedia(id);
        return Status::ok();
    });
    for (MediaId id : ids) selection.media.erase(id);
    pruneSelection();
}

bool App::mediaOffline(MediaId id) const {
    std::lock_guard lk(mediaMutex_);
    return offline_.count(id) != 0;
}

size_t App::offlineCount() const {
    std::lock_guard lk(mediaMutex_);
    return offline_.size();
}

void App::refreshMediaStatus() {
    std::vector<MediaItem> media;
    for (const auto& m : project().media) media.push_back(*m);
    const int proxyHeight = proxy::presetHeight(settings_.proxy.preset);
    proxy::ProxyManager* proxies = proxies_.get();
    auto q = posts_;
    Jobs::io().submit("Media status", JobPriority::Medium, [q, this, media, proxyHeight, proxies](JobContext&) {
        std::set<MediaId> offline;
        std::vector<std::pair<MediaId, FileIdentity>> changed;
        std::map<MediaId, std::string> proxyPaths;
        for (const auto& m : media) {
            if (m.path.empty()) continue;
            const FileIdentity id = fileIdentity(pathFromUtf8(m.path));
            if (!id.exists) {
                offline.insert(m.id);
                continue;
            }
            if ((m.fileSize || m.fileModifiedNs) && (id.size != m.fileSize || id.modifiedNs != m.fileModifiedNs))
                changed.emplace_back(m.id, id);
            if (const std::string p = proxies->proxyPath(m, proxyHeight); !p.empty()) proxyPaths[m.id] = p;
        }
        q->post([this, offline, changed, proxyPaths] {
            size_t before;
            {
                std::lock_guard lk(mediaMutex_);
                before = offline_.size();
                offline_ = offline;
                for (const auto& [id, p] : proxyPaths) proxyPaths_[id] = p;
            }
            if (!changed.empty()) {
                // Source files changed on disk: update identities so caches and decoders refresh.
                doc_->edit("Update Media", [&](ProjectEditor& pe) {
                    for (const auto& [id, fi] : changed) {
                        if (!pe.current().findMedia(id)) continue;
                        MediaItem& m = pe.media(id);
                        m.fileSize = fi.size;
                        m.fileModifiedNs = fi.modifiedNs;
                        preview_->frames().closeDecodersFor(m.path);
                    }
                    return Status::ok();
                }, EditOptions{"", false});
                notify(LogLevel::Info, std::to_string(changed.size()) + " " + tr("source file(s) changed on disk and were refreshed"));
                playback_->invalidate();
                preview_->invalidate();
            }
            if (!offline.empty() && offline.size() != before)
                notify(LogLevel::Warning, std::to_string(offline.size()) + " " + tr("media file(s) are offline"), tr("Relink..."),
                       [this] { ui_.showRelink = true; });
            if (offline.size() != before) preview_->invalidate();
        });
    });
}

void App::relinkMedia(MediaId id, const std::string& newPath) {
    std::map<MediaId, std::string> all = inferRelinks(project(), id, newPath);
    all[id] = newPath;
    const Status st = editProject("Relink Media", [&](ProjectEditor& pe) {
        for (const auto& [mid, path] : all)
            if (auto s = avc::relinkMedia(pe, mid, path); !s) return s;
        return Status::ok();
    });
    if (st) {
        notify(LogLevel::Info, std::string(tr("Relinked")) + " " + std::to_string(all.size()) + " " + tr("file(s)"));
        preview_->frames().closeAllDecoders();
        playback_->invalidate();
        refreshMediaStatus();
    }
}

void App::relinkSearchFolder(const std::string& folder) {
    auto missing = findMissingMedia(project());
    if (missing.empty()) return;
    auto q = posts_;
    Jobs::io().submit("Relink search", JobPriority::High, [q, this, missing, folder](JobContext& ctx) {
        auto found = searchFolderForMedia(missing, pathFromUtf8(folder), 8, ctx.token());
        q->post([this, found, total = missing.size()] {
            if (found.empty()) {
                notify(LogLevel::Warning, tr("No missing files were found in that folder"));
                return;
            }
            editProject("Relink Media", [&](ProjectEditor& pe) {
                for (const auto& [mid, path] : found)
                    if (auto s = avc::relinkMedia(pe, mid, path); !s) return s;
                return Status::ok();
            });
            notify(LogLevel::Info, std::string(tr("Relinked")) + " " + std::to_string(found.size()) + " / " + std::to_string(total));
            preview_->frames().closeAllDecoders();
            playback_->invalidate();
            refreshMediaStatus();
        });
    });
}

void App::createProxies(const std::set<MediaId>& ids) {
    const int h = proxy::presetHeight(settings_.proxy.preset);
    int n = 0;
    for (MediaId id : ids) {
        const MediaItem* m = project().findMedia(id);
        if (!m || m->info.kind != MediaKind::Video) continue;
        proxies_->request(*m, h);
        proxyRequested_.insert(id);
        ++n;
    }
    if (n) notify(LogLevel::Info, std::string(tr("Creating proxies")) + ": " + std::to_string(n));
}

std::string App::proxyFor(const MediaItem& m) {
    std::lock_guard lk(mediaMutex_);
    auto it = proxyPaths_.find(m.id);
    return it == proxyPaths_.end() ? std::string() : it->second;
}

int App::targetVideoTrackIndex() const {
    const Sequence* s = sequence();
    if (!s) return -1;
    std::vector<int> vids;
    for (int i : s->visualTrackIndices())
        if (s->tracks[static_cast<size_t>(i)]->kind == TrackKind::Video) vids.push_back(i);
    if (vids.empty()) return -1;
    return vids[static_cast<size_t>(std::clamp(targetVideoTrack, 0, static_cast<int>(vids.size()) - 1))];
}

int App::targetAudioTrackIndex() const {
    const Sequence* s = sequence();
    if (!s) return -1;
    const auto aud = s->audioTrackIndices();
    if (aud.empty()) return -1;
    return aud[static_cast<size_t>(std::clamp(targetAudioTrack, 0, static_cast<int>(aud.size()) - 1))];
}

Result<std::vector<ClipId>> App::addMediaToTimeline(MediaId id, Time at, int vTrack, int aTrack, bool insert) {
    const MediaItem* m = project().findMedia(id);
    if (!m) return Result<std::vector<ClipId>>::error("Media not found");
    const MediaItem media = *m;
    std::vector<ClipId> created;
    const Status st = editSequence(insert ? "Insert Clip" : "Overwrite Clip", [&](SequenceEditor& e) {
        auto r = edit::addMediaClip(e, media, at, media.info.hasVideo() ? vTrack : -1, media.info.hasAudio() ? aTrack : -1,
                                    insert ? edit::PlaceMode::Insert : edit::PlaceMode::Overwrite);
        if (r) created = *r;
        return r.status();
    });
    if (!st) return st;
    selection.clips = std::set<ClipId>(created.begin(), created.end());
    playback_->setEndTime(sequenceEnd());
    return created;
}

void App::appendMediaAtPlayhead(MediaId id) {
    auto r = addMediaToTimeline(id, playhead(), targetVideoTrackIndex(), targetAudioTrackIndex(), true);
    if (r && !r->empty()) {
        if (const Clip* c = sequence()->clip(r->front())) seek(c->end());
    }
}

// ------------------------------------------------------------------ transport

Time App::frameDuration() const {
    const Sequence* s = sequence();
    return Time::frameDuration(s ? s->frameRate : Rational{30, 1});
}

Time App::snapToFrame(Time t) const {
    const Sequence* s = sequence();
    return s ? t.snappedToFrame(s->frameRate, Rounding::Nearest) : t;
}

Time App::sequenceEnd() const {
    if (ProjectPtr p = planPreview())
        if (const Sequence* ps = p->active()) return ps->duration();
    const Sequence* s = sequence();
    return s ? s->duration() : Time{0};
}

Time App::playhead() const { return playback_->playing() ? playback_->position() : playhead_; }
bool App::playing() const { return playback_->playing(); }
double App::playSpeed() const { return playback_->speed(); }

void App::seek(Time t) {
    if (t < Time{0}) t = Time{0};
    t = snapToFrame(t);
    playhead_ = t;
    playback_->seek(t);
}

void App::scrub(Time t) {
    seek(t);
    if (!playing() && settings_.playback.audioScrubbing) playback_->scrub(playhead_);
}

void App::togglePlay() {
    if (playing()) {
        playback_->pause();
        playhead_ = snapToFrame(playback_->position());
        return;
    }
    const Time end = sequenceEnd();
    playback_->setEndTime(end);
    if (end.ticks > 0 && playhead_ >= end - frameDuration() && !loopPlayback) seek(Time{0});
    playback_->play(1.0);
}

void App::shuttle(int direction) {
    const double maxSpeed = std::max(1, settings_.playback.shuttleMaxSpeed);
    if (direction == 0) {
        if (playing()) togglePlay();
        return;
    }
    playback_->setEndTime(sequenceEnd());
    const double s = playing() ? playSpeed() : 0.0;
    double next = direction > 0 ? 1.0 : -1.0;
    if (direction > 0 && s > 0) next = std::min(maxSpeed, s * 2.0);
    if (direction < 0 && s < 0) next = std::max(-maxSpeed, s * 2.0);
    playback_->play(next);
}

void App::stepFrames(int n) {
    if (playing()) {
        playback_->pause();
        playhead_ = snapToFrame(playback_->position());
    }
    seek(playhead_ + Time{frameDuration().ticks * n});
}

void App::goToStart() { seek(Time{0}); }
void App::goToEnd() { seek(sequenceEnd()); }

void App::goToEdit(bool next) {
    const Sequence* s = sequence();
    if (!s) return;
    std::vector<Time> pts{Time{0}, s->duration()};
    for (const auto& t : s->tracks) {
        if (t->hidden && isVisualTrack(t->kind)) continue;
        for (const auto& c : t->clips) {
            pts.push_back(c->start);
            pts.push_back(c->end());
        }
    }
    for (const auto& m : s->markers) pts.push_back(m.time);
    std::sort(pts.begin(), pts.end());
    const Time half{frameDuration().ticks / 2};
    const Time now = playhead();
    if (next) {
        for (Time t : pts)
            if (t > now + half) return seek(t);
    } else {
        for (auto it = pts.rbegin(); it != pts.rend(); ++it)
            if (*it < now - half) return seek(*it);
    }
}

void App::setLoop(bool on) {
    loopPlayback = on;
    const Sequence* s = sequence();
    if (!on || !s) {
        playback_->setLoop(std::nullopt);
        return;
    }
    playback_->setLoop(s->workArea ? *s->workArea : TimeRange{Time{0}, s->duration()});
}

// ------------------------------------------------------------------ editing

Status App::editSequence(const std::string& label, const std::function<Status(SequenceEditor&)>& fn, const EditOptions& opt) {
    if (!sequence()) return Status::error("No sequence");
    Status st = doc_->editSequence(label, sequenceId(), fn, opt);
    if (!st) notify(LogLevel::Warning, std::string(tr(label.c_str())) + ": " + st.message());
    return st;
}

Status App::editProject(const std::string& label, const std::function<Status(ProjectEditor&)>& fn, const EditOptions& opt) {
    Status st = doc_->edit(label, fn, opt);
    if (!st) notify(LogLevel::Warning, std::string(tr(label.c_str())) + ": " + st.message());
    return st;
}

void App::setPlanPreview(ProjectPtr p) {
    {
        std::lock_guard lk(planPreviewMutex_);
        if (planPreview_ == p) return;
        planPreview_ = std::move(p);
    }
    playback_->setEndTime(sequenceEnd());
    wake();
}

ProjectPtr App::planPreview() const {
    std::lock_guard lk(planPreviewMutex_);
    return planPreview_;
}

void App::onDocumentChanged(const ChangeEvent& ev) {
    // The preview was computed from the previous document state.
    if (planPreview()) {
        ai_->discardPlan();
        setPlanPreview(nullptr);
    }
    if (ev.kind != ChangeEvent::Kind::Edit) pruneSelection();
    playback_->setEndTime(sequenceEnd());
    const Sequence* a = ev.before ? ev.before->active() : nullptr;
    const Sequence* b = ev.after ? ev.after->active() : nullptr;
    if (loopPlayback && (!a || !b || a->workArea != b->workArea || a->duration() != b->duration())) setLoop(true);
    wake();
}

void App::undo() {
    if (!doc_->canUndo()) return;
    const std::string label = doc_->undoLabel();
    doc_->undo();
    notify(LogLevel::Debug, std::string(tr("Undo")) + ": " + tr(label.c_str()));
}

void App::redo() {
    if (!doc_->canRedo()) return;
    const std::string label = doc_->redoLabel();
    doc_->redo();
    notify(LogLevel::Debug, std::string(tr("Redo")) + ": " + tr(label.c_str()));
}

void App::pruneSelection() {
    const Sequence* s = sequence();
    if (!s) {
        selection.clips.clear();
        return;
    }
    std::erase_if(selection.clips, [&](ClipId id) { return !s->clip(id); });
    std::erase_if(selection.media, [&](MediaId id) { return !project().findMedia(id); });
    if (selection.marker != kInvalidId &&
        std::none_of(s->markers.begin(), s->markers.end(), [&](const Marker& m) { return m.id == selection.marker; }))
        selection.marker = kInvalidId;
}

const Clip* App::primaryClip() const {
    const Sequence* s = sequence();
    if (!s || selection.clips.empty()) return nullptr;
    // Prefer a visual clip (inspector shows transform/effects).
    const Clip* first = nullptr;
    for (ClipId id : selection.clips) {
        const Clip* c = s->clip(id);
        if (!c) continue;
        if (!first) first = c;
        const Track* t = s->trackOfClip(id);
        if (t && isVisualTrack(t->kind)) return c;
    }
    return first;
}

void App::selectClip(ClipId id, bool additive, bool toggle) {
    const Sequence* s = sequence();
    if (!s || !s->clip(id)) return;
    const std::set<ClipId> group = edit::expandSelection(*s, {id}, linkedSelection, true);
    if (toggle) {
        const bool has = selection.clips.count(id) != 0;
        for (ClipId g : group) {
            if (has) selection.clips.erase(g);
            else selection.clips.insert(g);
        }
        return;
    }
    if (!additive) selection.clips.clear();
    selection.clips.insert(group.begin(), group.end());
    selection.marker = kInvalidId;
}

void App::selectAll() {
    const Sequence* s = sequence();
    if (!s) return;
    selection.clips.clear();
    for (const auto& t : s->tracks)
        if (!t->locked)
            for (const auto& c : t->clips) selection.clips.insert(c->id);
}

void App::splitAtPlayhead(bool allTracks) {
    const Time t = snapToFrame(playhead());
    if (!allTracks && !selection.clips.empty()) {
        const std::set<ClipId> ids = selection.clips;
        editSequence("Split", [&](SequenceEditor& e) {
            bool any = false;
            for (ClipId id : ids) {
                const Clip* c = e.clip(id);
                if (!c || !(c->start < t && t < c->end())) continue;
                auto r = edit::splitClip(e, id, t, linkedSelection);
                if (!r) return r.status();
                any = true;
            }
            return any ? Status::ok() : Status::error(tr("The playhead is not inside a selected clip"));
        });
        return;
    }
    editSequence("Split", [&](SequenceEditor& e) {
        auto r = edit::splitAtTime(e, t, {});
        if (!r) return r.status();
        return r->empty() ? Status::error(tr("No clip under the playhead")) : Status::ok();
    });
}

void App::deleteSelection(bool ripple) {
    if (selection.clips.empty()) {
        if (selection.marker != kInvalidId) {
            const MarkerId m = selection.marker;
            editSequence("Delete Marker", [&](SequenceEditor& e) {
                return edit::removeMarker(e, m) ? Status::ok() : Status::error("Marker not found");
            });
            selection.marker = kInvalidId;
        }
        return;
    }
    const std::set<ClipId> ids = selection.clips;
    if (editSequence(ripple ? "Ripple Delete" : "Delete",
                     [&](SequenceEditor& e) { return ripple ? edit::rippleDelete(e, ids) : edit::deleteClips(e, ids); }))
        selection.clips.clear();
}

void App::copySelection() {
    const Sequence* s = sequence();
    if (!s || selection.clips.empty()) return;
    clipboard_ = edit::copyClips(*s, selection.clips);
    notify(LogLevel::Debug, std::string(tr("Copied")) + " " + std::to_string(clipboard_.items.size()) + " " + tr("clip(s)"));
}

void App::cutSelection() {
    copySelection();
    deleteSelection(false);
}

void App::paste(bool insert) {
    if (clipboard_.empty()) return;
    std::vector<ClipId> created;
    const Time at = snapToFrame(playhead());
    if (!editSequence(insert ? "Paste Insert" : "Paste", [&](SequenceEditor& e) {
            auto r = edit::pasteClips(e, clipboard_, at, targetVideoTrackIndex(), targetAudioTrackIndex(),
                                      insert ? edit::PlaceMode::Insert : edit::PlaceMode::Overwrite);
            if (r) created = *r;
            return r.status();
        }))
        return;
    selection.clips = std::set<ClipId>(created.begin(), created.end());
    Time end = at;
    for (ClipId id : created)
        if (const Clip* c = sequence()->clip(id)) end = maxTime(end, c->end());
    seek(end);
}

void App::duplicateSelection() {
    if (selection.clips.empty()) return;
    const std::set<ClipId> ids = selection.clips;
    std::vector<ClipId> created;
    if (editSequence("Duplicate", [&](SequenceEditor& e) {
            auto r = edit::duplicateClips(e, ids);
            if (r) created = *r;
            return r.status();
        }))
        selection.clips = std::set<ClipId>(created.begin(), created.end());
}

void App::setSelectionEnabled(bool enabled) {
    if (selection.clips.empty()) return;
    const std::set<ClipId> ids = selection.clips;
    editSequence(enabled ? "Enable Clips" : "Disable Clips", [&](SequenceEditor& e) { return edit::setClipsEnabled(e, ids, enabled); });
}

void App::toggleSelectionEnabled() {
    const Sequence* s = sequence();
    if (!s) return;
    bool anyEnabled = false;
    for (ClipId id : selection.clips)
        if (const Clip* c = s->clip(id); c && c->enabled) anyEnabled = true;
    setSelectionEnabled(!anyEnabled);
}

void App::groupSelection(bool group) {
    if (selection.clips.empty()) return;
    const std::set<ClipId> ids = selection.clips;
    editSequence(group ? "Group" : "Ungroup", [&](SequenceEditor& e) { return group ? edit::groupClips(e, ids) : edit::ungroupClips(e, ids); });
}

void App::linkSelection(bool link) {
    if (selection.clips.empty()) return;
    const std::set<ClipId> ids = selection.clips;
    editSequence(link ? "Link" : "Unlink", [&](SequenceEditor& e) { return link ? edit::linkClips(e, ids) : edit::unlinkClips(e, ids); });
}

void App::nudgeSelection(int frames) {
    if (selection.clips.empty()) return;
    const std::set<ClipId> ids = selection.clips;
    const Time delta{frameDuration().ticks * frames};
    editSequence("Nudge", [&](SequenceEditor& e) { return edit::moveClips(e, ids, delta, 0, 0); }, EditOptions{"nudge"});
}

void App::setInPoint() {
    const Time t = snapToFrame(playhead());
    editSequence("Mark In", [&](SequenceEditor& e) {
        Sequence& s = e.props();
        const Time end = s.workArea && s.workArea->end() > t ? s.workArea->end() : maxTime(s.duration(), t + frameDuration());
        s.workArea = TimeRange{t, end - t};
        return Status::ok();
    });
}

void App::setOutPoint() {
    const Time t = snapToFrame(playhead());
    editSequence("Mark Out", [&](SequenceEditor& e) {
        Sequence& s = e.props();
        const Time start = s.workArea && s.workArea->start < t ? s.workArea->start : Time{0};
        if (t <= start) return Status::error(tr("Out point must be after the in point"));
        s.workArea = TimeRange{start, t - start};
        return Status::ok();
    });
}

void App::clearInOut() {
    const Sequence* s = sequence();
    if (!s || !s->workArea) return;
    editSequence("Clear In/Out", [&](SequenceEditor& e) {
        e.props().workArea.reset();
        return Status::ok();
    });
}

MarkerId App::addMarker(MarkerKind kind, std::string name) {
    MarkerId id = kInvalidId;
    const Time t = snapToFrame(playhead());
    const size_t n = sequence() ? sequence()->markers.size() + 1 : 1;
    editSequence("Add Marker", [&](SequenceEditor& e) {
        Marker m;
        m.time = t;
        m.kind = kind;
        m.name = name.empty() ? std::string(tr("Marker")) + " " + std::to_string(n) : name;
        id = edit::addMarker(e, m);
        return Status::ok();
    });
    selection.marker = id;
    return id;
}

int App::findFreeVisualTrack(TimeRange range, std::optional<TrackKind> preferKind) const {
    const Sequence* s = sequence();
    if (!s) return -1;
    const auto vis = s->visualTrackIndices();  // bottom -> top
    int highestUsed = -1;
    for (size_t i = 0; i < vis.size(); ++i)
        if (!edit::rangeIsEmpty(*s->tracks[static_cast<size_t>(vis[i])], range)) highestUsed = static_cast<int>(i);
    for (size_t i = static_cast<size_t>(highestUsed + 1); i < vis.size(); ++i) {
        const Track& t = *s->tracks[static_cast<size_t>(vis[i])];
        if (t.locked) continue;
        if (t.kind == TrackKind::Video || (preferKind && t.kind == *preferKind)) return vis[i];
    }
    return -1;
}

ClipId App::addTextClip(const std::string& text) {
    const Time start = snapToFrame(playhead());
    const Time dur = Time::fromSeconds(5.0);
    ClipId id = kInvalidId;
    editSequence("Add Title", [&](SequenceEditor& e) {
        int track = findFreeVisualTrack({start, dur}, TrackKind::Text);
        if (track < 0) {
            auto r = edit::addTrack(e, TrackKind::Video);
            if (!r) return r.status();
            track = e.seq().trackIndex(*r);
        }
        Clip c = makeTextClip(text, start, dur);
        id = c.id;
        return edit::placeClips(e, {{track, c}}, edit::PlaceMode::Overwrite);
    });
    if (id != kInvalidId) selection.clips = {id};
    return id;
}

ClipId App::addSubtitleClip(const std::string& text, Time start, Time duration) {
    ClipId id = kInvalidId;
    editSequence("Add Subtitle", [&](SequenceEditor& e) {
        int track = -1;
        for (int i = 0; i < e.trackCount(); ++i)
            if (e.trackAt(i).kind == TrackKind::Subtitle && !e.trackAt(i).locked) track = i;
        if (track < 0) {
            auto r = edit::addTrack(e, TrackKind::Subtitle, tr("Subtitles"));
            if (!r) return r.status();
            track = e.seq().trackIndex(*r);
        }
        Clip c = makeTextClip(text, start, duration, ClipKind::Subtitle);
        c.textStyle.fontSize = std::max(36.0f, static_cast<float>(e.seq().height) * 0.05f);
        c.textStyle.fontWeight = 600;
        c.textParams.setStatic("strokeColor", pv(0, 0, 0, 1));
        c.textParams.setStatic("strokeWidth", pv(c.textStyle.fontSize * 0.08f));
        id = c.id;
        return edit::placeClips(e, {{track, c}}, edit::PlaceMode::Overwrite);
    });
    if (id != kInvalidId) selection.clips = {id};
    return id;
}

ClipId App::addAdjustmentLayer() {
    const Time start = snapToFrame(playhead());
    const Time dur = Time::fromSeconds(5.0);
    ClipId id = kInvalidId;
    editSequence("Add Adjustment Layer", [&](SequenceEditor& e) {
        int track = findFreeVisualTrack({start, dur}, TrackKind::Adjustment);
        if (track < 0) {
            auto r = edit::addTrack(e, TrackKind::Video);
            if (!r) return r.status();
            track = e.seq().trackIndex(*r);
        }
        Clip c = makeAdjustmentClip(start, dur);
        id = c.id;
        return edit::placeClips(e, {{track, c}}, edit::PlaceMode::Overwrite);
    });
    if (id != kInvalidId) selection.clips = {id};
    return id;
}

ClipId App::addSolidClip(ParamValue color) {
    const Time start = snapToFrame(playhead());
    const Time dur = Time::fromSeconds(5.0);
    ClipId id = kInvalidId;
    editSequence("Add Color Matte", [&](SequenceEditor& e) {
        int track = findFreeVisualTrack({start, dur}, std::nullopt);
        if (track < 0) {
            auto r = edit::addTrack(e, TrackKind::Video);
            if (!r) return r.status();
            track = e.seq().trackIndex(*r);
        }
        Clip c = makeSolidClip(color, start, dur);
        id = c.id;
        return edit::placeClips(e, {{track, c}}, edit::PlaceMode::Overwrite);
    });
    if (id != kInvalidId) selection.clips = {id};
    return id;
}

void App::applyEffectToClip(ClipId id, const std::string& effectId) {
    const fx::EffectDef* def = fx::EffectRegistry::instance().find(effectId);
    const Sequence* s = sequence();
    if (!def || !s) return;
    const Track* t = s->trackOfClip(id);
    if (!t) return;
    const bool audioClip = !isVisualTrack(t->kind);
    if (audioClip != (def->kind == fx::EffectKind::Audio)) {
        notify(LogLevel::Warning, def->kind == fx::EffectKind::Audio ? tr("Audio effects apply to audio clips")
                                                                        : tr("Video effects apply to video clips"));
        return;
    }
    editSequence("Apply Effect", [&](SequenceEditor& e) {
        e.mutableClip(id).effects.push_back(fx::EffectRegistry::instance().instantiate(effectId));
        return Status::ok();
    });
}

void App::applyEffect(const std::string& effectId) {
    const fx::EffectDef* def = fx::EffectRegistry::instance().find(effectId);
    const Sequence* s = sequence();
    if (!def || !s) return;
    std::vector<ClipId> targets;
    for (ClipId id : selection.clips) {
        const Track* t = s->trackOfClip(id);
        if (t && isVisualTrack(t->kind) == (def->kind == fx::EffectKind::Video)) targets.push_back(id);
    }
    if (targets.empty()) {
        notify(LogLevel::Warning, def->kind == fx::EffectKind::Audio ? tr("Select an audio clip to apply this effect")
                                                                        : tr("Select a video clip to apply this effect"));
        return;
    }
    editSequence("Apply Effect", [&](SequenceEditor& e) {
        for (ClipId id : targets) e.mutableClip(id).effects.push_back(fx::EffectRegistry::instance().instantiate(effectId));
        return Status::ok();
    });
}

void App::applyTransition(const std::string& type, Time duration) {
    const Sequence* s = sequence();
    if (!s || selection.clips.empty()) {
        notify(LogLevel::Warning, tr("Select a clip to add a transition"));
        return;
    }
    const Time now = playhead();
    const std::set<ClipId> ids = selection.clips;
    editSequence("Add Transition", [&](SequenceEditor& e) {
        for (ClipId id : ids) {
            Clip& c = e.mutableClip(id);
            const Time d = minTime(duration, Time{c.duration.ticks / 2});
            const bool head = absTicks(now - c.start) <= absTicks(now - c.end());
            TransitionSpec spec;
            spec.type = type;
            spec.duration = d;
            if (head) c.transitionIn = spec;
            else c.transitionOut = spec;
        }
        return Status::ok();
    });
}

void App::setSelectionSpeed(Rational speed, bool ripple) {
    if (selection.clips.empty() || !speed.valid()) return;
    const std::set<ClipId> ids = selection.clips;
    editSequence("Change Speed", [&](SequenceEditor& e) {
        for (ClipId id : ids)
            if (auto st = edit::setClipSpeed(e, id, speed, ripple); !st) return st;
        return Status::ok();
    });
}

void App::reverseSelection() {
    const Sequence* s = sequence();
    if (!s || selection.clips.empty()) return;
    const std::set<ClipId> ids = selection.clips;
    editSequence("Reverse", [&](SequenceEditor& e) {
        for (ClipId id : ids)
            if (const Clip* c = e.clip(id))
                if (auto st = edit::setClipReverse(e, id, !c->reverse); !st) return st;
        return Status::ok();
    });
}

void App::freezeFrameAtPlayhead() {
    const Sequence* s = sequence();
    if (!s) return;
    const Time t = snapToFrame(playhead());
    ClipId target = kInvalidId;
    for (ClipId id : selection.clips)
        if (const Clip* c = s->clip(id); c && c->kind == ClipKind::Media && c->range().contains(t)) {
            const Track* tr = s->trackOfClip(id);
            if (tr && isVisualTrack(tr->kind)) target = id;
        }
    if (target == kInvalidId) {
        notify(LogLevel::Warning, tr("Select a video clip under the playhead"));
        return;
    }
    editSequence("Freeze Frame", [&](SequenceEditor& e) { return edit::insertFreezeFrame(e, target, t, Time::fromSeconds(2.0)).status(); });
}

void App::createCompoundFromSelection() {
    if (selection.clips.empty()) return;
    const std::set<ClipId> ids = selection.clips;
    const SequenceId sid = sequenceId();
    const std::string name = std::string(tr("Compound Clip")) + " " + std::to_string(project().sequences.size());
    if (editProject("Create Compound Clip", [&](ProjectEditor& pe) { return edit::createCompoundClip(pe, sid, ids, name).status(); }))
        selection.clips.clear();
}

void App::addTrack(TrackKind kind) {
    editSequence("Add Track", [&](SequenceEditor& e) { return edit::addTrack(e, kind).status(); });
}

void App::closeGapAtPlayhead() {
    const int track = targetVideoTrackIndex();
    const Time t = playhead();
    editSequence("Close Gap", [&](SequenceEditor& e) { return edit::closeGap(e, track, t); });
}

// ------------------------------------------------------------------ export

exp::ExportSettings App::defaultExportSettings() const {
    exp::ExportSettings s = exp::applyPreset(settings_.exporting.defaultPreset, exp::ExportSettings{});
    s.preferHardware = settings_.exporting.preferHardwareEncoder;
    s.verify = settings_.exporting.verifyAfterExport;
    fs::path dir;
    if (!settings_.exporting.defaultDir.empty()) dir = pathFromUtf8(settings_.exporting.defaultDir);
    else if (!projectPath_.empty()) dir = pathFromUtf8(projectPath_).parent_path();
    else dir = userDocumentsDir();
    const std::string base = sanitizeFileName(project().name);
    const std::string ext = exp::defaultExtension(s.container);
    fs::path out = dir / pathFromUtf8(base + ext);
    std::error_code ec;
    for (int i = 2; fs::exists(out, ec) && i < 1000; ++i) out = dir / pathFromUtf8(base + " (" + std::to_string(i) + ")" + ext);
    s.outputPath = pathToUtf8(out);
    return s;
}

std::shared_ptr<exp::ExportJob> App::queueExport(exp::ExportSettings s) {
    if (!sequence()) return nullptr;
    if (!s.range) {
        if (sequence()->workArea) s.range = sequence()->workArea;
    }
    exp::DeviceFactory factory = headless_ ? exp::DeviceFactory([] { return gpu::createCpuDevice(); }) : exp::defaultDeviceFactory();
    auto job = std::make_shared<exp::ExportJob>(doc_->snapshot(), sequenceId(), s, factory);
    exports_->add(job);
    ui_.showExport = true;
    notify(LogLevel::Info, std::string(tr("Export queued")) + ": " + pathToUtf8(pathFromUtf8(s.outputPath).filename()));
    return job;
}

}  // namespace avc::ui
