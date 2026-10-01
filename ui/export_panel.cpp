// Export: preset-based settings with live validation (encoder availability,
// container compatibility, estimated size vs free disk space) and the export
// queue (progress, ETA, pause/resume, cancel, retry, verification report).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "encode/encoder.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace fs = std::filesystem;

namespace {

bool containerSupports(const std::string& container, enc::VideoCodec c) {
    using enc::VideoCodec;
    if (container == "mp4") return c == VideoCodec::H264 || c == VideoCodec::HEVC || c == VideoCodec::AV1 || c == VideoCodec::MPEG4;
    if (container == "mov")
        return c == VideoCodec::H264 || c == VideoCodec::HEVC || c == VideoCodec::ProRes || c == VideoCodec::DNxHR || c == VideoCodec::MPEG4;
    if (container == "webm") return c == VideoCodec::VP9 || c == VideoCodec::AV1;
    return true;  // mkv
}

bool audioSupported(const std::string& container, enc::AudioCodec a) {
    using enc::AudioCodec;
    if (container == "webm") return a == AudioCodec::Opus;
    if (container == "mp4") return a == AudioCodec::AAC || a == AudioCodec::MP3 || a == AudioCodec::Opus || a == AudioCodec::FLAC;
    if (container == "mov") return a == AudioCodec::AAC || a == AudioCodec::PCM16 || a == AudioCodec::PCM24;
    return true;
}

std::string bytesText(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(b) / (1ull << 30));
    else std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1ull << 20));
    return buf;
}

std::string etaText(double sec) {
    if (sec <= 0) return "-";
    char buf[32];
    const int s = static_cast<int>(sec + 0.5);
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

}  // namespace

void ExportPanel::draw(App& app) {
    if (!initialized_ || projectPathAtInit_ != app.projectPath()) {
        settings_ = app.defaultExportSettings();
        std::snprintf(path_, sizeof path_, "%s", settings_.outputPath.c_str());
        initialized_ = true;
        projectPathAtInit_ = app.projectPath();
    }
    if (ImGui::BeginTabBar("##exporttabs")) {
        if (ImGui::BeginTabItem(tr("Settings"))) {
            drawSettings(app);
            ImGui::EndTabItem();
        }
        std::string queueLabel = std::string(tr("Queue")) + " (" + std::to_string(app.exports().jobs().size()) + ")###queue";
        if (ImGui::BeginTabItem(queueLabel.c_str())) {
            drawQueue(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void ExportPanel::drawSettings(App& app) {
    const Sequence* seq = app.sequence();
    if (!seq) return;
    auto& cat = enc::EncoderCatalog::instance();
    const float labelW = ImGui::GetFontSize() * 8;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-1);
    };

    // Preset
    const exp::Preset* cur = exp::findPreset(settings_.presetId);
    label(tr("Preset"));
    if (ImGui::BeginCombo("##preset", cur ? tr(cur->name.c_str()) : tr("Custom"))) {
        for (const auto& p : exp::presets())
            if (ImGui::Selectable(tr(p.name.c_str()), p.id == settings_.presetId)) {
                const std::string out = path_;
                settings_ = exp::applyPreset(p.id, settings_);
                // Keep the folder/name, switch the extension.
                fs::path fp = pathFromUtf8(out);
                fp.replace_extension(pathFromUtf8(exp::defaultExtension(settings_.container)));
                std::snprintf(path_, sizeof path_, "%s", pathToUtf8(fp).c_str());
            }
        ImGui::EndCombo();
    }
    if (cur && !cur->description.empty()) {
        ImGui::SetCursorPosX(labelW);
        ImGui::TextDisabled("%s", tr(cur->description.c_str()));
    }

    // Output
    label(tr("Output File"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 6);
    ImGui::InputText("##out", path_, sizeof path_);
    ImGui::SameLine();
    if (ImGui::Button(tr("Browse..."))) {
        const std::string ext = exp::defaultExtension(settings_.container);
        auto p = app.dialogs().saveFile(tr("Export As"), {{settings_.container, "*" + ext}},
                                        pathToUtf8(pathFromUtf8(path_).filename()), ext.substr(1));
        if (p) std::snprintf(path_, sizeof path_, "%s", p->c_str());
    }

    ImGui::SeparatorText(tr("Video"));
    label(tr("Container"));
    const char* containers[] = {"mp4", "mov", "mkv", "webm"};
    if (ImGui::BeginCombo("##container", settings_.container.c_str())) {
        for (const char* c : containers)
            if (ImGui::Selectable(c, settings_.container == c)) {
                settings_.container = c;
                settings_.presetId = "custom";
                fs::path fp = pathFromUtf8(path_);
                fp.replace_extension(pathFromUtf8(exp::defaultExtension(c)));
                std::snprintf(path_, sizeof path_, "%s", pathToUtf8(fp).c_str());
            }
        ImGui::EndCombo();
    }
    label(tr("Codec"));
    if (ImGui::BeginCombo("##codec", enc::videoCodecName(settings_.videoCodec))) {
        for (enc::VideoCodec c : {enc::VideoCodec::H264, enc::VideoCodec::HEVC, enc::VideoCodec::AV1, enc::VideoCodec::VP9,
                                  enc::VideoCodec::ProRes, enc::VideoCodec::DNxHR, enc::VideoCodec::MPEG4}) {
            const enc::EncoderInfo* best = cat.best(c, settings_.preferHardware);
            std::string name = enc::videoCodecName(c);
            if (!best) name += std::string("  (") + tr("not available") + ")";
            if (ImGui::Selectable(name.c_str(), settings_.videoCodec == c, best ? 0 : ImGuiSelectableFlags_Disabled)) {
                settings_.videoCodec = c;
                settings_.encoder = "auto";
                settings_.presetId = "custom";
            }
        }
        ImGui::EndCombo();
    }
    label(tr("Encoder"));
    const std::string encLabel = settings_.encoder == "auto" ? std::string(tr("Automatic (best available)")) : settings_.encoder;
    if (ImGui::BeginCombo("##encoder", encLabel.c_str())) {
        if (ImGui::Selectable(tr("Automatic (best available)"), settings_.encoder == "auto")) settings_.encoder = "auto";
        for (const auto& e : cat.encoders(settings_.videoCodec)) {
            std::string name = e.displayName + (e.hardware ? "  [GPU]" : "  [CPU]");
            if (!e.available) name += "  - " + e.reason;
            if (ImGui::Selectable(name.c_str(), settings_.encoder == e.name, e.available ? 0 : ImGuiSelectableFlags_Disabled))
                settings_.encoder = e.name;
        }
        ImGui::EndCombo();
    }
    if (const enc::EncoderInfo* chosen = settings_.encoder == "auto" ? cat.best(settings_.videoCodec, settings_.preferHardware)
                                                                     : cat.find(settings_.encoder)) {
        ImGui::SetCursorPosX(labelW);
        ImGui::TextDisabled("%s %s", tr("Will use:"), chosen->displayName.c_str());
    }
    ImGui::SetCursorPosX(labelW);
    ImGui::Checkbox(tr("Prefer hardware encoder"), &settings_.preferHardware);

    label(tr("Resolution"));
    const int w = settings_.width > 0 ? settings_.width : seq->width;
    const int h = settings_.height > 0 ? settings_.height : seq->height;
    char resLabel[64];
    std::snprintf(resLabel, sizeof resLabel, "%dx%d%s", w, h, settings_.width == 0 ? tr("(sequence)") : "");
    if (ImGui::BeginCombo("##res", resLabel)) {
        struct R {
            const char* name;
            int w, h;
        };
        const R list[] = {{"Sequence", 0, 0},          {"3840x2160 (4K UHD)", 3840, 2160}, {"2560x1440", 2560, 1440},
                          {"1920x1080 (Full HD)", 1920, 1080}, {"1280x720 (HD)", 1280, 720},   {"1080x1920 (Vertical)", 1080, 1920},
                          {"1080x1080 (Square)", 1080, 1080},  {"854x480", 854, 480}};
        for (const R& r : list)
            if (ImGui::Selectable(tr(r.name), settings_.width == r.w && settings_.height == r.h)) {
                settings_.width = r.w;
                settings_.height = r.h;
            }
        ImGui::EndCombo();
    }
    label(tr("Frame Rate"));
    const std::string fr = settings_.frameRate.valid() ? fpsText(settings_.frameRate) : std::string(tr("Sequence")) + " (" + fpsText(seq->frameRate) + ")";
    if (ImGui::BeginCombo("##fps", fr.c_str())) {
        const std::pair<const char*, Rational> rates[] = {{"Sequence", {0, 1}},     {"23.976", {24000, 1001}}, {"24", {24, 1}},
                                                          {"25", {25, 1}},          {"29.97", {30000, 1001}},  {"30", {30, 1}},
                                                          {"50", {50, 1}},          {"59.94", {60000, 1001}},  {"60", {60, 1}}};
        for (const auto& [n, r] : rates)
            if (ImGui::Selectable(tr(n), settings_.frameRate == r)) settings_.frameRate = r;
        ImGui::EndCombo();
    }
    label(tr("Rate Control"));
    int rc = static_cast<int>(settings_.rateControl);
    const char* rcs[] = {tr("Constant Quality"), tr("Variable Bitrate (VBR)"), tr("Constant Bitrate (CBR)")};
    if (ImGui::Combo("##rc", &rc, rcs, 3)) settings_.rateControl = static_cast<enc::RateControl>(rc);
    if (settings_.rateControl == enc::RateControl::Quality) {
        label(tr("Quality"));
        ImGui::SliderInt("##q", &settings_.quality, 0, 51, "%d (lower = better)");
    } else {
        label(tr("Bitrate"));
        ImGui::DragInt("##br", &settings_.bitrateKbps, 100, 500, 400000, "%d kbps");
    }
    if (settings_.videoCodec == enc::VideoCodec::HEVC || settings_.videoCodec == enc::VideoCodec::AV1 ||
        settings_.videoCodec == enc::VideoCodec::ProRes || settings_.videoCodec == enc::VideoCodec::DNxHR) {
        label(tr("Bit Depth"));
        int bd = settings_.bitDepth >= 10 ? 1 : 0;
        const char* bds[] = {"8-bit", "10-bit"};
        if (ImGui::Combo("##bd", &bd, bds, 2)) settings_.bitDepth = bd ? 10 : 8;
    }

    ImGui::SeparatorText(tr("Audio"));
    ImGui::SetCursorPosX(labelW);
    ImGui::Checkbox(tr("Include audio"), &settings_.includeAudio);
    if (settings_.includeAudio) {
        label(tr("Audio Codec"));
        if (ImGui::BeginCombo("##acodec", enc::audioCodecName(settings_.audioCodec))) {
            for (enc::AudioCodec a : {enc::AudioCodec::AAC, enc::AudioCodec::Opus, enc::AudioCodec::MP3, enc::AudioCodec::FLAC,
                                      enc::AudioCodec::PCM16, enc::AudioCodec::PCM24})
                if (ImGui::Selectable(enc::audioCodecName(a), settings_.audioCodec == a)) settings_.audioCodec = a;
            ImGui::EndCombo();
        }
        label(tr("Audio Bitrate"));
        ImGui::DragInt("##abr", &settings_.audioBitrateKbps, 8, 64, 512, "%d kbps");
        label(tr("Sample Rate"));
        int sr = settings_.sampleRate == 44100 ? 0 : 1;
        const char* srs[] = {"44100 Hz", "48000 Hz"};
        if (ImGui::Combo("##sr", &sr, srs, 2)) settings_.sampleRate = sr ? 48000 : 44100;
    }

    ImGui::SeparatorText(tr("Range"));
    const char* ranges[] = {tr("Entire sequence (or In/Out when set)"), tr("In/Out only")};
    label(tr("Range"));
    ImGui::Combo("##range", &rangeMode_, ranges, seq->workArea ? 2 : 1);
    ImGui::SetCursorPosX(labelW);
    ImGui::Checkbox(tr("Verify the file after export"), &settings_.verify);

    // Validation
    settings_.outputPath = path_;
    const TimeRange range = seq->workArea ? *seq->workArea : TimeRange{Time{0}, seq->duration()};
    const double fps = (settings_.frameRate.valid() ? settings_.frameRate : seq->frameRate).toDouble();
    const uint64_t est = exp::estimateOutputBytes(settings_, w, h, fps, range.duration.seconds());
    std::vector<std::string> problems;
    if (seq->duration().ticks <= 0) problems.push_back(tr("The sequence is empty."));
    if (settings_.outputPath.empty()) problems.push_back(tr("Choose an output file."));
    if (!containerSupports(settings_.container, settings_.videoCodec))
        problems.push_back(std::string(enc::videoCodecName(settings_.videoCodec)) + " " + tr("cannot be stored in") + " ." + settings_.container);
    if (settings_.includeAudio && !audioSupported(settings_.container, settings_.audioCodec))
        problems.push_back(std::string(enc::audioCodecName(settings_.audioCodec)) + " " + tr("audio cannot be stored in") + " ." + settings_.container);
    if (!cat.best(settings_.videoCodec, settings_.preferHardware)) problems.push_back(tr("No encoder is available for this codec."));
    const fs::path outDir = pathFromUtf8(settings_.outputPath).parent_path();
    std::error_code ec;
    if (!outDir.empty() && !fs::exists(outDir, ec)) problems.push_back(tr("The output folder does not exist."));
    const auto disk = outDir.empty() ? std::nullopt : queryDiskSpace(outDir);
    if (disk && disk->available < est + est / 10) problems.push_back(tr("Not enough free disk space for the estimated file size."));
    for (const auto& j : app.exports().jobs())
        if (j->settings().outputPath == settings_.outputPath && (j->progress().state == exp::ExportState::Queued ||
                                                                  j->progress().state == exp::ExportState::Rendering))
            problems.push_back(tr("Another queued export writes the same file."));

    ImGui::Separator();
    ImGui::Text("%s: %s   %s: %s   %s: %s", tr("Duration"), formatTime(range.duration, seq->frameRate).c_str(), tr("Estimated size"),
                bytesText(est).c_str(), tr("Free space"), disk ? bytesText(disk->available).c_str() : "?");
    if (fs::exists(pathFromUtf8(settings_.outputPath), ec))
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", tr("The file exists and will be replaced when the export succeeds."));
    for (const auto& p : problems) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", p.c_str());
    ImGui::BeginDisabled(!problems.empty());
    if (ImGui::Button(tr("Add to Export Queue"), ImVec2(ImGui::GetFontSize() * 12, 0))) {
        exp::ExportSettings s = settings_;
        if (rangeMode_ == 1 && seq->workArea) s.range = seq->workArea;
        app.queueExport(s);
        app.settings().exporting.defaultPreset = settings_.presetId == "custom" ? app.settings().exporting.defaultPreset : settings_.presetId;
        // Next export gets a fresh, unique name.
        initialized_ = false;
    }
    ImGui::EndDisabled();
}

void ExportPanel::drawQueue(App& app) {
    auto jobs = app.exports().jobs();
    if (jobs.empty()) {
        ImGui::TextDisabled("%s", tr("No exports yet."));
        return;
    }
    if (ImGui::BeginTable("##queue", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn(tr("File"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn(tr("Status"));
        ImGui::TableSetupColumn(tr("Progress"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(tr("Speed"));
        ImGui::TableSetupColumn(tr("Remaining"));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11);
        ImGui::TableHeadersRow();
        for (const auto& job : jobs) {
            const exp::ExportProgress p = job->progress();
            ImGui::PushID(static_cast<int>(job->id()));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pathToUtf8(pathFromUtf8(job->settings().outputPath).filename()).c_str());
            tooltip(job->settings().outputPath.c_str());
            ImGui::TableNextColumn();
            const bool failed = p.state == exp::ExportState::Failed;
            if (failed) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", tr(exp::exportStateName(p.state)));
            else ImGui::TextUnformatted(tr(exp::exportStateName(p.state)));
            if (!p.message.empty()) tooltip(p.message.c_str());
            ImGui::TableNextColumn();
            char overlay[64];
            std::snprintf(overlay, sizeof overlay, "%lld / %lld", static_cast<long long>(p.framesDone), static_cast<long long>(p.totalFrames));
            ImGui::ProgressBar(p.fraction(), ImVec2(-1, 0), overlay);
            ImGui::TableNextColumn();
            if (p.fps > 0) ImGui::Text("%.1f fps (%.2fx)", p.fps, p.speed);
            if (!p.encoder.empty()) tooltip(p.encoder.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.state == exp::ExportState::Rendering ? etaText(p.etaSec).c_str() : "");
            ImGui::TableNextColumn();
            const float fh = ImGui::GetFrameHeight();
            const bool running = p.state == exp::ExportState::Rendering || p.state == exp::ExportState::Preparing ||
                                 p.state == exp::ExportState::Paused;
            if (running) {
                if (iconButton("##pause", job->paused() ? Icon::Play : Icon::Pause, job->paused() ? tr("Resume") : tr("Pause"), false, fh)) {
                    if (job->paused()) app.exports().resume(job->id());
                    else app.exports().pause(job->id());
                }
                ImGui::SameLine(0, 2);
            }
            if ((running || p.state == exp::ExportState::Queued) && iconButton("##cancel", Icon::Stop, tr("Cancel"), false, fh))
                app.exports().cancel(job->id());
            if (failed || p.state == exp::ExportState::Cancelled) {
                if (ImGui::SmallButton(tr("Retry"))) app.exports().retry(job->id());
                ImGui::SameLine(0, 2);
            }
            if (p.state == exp::ExportState::Done) {
                if (iconButton("##folder", Icon::Folder, tr("Show in Folder"), false, fh)) revealInFileManager(pathFromUtf8(job->settings().outputPath));
                ImGui::SameLine(0, 2);
                const auto& rep = job->report();
                if (rep.ok) drawIcon(ImGui::GetWindowDrawList(), Icon::Check, ImGui::GetCursorScreenPos() + ImVec2(fh * 0.5f, fh * 0.5f), fh * 0.6f,
                                     IM_COL32(90, 210, 110, 255));
                else drawIcon(ImGui::GetWindowDrawList(), Icon::Warning, ImGui::GetCursorScreenPos() + ImVec2(fh * 0.5f, fh * 0.5f), fh * 0.6f,
                              IM_COL32(240, 190, 60, 255));
                ImGui::Dummy(ImVec2(fh, fh));
                tooltip(rep.summary().c_str());
                ImGui::SameLine(0, 2);
            }
            if (!running && iconButton("##remove", Icon::Close, tr("Remove from list"), false, fh)) app.exports().remove(job->id());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

}  // namespace avc::ui
