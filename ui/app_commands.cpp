// Default action table. Ids are stable (settings store overrides by id).
// Shortcut defaults per keymap preset: "avicap" (default), "premiere", "resolve".

#include <imgui.h>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "ui/app.h"

namespace avc::ui {

void App::registerCommands() {
    CommandRegistry& r = commands_;
    auto add = [&](std::string id, const char* label, std::string category, std::map<std::string, std::string> keys,
                   std::function<void()> fn, std::function<bool()> enabled = {}, bool repeat = false) {
        Action a;
        a.id = std::move(id);
        a.label = label;
        a.category = std::move(category);
        a.defaults = std::move(keys);
        a.run = std::move(fn);
        a.enabled = std::move(enabled);
        a.repeat = repeat;
        r.add(std::move(a));
    };
    auto hasSel = [this] { return !selection.clips.empty(); };
    auto hasSeq = [this] { return sequence() != nullptr; };

    // ---------------------------------------------------------------- File
    add("file.new", "New Project", "File", {{"avicap", "Ctrl+N"}}, [this] { guardUnsaved([this] { ui_.showNewProject = true; }); });
    add("file.open", "Open Project...", "File", {{"avicap", "Ctrl+O"}}, [this] { openProjectDialog(); });
    add("file.save", "Save", "File", {{"avicap", "Ctrl+S"}}, [this] { saveProject(); }, [this] { return !busySaving(); });
    add("file.saveAs", "Save As...", "File", {{"avicap", "Ctrl+Shift+S"}}, [this] { saveProjectAsDialog(); },
        [this] { return !busySaving(); });
    add("file.import", "Import Media...", "File", {{"avicap", "Ctrl+I"}}, [this] { importDialog(); });
    add("file.importFolder", "Import Folder...", "File", {}, [this] { importFolderDialog(); });
    add("file.export", "Export...", "File", {{"avicap", "Ctrl+M"}, {"resolve", "Ctrl+Shift+E"}}, [this] { ui_.showExport = true; });
    add("file.exit", "Exit", "File", {{"avicap", "Alt+F4"}}, [this] { requestExit(); });

    // ---------------------------------------------------------------- Edit
    add("edit.undo", "Undo", "Edit", {{"avicap", "Ctrl+Z"}}, [this] { undo(); }, [this] { return doc_->canUndo(); }, true);
    add("edit.redo", "Redo", "Edit", {{"avicap", "Ctrl+Shift+Z"}}, [this] { redo(); }, [this] { return doc_->canRedo(); }, true);
    add("edit.redoAlt", "Redo (Alternate)", "Edit", {{"avicap", "Ctrl+Y"}}, [this] { redo(); }, [this] { return doc_->canRedo(); }, true);
    add("edit.cut", "Cut", "Edit", {{"avicap", "Ctrl+X"}}, [this] { cutSelection(); }, hasSel);
    add("edit.copy", "Copy", "Edit", {{"avicap", "Ctrl+C"}}, [this] { copySelection(); }, hasSel);
    add("edit.paste", "Paste", "Edit", {{"avicap", "Ctrl+V"}}, [this] { paste(false); }, [this] { return canPaste(); });
    add("edit.pasteInsert", "Paste Insert", "Edit", {{"avicap", "Ctrl+Shift+V"}}, [this] { paste(true); },
        [this] { return canPaste(); });
    add("edit.duplicate", "Duplicate", "Edit", {{"avicap", "Ctrl+D"}}, [this] { duplicateSelection(); }, hasSel);
    add("edit.delete", "Delete", "Edit", {{"avicap", "Delete"}, {"resolve", "Backspace"}}, [this] { deleteSelection(false); },
        [this] { return hasClipSelection() || selection.marker != kInvalidId; });
    add("edit.rippleDelete", "Ripple Delete", "Edit", {{"avicap", "Shift+Delete"}, {"resolve", "Delete"}},
        [this] { deleteSelection(true); }, hasSel);
    add("edit.selectAll", "Select All", "Edit", {{"avicap", "Ctrl+A"}}, [this] { selectAll(); }, hasSeq);
    add("edit.deselect", "Deselect All", "Edit", {{"avicap", "Ctrl+Shift+A"}}, [this] { selection.clear(); });
    add("edit.preferences", "Preferences...", "Edit", {{"avicap", "Ctrl+,"}}, [this] { ui_.showSettings = true; });
    add("edit.shortcuts", "Keyboard Shortcuts...", "Edit", {{"avicap", "Ctrl+Alt+K"}}, [this] {
        ui_.showSettings = true;
        ui_.settingsTab = "Keyboard";
    });

    // ---------------------------------------------------------------- Tools
    auto tool = [this](Tool t) { return [this, t] { this->tool = t; }; };
    add("tool.select", "Selection Tool", "Tools", {{"avicap", "V"}, {"resolve", "A"}}, tool(Tool::Select));
    add("tool.razor", "Razor Tool", "Tools", {{"avicap", "C"}, {"resolve", "B"}}, tool(Tool::Razor));
    add("tool.ripple", "Ripple Edit Tool", "Tools", {{"avicap", "B"}, {"resolve", "T"}}, tool(Tool::Ripple));
    add("tool.roll", "Rolling Edit Tool", "Tools", {{"avicap", "N"}, {"resolve", "Shift+T"}}, tool(Tool::Roll));
    add("tool.slip", "Slip Tool", "Tools", {{"avicap", "Y"}}, tool(Tool::Slip));
    add("tool.slide", "Slide Tool", "Tools", {{"avicap", "U"}}, tool(Tool::Slide));

    // ---------------------------------------------------------------- Timeline
    add("timeline.split", "Split at Playhead", "Timeline", {{"avicap", "Ctrl+K"}, {"resolve", "Ctrl+B"}},
        [this] { splitAtPlayhead(false); }, hasSeq);
    add("timeline.splitAll", "Split All Tracks at Playhead", "Timeline", {{"avicap", "Ctrl+Shift+K"}, {"resolve", "Ctrl+Shift+B"}},
        [this] { splitAtPlayhead(true); }, hasSeq);
    add("timeline.snapping", "Toggle Snapping", "Timeline", {{"avicap", "S"}, {"resolve", "N"}}, [this] {
        snapping = !snapping;
        settings_.ui.snapping = snapping;
    });
    add("timeline.linked", "Toggle Linked Selection", "Timeline", {{"avicap", "Ctrl+Alt+L"}, {"resolve", "Ctrl+Shift+L"}},
        [this] { linkedSelection = !linkedSelection; });
    add("timeline.zoomIn", "Zoom In", "Timeline", {{"avicap", "="}}, [this] { ++ui_.zoomSteps; }, {}, true);
    add("timeline.zoomOut", "Zoom Out", "Timeline", {{"avicap", "-"}}, [this] { --ui_.zoomSteps; }, {}, true);
    add("timeline.zoomFit", "Zoom to Fit", "Timeline", {{"avicap", "Shift+Z"}, {"premiere", "\\"}}, [this] { ui_.zoomToFit = true; });
    add("timeline.marker", "Add Marker", "Timeline", {{"avicap", "M"}}, [this] { addMarker(); }, hasSeq);
    add("timeline.markIn", "Mark In", "Timeline", {{"avicap", "I"}}, [this] { setInPoint(); }, hasSeq);
    add("timeline.markOut", "Mark Out", "Timeline", {{"avicap", "O"}}, [this] { setOutPoint(); }, hasSeq);
    add("timeline.clearInOut", "Clear In/Out", "Timeline", {{"avicap", "Alt+X"}}, [this] { clearInOut(); }, hasSeq);
    add("timeline.addTitle", "Add Title", "Timeline", {{"avicap", "Ctrl+T"}}, [this] { addTextClip(tr("Title")); }, hasSeq);
    add("timeline.addSubtitle", "Add Subtitle", "Timeline", {{"avicap", "Ctrl+Shift+T"}},
        [this] { addSubtitleClip(tr("Subtitle"), snapToFrame(playhead()), Time::fromSeconds(3.0)); }, hasSeq);
    add("timeline.addAdjustment", "Add Adjustment Layer", "Timeline", {}, [this] { addAdjustmentLayer(); }, hasSeq);
    add("timeline.addSolid", "Add Color Matte", "Timeline", {}, [this] { addSolidClip(pv(0.1f, 0.1f, 0.1f, 1)); }, hasSeq);
    add("timeline.enable", "Enable/Disable Clip", "Timeline", {{"avicap", "Shift+E"}, {"resolve", "D"}},
        [this] { toggleSelectionEnabled(); }, hasSel);
    add("timeline.group", "Group", "Timeline", {{"avicap", "Ctrl+G"}}, [this] { groupSelection(true); }, hasSel);
    add("timeline.ungroup", "Ungroup", "Timeline", {{"avicap", "Ctrl+Shift+G"}}, [this] { groupSelection(false); }, hasSel);
    add("timeline.link", "Link", "Timeline", {{"avicap", "Ctrl+L"}}, [this] { linkSelection(true); }, hasSel);
    add("timeline.unlink", "Unlink", "Timeline", {{"avicap", "Ctrl+Shift+L"}, {"resolve", "Ctrl+Alt+L"}},
        [this] { linkSelection(false); }, hasSel);
    add("timeline.speed", "Speed/Duration...", "Timeline", {{"avicap", "Ctrl+R"}}, [this] { ui_.showSpeedDialog = true; }, hasSel);
    add("timeline.reverse", "Reverse", "Timeline", {}, [this] { reverseSelection(); }, hasSel);
    add("timeline.freeze", "Freeze Frame", "Timeline", {{"avicap", "Shift+F"}}, [this] { freezeFrameAtPlayhead(); }, hasSel);
    add("timeline.compound", "Create Compound Clip", "Timeline", {{"avicap", "Alt+C"}}, [this] { createCompoundFromSelection(); },
        hasSel);
    add("timeline.nudgeLeft", "Nudge Left", "Timeline", {{"avicap", "Alt+Left"}, {"premiere", "Alt+Left"}},
        [this] { nudgeSelection(-1); }, hasSel, true);
    add("timeline.nudgeRight", "Nudge Right", "Timeline", {{"avicap", "Alt+Right"}}, [this] { nudgeSelection(1); }, hasSel, true);
    add("timeline.closeGap", "Close Gap", "Timeline", {}, [this] { closeGapAtPlayhead(); }, hasSeq);
    add("timeline.addVideoTrack", "Add Video Track", "Timeline", {}, [this] { addTrack(TrackKind::Video); }, hasSeq);
    add("timeline.addAudioTrack", "Add Audio Track", "Timeline", {}, [this] { addTrack(TrackKind::Audio); }, hasSeq);
    add("timeline.crossDissolve", "Add Cross Dissolve", "Timeline", {{"avicap", "Ctrl+Shift+D"}, {"resolve", "Ctrl+T"}},
        [this] { applyTransition("cross-dissolve", Time::fromSeconds(1.0)); }, hasSel);

    // ---------------------------------------------------------------- Playback
    add("playback.toggle", "Play/Pause", "Playback", {{"avicap", "Space"}}, [this] { togglePlay(); }, hasSeq);
    add("playback.forward", "Play Forward (Shuttle)", "Playback", {{"avicap", "L"}}, [this] {
        if (ImGui::IsKeyDown(ImGuiKey_K)) stepFrames(1);
        else shuttle(1);
    }, hasSeq, true);
    add("playback.backward", "Play Backward (Shuttle)", "Playback", {{"avicap", "J"}}, [this] {
        if (ImGui::IsKeyDown(ImGuiKey_K)) stepFrames(-1);
        else shuttle(-1);
    }, hasSeq, true);
    add("playback.stop", "Stop", "Playback", {{"avicap", "K"}}, [this] { shuttle(0); }, hasSeq);
    add("playback.prevFrame", "Previous Frame", "Playback", {{"avicap", "Left"}}, [this] { stepFrames(-1); }, hasSeq, true);
    add("playback.nextFrame", "Next Frame", "Playback", {{"avicap", "Right"}}, [this] { stepFrames(1); }, hasSeq, true);
    add("playback.back10", "Back 10 Frames", "Playback", {{"avicap", "Shift+Left"}}, [this] { stepFrames(-10); }, hasSeq, true);
    add("playback.forward10", "Forward 10 Frames", "Playback", {{"avicap", "Shift+Right"}}, [this] { stepFrames(10); }, hasSeq, true);
    add("playback.start", "Go to Start", "Playback", {{"avicap", "Home"}}, [this] { goToStart(); }, hasSeq);
    add("playback.end", "Go to End", "Playback", {{"avicap", "End"}}, [this] { goToEnd(); }, hasSeq);
    add("playback.prevEdit", "Previous Edit Point", "Playback", {{"avicap", "Up"}}, [this] { goToEdit(false); }, hasSeq, true);
    add("playback.nextEdit", "Next Edit Point", "Playback", {{"avicap", "Down"}}, [this] { goToEdit(true); }, hasSeq, true);
    add("playback.loop", "Loop Playback", "Playback", {{"avicap", "Ctrl+Shift+Space"}}, [this] { setLoop(!loopPlayback); });
    add("playback.fullscreen", "Full Screen Viewer", "Playback", {{"avicap", "Ctrl+F"}, {"premiere", "Ctrl+`"}},
        [this] { ui_.fullscreenViewer = !ui_.fullscreenViewer; });

    // ---------------------------------------------------------------- View
    auto toggle = [](bool& b) { return [&b] { b = !b; }; };
    add("view.media", "Media", "View", {{"avicap", "Shift+1"}}, toggle(ui_.showMedia));
    add("view.viewer", "Viewer", "View", {{"avicap", "Shift+2"}}, toggle(ui_.showViewer));
    add("view.timeline", "Timeline", "View", {{"avicap", "Shift+3"}}, toggle(ui_.showTimeline));
    add("view.inspector", "Inspector", "View", {{"avicap", "Shift+4"}}, toggle(ui_.showInspector));
    add("view.effects", "Effects", "View", {{"avicap", "Shift+5"}}, toggle(ui_.showEffects));
    add("view.color", "Color", "View", {{"avicap", "Shift+6"}}, toggle(ui_.showColor));
    add("view.mixer", "Audio Mixer", "View", {{"avicap", "Shift+7"}}, toggle(ui_.showMixer));
    add("view.scopes", "Scopes", "View", {{"avicap", "Shift+8"}}, toggle(ui_.showScopes));
    add("view.export", "Export Queue", "View", {{"avicap", "Shift+9"}}, toggle(ui_.showExport));
    add("view.sounds", "Sounds", "View", {}, toggle(ui_.showSounds));
    add("view.ai", "AI Tools", "View", {}, toggle(ui_.showAi));
    add("view.markers", "Markers", "View", {}, toggle(ui_.showMarkers));
    add("view.history", "History", "View", {}, toggle(ui_.showHistory));
    add("view.diagnostics", "Diagnostics", "View", {{"avicap", "Ctrl+Shift+F12"}}, toggle(ui_.showDiagnostics));
    add("view.resetLayout", "Reset Layout", "View", {}, [this] { ui_.resetLayout = true; });
    add("view.commandPalette", "Command Palette", "View", {{"avicap", "Ctrl+Shift+P"}}, [this] { ui_.showCommandPalette = true; });

    // ---------------------------------------------------------------- Help
    add("help.about", "About AviCap Studio", "Help", {}, [this] { ui_.showAbout = true; });
    add("help.logs", "Open Logs Folder", "Help", {}, [] { openWithShell(pathToUtf8(logsDir())); });
    add("help.dataFolder", "Open Data Folder", "Help", {}, [this] { openWithShell(pathToUtf8(dataDir_)); });
}

}  // namespace avc::ui
