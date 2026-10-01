// AI panel: natural-language editing (Intent -> Plan -> Preview -> Apply),
// silence removal with a timeline preview, beats, scenes and highlights.
// All processing is local; nothing is uploaded.

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "core/i18n.h"
#include "core/net.h"
#include "core/platform.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

const ImVec4 kErrorColor(1.0f, 0.45f, 0.40f, 1.0f);
const ImVec4 kNoteColor(1.0f, 0.80f, 0.35f, 1.0f);
const ImVec4 kOkColor(0.55f, 0.85f, 0.55f, 1.0f);

void busyBar(App& app) {
    AiService& ai = app.ai();
    if (!ai.busy()) return;
    char overlay[160];
    std::snprintf(overlay, sizeof overlay, "%s  %d%%", ai.busyLabel().c_str(), static_cast<int>(ai.progress() * 100));
    ImGui::ProgressBar(ai.progress(), ImVec2(-ImGui::GetFrameHeight() * 4.5f, 0), overlay);
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) ai.cancel();
}

// Wrapping row of small buttons.
bool chip(const char* label) {
    const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w < right) ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    const bool r = ImGui::SmallButton(label);
    ImGui::PopStyleVar();
    return r;
}

std::string seconds(double s) {
    char buf[32];
    if (s >= 60) std::snprintf(buf, sizeof buf, "%d:%04.1f", static_cast<int>(s / 60), s - 60 * static_cast<int>(s / 60));
    else std::snprintf(buf, sizeof buf, "%.1f %s", s, tr("sec"));
    return buf;
}

}  // namespace

void AiPanel::draw(App& app) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", tr("Everything is processed on this PC. Your video is never uploaded."));
    ImGui::PopStyleColor();
    busyBar(app);
    if (!ImGui::BeginTabBar("##ai_tabs")) return;
    if (ImGui::BeginTabItem(tr("AI Edit"))) {
        drawPlan(app);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tr("Silence Removal"))) {
        drawSilence(app);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tr("Beats & Scenes"))) {
        drawAnalysis(app);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// ------------------------------------------------------------------ natural-language editing

void AiPanel::drawPlan(App& app) {
    AiService& ai = app.ai();
    auto& st = ai.planState();
    hintText(tr("Describe the edit you want in plain words. The plan is shown first; nothing changes until you press Apply."));

    const float h = ImGui::GetTextLineHeight() * 4.2f;
    ImGui::SetNextItemWidth(-1);
    const bool submit = ImGui::InputTextMultiline("##prompt", prompt_, sizeof prompt_, ImVec2(-1, h),
                                                  ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue);
    tooltip(tr("Enter: create the plan / Ctrl+Enter: new line"));

    // Examples: click to add to the instruction.
    ImGui::TextDisabled("%s", tr("Examples:"));
    for (const char* ex : ai::exampleInstructions()) {
        if (chip(tr(ex))) {
            const size_t len = std::strlen(prompt_);
            std::string add = std::string(len > 0 ? (currentLanguage() == Language::Japanese ? "、" : ", ") : "") + tr(ex);
            if (len + add.size() < sizeof prompt_) std::memcpy(prompt_ + len, add.c_str(), add.size() + 1);
        }
    }
    ImGui::Spacing();

    drawProvider(app);

    const bool hasPrompt = prompt_[0] != 0;
    const bool cloud = ai.useCloud();
    ImGui::BeginDisabled(!hasPrompt || ai.cloudBusy());
    if (iconTextButton("##makeplan", Icon::Check, cloud ? tr("Create Plan with Claude") : tr("Create Plan")) || (submit && hasPrompt)) {
        if (cloud) ai.makeCloudPlan(prompt_);
        else ai.makePlan(prompt_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) {
        prompt_[0] = 0;
        ai.discardPlan();
        st = {};
    }
    if (ai.cloudBusy()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Asking Claude..."));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Cancel"))) ai.cancelCloud();
    } else if (cloud && !st.error.empty() && hasPrompt) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Use the offline rules instead"))) ai.makePlan(prompt_);
    }

    if (!st.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", st.error.c_str());
        ImGui::PopStyleColor();
    }
    if (st.plan.steps.empty() && st.plan.notes.empty()) return;

    ImGui::SeparatorText(tr("Plan"));
    bool changed = false;
    for (size_t i = 0; i < st.plan.steps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool on = i < st.enabled.size() && st.enabled[i];
        char label[32];
        std::snprintf(label, sizeof label, "%zu.", i + 1);
        if (ImGui::Checkbox(label, &on)) {
            st.enabled[i] = on;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextWrapped("%s", st.plan.steps[i].description.c_str());
        ImGui::PopID();
    }
    for (const auto& n : st.plan.notes) {
        ImGui::PushStyleColor(ImGuiCol_Text, kNoteColor);
        ImGui::TextWrapped("%s", n.c_str());
        ImGui::PopStyleColor();
    }
    if (changed && st.previewing) ai.discardPlan();

    const bool anyEnabled = std::any_of(st.enabled.begin(), st.enabled.end(), [](bool b) { return b; });
    ImGui::Spacing();
    ImGui::BeginDisabled(!anyEnabled || ai.busy());
    if (iconTextButton("##preview", Icon::Play, tr("Preview"), tr("Show the result in the viewer and timeline without changing the project"),
                       st.previewing))
        ai.previewPlan();
    ImGui::SameLine();
    if (iconTextButton("##apply", Icon::Check, tr("Apply"), tr("Apply the plan as one step (Ctrl+Z undoes it)"))) ai.applyPlan();
    ImGui::EndDisabled();
    if (st.previewing) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Discard"))) ai.discardPlan();
    }

    if (st.previewing || !st.report.log.empty()) {
        ImGui::SeparatorText(st.previewing ? tr("Preview") : tr("Result"));
        const auto& r = st.report;
        ImGui::Text("%s: %s -> %s", tr("Duration"), seconds(r.durationBefore.seconds()).c_str(), seconds(r.durationAfter.seconds()).c_str());
        ImGui::Text("%s: %zu -> %zu", tr("Clips"), r.clipsBefore, r.clipsAfter);
        for (const auto& line : r.log) {
            ImGui::PushStyleColor(ImGuiCol_Text, kOkColor);
            ImGui::TextWrapped("- %s", line.c_str());
            ImGui::PopStyleColor();
        }
    }
}

// ------------------------------------------------------------------ provider (local / Claude)

void AiPanel::drawProvider(App& app) {
    AiService& ai = app.ai();
    AppSettings& set = app.settings();
    bool cloudSel = set.ai.assistantProvider == "anthropic";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Assistant:"));
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Offline rules"), !cloudSel)) {
        set.ai.assistantProvider = "local";
        app.saveSettings();
    }
    tooltip(tr("Understands common instructions. Works without internet; nothing is sent."));
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Claude (cloud, opt-in)"), cloudSel)) {
        set.ai.assistantProvider = "anthropic";
        app.saveSettings();
    }
    tooltip(tr("Understands free-form instructions. Sends your instruction and a text description of the timeline to Anthropic."));
    cloudSel = set.ai.assistantProvider == "anthropic";
    if (!cloudSel) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.17f, 0.22f, 1.0f));
    ImGui::BeginChild("##cloud", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    if (!ai.transportAvailable) {
        ImGui::TextWrapped("%s", tr("The cloud assistant is only available in the Windows version."));
    } else if (!set.ai.cloudConsent) {
        ImGui::TextWrapped("%s", tr("Before using Claude, please check what is sent:"));
        ImGui::BulletText("%s", tr("Your instruction text"));
        ImGui::BulletText("%s", tr("A text description of the timeline: track names, clip file names and times, title/subtitle text, markers"));
        ImGui::PushStyleColor(ImGuiCol_Text, kOkColor);
        ImGui::BulletText("%s", tr("Never sent: video, audio, images, file paths"));
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", tr("Data is sent to api.anthropic.com over HTTPS using your own API key and is handled under Anthropic's terms. Usage is billed to your Anthropic account."));
        if (ImGui::Button(tr("I agree - enable Claude"))) {
            set.ai.cloudConsent = true;
            set.privacy.allowNetwork = true;
            app.saveSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Use offline rules"))) {
            set.ai.assistantProvider = "local";
            app.saveSettings();
        }
    } else if (!set.privacy.allowNetwork) {
        ImGui::TextWrapped("%s", tr("Network access is turned off in Settings > AI & Privacy."));
        if (ImGui::Button(tr("Allow network access"))) {
            set.privacy.allowNetwork = true;
            app.saveSettings();
        }
    } else if (ai.apiKey().empty()) {
        ImGui::TextWrapped("%s", tr("Enter your Anthropic API key (from the Claude Console)."));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##apikey", "sk-ant-...", keyBuf_, sizeof keyBuf_, ImGuiInputTextFlags_Password);
        ImGui::Checkbox(tr("Remember on this PC (encrypted for your Windows account)"), &rememberKey_);
        ImGui::BeginDisabled(keyBuf_[0] == 0);
        if (ImGui::Button(tr("Use this key"))) {
            Status s = ai.setApiKey(keyBuf_, rememberKey_);
            std::memset(keyBuf_, 0, sizeof keyBuf_);
            if (!s) app.notify(LogLevel::Warning, s.message());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Get an API key"))) openWithShell("https://platform.claude.com/");
    } else {
        ImGui::TextDisabled("%s: %s  /  %s: api.anthropic.com", tr("Model"), set.ai.anthropicModel.c_str(), tr("Sent to"));
        if (ai.apiKeyFromEnvironment()) {
            ImGui::TextDisabled("%s", tr("Using the key from the ANTHROPIC_API_KEY environment variable."));
        } else if (ImGui::SmallButton(tr("Forget the API key"))) {
            ai.forgetApiKey();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Withdraw consent"))) {
            set.ai.cloudConsent = false;
            set.ai.assistantProvider = "local";
            app.saveSettings();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ silence

void AiPanel::drawSilence(App& app) {
    AiService& ai = app.ai();
    hintText(tr("Finds pauses in speech and removes them from every track at once, so video and audio stay in sync."));
    ai::SilenceOptions& o = ai.silenceOptions;
    ImGui::Checkbox(tr("Automatic threshold"), &o.autoThreshold);
    tooltip(tr("Measures the background noise and picks the threshold for you"));
    ImGui::BeginDisabled(o.autoThreshold);
    ImGui::SliderFloat(formRow(tr("Threshold")), &o.thresholdDb, -70.0f, -20.0f, "%.0f dB");
    ImGui::EndDisabled();
    float minSil = static_cast<float>(o.minSilenceSec), pad = static_cast<float>(o.paddingSec);
    if (ImGui::SliderFloat(formRow(tr("Shortest pause")), &minSil, 0.2f, 3.0f, "%.2f s")) o.minSilenceSec = minSil;
    tooltip(tr("Pauses shorter than this are kept"));
    if (ImGui::SliderFloat(formRow(tr("Padding")), &pad, 0.0f, 0.5f, "%.2f s")) o.paddingSec = pad;
    tooltip(tr("Time kept before and after speech so words are not clipped"));

    const std::set<MediaId> voice = ai.voiceMedia();
    if (voice.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Add a clip with sound to the timeline first."));
        return;
    }
    bool analysed = true;
    for (MediaId id : voice) analysed &= ai.silence(id) != nullptr;

    ImGui::Spacing();
    ImGui::BeginDisabled(ai.busy());
    if (iconTextButton("##detect", Icon::Search, tr("Detect Silence"))) {
        ai.analyze(AnalysisKind::Silence, voice, [&ai](bool ok, const std::string&) {
            if (ok) ai.showSilenceOverlay = true;
        });
    }
    ImGui::EndDisabled();
    if (!analysed) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Not analysed yet"));
        return;
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("Show on timeline"), &ai.showSilenceOverlay);

    const auto ranges = ai.silenceRangesOnTimeline();
    Time total{0};
    for (const auto& r : ranges) total += r.duration;
    const Sequence* seq = app.sequence();
    const double dur = seq ? seq->duration().seconds() : 0.0;
    ImGui::SeparatorText(tr("Result"));
    if (ranges.empty()) {
        ImGui::TextUnformatted(tr("No silence found"));
        return;
    }
    ImGui::Text("%s: %zu   %s: %s (%.0f%%)", tr("Pauses"), ranges.size(), tr("Total"), seconds(total.seconds()).c_str(),
                dur > 0 ? total.seconds() / dur * 100.0 : 0.0);
    for (MediaId id : voice)
        if (const ai::SilenceResult* r = ai.silence(id)) {
            ImGui::TextDisabled("%s: %.0f dB / %s: %.0f dB", tr("Noise floor"), r->noiseFloorDb, tr("Threshold"), r->thresholdDb);
            break;
        }

    if (iconTextButton("##remove", Icon::Razor, tr("Remove Silence"), tr("Ripple delete on all tracks (one undo step)"))) {
        if (Status s = ai.removeSilence(); !s) app.notify(LogLevel::Warning, s.message());
    }
    ImGui::SameLine();
    if (iconTextButton("##mark", Icon::Marker, tr("Add as Markers"), tr("Mark the pauses instead of cutting them"))) {
        if (Status s = ai.markSilence(); !s) app.notify(LogLevel::Warning, s.message());
    }

    // Clickable list: jump to a pause to listen before cutting.
    ImGui::BeginChild("##silences", ImVec2(0, std::min(220.0f, ImGui::GetTextLineHeightWithSpacing() * (ranges.size() + 1.0f))),
                      ImGuiChildFlags_Borders);
    for (size_t i = 0; i < ranges.size(); ++i) {
        char row[96];
        std::snprintf(row, sizeof row, "%3zu  %s  (%.2f s)", i + 1, formatTime(ranges[i].start, seq->frameRate).c_str(),
                      ranges[i].duration.seconds());
        if (ImGui::Selectable(row)) app.seek(ranges[i].start > Time::fromSeconds(0.5) ? ranges[i].start - Time::fromSeconds(0.5) : Time{0});
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ beats, scenes, highlights

void AiPanel::drawAnalysis(App& app) {
    AiService& ai = app.ai();
    const Sequence* seq = app.sequence();
    if (!seq) return;

    // Music beats
    ImGui::SeparatorText(tr("Music Beats"));
    const auto music = ai.musicMedia();
    if (!music) {
        hintText(tr("Add music (BGM) to an audio track to detect its beats."));
        if (iconTextButton("##addbgm", Icon::Music, tr("Add Music..."))) app.addAudioDialog(true);
    } else {
        const MediaItem* m = app.project().findMedia(*music);
        ImGui::Text("%s: %s", tr("Music"), m ? m->name.c_str() : "?");
        if (const ai::BeatResult* b = ai.beats(*music))
            ImGui::TextDisabled("%.1f BPM  (%s %.0f%%, %zu %s)", b->bpm, tr("confidence"), b->confidence * 100.0f, b->beats.size(), tr("beats"));
        ImGui::BeginDisabled(ai.busy());
        if (iconTextButton("##beats", Icon::Marker, tr("Add Beat Markers"), tr("Markers on every beat; clips snap to them"))) {
            const MediaId id = *music;
            ai.analyze(AnalysisKind::Beats, {id}, [&app](bool ok, const std::string& msg) {
                if (!ok) return app.notify(LogLevel::Warning, msg);
                if (Status s = app.ai().addBeatMarkers(); !s) app.notify(LogLevel::Warning, s.message());
            });
        }
        ImGui::EndDisabled();
    }

    // Scene changes
    ImGui::SeparatorText(tr("Scene Changes"));
    const bool selOnly = !app.selection.clips.empty();
    const std::set<MediaId> videos = ai.videoMedia(selOnly);
    hintText(selOnly ? tr("Analyses the selected clips.") : tr("Analyses every video clip (select clips to limit it)."));
    ImGui::SliderFloat(formRow(tr("Sensitivity")), &sensitivity_, 0.1f, 1.0f, "%.2f");
    tooltip(tr("Higher finds more cuts"));
    ai.sceneOptions.threshold = std::clamp(0.55f - sensitivity_ * 0.45f, 0.08f, 0.6f);
    ImGui::BeginDisabled(ai.busy() || videos.empty());
    auto scenes = [&](bool split) {
        ai.analyze(AnalysisKind::Scenes, videos, [&app, split](bool ok, const std::string& msg) {
            if (!ok) return app.notify(LogLevel::Warning, msg);
            if (Status s = app.ai().addSceneMarkers(split); !s) app.notify(LogLevel::Warning, s.message());
        });
    };
    if (iconTextButton("##scenemarkers", Icon::Marker, tr("Add Scene Markers"))) scenes(false);
    ImGui::SameLine();
    if (iconTextButton("##scenesplit", Icon::Razor, tr("Split at Scenes"), tr("Cuts the clips at every scene change (one undo step)"))) scenes(true);
    ImGui::EndDisabled();
    if (videos.empty()) ImGui::TextDisabled("%s", tr("No video clips on the timeline."));

    // Highlights
    ImGui::SeparatorText(tr("Highlights"));
    hintText(tr("Finds the loudest moments (laughter, cheering, emphasis) of the dialogue."));
    ImGui::SliderInt(formRow(tr("Count")), &ai.highlightCount, 1, 20);
    float win = static_cast<float>(ai.highlightWindowSec);
    if (ImGui::SliderFloat(formRow(tr("Length")), &win, 1.0f, 15.0f, "%.0f s")) ai.highlightWindowSec = win;
    const std::set<MediaId> voice = ai.voiceMedia();
    ImGui::BeginDisabled(ai.busy() || voice.empty());
    if (iconTextButton("##highlights", Icon::Marker, tr("Add Highlight Markers"))) {
        ai.analyze(AnalysisKind::Highlights, voice, [&app](bool ok, const std::string& msg) {
            if (!ok) return app.notify(LogLevel::Warning, msg);
            if (Status s = app.ai().addHighlightMarkers(); !s) app.notify(LogLevel::Warning, s.message());
        });
    }
    ImGui::EndDisabled();
}

}  // namespace avc::ui
