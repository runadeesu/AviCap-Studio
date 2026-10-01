// Diagnostics: system and GPU information, live performance counters,
// background jobs, the in-memory log and cache usage/maintenance.

#include <algorithm>
#include <cstring>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "decode/video_decoder.h"
#include "encode/encoder.h"
#include "ui/panels.h"
#include "ui/widgets.h"

#include "avicap_build_info.h"

namespace avc::ui {

namespace {
std::string mb(uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    return buf;
}
}  // namespace

void DiagnosticsPanel::draw(App& app) {
    if (!ImGui::BeginTabBar("##diag")) return;
    if (ImGui::BeginTabItem(tr("Performance"))) {
        const auto ps = app.preview().stats();
        const auto f = app.preview().latest();
        const auto fc = app.preview().frames().cache().stats();
        const auto mem = queryMemoryStatus();
        if (ImGui::BeginTable("##perf", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            auto row = [](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(k);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(v.c_str());
            };
            char buf[128];
            std::snprintf(buf, sizeof buf, "%.1f fps (%.1f ms)", app.stats.uiFps, app.stats.uiFrameMs);
            row(tr("UI frame rate"), buf);
            std::snprintf(buf, sizeof buf, "%.1f ms avg, last %.1f ms", ps.avgRenderMs, f.renderMs);
            row(tr("Preview render time"), buf);
            std::snprintf(buf, sizeof buf, "%dx%d (1/%d), auto 1/%d", f.width, f.height, f.divisor, ps.autoDivisor);
            row(tr("Preview resolution"), buf);
            row(tr("Frames rendered / dropped"), std::to_string(ps.rendered) + " / " + std::to_string(ps.superseded));
            row(tr("Decoder"), ps.decoder.empty() ? "-" : ps.decoder);
            row(tr("Open decoders"), std::to_string(ps.openDecoders));
            row(tr("Frame cache"), mb(fc.bytes) + " / " + mb(fc.budget) + " (" + std::to_string(fc.frames) + " frames)");
            row(tr("GPU textures"), mb(app.device().textureMemoryInUse()));
            if (app.device().videoMemoryBudget() > 0)
                row(tr("Video memory"), mb(app.device().videoMemoryUsage()) + " / " + mb(app.device().videoMemoryBudget()));
            row(tr("Thumbnail textures"), std::to_string(app.assets().textureCount()));
            row(tr("Background asset jobs"), std::to_string(app.assets().pendingJobs()));
            row(tr("Audio output"), app.playback().output().deviceName() + " (" + std::to_string(app.playback().output().sampleRate()) + " Hz)");
            row(tr("Audio underruns"), std::to_string(app.playback().underruns()));
            row(tr("Process memory"), mb(mem.processWorkingSet) + " (" + tr("private") + " " + mb(mem.processPrivateBytes) + ")");
            row(tr("System memory"), mb(mem.availablePhysical) + " " + tr("free of") + " " + mb(mem.totalPhysical));
            ImGui::EndTable();
        }
        if (app.stats.lowMemory) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", tr("Low memory mode is active."));
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tr("System"))) {
        const auto& di = app.device().info();
        ImGui::Text("AviCap Studio %s (%s, %s)", AVICAP_VERSION_STRING, AVICAP_BUILD_DATE, AVICAP_COMPILER);
        ImGui::Text("%s: %s", tr("OS"), osDescription().c_str());
        ImGui::Text("%s: %s (%u %s)", tr("CPU"), cpuDescription().c_str(), hardwareThreads(), tr("threads"));
        ImGui::Text("%s: %s - %s (%s)", tr("GPU"), di.name.c_str(), di.adapter.c_str(), di.vendor.c_str());
        if (di.dedicatedVideoMemory) ImGui::Text("%s: %s", tr("Dedicated video memory"), mb(di.dedicatedVideoMemory).c_str());
        if (di.software) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("Software rendering is active (slow)."));
        ImGui::SeparatorText(tr("Hardware decoding"));
        static std::string hw = hardwareDecodeSummary();
        ImGui::TextWrapped("%s", hw.c_str());
        ImGui::SeparatorText(tr("Encoders"));
        static std::string encs = enc::EncoderCatalog::instance().summary();
        ImGui::TextWrapped("%s", encs.c_str());
        ImGui::SeparatorText(tr("Folders"));
        ImGui::Text("%s: %s", tr("Data"), pathToUtf8(app.dataDir()).c_str());
        ImGui::Text("%s: %s", tr("Logs"), pathToUtf8(logsDir()).c_str());
        ImGui::Text("%s: %s", tr("Cache"), pathToUtf8(app.assets().store().root()).c_str());
        if (ImGui::Button(tr("Open Logs Folder"))) openWithShell(pathToUtf8(logsDir()));
        ImGui::SameLine();
        if (ImGui::Button(tr("Open Data Folder"))) openWithShell(pathToUtf8(app.dataDir()));
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tr("Jobs"))) {
        auto jobs = Jobs::allActiveJobs();
        if (jobs.empty()) ImGui::TextDisabled("%s", tr("No background jobs."));
        for (const auto& j : jobs) {
            ImGui::PushID(j.get());
            ImGui::ProgressBar(j->progress.load(), ImVec2(ImGui::GetFontSize() * 8, 0));
            ImGui::SameLine();
            ImGui::Text("%s %s", j->name.c_str(), j->message().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Cancel"))) j->token.cancel();
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tr("Log"))) {
        const char* levels[] = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::Combo("##lvl", &minLevel_, levels, 6);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputTextWithHint("##filter", tr("Filter"), filter_, sizeof filter_);
        ImGui::SameLine();
        ImGui::Checkbox(tr("Auto-scroll"), &autoScroll_);
        ImGui::SameLine();
        const bool copy = ImGui::Button(tr("Copy"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Open Logs Folder"))) openWithShell(pathToUtf8(logsDir()));
        ImGui::BeginChild("##logtext", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        std::string copied;
        for (const auto& r : log::recent(2000)) {
            if (static_cast<int>(r.level) < minLevel_) continue;
            const std::string line = log::formatRecord(r);
            if (filter_[0] && line.find(filter_) == std::string::npos) continue;
            ImVec4 c = r.level >= LogLevel::Error ? ImVec4(1, 0.45f, 0.4f, 1) : r.level == LogLevel::Warning ? ImVec4(1, 0.8f, 0.35f, 1)
                                                                                                         : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(c, "%s", line.c_str());
            if (copy) copied += line + "\n";
        }
        if (copy) ImGui::SetClipboardText(copied.c_str());
        if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tr("Cache"))) {
        static std::vector<cache::CategoryUsage> usage;
        static double lastScan = -100;
        if (app.timeSec() - lastScan > 5.0) {
            usage = app.assets().store().usage();
            lastScan = app.timeSec();
        }
        uint64_t total = 0;
        for (const auto& u : usage) total += u.bytes;
        ImGui::Text("%s: %s (%s %.0f GB)", tr("Total"), mb(total).c_str(), tr("limit"), app.settings().cache.maxGB);
        hintText(tr("The cache only holds derived data (thumbnails, waveforms, proxies, shaders). Original media is never stored here."));
        for (const auto& u : usage) {
            ImGui::PushID(u.category.c_str());
            ImGui::BulletText("%s: %s (%llu %s)", u.category.c_str(), mb(u.bytes).c_str(), static_cast<unsigned long long>(u.files), tr("files"));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Clear"))) {
                app.assets().store().clearCategory(u.category);
                lastScan = -100;
            }
            ImGui::PopID();
        }
        if (ImGui::Button(tr("Clean Up Now"))) {
            const uint64_t maxBytes = static_cast<uint64_t>(app.settings().cache.maxGB * 1024.0 * 1024.0 * 1024.0);
            const uint64_t freed = app.assets().store().cleanup(maxBytes, app.settings().cache.maxAgeDays);
            app.notify(LogLevel::Info, std::string(tr("Cache cleanup freed")) + " " + mb(freed));
            lastScan = -100;
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

}  // namespace avc::ui
