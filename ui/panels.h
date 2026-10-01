#pragma once
// Editor panels. Each panel keeps only view state (scroll, zoom, filters);
// all document changes go through App so they are undoable and journaled.

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "export/export_settings.h"
#include "timeline/snap.h"
#include "ui/app.h"

namespace avc::ui {

// Drag & drop payload ids shared between panels.
inline constexpr const char* kPayloadMedia = "AVC_MEDIA";            // MediaId
inline constexpr const char* kPayloadEffect = "AVC_EFFECT";          // char[] effect id
inline constexpr const char* kPayloadTransition = "AVC_TRANSITION";  // char[] transition id
inline constexpr const char* kPayloadSoundFile = "AVC_SOUND";        // char[]: 'b'|'s' (bgm/sfx) + UTF-8 path

class TimelinePanel {
public:
    void draw(App& app);

    struct Lane {
        int track = -1;
        float y = 0;  // screen y of the lane top
        float h = 0;
        bool audio = false;
    };
    // Where a drop at `screen` would land (external file drops).
    struct DropTarget {
        Time time;
        int videoTrack = -1;
        int audioTrack = -1;
    };
    std::optional<DropTarget> dropTargetAt(const App& app, ImVec2 screen) const;

    // View state (also used by tests to synthesize input).
    [[nodiscard]] float timeToX(Time t) const;
    [[nodiscard]] Time xToTime(float x) const;
    [[nodiscard]] const std::vector<Lane>& lanes() const { return lanes_; }
    [[nodiscard]] const Lane* laneForTrack(int track) const;
    [[nodiscard]] double pixelsPerSecond() const { return pps_; }
    void setZoom(double pps, double scrollSeconds) {
        pps_ = pps;
        scroll_ = scrollSeconds;
        fitPending_ = false;
    }
    [[nodiscard]] ImVec2 areaMin() const { return areaMin_; }
    [[nodiscard]] ImVec2 areaMax() const { return areaMax_; }

private:
    enum class DragKind { None, Move, TrimHead, TrimTail, Roll, Slip, Slide, Marquee, Marker, TrackHeight };
    struct Drag {
        DragKind kind = DragKind::None;
        ImVec2 startMouse{};
        Time startTime;
        ClipId clip = kInvalidId;
        ClipId other = kInvalidId;  // roll partner
        int lane = -1;
        std::set<ClipId> clips;
        TimeRange range;
        std::vector<SnapPoint> snaps;
        bool moved = false;
        bool ripple = false;
        bool wasSelected = false;
        MarkerId marker = kInvalidId;
        Time markerOrig;
        int track = -1;
        float trackHeight = 0;
        float newHeight = 0;
        std::set<ClipId> marqueeBase;
    };

    void drawToolbar(App& app);
    void layoutLanes(const Sequence& seq, float top, float width);
    void drawRuler(App& app, const Sequence& seq, ImDrawList* dl, ImVec2 min, ImVec2 max);
    void drawHeaders(App& app, const Sequence& seq, ImDrawList* dl);
    void drawClips(App& app, const Sequence& seq, ImDrawList* dl);
    void drawClip(App& app, const Sequence& seq, const Track& track, const Clip& c, const Lane& lane, ImDrawList* dl);
    void handleLaneInput(App& app, const Sequence& seq);
    void handleDrops(App& app, const Sequence& seq);
    void updateDrag(App& app, const Sequence& seq);
    void finishDrag(App& app);
    void clipContextMenu(App& app);
    void emptyContextMenu(App& app);
    void applyZoomRequests(App& app, const Sequence& seq);
    int laneAt(float y) const;
    struct Hit {
        const Clip* clip = nullptr;
        int lane = -1;
        int track = -1;
        enum Zone { None, Body, Head, Tail } zone = None;
    };
    Hit hitTest(const Sequence& seq, ImVec2 p) const;
    Time snapThreshold() const;

    double pps_ = 60.0;   // pixels per second
    double scroll_ = 0.0;  // seconds at the left edge
    float scrollY_ = 0.0f;
    float contentH_ = 0.0f;
    float headerW_ = 168.0f;
    float rulerH_ = 30.0f;
    ImVec2 areaMin_{}, areaMax_{};
    std::vector<Lane> lanes_;
    bool fitPending_ = true;
    Drag drag_;
    ProjectPtr preview_;        // live result of the current drag / drop
    std::string previewError_;
    std::function<Status(SequenceEditor&)> pendingOp_;
    std::string pendingLabel_;
    std::optional<Time> snapLine_;
    std::optional<Time> razorLine_;
    ClipId contextClip_ = kInvalidId;
    int contextTrack_ = -1;
    Time contextTime_;
    char renameBuf_[128] = {};
    int renameTrack_ = -1;
    MarkerId editMarker_ = kInvalidId;
    char markerName_[128] = {};
    char markerComment_[256] = {};
    ClipId dropHighlight_ = kInvalidId;
};

class MediaPanel {
public:
    void draw(App& app);

private:
    char search_[128] = {};
    bool grid_ = true;
    float thumbSize_ = 1.0f;
    int sort_ = 0;  // 0 name, 1 date added, 2 duration, 3 type
    int filter_ = 0;  // 0 all, 1 video, 2 audio, 3 image
    MediaId propertiesFor_ = kInvalidId;
};

class ViewerPanel {
public:
    void draw(App& app);
    bool drawImage(App& app, ImVec2 size, bool fullscreen);  // shared with the full-screen viewer

private:
    void drawTransport(App& app);
    void drawWelcome(App& app, ImVec2 size);
    void drawOverlays(App& app, ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax);
    int zoom_ = 0;  // 0 fit, otherwise percent
    bool safeAreas_ = false;
    bool showStats_ = false;
    // Transform handle drag
    bool dragging_ = false;
    ImVec2 dragStart_{};
    ParamValue posStart_{};
    ClipId dragClip_ = kInvalidId;
};

class InspectorPanel {
public:
    void draw(App& app);

private:
    void drawClip(App& app, const Sequence& seq, const Clip& clip);
    void drawParamSet(App& app, const Clip& clip, const std::vector<ParamDef>& defs, int which, const char* groupFilter);
    void drawEffects(App& app, const Sequence& seq, const Clip& clip, bool audio);
    void drawTextSection(App& app, const Clip& clip);
    void drawSequence(App& app, const Sequence& seq);
    std::string textBuf_;
    ClipId textFor_ = kInvalidId;
};

class EffectsPanel {
public:
    void draw(App& app);

private:
    char search_[96] = {};
};

class ColorPanel {
public:
    void draw(App& app);

private:
    bool colorWheel(const char* id, ParamValue& v, float radius);
    bool curveEditor(const char* id, std::string& points, ImU32 color, float size, bool centered);
    int curveChannel_ = 0;
    int dragPoint_ = -1;
};

class ExportPanel {
public:
    void draw(App& app);

private:
    void drawSettings(App& app);
    void drawQueue(App& app);
    exp::ExportSettings settings_;
    bool initialized_ = false;
    std::string projectPathAtInit_;
    char path_[1024] = {};
    int rangeMode_ = 0;  // 0 whole/work area, 1 in-out only, 2 custom
};

class MixerPanel {
public:
    void draw(App& app);
};

class ScopesPanel {
public:
    void draw(App& app);

private:
    int mode_ = 0;  // 0 waveform, 1 parade, 2 vectorscope, 3 histogram
    std::shared_ptr<const cache::Image> source_;
    std::shared_ptr<cache::Image> rendered_;
};

class HistoryPanel {
public:
    void draw(App& app);
};

class MarkersPanel {
public:
    void draw(App& app);
};

class DiagnosticsPanel {
public:
    void draw(App& app);

private:
    int minLevel_ = 2;  // Info
    char filter_[96] = {};
    bool autoScroll_ = true;
};

class AiPanel {
public:
    void draw(App& app);

private:
    void drawPlan(App& app);
    void drawProvider(App& app);
    void drawSilence(App& app);
    void drawAnalysis(App& app);
    char prompt_[2048] = {};
    char keyBuf_[256] = {};
    bool rememberKey_ = true;
    float sensitivity_ = 0.5f;
};

class SoundsPanel {
public:
    void draw(App& app);

private:
    void drawLibrary(App& app);
    void drawWebSounds(App& app);
    void soundRow(App& app, const SoundItem& s, int index);
    bool scanned_ = false;
    char search_[128] = {};
    int filter_ = 0;  // 0 all, 1 bgm, 2 sfx, 3 favourites, 4 recent
    char webSearch_[128] = {};
    char pageUrl_[512] = {};
    std::vector<SoundItem> downloads_;
    double lastDownloadScan_ = -100;
    std::string importMessage_;
};

class SettingsWindow {
public:
    void draw(App& app);

private:
    void drawShortcuts(App& app);
    AppSettings working_;
    bool loaded_ = false;
    std::string capturing_;  // action id waiting for a key chord
    char search_[96] = {};
    std::vector<audio::AudioDeviceInfo> devices_;
};

}  // namespace avc::ui
