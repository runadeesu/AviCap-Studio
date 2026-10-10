// Audio mixer: one strip per audio track (meter, fader, pan, mute/solo) plus
// the master bus. Fader moves are undoable and merge while dragging.

#include <algorithm>
#include <cstdio>

#include "core/i18n.h"
#include "ui/panels.h"
#include "ui/style.h"
#include "ui/widgets.h"

namespace avc::ui {

void MixerPanel::draw(App& app) {
    const Sequence* seq = app.sequence();
    if (!seq) return;
    const auto meters = app.playback().trackMeters();
    const float fs = ImGui::GetFontSize();
    // Strips share the panel width (narrow docks show every channel), within limits.
    const int strips = static_cast<int>(seq->audioTrackIndices().size()) + 1;
    const float stripW = std::clamp((ImGui::GetContentRegionAvail().x - 6.0f * static_cast<float>(strips - 1)) / static_cast<float>(strips),
                                    fs * 3.3f, fs * 4.4f);
    const float frame = ImGui::GetFrameHeight();
    // Everything in a strip except the fader: name, pan, dB readout, M/S, padding.
    const float chrome = fs * 1.5f + frame * 0.7f + 2 + ImGui::GetTextLineHeightWithSpacing() + 2 + frame * 0.8f + 18;
    const float faderH = std::max(fs * 4.0f, ImGui::GetContentRegionAvail().y - chrome - ImGui::GetStyle().ScrollbarSize - 4);
    ImGui::BeginChild("##strips", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    auto strip = [&](const char* name, float& db, float* pan, const audio::Meter& meter, bool* mute, bool* solo,
                     const std::function<void(float, float, bool, bool, const char*)>& commit) {
        ImGui::PushID(name);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float pad = 6.0f;
        const float cardH = faderH + chrome;
        dl->AddRectFilled(p0, p0 + ImVec2(stripW, cardH), pal::kPanelRaised, 8.0f);
        ImGui::BeginGroup();
        // Name (centred, clipped)
        {
            ImFont* bold = boldFont() ? boldFont() : ImGui::GetFont();
            const float size = fs * 0.85f;
            const ImVec2 ts = bold->CalcTextSizeA(size, FLT_MAX, 0.0f, name);
            dl->PushClipRect(p0, p0 + ImVec2(stripW - 2, fs * 1.6f), true);
            dl->AddText(bold, size, ImVec2(p0.x + std::max(pad, (stripW - ts.x) * 0.5f), p0.y + pad), pal::kText, name);
            dl->PopClipRect();
        }
        ImGui::SetCursorScreenPos(p0 + ImVec2(pad, fs * 1.5f));
        if (pan) {
            float p = *pan;
            if (slimSlider("##pan", &p, -1.0f, 1.0f, stripW - pad * 2, true)) commit(db, p, mute && *mute, solo && *solo, "pan");
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) commit(db, 0.0f, mute && *mute, solo && *solo, "pan");
            char pt[16];
            if (p < -0.01f) std::snprintf(pt, sizeof pt, "L%.0f", -p * 100);
            else if (p > 0.01f) std::snprintf(pt, sizeof pt, "R%.0f", p * 100);
            else std::snprintf(pt, sizeof pt, "C");
            tooltip((std::string(tr("Pan")) + ": " + pt + "  (" + tr("double-click: center") + ")").c_str());
        } else {
            ImGui::Dummy(ImVec2(stripW - pad * 2, ImGui::GetFrameHeight() * 0.7f));
        }
        // Fader + meter
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, ImGui::GetCursorScreenPos().y + 2));
        float d = db;
        const float faderW = (stripW - pad * 2) * 0.55f;
        if (vFader("##fader", &d, -60.0f, 12.0f, ImVec2(faderW, faderH))) commit(d, pan ? *pan : 0, mute && *mute, solo && *solo, "vol");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) commit(0.0f, pan ? *pan : 0, mute && *mute, solo && *solo, "vol");
        tooltip(tr("Volume (double-click: 0 dB)"));
        ImGui::SameLine(0, 4);
        levelMeter("##meter", meter, ImVec2(stripW - pad * 2 - faderW - 4, faderH));
        // dB readout
        char vol[24];
        std::snprintf(vol, sizeof vol, "%+.1f dB", db);
        {
            const ImVec2 ts = ImGui::CalcTextSize(vol);
            ImGui::SetCursorScreenPos(ImVec2(p0.x + (stripW - ts.x) * 0.5f, ImGui::GetCursorScreenPos().y + 2));
            ImGui::TextColored(pal::v4(pal::kTextMuted), "%s", vol);
        }
        // Mute / solo pills
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, ImGui::GetCursorScreenPos().y));
        const float pillW = (stripW - pad * 2 - 4) * 0.5f;
        auto pill = [&](const char* id, const char* label, bool on, ImU32 onColor, const char* tip) {
            const ImVec2 pp = ImGui::GetCursorScreenPos();
            const float ph = ImGui::GetFrameHeight() * 0.8f;
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(pillW, ph));
            const bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(pp, pp + ImVec2(pillW, ph), on ? onColor : hov ? pal::kPanelActive : pal::kPanelHover, ph * 0.5f);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(pp + ImVec2((pillW - ts.x) * 0.5f, (ph - ts.y) * 0.5f), on ? IM_COL32(20, 20, 24, 255) : pal::kTextMuted, label);
            tooltip(tip);
            return pressed;
        };
        if (mute) {
            if (pill("##m", "M", *mute, pal::kDanger, tr("Mute"))) commit(db, pan ? *pan : 0, !*mute, solo && *solo, "mute");
            ImGui::SameLine(0, 4);
        }
        if (solo) {
            if (pill("##s", "S", *solo, pal::kWarning, tr("Solo"))) commit(db, pan ? *pan : 0, mute && *mute, !*solo, "solo");
        }
        if (!mute && !solo) ImGui::Dummy(ImVec2(pillW, ImGui::GetFrameHeight() * 0.8f));
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(stripW, cardH));
        ImGui::PopID();
        ImGui::SameLine(0, 6);
    };
    for (int i : seq->audioTrackIndices()) {
        const Track& t = *seq->tracks[static_cast<size_t>(i)];
        float db = t.volumeDb, pan = t.pan;
        bool mute = t.muted, solo = t.solo;
        auto it = meters.find(t.id);
        const audio::Meter m = it == meters.end() ? audio::Meter{} : it->second;
        const TrackId id = t.id;
        strip(t.name.c_str(), db, &pan, m, &mute, &solo, [&app, id](float v, float p, bool mu, bool so, const char* what) {
            app.editSequence("Mixer", [&](SequenceEditor& e) {
                Track& mt = e.mutableTrackById(id);
                mt.volumeDb = v;
                mt.pan = p;
                mt.muted = mu;
                mt.solo = so;
                return Status::ok();
            }, EditOptions{std::string("mixer:") + std::to_string(id) + what});
        });
    }
    float master = seq->masterVolumeDb;
    const audio::Meter mm = app.playback().masterMeter();
    char device[256];
    std::snprintf(device, sizeof device, "%s: %s\n%s: %llu", tr("Output"), app.playback().output().deviceName().c_str(), tr("Underruns"),
                  static_cast<unsigned long long>(app.playback().underruns()));
    strip(tr("Master"), master, nullptr, mm, nullptr, nullptr, [&app](float v, float, bool, bool, const char*) {
        app.editSequence("Master Volume", [&](SequenceEditor& e) {
            e.props().masterVolumeDb = v;
            return Status::ok();
        }, EditOptions{"mixer:master"});
    });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", device);
    ImGui::NewLine();
    ImGui::EndChild();
}

}  // namespace avc::ui
