// Preferences window (working copy, applied on OK/Apply) and the keyboard
// shortcut editor with conflict detection and presets.

#include <algorithm>
#include <cstring>

#include "core/i18n.h"
#include "core/strings.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

float labelWidth() { return std::min(ImGui::GetFontSize() * 15.0f, ImGui::GetContentRegionAvail().x * 0.45f); }

std::string hidden(const char* label) { return std::string("##") + label; }

bool rowSliderInt(const char* label, int* v, int lo, int hi, const char* fmt = "%d") {
    formLabel(label, labelWidth());
    return ImGui::SliderInt(hidden(label).c_str(), v, lo, hi, fmt);
}

bool rowSliderFloat(const char* label, float* v, float lo, float hi, const char* fmt = "%.2f") {
    formLabel(label, labelWidth());
    return ImGui::SliderFloat(hidden(label).c_str(), v, lo, hi, fmt);
}

bool rowInputText(const char* label, char* buf, size_t size) {
    formLabel(label, labelWidth());
    return ImGui::InputText(hidden(label).c_str(), buf, size);
}

bool comboString(const char* label, std::string& value, const std::vector<std::pair<std::string, const char*>>& options) {
    const char* preview = value.c_str();
    for (const auto& [id, name] : options)
        if (id == value) preview = name;
    bool changed = false;
    formLabel(label, labelWidth());
    if (ImGui::BeginCombo(hidden(label).c_str(), preview)) {
        for (const auto& [id, name] : options)
            if (ImGui::Selectable(name, id == value)) {
                value = id;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

bool pathField(App& app, const char* label, std::string& value, bool folder) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    ImGui::PushID(label);
    formLabel(label, labelWidth());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 6);
    bool changed = false;
    if (ImGui::InputText("##path", buf, sizeof buf)) {
        value = buf;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Browse..."))) {
        if (folder) {
            if (auto d = app.dialogs().pickFolder(label)) {
                value = *d;
                changed = true;
            }
        } else {
            auto f = app.dialogs().openFiles(label, {{tr("All files"), "*.*"}}, false);
            if (!f.empty()) {
                value = f.front();
                changed = true;
            }
        }
    }
    ImGui::PopID();
    return changed;
}

}  // namespace

void SettingsWindow::draw(App& app) {
    UiState& ui = app.ui();
    if (!ui.showSettings) {
        loaded_ = false;
        capturing_.clear();
        ui.capturingShortcut = false;
        return;
    }
    if (!loaded_) {
        working_ = app.settings();
        working_.keyboard.overrides = app.commands().saveOverrides();
        working_.keyboard.preset = app.commands().preset();
        devices_ = audio::listAudioOutputs();
        loaded_ = true;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52, ImGui::GetFontSize() * 34), ImGuiCond_FirstUseEver);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool open = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(tr("Preferences"), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    floatingWindowDecor();
    if (!visible) {
        ImGui::End();
        if (!open) ui.showSettings = false;
        return;
    }
    // Sidebar with the categories (Windows 11 settings style), page on the right.
    struct Category {
        const char* id;
        Icon icon;
    };
    static const Category categories[] = {
        {"General", Icon::Gear},       {"Interface", Icon::Palette}, {"Playback", Icon::Play},      {"Performance", Icon::Bolt},
        {"GPU", Icon::Grid},           {"Audio", Icon::Volume},      {"Proxies", Icon::Film},      {"Cache", Icon::Folder},
        {"AI & Privacy", Icon::Sparkle}, {"Export", Icon::Export},   {"Keyboard", Icon::List},
    };
    if (!ui.settingsTab.empty()) {
        page_ = ui.settingsTab;
        ui.settingsTab.clear();
    }
    if (page_.empty()) page_ = "General";
    const float fs = ImGui::GetFontSize();
    const float footer = ImGui::GetFrameHeight() + 24;
    const float navW = fs * 13.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal::kAppBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    ImGui::BeginChild("##nav", ImVec2(navW, -footer), ImGuiChildFlags_AlwaysUseWindowPadding);
    for (const Category& c : categories)
        if (navItem(c.id, c.icon, tr(c.id), page_ == c.id)) page_ = c.id;
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 14));
    ImGui::BeginChild("##settingsbody", ImVec2(0, -footer), ImGuiChildFlags_AlwaysUseWindowPadding);
    headingText(tr(page_.c_str()), 1.25f);
    ImGui::Spacing();
    ImGui::Spacing();
    {
        auto tab = [&](const char* name) { return page_ == name; };
        if (tab("General")) {
            comboString(tr("Language"), working_.general.language, {{"auto", tr("System default")}, {"en", "English"}, {"ja", "日本語"}});
            rowSliderInt(tr("Autosave snapshot interval (s)"), &working_.general.autosaveIntervalSec, 15, 600);
            helpMarker(tr("Every edit is journaled immediately; this controls how often a full snapshot is written."));
            pathField(app, tr("Default project folder"), working_.general.defaultProjectDir, true);
            checkbox(tr("Reopen the last project at startup"), &working_.general.reopenLastProject);
        }
        if (tab("Interface")) {
            rowSliderFloat(tr("UI scale (0 = Windows setting)"), &working_.ui.uiScale, 0.0f, 3.0f, "%.2f");
            comboString(tr("Time display"), working_.ui.timecodeStyle,
                        {{"timecode", tr("Timecode")}, {"frames", tr("Frames")}, {"seconds", tr("Seconds")}});
            checkbox(tr("Show tooltips"), &working_.ui.showTooltips);
            formLabel(tr("Accent color"));
            accentSwatches("##accent", working_.ui.accent);
            checkbox(tr("High contrast theme"), &working_.ui.highContrast);
        }
        if (tab("Playback")) {
            comboString(tr("Preview quality"), working_.playback.previewQuality,
                        {{"auto", tr("Auto")}, {"full", tr("Full")}, {"half", "1/2"}, {"quarter", "1/4"}, {"eighth", "1/8"}});
            checkbox(tr("Adaptive quality during playback"), &working_.playback.adaptiveQuality);
            checkbox(tr("Audio scrubbing"), &working_.playback.audioScrubbing);
            checkbox(tr("Loop playback"), &working_.playback.loop);
            rowSliderInt(tr("Maximum shuttle speed (J/L)"), &working_.playback.shuttleMaxSpeed, 2, 32);
        }
        if (tab("Performance")) {
            checkbox(tr("Hardware video decoding (restart required)"), &working_.performance.hardwareDecode);
            rowSliderInt(tr("Frame cache (MB)"), &working_.performance.frameCacheMB, 128, 16384);
            rowSliderInt(tr("Maximum open decoders"), &working_.performance.maxOpenDecoders, 4, 64);
            rowSliderInt(tr("Decode threads (0 = auto)"), &working_.performance.decodeThreads, 0, 32);
        }
        if (tab("GPU")) {
            comboString(tr("Graphics API (restart required)"), working_.gpu.api,
                        {{"auto", tr("Automatic")}, {"d3d11", "Direct3D 11"}, {"warp", tr("Software (WARP)")}});
            char adapter[128];
            std::snprintf(adapter, sizeof adapter, "%s", working_.gpu.adapter.c_str());
            if (rowInputText(tr("Preferred adapter (name contains)"), adapter, sizeof adapter)) working_.gpu.adapter = adapter;
            ImGui::TextDisabled("%s: %s", tr("Current"), app.device().info().adapter.c_str());
        }
        if (tab("Audio")) {
            std::vector<std::pair<std::string, const char*>> opts{{"", tr("System default")}};
            for (const auto& d : devices_) opts.emplace_back(d.id, d.name.c_str());
            comboString(tr("Output device (restart required)"), working_.audio.outputDevice, opts);
            rowSliderInt(tr("Buffer (ms)"), &working_.audio.bufferMs, 10, 200);
        }
        if (tab("Proxies")) {
            checkbox(tr("Use proxies for playback when available"), &working_.proxy.useProxies);
            comboString(tr("Proxy resolution"), working_.proxy.preset, {{"360p", "360p"}, {"540p", "540p"}, {"720p", "720p"}, {"1080p", "1080p"}});
            rowSliderInt(tr("Create automatically above height (0 = never)"), &working_.proxy.autoCreateAboveHeight, 0, 4320);
            pathField(app, tr("Proxy folder (empty = cache)"), working_.proxy.location, true);
            hintText(tr("Exports always use the original media."));
        }
        if (tab("Cache")) {
            float gb = static_cast<float>(working_.cache.maxGB);
            if (rowSliderFloat(tr("Maximum cache size (GB)"), &gb, 1.0f, 500.0f, "%.0f")) working_.cache.maxGB = gb;
            rowSliderInt(tr("Delete entries older than (days)"), &working_.cache.maxAgeDays, 1, 365);
            checkbox(tr("Clean up automatically at startup"), &working_.cache.autoCleanup);
            pathField(app, tr("Cache folder (restart required)"), working_.cache.location, true);
        }
        if (tab("AI & Privacy")) {
            ImGui::TextWrapped("%s", tr("AviCap Studio works locally. Nothing is uploaded unless you explicitly enable a cloud feature below."));
            checkbox(tr("Allow network access (sound search, cloud AI)"), &working_.privacy.allowNetwork);
            checkbox(tr("Allow sending transcripts to the cloud AI assistant"), &working_.ai.cloudConsent);
            helpMarker(tr("Only the text you type and timeline metadata/transcripts are sent - never video or audio files."));
            comboString(tr("Assistant provider"), working_.ai.assistantProvider, {{"local", tr("Local rules (offline)")}, {"anthropic", "Claude (Anthropic API)"}});
            char model[96];
            std::snprintf(model, sizeof model, "%s", working_.ai.anthropicModel.c_str());
            if (rowInputText(tr("Claude model"), model, sizeof model)) working_.ai.anthropicModel = model;
            pathField(app, tr("Whisper model file (ggml)"), working_.ai.whisperModelPath, false);
            comboString(tr("Caption language"), working_.ai.captionLanguage, {{"auto", tr("Auto detect")}, {"ja", "日本語"}, {"en", "English"}});
            ImGui::Separator();
            checkbox(tr("Write crash dumps (stay on this PC)"), &working_.privacy.crashDumps);
            checkbox(tr("Share anonymous usage statistics (opt-in)"), &working_.privacy.analyticsOptIn);
            helpMarker(tr("No statistics are collected or sent unless this is enabled."));
        }
        if (tab("Export")) {
            std::vector<std::pair<std::string, const char*>> opts;
            for (const auto& p : exp::presets()) opts.emplace_back(p.id, tr(p.name.c_str()));
            comboString(tr("Default preset"), working_.exporting.defaultPreset, opts);
            pathField(app, tr("Default export folder"), working_.exporting.defaultDir, true);
            checkbox(tr("Verify files after export"), &working_.exporting.verifyAfterExport);
            checkbox(tr("Prefer hardware encoders"), &working_.exporting.preferHardwareEncoder);
        }
        if (tab("Keyboard")) {
            drawShortcuts(app);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    auto apply = [&] {
        app.settings() = working_;
        app.commands().setPreset(working_.keyboard.preset);
        app.commands().loadOverrides(working_.keyboard.overrides);
        app.snapping = working_.ui.snapping;
        app.loopPlayback = working_.playback.loop;
        setTooltipsEnabled(working_.ui.showTooltips);
        setTimeStyle(working_.ui.timecodeStyle);
        app.applySettings();
        app.saveSettings();
    };
    // Footer: hairline, then Cancel / Apply / OK on the right.
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        const float y = ImGui::GetCursorScreenPos().y;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ImGui::GetWindowWidth(), y), pal::kStroke);
        const float bw = fs * 6.5f;
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - (bw * 3 + 8 * 2 + 16), ImGui::GetCursorPosY() + 12));
        if (ImGui::Button(tr("Cancel"), ImVec2(bw, 0)) || !open) ui.showSettings = false;
        ImGui::SameLine(0, 8);
        if (ImGui::Button(tr("Apply"), ImVec2(bw, 0))) apply();
        ImGui::SameLine(0, 8);
        if (primaryButton(tr("OK"), ImVec2(bw, 0))) {
            apply();
            ui.showSettings = false;
        }
    }
    ImGui::End();
}

void SettingsWindow::drawShortcuts(App& app) {
    CommandRegistry& reg = app.commands();
    // Edit a scratch registry state through the working copy.
    std::string preset = working_.keyboard.preset;
    if (comboString(tr("Keymap preset"), preset, {{"avicap", "AviCap"}, {"premiere", "Premiere Pro"}, {"resolve", "DaVinci Resolve"}}))
        working_.keyboard.preset = preset;
    ImGui::SameLine();
    if (ImGui::Button(tr("Reset All"))) working_.keyboard.overrides.clear();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##scsearch", tr("Search actions or keys"), search_, sizeof search_);

    // Effective chord under the working preset + overrides.
    const std::string savedPreset = reg.preset();
    const auto savedOverrides = reg.saveOverrides();
    reg.setPreset(working_.keyboard.preset);
    reg.loadOverrides(working_.keyboard.overrides);

    // Capture a new chord.
    if (!capturing_.empty()) {
        app.ui().capturingShortcut = true;
        const ImGuiIO& io = ImGui::GetIO();
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            const ImGuiKey key = static_cast<ImGuiKey>(k);
            if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
                key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper ||
                (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) || key == ImGuiKey_ReservedForModCtrl ||
                key == ImGuiKey_ReservedForModShift || key == ImGuiKey_ReservedForModAlt || key == ImGuiKey_ReservedForModSuper)
                continue;
            if (!ImGui::IsKeyPressed(key, false)) continue;
            if (key == ImGuiKey_Escape && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
                capturing_.clear();
            } else if (key == ImGuiKey_Backspace && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
                working_.keyboard.overrides[capturing_] = "";  // unbound
                capturing_.clear();
            } else {
                KeyChord c{key, io.KeyCtrl, io.KeyShift, io.KeyAlt};
                if (!c.toString().empty() && c.toString().find('?') == std::string::npos) {
                    if (c == reg.defaultShortcut(capturing_)) working_.keyboard.overrides.erase(capturing_);
                    else working_.keyboard.overrides[capturing_] = c.toString();
                    capturing_.clear();
                }
            }
            break;
        }
        reg.loadOverrides(working_.keyboard.overrides);
    } else {
        app.ui().capturingShortcut = false;
    }

    if (ImGui::BeginTable("##shortcuts", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Category"));
        ImGui::TableSetupColumn(tr("Action"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Shortcut"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
        ImGui::TableHeadersRow();
        const std::string needle = search_;
        for (const Action& a : reg.actions()) {
            const std::string label = tr(a.label);
            const KeyChord chord = reg.shortcut(a.id);
            const std::string keys = chord.toString();
            if (!needle.empty() && label.find(needle) == std::string::npos && keys.find(needle) == std::string::npos &&
                std::string(a.label).find(needle) == std::string::npos)
                continue;
            ImGui::PushID(a.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", tr(a.category.c_str()));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label.c_str());
            const auto conflicts = reg.conflicts(chord, a.id);
            if (!conflicts.empty()) {
                std::string msg = tr("Also used by:");
                for (const auto& c : conflicts)
                    if (const Action* o = reg.find(c)) msg += std::string(" ") + tr(o->label);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "(!)");
                tooltip(msg.c_str());
            }
            ImGui::TableNextColumn();
            const bool cap = capturing_ == a.id;
            const std::string btn = cap ? std::string(tr("Press keys...")) : (keys.empty() ? std::string("-") : keys);
            if (ImGui::Button((btn + "##key").c_str(), ImVec2(-1, 0))) capturing_ = a.id;
            if (cap) tooltip(tr("Esc: cancel, Backspace: remove"));
            ImGui::TableNextColumn();
            if (reg.hasOverride(a.id) && ImGui::SmallButton(tr("Reset"))) working_.keyboard.overrides.erase(a.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // Restore the live registry (applied on OK/Apply).
    reg.setPreset(savedPreset);
    reg.loadOverrides(savedOverrides);
}

}  // namespace avc::ui
