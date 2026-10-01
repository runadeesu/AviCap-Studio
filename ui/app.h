#pragma once
// Application controller. Owns the document and all engines (preview,
// playback, caches, proxies, exports, autosave) and implements the operations
// the panels and commands invoke. It contains no drawing code, so the whole
// editing workflow can be driven and tested headless.
//
// Threading: everything here is called on the UI thread. Slow work (probing,
// loading, saving, thumbnails, waveforms, proxies, exports, analysis) runs on
// worker threads and reports back through post(), which tick() drains.

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "audio/playback.h"
#include "core/log.h"
#include "core/settings.h"
#include "export/exporter.h"
#include "project/journal.h"
#include "proxy/proxy.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"
#include "ui/ai_service.h"
#include "ui/assets.h"
#include "ui/commands.h"
#include "ui/preview.h"

namespace avc::ui {

struct PostQueue;
std::string fpsText(Rational r);  // "29.97", "30"

enum class Tool { Select, Razor, Ripple, Roll, Slip, Slide };
const char* toolLabel(Tool t);

struct Notification {
    uint64_t id = 0;
    LogLevel level = LogLevel::Info;
    std::string text;
    double createdSec = 0;
    std::string actionLabel;
    std::function<void()> action;
};

struct Selection {
    std::set<ClipId> clips;
    std::set<MediaId> media;
    MarkerId marker = kInvalidId;
    void clear() {
        clips.clear();
        media.clear();
        marker = kInvalidId;
    }
};

struct FileFilter {
    std::string name;      // "Video files"
    std::string patterns;  // "*.mp4;*.mov"
};

// Native file dialogs (Windows) or scripted answers (tests / headless).
class IDialogs {
public:
    virtual ~IDialogs() = default;
    virtual std::vector<std::string> openFiles(const std::string& title, const std::vector<FileFilter>& filters, bool multiple) = 0;
    virtual std::optional<std::string> saveFile(const std::string& title, const std::vector<FileFilter>& filters,
                                                const std::string& defaultName, const std::string& defaultExt) = 0;
    virtual std::optional<std::string> pickFolder(const std::string& title) = 0;
};

// Requests from commands to the window layer (dialogs, panels, view changes).
struct UiState {
    bool showMedia = true, showViewer = true, showTimeline = true, showInspector = true, showEffects = true;
    bool showExport = false, showMixer = true, showScopes = false, showDiagnostics = false, showHistory = false;
    bool showColor = true, showAudioFx = false, showSounds = true, showAi = true, showMarkers = false;
    bool showSettings = false, showAbout = false, showNewProject = false, showCommandPalette = false;
    bool showSpeedDialog = false, showShortcuts = false, showRecovery = false, showRelink = false;
    bool resetLayout = false;
    bool capturingShortcut = false;  // shortcut editor is waiting for a key chord
    int zoomSteps = 0;         // timeline zoom in (+) / out (-) requests
    bool zoomToFit = false;
    bool fullscreenViewer = false;
    std::function<void()> afterUnsavedCheck;  // pending action guarded by "save changes?"
    bool unsavedPrompt = false;
    bool exitConfirmed = false;
    std::string settingsTab;
    std::string workspace = "edit";  // edit | color | audio | export
    std::string applyWorkspace;      // requested layout change (rebuilt by the main window)
    bool showShortcutSheet = false;
};

struct AppOptions {
    std::filesystem::path dataDir;          // settings / cache / autosave root (empty = platform default)
    bool headless = false;                  // CPU device and silent audio (tests, automation)
    std::unique_ptr<gpu::Device> device;    // shared UI + preview device (created when null)
    std::string workerExecutable;           // avicap-cli used for proxy generation
    std::unique_ptr<IDialogs> dialogs;      // null = headless dialogs (return nothing)
    bool loadSettings = true;
};

class App {
public:
    explicit App(AppOptions opt);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ---------------------------------------------------------------- loop
    void tick();  // once per UI frame
    void setWakeCallback(std::function<void()> fn);
    void wake();
    void post(std::function<void()> fn);  // any thread -> UI thread
    [[nodiscard]] bool animating() const;  // something on screen changes without input
    [[nodiscard]] double timeSec() const;  // monotonic seconds since start

    // ---------------------------------------------------------------- state
    Document& doc() { return *doc_; }
    [[nodiscard]] const Project& project() const { return doc_->project(); }
    [[nodiscard]] const Sequence* sequence() const { return doc_->project().active(); }
    [[nodiscard]] SequenceId sequenceId() const { return doc_->project().activeSequence; }
    AppSettings& settings() { return settings_; }
    void applySettings();
    void saveSettings();
    gpu::Device& device() { return *device_; }
    PreviewRenderer& preview() { return *preview_; }
    audio::PlaybackEngine& playback() { return *playback_; }
    AssetService& assets() { return *assets_; }
    proxy::ProxyManager& proxies() { return *proxies_; }
    exp::ExportQueue& exports() { return *exports_; }
    CommandRegistry& commands() { return commands_; }
    AutosaveManager& autosave() { return *autosave_; }
    IDialogs& dialogs() { return *dialogs_; }
    UiState& ui() { return ui_; }
    AiService& ai() { return *ai_; }
    [[nodiscard]] const std::string& projectPath() const { return projectPath_; }

    // AI plan preview: shown instead of the document in the viewer, timeline
    // and playback until applied or discarded (any document edit discards it).
    void setPlanPreview(ProjectPtr p);
    [[nodiscard]] ProjectPtr planPreview() const;
    [[nodiscard]] std::string windowTitle() const;
    [[nodiscard]] const std::filesystem::path& dataDir() const { return dataDir_; }
    [[nodiscard]] bool headless() const { return headless_; }

    Selection selection;
    Tool tool = Tool::Select;
    bool linkedSelection = true;
    bool snapping = true;
    int targetVideoTrack = 0;   // family position (V1 = 0) used by insert / paste
    int targetAudioTrack = 0;   // family position (A1 = 0)
    PreviewQuality previewQuality = PreviewQuality::Auto;
    bool loopPlayback = false;
    bool scopesVisible = false;
    int viewerWidth = 0, viewerHeight = 0;  // set by the viewer panel

    // ---------------------------------------------------------------- project
    void newProject(const std::string& name, const ProjectSettings& s);
    void openProject(const std::string& path);
    void saveProject(std::function<void(bool)> done = {});  // falls back to Save As when unsaved
    void saveProjectAs(const std::string& path, std::function<void(bool)> done = {});
    [[nodiscard]] bool busySaving() const { return saving_ > 0; }
    [[nodiscard]] bool busyLoading() const { return loading_; }
    // Runs `fn` now, or after the user answered the "save changes?" prompt.
    void guardUnsaved(std::function<void()> fn);
    void requestExit();
    void prepareExit();  // stops playback/exports and closes autosave cleanly
    std::vector<RecoveryCandidate>& recoveryCandidates() { return recovery_; }
    void recoverProject(const RecoveryCandidate& c);
    void discardRecovery(const RecoveryCandidate& c);
    // Dialog-driven helpers used by commands and menus.
    void openProjectDialog();
    void saveProjectAsDialog(std::function<void(bool)> done = {});
    void importDialog();
    void importFolderDialog();
    // Imports audio files and places them at the playhead on a free audio
    // track: music goes to A2 and below, sound effects to A3 and below.
    void addAudioDialog(bool music);
    void setWorkspace(const std::string& name);

    // ---------------------------------------------------------------- media
    void importFiles(std::vector<std::string> paths, std::function<void(const std::vector<MediaId>&)> done = {});
    [[nodiscard]] int importsInProgress() const { return importing_; }
    void removeMedia(const std::set<MediaId>& ids);
    [[nodiscard]] bool mediaOffline(MediaId id) const;
    [[nodiscard]] size_t offlineCount() const;
    void relinkMedia(MediaId id, const std::string& newPath);
    void relinkSearchFolder(const std::string& folder);
    void createProxies(const std::set<MediaId>& ids);
    void refreshMediaStatus();
    Result<std::vector<ClipId>> addMediaToTimeline(MediaId id, Time at, int videoTrackIndex, int audioTrackIndex, bool insert);
    // Inserts at the playhead on the target tracks and moves the playhead after it.
    void appendMediaAtPlayhead(MediaId id);
    // Places audio-only media on the first free audio track at or below the
    // given family position (a new track is added when all are busy).
    Result<ClipId> placeAudio(MediaId id, Time at, int firstAudioTrack);
    // Absolute track indices for the targeted family positions (-1 if absent).
    [[nodiscard]] int targetVideoTrackIndex() const;
    [[nodiscard]] int targetAudioTrackIndex() const;

    // ---------------------------------------------------------------- transport
    [[nodiscard]] Time playhead() const;
    void seek(Time t);
    void scrub(Time t);
    [[nodiscard]] bool playing() const;
    [[nodiscard]] double playSpeed() const;
    void togglePlay();
    void shuttle(int direction);  // J (-1), K (0), L (+1)
    void stepFrames(int n);
    void goToStart();
    void goToEnd();
    void goToEdit(bool next);
    void setLoop(bool on);
    [[nodiscard]] Time frameDuration() const;
    [[nodiscard]] Time snapToFrame(Time t) const;

    // ---------------------------------------------------------------- editing
    Status editSequence(const std::string& label, const std::function<Status(SequenceEditor&)>& fn, const EditOptions& opt = {});
    Status editProject(const std::string& label, const std::function<Status(ProjectEditor&)>& fn, const EditOptions& opt = {});
    void undo();
    void redo();
    void splitAtPlayhead(bool allTracks);
    void deleteSelection(bool ripple);
    void copySelection();
    void cutSelection();
    void paste(bool insert);
    [[nodiscard]] bool canPaste() const { return !clipboard_.empty(); }
    void duplicateSelection();
    void selectAll();
    void selectClip(ClipId id, bool additive, bool toggle);
    void setSelectionEnabled(bool enabled);
    void toggleSelectionEnabled();
    void groupSelection(bool group);
    void linkSelection(bool link);
    void nudgeSelection(int frames);
    void setInPoint();
    void setOutPoint();
    void clearInOut();
    MarkerId addMarker(MarkerKind kind = MarkerKind::Standard, std::string name = {});
    ClipId addTextClip(const std::string& text = "Title");
    ClipId addSubtitleClip(const std::string& text, Time start, Time duration);
    ClipId addAdjustmentLayer();
    ClipId addSolidClip(ParamValue color);
    void applyEffect(const std::string& effectId);
    void applyEffectToClip(ClipId id, const std::string& effectId);
    void applyTransition(const std::string& type, Time duration);
    void setSelectionSpeed(Rational speed, bool ripple);
    void reverseSelection();
    void freezeFrameAtPlayhead();
    void createCompoundFromSelection();
    void addTrack(TrackKind kind);
    void closeGapAtPlayhead();
    [[nodiscard]] const Clip* primaryClip() const;  // first selected clip
    [[nodiscard]] bool hasClipSelection() const { return !selection.clips.empty(); }
    void pruneSelection();

    // ---------------------------------------------------------------- export
    [[nodiscard]] exp::ExportSettings defaultExportSettings() const;
    std::shared_ptr<exp::ExportJob> queueExport(exp::ExportSettings s);

    // ---------------------------------------------------------------- notifications
    void notify(LogLevel level, std::string text, std::string actionLabel = {}, std::function<void()> action = {});
    std::deque<Notification>& notifications() { return notes_; }
    [[nodiscard]] std::string statusText() const;

    // ---------------------------------------------------------------- stats
    struct Stats {
        double uiFrameMs = 0;
        double uiFps = 0;
        uint64_t frames = 0;
        bool lowMemory = false;
        bool lowDisk = false;
    } stats;

private:
    void registerCommands();
    static ProjectPtr makeLocalizedProject(const std::string& name, const ProjectSettings& s);
    void attachDocument(ProjectPtr p, const std::string& path, bool clean);
    void updatePreviewRequest();
    void checkResources();
    void onDocumentChanged(const ChangeEvent& ev);
    Time sequenceEnd() const;
    std::string proxyFor(const MediaItem& m);
    int findFreeVisualTrack(TimeRange range, std::optional<TrackKind> preferKind) const;

    bool headless_ = false;
    std::filesystem::path dataDir_;
    AppSettings settings_;
    std::unique_ptr<IDialogs> dialogs_;
    std::unique_ptr<gpu::Device> device_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<AutosaveManager> autosave_;
    std::unique_ptr<cache::CacheStore> cacheStore_;
    std::unique_ptr<AssetService> assets_;
    std::unique_ptr<proxy::ProxyManager> proxies_;
    std::unique_ptr<PreviewRenderer> preview_;
    std::unique_ptr<audio::PlaybackEngine> playback_;
    std::unique_ptr<exp::ExportQueue> exports_;
    CommandRegistry commands_;
    UiState ui_;
    std::unique_ptr<AiService> ai_;
    mutable std::mutex planPreviewMutex_;
    ProjectPtr planPreview_;

    std::shared_ptr<PostQueue> posts_;
    std::string projectPath_;
    Time playhead_;
    bool wasPlaying_ = false;
    PreviewRequest lastRequest_;
    std::vector<RecoveryCandidate> recovery_;
    edit::Clipboard clipboard_;
    int saving_ = 0;
    bool loading_ = false;
    int importing_ = 0;
    int docListener_ = 0;

    std::deque<Notification> notes_;
    uint64_t nextNote_ = 1;

    mutable std::mutex mediaMutex_;
    std::set<MediaId> offline_;
    std::map<MediaId, std::string> proxyPaths_;
    std::set<MediaId> proxyRequested_;

    double lastResourceCheck_ = -100;
    double startSteady_ = 0;
    double lastTick_ = 0;
};

}  // namespace avc::ui
