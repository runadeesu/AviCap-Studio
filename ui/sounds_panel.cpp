// Sounds panel: BGM / sound-effect library with preview, favourites and
// drag & drop to the timeline, plus the legal workflow for sounds from
// Myinstants (open the site in the browser, download there, import here).

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "media/probe.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace fs = std::filesystem;

namespace {

std::string lengthText(double s) {
    if (s < 0) return "--:--";
    char buf[32];
    if (s < 60) std::snprintf(buf, sizeof buf, "%.1f s", s);
    else std::snprintf(buf, sizeof buf, "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    return buf;
}

bool containsCaseless(const std::string& hay, const char* needle) {
    if (!needle[0]) return true;
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

void playButton(App& app, const std::string& path) {
    SoundPreviewer& pv = app.sounds().previewer();
    const bool playingThis = pv.playing() && pv.current() == path;
    if (iconButton("##play", playingThis ? Icon::Stop : Icon::Play, playingThis ? tr("Stop") : tr("Preview"))) {
        if (playingThis) pv.stop();
        else pv.play(path);
    }
}

}  // namespace

void SoundsPanel::draw(App& app) {
    SoundLibrary& lib = app.sounds();
    if (!scanned_) {
        scanned_ = true;
        lib.rescan();
    }
    if (!ImGui::BeginTabBar("##sound_tabs")) return;
    if (ImGui::BeginTabItem(tr("Library"))) {
        drawLibrary(app);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Myinstants")) {
        drawWebSounds(app);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void SoundsPanel::soundRow(App& app, const SoundItem& s, int index) {
    SoundLibrary& lib = app.sounds();
    ImGui::PushID(index);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    playButton(app, s.path);
    ImGui::TableNextColumn();
    const bool music = s.category == "bgm";
    ImGui::Selectable(s.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                                 ImGuiSelectableFlags_AllowDoubleClick);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        lib.markUsed(s.path);
        app.addSoundFiles({s.path}, music);
    }
    if (ImGui::BeginDragDropSource()) {
        std::string payload = std::string(music ? "b" : "s") + s.path;
        ImGui::SetDragDropPayload(kPayloadSoundFile, payload.c_str(), payload.size() + 1);
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered()) {
        std::string tip = s.path;
        if (!s.source.empty()) tip += "\n" + std::string(tr("Source")) + ": " + s.source;
        if (!s.sourceUrl.empty()) tip += "\n" + s.sourceUrl;
        tip += "\n\n";
        tip += tr("Double-click: add at the playhead / drag to the timeline");
        tooltip(tip.c_str());
    }
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem(tr("Add at Playhead"))) {
            lib.markUsed(s.path);
            app.addSoundFiles({s.path}, music);
        }
        if (ImGui::MenuItem(tr("Show in Explorer"))) revealInFileManager(pathFromUtf8(s.path));
        if (!s.sourceUrl.empty() && ImGui::MenuItem(tr("Open Original Page"))) openWithShell(s.sourceUrl);
        ImGui::EndPopup();
    }
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", music ? "BGM" : tr("SFX"));
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(lengthText(s.durationSec).c_str());
    ImGui::TableNextColumn();
    const bool fav = lib.isFavorite(s.path);
    ImGui::PushStyleColor(ImGuiCol_Text, fav ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::SmallButton(fav ? "★" : "☆")) lib.setFavorite(s.path, !fav);
    ImGui::PopStyleColor();
    tooltip(fav ? tr("Remove from favourites") : tr("Add to favourites"));
    ImGui::TableNextColumn();
    if (iconButton("##add", Icon::Plus, tr("Add at Playhead"))) {
        lib.markUsed(s.path);
        app.addSoundFiles({s.path}, music);
    }
    ImGui::PopID();
}

void SoundsPanel::drawLibrary(App& app) {
    SoundLibrary& lib = app.sounds();
    // Toolbar
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 3 - 12));
    ImGui::InputTextWithHint("##search", tr("Search sounds"), search_, sizeof search_);
    ImGui::SameLine();
    if (iconButton("##rescan", Icon::Reset, tr("Rescan the library folders"))) lib.rescan();
    ImGui::SameLine();
    if (iconButton("##folder", Icon::Folder, tr("Add a folder to the library..."))) {
        if (auto f = app.dialogs().pickFolder(tr("Add a folder to the library"))) {
            auto& v = app.settings().sounds.libraryFolders;
            if (std::find(v.begin(), v.end(), *f) == v.end()) v.push_back(*f);
            app.saveSettings();
            lib.rescan();
        }
    }
    const char* filters[] = {tr("All"), "BGM", tr("SFX"), tr("Favourites"), tr("Recent")};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(filters[i], filter_ == i)) filter_ = i;
    }
    if (lib.scanning()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Scanning..."));
    }

    // Items to show
    std::vector<const SoundItem*> rows;
    const auto& recent = app.settings().sounds.recent;
    if (filter_ == 4) {
        for (const auto& p : recent)
            if (const SoundItem* s = lib.find(p); s && containsCaseless(s->name, search_)) rows.push_back(s);
    } else {
        for (const auto& s : lib.items()) {
            if (!containsCaseless(s.name, search_)) continue;
            if (filter_ == 1 && s.category != "bgm") continue;
            if (filter_ == 2 && s.category != "sfx") continue;
            if (filter_ == 3 && !lib.isFavorite(s.path)) continue;
            rows.push_back(&s);
        }
    }

    if (lib.items().empty() && !lib.scanning()) {
        ImGui::Spacing();
        hintText(tr("Put BGM and sound-effect files in the \"AviCap Sounds\" folder (in your Music folder) or add your own folder. Files are used from where they are; nothing is uploaded."));
        if (iconTextButton("##openlib", Icon::Folder, tr("Open the Sounds Folder"))) {
            std::error_code ec;
            const fs::path d = lib.rootFolder();
            fs::create_directories(d / "BGM", ec);
            fs::create_directories(d / "SFX", ec);
            openWithShell(pathToUtf8(d));
        }
        ImGui::SameLine();
        if (iconTextButton("##addbgm", Icon::Music, tr("Add Music..."))) app.addAudioDialog(true);
        return;
    }

    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##sounds", 6, tf, ImVec2(0, std::max(80.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing())))) {
        const float fs = ImGui::GetFontSize();
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 1.6f);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Type"), ImGuiTableColumnFlags_WidthFixed, fs * 3.5f);
        ImGui::TableSetupColumn(tr("Length"), ImGuiTableColumnFlags_WidthFixed, fs * 3.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 1.6f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(rows.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) soundRow(app, *rows[static_cast<size_t>(i)], i);
        ImGui::EndTable();
    }
    char count[64];
    std::snprintf(count, sizeof count, "%zu / %zu", rows.size(), lib.items().size());
    ImGui::TextDisabled("%s %s", count, tr("sounds"));
    if (const std::string err = lib.previewer().lastError(); !err.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", err.c_str());
    }
}

void SoundsPanel::drawWebSounds(App& app) {
    SoundLibrary& lib = app.sounds();
    hintText(tr("Myinstants has no official API, so AviCap Studio does not download from it automatically. Open the site in your browser, download the sounds you like with the site's own download button, then import the downloaded files here."));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", tr("Sounds belong to their uploaders. Check the terms of use before using a sound in your video."));
    ImGui::PopStyleColor();

    // Step 1: browser
    ImGui::SeparatorText(tr("1. Find a sound in your browser"));
    ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x * 0.6f));
    const bool go = ImGui::InputTextWithHint("##websearch", tr("Search words (optional)"), webSearch_, sizeof webSearch_,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (iconTextButton("##openweb", Icon::Search, tr("Open in Browser")) || go) openWithShell(SoundLibrary::myinstantsUrl(webSearch_));
    tooltip(tr("Opens myinstants.com in your web browser"));

    // Step 2: import the downloaded file
    ImGui::SeparatorText(tr("2. Import the downloaded file"));
    ImGui::InputTextWithHint(formRow(tr("Original page")), tr("Page URL for credit (optional)"), pageUrl_, sizeof pageUrl_);
    auto import = [&](const std::string& path, bool music) {
        auto r = lib.importFile(path, music, "Myinstants", pageUrl_);
        if (r) {
            importMessage_ = std::string(tr("Imported")) + ": " + pathToUtf8(pathFromUtf8(*r).filename());
            pageUrl_[0] = 0;
            lib.rescan();
        } else {
            importMessage_ = r.errorMessage();
        }
    };
    if (iconTextButton("##pickfile", Icon::Folder, tr("Choose a File..."))) {
        std::string patterns;
        for (const auto& e : supportedAudioExtensions()) patterns += (patterns.empty() ? "*" : ";*") + e;
        for (const auto& f : app.dialogs().openFiles(tr("Import Sound"), {{tr("Audio files"), patterns}}, true)) import(f, false);
    }
    ImGui::SameLine();
    bool watch = app.settings().sounds.watchDownloads;
    if (ImGui::Checkbox(tr("Show new downloads"), &watch)) {
        app.settings().sounds.watchDownloads = watch;
        app.saveSettings();
        lastDownloadScan_ = -100;
    }
    tooltip(tr("Lists audio files downloaded in the last 24 hours (only while this panel is open). Nothing is imported until you click Import."));
    if (watch && app.timeSec() - lastDownloadScan_ > 3.0) {
        lastDownloadScan_ = app.timeSec();
        downloads_ = lib.recentDownloads(24.0);
    }
    if (!importMessage_.empty()) ImGui::TextDisabled("%s", importMessage_.c_str());

    if (watch) {
        if (downloads_.empty()) {
            ImGui::TextDisabled("%s", tr("No audio files downloaded in the last 24 hours."));
        } else if (ImGui::BeginTable("##downloads", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            const float fs = ImGui::GetFontSize();
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 1.6f);
            ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            for (size_t i = 0; i < downloads_.size() && i < 50; ++i) {
                const SoundItem& d = downloads_[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                playButton(app, d.path);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(d.name.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(tr("Import as SFX"))) import(d.path, false);
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(tr("Import as BGM"))) import(d.path, true);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Imported sounds are copied to AviCap Sounds\\SFX\\Myinstants. The downloaded file is not moved or deleted."));
}

}  // namespace avc::ui
