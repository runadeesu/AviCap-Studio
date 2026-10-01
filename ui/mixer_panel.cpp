// Audio mixer: one strip per audio track (meter, fader, pan, mute/solo) plus
// the master bus. Fader moves are undoable and merge while dragging.

#include <algorithm>
#include <cstdio>

#include "core/i18n.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

void MixerPanel::draw(App& app) {
    const Sequence* seq = app.sequence();
    if (!seq) return;
    const auto meters = app.playback().trackMeters();
    const float fs = ImGui::GetFontSize();
    const float stripW = fs * 4.6f;
    const float faderH = std::max(fs * 6.0f, ImGui::GetContentRegionAvail().y - fs * 6.5f);
    ImGui::BeginChild("##strips", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    auto strip = [&](const char* name, float& db, float* pan, const audio::Meter& meter, bool* mute, bool* solo,
                     const std::function<void(float, float, bool, bool, const char*)>& commit) {
        ImGui::BeginGroup();
        ImGui::PushID(name);
        ImGui::TextUnformatted(name);
        if (pan) {
            ImGui::SetNextItemWidth(stripW);
            float p = *pan;
            if (ImGui::SliderFloat("##pan", &p, -1.0f, 1.0f, p < -0.01f ? "L%.2f" : p > 0.01f ? "R%.2f" : "C")) commit(db, p, mute && *mute, solo && *solo, "pan");
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) commit(db, 0.0f, mute && *mute, solo && *solo, "pan");
        }
        float d = db;
        if (ImGui::VSliderFloat("##fader", ImVec2(stripW * 0.45f, faderH), &d, -60.0f, 12.0f, "%.1f")) commit(d, pan ? *pan : 0, mute && *mute, solo && *solo, "vol");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) commit(0.0f, pan ? *pan : 0, mute && *mute, solo && *solo, "vol");
        tooltip(tr("Volume (double-click: 0 dB)"));
        ImGui::SameLine(0, 4);
        levelMeter("##meter", meter, ImVec2(stripW * 0.4f, faderH));
        if (mute) {
            bool m = *mute;
            if (textToggle("M", &m, tr("Mute"), IM_COL32(200, 70, 60, 255))) commit(db, pan ? *pan : 0, m, solo && *solo, "mute");
            ImGui::SameLine(0, 2);
        }
        if (solo) {
            bool s = *solo;
            if (textToggle("S", &s, tr("Solo"), IM_COL32(200, 160, 40, 255))) commit(db, pan ? *pan : 0, mute && *mute, s, "solo");
        }
        char peak[32];
        std::snprintf(peak, sizeof peak, "%.1f", std::max(linearToDb(meter.peak[0]), linearToDb(meter.peak[1])));
        ImGui::TextDisabled("%s", meter.peak[0] > 0 || meter.peak[1] > 0 ? peak : "-inf");
        ImGui::PopID();
        ImGui::EndGroup();
        ImGui::SameLine(0, fs);
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
    ImGui::SeparatorText("");
    float master = seq->masterVolumeDb;
    const audio::Meter mm = app.playback().masterMeter();
    strip(tr("Master"), master, nullptr, mm, nullptr, nullptr, [&app](float v, float, bool, bool, const char*) {
        app.editSequence("Master Volume", [&](SequenceEditor& e) {
            e.props().masterVolumeDb = v;
            return Status::ok();
        }, EditOptions{"mixer:master"});
    });
    ImGui::NewLine();
    ImGui::TextDisabled("%s: %s   %s: %llu", tr("Output"), app.playback().output().deviceName().c_str(), tr("Underruns"),
                        static_cast<unsigned long long>(app.playback().underruns()));
    ImGui::EndChild();
}

}  // namespace avc::ui
