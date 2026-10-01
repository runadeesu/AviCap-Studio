// Media browser: imported files with cached thumbnails, search/filter/sort,
// grid and list views, proxy status, offline media and drag to the timeline.

#include <algorithm>
#include <cstring>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

bool containsCaseInsensitive(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    std::string a = hay, b = needle;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a.find(b) != std::string::npos;
}

Icon mediaIcon(const MediaItem& m) {
    switch (m.info.kind) {
    case MediaKind::Audio: return Icon::Music;
    case MediaKind::Image: return Icon::Image;
    default: return Icon::Film;
    }
}

std::string mediaSummary(const MediaItem& m) {
    std::string s;
    if (const auto* v = m.info.primaryVideo()) {
        s += std::to_string(v->width) + "x" + std::to_string(v->height);
        if (m.info.kind == MediaKind::Video) s += "  " + fpsText(v->frameRate) + " fps";
        s += "  " + v->codec;
    }
    if (const auto* a = m.info.primaryAudio()) {
        if (!s.empty()) s += "  |  ";
        s += a->codec + " " + std::to_string(a->sampleRate / 1000) + "kHz " + std::to_string(a->channels) + "ch";
    }
    return s;
}

}  // namespace

void MediaPanel::draw(App& app) {
    const float fh = ImGui::GetFrameHeight();
    if (iconButton("##import", Icon::Plus, tr("Import Media... (Ctrl+I)"), false, fh)) app.importDialog();
    ImGui::SameLine(0, 2);
    if (iconButton("##importdir", Icon::Folder, tr("Import Folder..."), false, fh)) app.importFolderDialog();
    ImGui::SameLine(0, 8);
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - fh * 8.5f));
    ImGui::InputTextWithHint("##search", tr("Search media"), search_, sizeof search_);
    ImGui::SameLine(0, 4);
    const char* filters[] = {tr("All"), tr("Video"), tr("Audio"), tr("Images")};
    ImGui::SetNextItemWidth(fh * 3.5f);
    ImGui::Combo("##filter", &filter_, filters, 4);
    ImGui::SameLine(0, 4);
    if (iconButton("##view", grid_ ? Icon::Image : Icon::Text, grid_ ? tr("List view") : tr("Grid view"), false, fh)) grid_ = !grid_;
    ImGui::SameLine(0, 2);
    const char* sorts[] = {tr("Name"), tr("Imported"), tr("Duration"), tr("Type")};
    ImGui::SetNextItemWidth(fh * 3.0f);
    ImGui::Combo("##sort", &sort_, sorts, 4);

    const size_t offline = app.offlineCount();
    if (offline > 0) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(90, 30, 30, 255));
        ImGui::BeginChild("##offline", ImVec2(0, fh * 1.4f), ImGuiChildFlags_None);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("  %zu %s", offline, tr("media file(s) are offline"));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Relink..."))) app.ui().showRelink = true;
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (app.importsInProgress() > 0) ImGui::TextDisabled("%s", tr("Importing media..."));

    // Filtered, sorted list.
    std::vector<const MediaItem*> items;
    for (const auto& m : app.project().media) {
        if (filter_ == 1 && m->info.kind != MediaKind::Video) continue;
        if (filter_ == 2 && m->info.kind != MediaKind::Audio) continue;
        if (filter_ == 3 && m->info.kind != MediaKind::Image) continue;
        if (!containsCaseInsensitive(m->name, search_)) continue;
        items.push_back(m.get());
    }
    std::stable_sort(items.begin(), items.end(), [&](const MediaItem* a, const MediaItem* b) {
        switch (sort_) {
        case 1: return a->id < b->id;
        case 2: return a->info.duration > b->info.duration;
        case 3: return static_cast<int>(a->info.kind) < static_cast<int>(b->info.kind);
        default: return a->name < b->name;
        }
    });

    ImGui::BeginChild("##mediaitems", ImVec2(0, 0), ImGuiChildFlags_None);
    if (items.empty()) {
        ImGui::Spacing();
        if (app.project().media.empty()) {
            const float w = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 16);
            if (ImGui::Button(tr("+ Import Media"), ImVec2(w, ImGui::GetFrameHeight() * 2.0f))) app.importDialog();
            ImGui::Spacing();
            ImGui::TextWrapped("%s", tr("Drop video, audio or image files here, or click + to import."));
            ImGui::TextDisabled("%s", tr("Supported: MP4, MOV, MKV, WebM, AVI, MP3, WAV, AAC, FLAC, PNG, JPEG and more."));
        } else {
            ImGui::TextWrapped("%s", tr("No media matches the filter."));
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    MediaId contextMedia = kInvalidId;
    auto handleItem = [&](const MediaItem& m) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (io.KeyCtrl) {
                if (!app.selection.media.erase(m.id)) app.selection.media.insert(m.id);
            } else if (io.KeyShift) {
                app.selection.media.insert(m.id);
            } else {
                app.selection.media = {m.id};
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.appendMediaAtPlayhead(m.id);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            if (!app.selection.media.count(m.id)) app.selection.media = {m.id};
            contextMedia = m.id;
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const MediaId id = m.id;
            ImGui::SetDragDropPayload(kPayloadMedia, &id, sizeof id);
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::TextDisabled("%s", tr("Drop on the timeline (Ctrl: insert)"));
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::TextDisabled("%s", mediaSummary(m).c_str());
            ImGui::TextDisabled("%s", m.path.c_str());
            ImGui::EndTooltip();
        }
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (grid_) {
        const float tileW = fh * 6.0f * thumbSize_;
        const float thumbH = tileW * 9.0f / 16.0f;
        const float tileH = thumbH + ImGui::GetTextLineHeightWithSpacing() * 2.0f + 6;
        const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + 6) / (tileW + 6)));
        ImGuiListClipper clipper;
        const int rows = (static_cast<int>(items.size()) + cols - 1) / cols;
        clipper.Begin(rows, tileH + 6);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int c = 0; c < cols; ++c) {
                    const size_t idx = static_cast<size_t>(row * cols + c);
                    if (idx >= items.size()) break;
                    const MediaItem& m = *items[idx];
                    if (c > 0) ImGui::SameLine(0, 6);
                    ImGui::PushID(static_cast<int>(m.id));
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##tile", ImVec2(tileW, tileH));
                    handleItem(m);
                    const bool sel = app.selection.media.count(m.id) != 0;
                    const bool off = app.mediaOffline(m.id);
                    dl->AddRectFilled(p, p + ImVec2(tileW, tileH), sel ? IM_COL32(60, 90, 140, 255) : IM_COL32(40, 41, 46, 255), 4.0f);
                    const ImVec2 t0 = p + ImVec2(3, 3), t1 = p + ImVec2(tileW - 3, 3 + thumbH);
                    dl->AddRectFilled(t0, t1, IM_COL32(15, 15, 17, 255), 3.0f);
                    bool drewThumb = false;
                    if (!off && m.info.hasVideo()) {
                        const Time at = m.info.kind == MediaKind::Image ? Time{0} : Time{m.info.duration.ticks / 3};
                        const ThumbnailRef th = app.assets().thumbnail(m, at, 160);
                        if (th && th.texture != ImTextureID_Invalid && th.height > 0) {
                            // Letterbox inside the tile.
                            const float ar = static_cast<float>(th.width) / static_cast<float>(th.height);
                            float w = t1.x - t0.x, h = w / ar;
                            if (h > t1.y - t0.y) {
                                h = t1.y - t0.y;
                                w = h * ar;
                            }
                            const ImVec2 o = t0 + ImVec2((t1.x - t0.x - w) * 0.5f, (t1.y - t0.y - h) * 0.5f);
                            dl->AddImage(ImTextureRef(th.texture), o, o + ImVec2(w, h));
                            drewThumb = true;
                        }
                    }
                    if (!drewThumb) {
                        drawIcon(dl, mediaIcon(m), (t0 + t1) * 0.5f, thumbH * 0.45f, IM_COL32(150, 150, 160, 255));
                        if (m.info.kind == MediaKind::Audio) {
                            if (auto peaks = app.assets().waveform(m)) {
                                const int cols2 = static_cast<int>(t1.x - t0.x);
                                const auto mm = peaks->query(0, peaks->durationSeconds(), cols2);
                                const float mid = (t0.y + t1.y) * 0.5f, half = (t1.y - t0.y) * 0.45f;
                                for (int i = 0; i < static_cast<int>(mm.size()); ++i)
                                    dl->AddLine(ImVec2(t0.x + static_cast<float>(i), mid - mm[static_cast<size_t>(i)].second * half),
                                                ImVec2(t0.x + static_cast<float>(i), mid - mm[static_cast<size_t>(i)].first * half + 1),
                                                IM_COL32(80, 190, 120, 255));
                            }
                        }
                    }
                    if (off) {
                        dl->AddRectFilled(t0, t1, IM_COL32(120, 20, 20, 140), 3.0f);
                        dl->AddText(t0 + ImVec2(4, 4), IM_COL32(255, 220, 220, 255), tr("Offline"));
                    }
                    // Duration badge
                    if (m.info.kind != MediaKind::Image) {
                        const std::string d = formatDuration(m.info.duration);
                        const ImVec2 ts = ImGui::CalcTextSize(d.c_str());
                        dl->AddRectFilled(ImVec2(t1.x - ts.x - 6, t1.y - ts.y - 2), t1, IM_COL32(0, 0, 0, 170), 3.0f);
                        dl->AddText(ImVec2(t1.x - ts.x - 3, t1.y - ts.y - 1), IM_COL32(230, 230, 230, 255), d.c_str());
                    }
                    const auto ps = app.proxies().status(m.id);
                    if (ps.state == proxy::ProxyState::Running || ps.state == proxy::ProxyState::Queued) {
                        dl->AddRectFilled(ImVec2(t0.x, t1.y - 3), ImVec2(t0.x + (t1.x - t0.x) * ps.progress, t1.y), IM_COL32(240, 180, 60, 255));
                    } else if (ps.state == proxy::ProxyState::Ready) {
                        dl->AddRectFilled(t0, t0 + ImVec2(16, 14), IM_COL32(200, 140, 40, 230), 2.0f);
                        dl->AddText(t0 + ImVec2(4, 0), IM_COL32(0, 0, 0, 255), "P");
                    }
                    dl->PushClipRect(p, p + ImVec2(tileW, tileH), true);
                    dl->AddText(ImVec2(p.x + 4, t1.y + 3), IM_COL32(230, 230, 235, 255), m.name.c_str());
                    const char* kind = m.info.kind == MediaKind::Audio ? tr("Audio") : m.info.kind == MediaKind::Image ? tr("Image") : tr("Video");
                    dl->AddText(ImVec2(p.x + 4, t1.y + 3 + ImGui::GetTextLineHeight()), IM_COL32(140, 140, 150, 255), kind);
                    dl->PopClipRect();
                    ImGui::PopID();
                }
            }
        }
    } else if (ImGui::BeginTable("##medialist", 6,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(tr("Duration"));
        ImGui::TableSetupColumn(tr("Format"));
        ImGui::TableSetupColumn(tr("Audio"));
        ImGui::TableSetupColumn(tr("Proxy"));
        ImGui::TableSetupColumn(tr("Path"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(items.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const MediaItem& m = *items[static_cast<size_t>(i)];
                ImGui::PushID(static_cast<int>(m.id));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool sel = app.selection.media.count(m.id) != 0;
                const bool off = app.mediaOffline(m.id);
                if (off) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
                ImGui::Selectable(m.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                if (off) ImGui::PopStyleColor();
                handleItem(m);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.info.kind == MediaKind::Image ? "-" : formatDuration(m.info.duration).c_str());
                ImGui::TableNextColumn();
                if (const auto* v = m.info.primaryVideo())
                    ImGui::Text("%dx%d %s", v->width, v->height, m.info.kind == MediaKind::Video ? fpsText(v->frameRate).c_str() : "");
                ImGui::TableNextColumn();
                if (const auto* a = m.info.primaryAudio()) ImGui::Text("%d Hz %dch", a->sampleRate, a->channels);
                ImGui::TableNextColumn();
                const auto ps = app.proxies().status(m.id);
                if (ps.state == proxy::ProxyState::Ready) ImGui::TextUnformatted(tr("Ready"));
                else if (ps.state == proxy::ProxyState::Running) ImGui::Text("%.0f%%", ps.progress * 100);
                else if (ps.state == proxy::ProxyState::Queued) ImGui::TextUnformatted(tr("Queued"));
                else if (ps.state == proxy::ProxyState::Failed) ImGui::TextUnformatted(tr("Failed"));
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", m.path.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (contextMedia != kInvalidId) ImGui::OpenPopup("##mediactx");
    if (ImGui::BeginPopup("##mediactx")) {
        const std::set<MediaId> sel = app.selection.media;
        const MediaItem* first = sel.empty() ? nullptr : app.project().findMedia(*sel.begin());
        if (ImGui::MenuItem(tr("Insert at Playhead"), nullptr, false, first != nullptr)) app.appendMediaAtPlayhead(first->id);
        if (ImGui::MenuItem(tr("Create Proxy"))) app.createProxies(sel);
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Reveal in Explorer"), nullptr, false, first != nullptr)) revealInFileManager(pathFromUtf8(first->path));
        if (ImGui::MenuItem(tr("Relink..."), nullptr, false, first != nullptr)) {
            auto files = app.dialogs().openFiles(tr("Locate Media"), {{tr("All files"), "*.*"}}, false);
            if (!files.empty()) app.relinkMedia(first->id, files.front());
        }
        if (ImGui::MenuItem(tr("Properties"), nullptr, false, first != nullptr)) propertiesFor_ = first->id;
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Remove from Project"))) app.removeMedia(sel);
        ImGui::EndPopup();
    }
    if (propertiesFor_ != kInvalidId) ImGui::OpenPopup("##mediaprops");
    if (ImGui::BeginPopup("##mediaprops")) {
        if (const MediaItem* m = app.project().findMedia(propertiesFor_)) {
            ImGui::TextUnformatted(m->name.c_str());
            ImGui::Separator();
            ImGui::Text("%s: %s", tr("Path"), m->path.c_str());
            ImGui::Text("%s: %s", tr("Container"), m->info.container.c_str());
            ImGui::Text("%s: %s", tr("Duration"), formatDuration(m->info.duration).c_str());
            ImGui::Text("%s: %.2f MB", tr("Size"), static_cast<double>(m->fileSize) / (1024.0 * 1024.0));
            for (const auto& v : m->info.video)
                ImGui::BulletText("%s %dx%d %s fps %d-bit %s %s", v.codec.c_str(), v.width, v.height, fpsText(v.frameRate).c_str(),
                                  v.bitDepth, v.colorTransfer.empty() ? "bt709" : v.colorTransfer.c_str(), v.hasAlpha ? "alpha" : "");
            for (const auto& a : m->info.audio) ImGui::BulletText("%s %d Hz %d ch", a.codec.c_str(), a.sampleRate, a.channels);
            if (!m->info.timecode.empty()) ImGui::Text("%s: %s", tr("Timecode"), m->info.timecode.c_str());
        }
        if (ImGui::Button(tr("Close"))) {
            propertiesFor_ = kInvalidId;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        propertiesFor_ = kInvalidId;
    }

    // Empty-area drop target is handled by the main window (OS file drops).
    ImGui::EndChild();
    if (ImGui::IsItemHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0)
        thumbSize_ = std::clamp(thumbSize_ + ImGui::GetIO().MouseWheel * 0.1f, 0.6f, 2.5f);
}

}  // namespace avc::ui
