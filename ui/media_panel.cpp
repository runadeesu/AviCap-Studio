// Media browser: imported files with cached thumbnails, search/filter/sort,
// grid and list views, proxy status, offline media and drag to the timeline.

#include <algorithm>
#include <cstring>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "ui/panels.h"
#include "ui/style.h"
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
    // Row 1: import + search
    if (primaryButton(tr("+ Import"), ImVec2(0, fh))) app.importDialog();
    tooltip(tr("Import Media... (Ctrl+I)"));
    ImGui::SameLine(0, 4);
    if (iconButton("##importdir", Icon::Folder, tr("Import Folder..."), false, fh)) app.importFolderDialog();
    ImGui::SameLine(0, 8);
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##search", tr("Search media"), search_, sizeof search_);
    // Row 2: type chips, sort and view mode on the right.
    const char* filters[] = {tr("All"), tr("Video"), tr("Audio"), tr("Images")};
    const float rightW = fh;
    const float rowRight = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    for (int i = 0; i < 4; ++i) {
        if (i) {
            const float w = ImGui::CalcTextSize(filters[i]).x + ImGui::GetStyle().FramePadding.x * 1.8f;
            if (ImGui::GetItemRectMax().x + 6 + w < rowRight - rightW - 8) ImGui::SameLine(0, 6);
        }
        if (chipButton(filters[i], filter_ == i)) filter_ = i;
    }
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(rowRight - rightW, ImGui::GetCursorScreenPos().y - 3));
    if (iconButton("##viewopts", Icon::More, tr("Sort and view"), false, fh)) ImGui::OpenPopup("##mediaopts");
    if (ImGui::BeginPopup("##mediaopts")) {
        ImGui::TextDisabled("%s", tr("Sort"));
        const char* sorts[] = {tr("Name"), tr("Date Imported"), tr("Duration"), tr("Type")};
        for (int i = 0; i < 4; ++i)
            if (ImGui::MenuItem(sorts[i], nullptr, sort_ == i)) sort_ = i;
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Grid view"), nullptr, grid_)) grid_ = true;
        if (ImGui::MenuItem(tr("List view"), nullptr, !grid_)) grid_ = false;
        ImGui::EndPopup();
    }
    ImGui::Spacing();

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
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const ImVec2 size(avail.x, std::max(ImGui::GetFontSize() * 12.0f, avail.y - 4));
            if (dropZone("##dropzone", size, Icon::Plus, tr("Import Media"), tr("Click here, or drop video, audio and image files"),
                         tr("Supported: MP4, MOV, MKV, WebM, AVI, MP3, WAV, AAC, FLAC, PNG, JPEG and more.")))
                app.importDialog();
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
        // Responsive grid: at least two columns, tiles fill the width.
        const float gap = 8.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float minW = fh * 4.6f * thumbSize_;
        const int cols = std::max(avail > minW * 2 ? 2 : 1, static_cast<int>((avail + gap) / (minW + gap)));
        const float tileW = (avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float thumbH = std::round(tileW * 9.0f / 16.0f);
        const float lineH = ImGui::GetTextLineHeight();
        const float tileH = thumbH + lineH + 8;
        const float fs = ImGui::GetFontSize();
        ImGuiListClipper clipper;
        const int rows = (static_cast<int>(items.size()) + cols - 1) / cols;
        clipper.Begin(rows, tileH + gap);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int c = 0; c < cols; ++c) {
                    const size_t idx = static_cast<size_t>(row * cols + c);
                    if (idx >= items.size()) break;
                    const MediaItem& m = *items[idx];
                    if (c > 0) ImGui::SameLine(0, gap);
                    ImGui::PushID(static_cast<int>(m.id));
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##tile", ImVec2(tileW, tileH));
                    const bool hov = ImGui::IsItemHovered();
                    handleItem(m);
                    const bool sel = app.selection.media.count(m.id) != 0;
                    const bool off = app.mediaOffline(m.id);
                    const float rr = 8.0f;
                    const ImVec2 t0 = p, t1 = p + ImVec2(tileW, thumbH);
                    const bool audioOnly = m.info.kind == MediaKind::Audio;
                    if (audioOnly) dl->AddRectFilledMultiColor(t0, t1, IM_COL32(16, 70, 56, 255), IM_COL32(20, 60, 80, 255), IM_COL32(10, 32, 30, 255),
                                                               IM_COL32(12, 28, 40, 255));
                    else dl->AddRectFilled(t0, t1, IM_COL32(8, 9, 12, 255), rr);
                    bool drewThumb = false;
                    if (!off && m.info.hasVideo()) {
                        const Time at = m.info.kind == MediaKind::Image ? Time{0} : Time{m.info.duration.ticks / 3};
                        const ThumbnailRef th = app.assets().thumbnail(m, at, 160);
                        if (th && th.texture != ImTextureID_Invalid && th.height > 0) {
                            // Cover fit: crop the thumbnail to the tile's 16:9 box.
                            const float ar = static_cast<float>(th.width) / static_cast<float>(th.height);
                            const float boxAr = tileW / thumbH;
                            ImVec2 uv0(0, 0), uv1(1, 1);
                            if (ar > boxAr) {
                                const float k = boxAr / ar;
                                uv0.x = (1 - k) * 0.5f;
                                uv1.x = 1 - uv0.x;
                            } else {
                                const float k = ar / boxAr;
                                uv0.y = (1 - k) * 0.5f;
                                uv1.y = 1 - uv0.y;
                            }
                            dl->AddImageRounded(ImTextureRef(th.texture), t0, t1, uv0, uv1, IM_COL32(255, 255, 255, 255), rr);
                            drewThumb = true;
                        }
                    }
                    if (!drewThumb) {
                        if (audioOnly) {
                            if (auto peaks = app.assets().waveform(m)) {
                                const int cols2 = static_cast<int>((t1.x - t0.x - 12) / 2);
                                const auto mm = peaks->query(0, peaks->durationSeconds(), std::max(1, cols2));
                                const float mid = (t0.y + t1.y) * 0.5f, half = (t1.y - t0.y) * 0.36f;
                                for (int i = 0; i < static_cast<int>(mm.size()); ++i) {
                                    const float x = t0.x + 6 + static_cast<float>(i) * 2.0f;
                                    dl->AddRectFilled(ImVec2(x, mid - mm[static_cast<size_t>(i)].second * half),
                                                      ImVec2(x + 1.4f, mid - mm[static_cast<size_t>(i)].first * half + 1), IM_COL32(72, 220, 160, 230));
                                }
                            } else {
                                drawIcon(dl, Icon::Music, (t0 + t1) * 0.5f, thumbH * 0.4f, IM_COL32(120, 220, 180, 200));
                            }
                        } else {
                            drawIcon(dl, mediaIcon(m), (t0 + t1) * 0.5f, thumbH * 0.4f, pal::kTextFaint);
                        }
                    }
                    if (off) {
                        dl->AddRectFilled(t0, t1, IM_COL32(120, 20, 20, 150), rr);
                        dl->AddText(t0 + ImVec2(6, 6), IM_COL32(255, 220, 220, 255), tr("Offline"));
                    }
                    // Duration pill (bottom right)
                    if (m.info.kind != MediaKind::Image) {
                        const std::string d = formatDuration(m.info.duration);
                        const float small = fs * 0.78f;
                        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0.0f, d.c_str());
                        const ImVec2 b1 = t1 - ImVec2(5, 5), b0 = b1 - ts - ImVec2(10, 4);
                        dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 170), (b1.y - b0.y) * 0.5f);
                        dl->AddText(ImGui::GetFont(), small, b0 + ImVec2(5, 2), IM_COL32(240, 240, 240, 255), d.c_str());
                    }
                    const auto ps = app.proxies().status(m.id);
                    if (ps.state == proxy::ProxyState::Running || ps.state == proxy::ProxyState::Queued) {
                        dl->AddRectFilled(ImVec2(t0.x + 4, t1.y - 4), ImVec2(t0.x + 4 + (t1.x - t0.x - 8) * ps.progress, t1.y - 2), pal::kWarning, 1.0f);
                    } else if (ps.state == proxy::ProxyState::Ready) {
                        const ImVec2 b0 = t0 + ImVec2(5, 5);
                        dl->AddRectFilled(b0, b0 + ImVec2(fs * 1.1f, fs * 1.0f), pal::withAlpha(pal::kWarning, 0.9f), 4.0f);
                        dl->AddText(ImGui::GetFont(), fs * 0.75f, b0 + ImVec2(fs * 0.3f, fs * 0.1f), IM_COL32(20, 20, 20, 255), "P");
                    }
                    // Selection / hover ring
                    if (sel) dl->AddRect(t0 - ImVec2(1, 1), t1 + ImVec2(1, 1), pal::kAccentHover, rr + 1, 2.0f);
                    else if (hov) dl->AddRect(t0, t1, IM_COL32(255, 255, 255, 70), rr, 1.0f);
                    // Name (one line, with a small type icon)
                    const float ny = t1.y + 4;
                    drawIcon(dl, mediaIcon(m), ImVec2(p.x + fs * 0.45f, ny + lineH * 0.5f), fs * 0.62f, sel ? pal::kAccentText : pal::kTextFaint);
                    dl->PushClipRect(ImVec2(p.x, ny), ImVec2(p.x + tileW, ny + lineH + 2), true);
                    dl->AddText(ImVec2(p.x + fs * 1.0f, ny), sel ? pal::kText : pal::kTextMuted, m.name.c_str());
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
