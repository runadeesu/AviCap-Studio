// Video scopes computed from a small RGBA copy of the program frame (made
// on the preview thread): luma waveform, RGB parade, vectorscope, histogram.

#include <algorithm>
#include <cmath>

#include "core/i18n.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

std::shared_ptr<cache::Image> buildScope(const cache::Image& src, int mode) {
    auto out = std::make_shared<cache::Image>();
    const int W = mode == 2 ? 256 : mode == 3 ? 256 : src.width;
    const int H = mode == 2 ? 256 : mode == 3 ? 128 : 256;
    out->width = W;
    out->height = H;
    std::vector<uint32_t> acc(static_cast<size_t>(W) * static_cast<size_t>(H) * 3, 0);
    auto add = [&](int x, int y, int ch, uint32_t v = 1) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        acc[(static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 3 + static_cast<size_t>(ch)] += v;
    };
    const int sw = src.width, sh = src.height;
    std::vector<uint32_t> hist(256 * 4, 0);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const uint8_t* p = &src.rgba[(static_cast<size_t>(y) * static_cast<size_t>(sw) + static_cast<size_t>(x)) * 4];
            const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
            const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            switch (mode) {
            case 0: {
                const int yy = H - 1 - static_cast<int>(luma * (H - 1));
                add(x, yy, 0);
                add(x, yy, 1);
                add(x, yy, 2);
                break;
            }
            case 1: {
                const int third = W / 3;
                const int px = x / 3;
                add(px, H - 1 - static_cast<int>(r * (H - 1)), 0);
                add(third + px, H - 1 - static_cast<int>(g * (H - 1)), 1);
                add(2 * third + px, H - 1 - static_cast<int>(b * (H - 1)), 2);
                break;
            }
            case 2: {
                // BT.709 chroma; 75% saturation reaches ~0.85 of the radius.
                const float cb = (b - luma) / 1.8556f, cr = (r - luma) / 1.5748f;
                const int vx = 128 + static_cast<int>(cb * 2.0f * 120.0f);
                const int vy = 128 - static_cast<int>(cr * 2.0f * 120.0f);
                add(vx, vy, 0, 1);
                add(vx, vy, 1, 1);
                add(vx, vy, 2, 1);
                break;
            }
            default:
                ++hist[p[0]];
                ++hist[256 + p[1]];
                ++hist[512 + p[2]];
                ++hist[768 + std::min(255, static_cast<int>(luma * 255.0f))];
                break;
            }
        }
    out->rgba.assign(static_cast<size_t>(W) * static_cast<size_t>(H) * 4, 0);
    if (mode == 3) {
        uint32_t peak = 1;
        for (int i = 0; i < 1024; ++i) peak = std::max(peak, hist[static_cast<size_t>(i)]);
        for (int x = 0; x < W; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                const float v = std::sqrt(static_cast<float>(hist[static_cast<size_t>(ch * 256 + x)]) / static_cast<float>(peak));
                const int top = H - 1 - static_cast<int>(v * (H - 1));
                for (int y = top; y < H; ++y) {
                    uint8_t* o = &out->rgba[(static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 4];
                    o[ch] = static_cast<uint8_t>(std::min(255, o[ch] + 150));
                    o[3] = 255;
                }
            }
        return out;
    }
    // Map counts to intensity with a soft response so faint traces stay visible.
    const float norm = 6.0f / std::max(1.0f, static_cast<float>(sw * sh) / static_cast<float>(W * (mode == 2 ? 24 : 2)));
    for (size_t i = 0; i < static_cast<size_t>(W) * static_cast<size_t>(H); ++i)
        for (int ch = 0; ch < 3; ++ch) {
            const float v = 1.0f - std::exp(-static_cast<float>(acc[i * 3 + static_cast<size_t>(ch)]) * norm);
            uint8_t& o = out->rgba[i * 4 + static_cast<size_t>(ch)];
            float tint = 1.0f;
            if (mode == 1) tint = 1.0f;  // parade channels already separated
            o = static_cast<uint8_t>(std::clamp(v * 255.0f * tint, 0.0f, 255.0f));
            if (mode == 0 || mode == 2) {  // white/green-ish trace
                if (ch == 1) o = static_cast<uint8_t>(std::min(255.0f, v * 255.0f));
                else o = static_cast<uint8_t>(v * 200.0f);
            }
        }
    for (size_t i = 0; i < static_cast<size_t>(W) * static_cast<size_t>(H); ++i) out->rgba[i * 4 + 3] = 255;
    return out;
}

}  // namespace

void ScopesPanel::draw(App& app) {
    const char* modes[] = {tr("Waveform"), tr("RGB Parade"), tr("Vectorscope"), tr("Histogram")};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::Combo("##scope", &mode_, modes, 4)) source_.reset();
    const PreviewFrame f = app.preview().latest();
    if (!f.scopes) {
        ImGui::TextDisabled("%s", tr("Waiting for a frame..."));
        return;
    }
    if (f.scopes != source_ || !rendered_) {
        source_ = f.scopes;
        rendered_ = buildScope(*f.scopes, mode_);
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float aspect = static_cast<float>(rendered_->width) / static_cast<float>(rendered_->height);
    ImVec2 size(avail.x, avail.x / aspect);
    if (size.y > avail.y) size = ImVec2(avail.y * aspect, avail.y);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImTextureID tex = app.assets().dynamicTexture("scopes", rendered_);
    if (tex != ImTextureID_Invalid) ImGui::Image(ImTextureRef(tex), size);
    else ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 grid = IM_COL32(200, 200, 120, 90);
    if (mode_ == 0 || mode_ == 1) {
        for (int pct : {0, 25, 50, 75, 100}) {
            const float y = p0.y + size.y * (1.0f - static_cast<float>(pct) / 100.0f);
            dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), grid);
            char buf[8];
            std::snprintf(buf, sizeof buf, "%d", pct);
            dl->AddText(ImVec2(p0.x + 2, y - ImGui::GetFontSize()), grid, buf);
        }
    } else if (mode_ == 2) {
        const ImVec2 c = p0 + size * 0.5f;
        dl->AddCircle(c, size.x * 0.47f, grid, 64);
        dl->AddLine(c - ImVec2(size.x * 0.47f, 0), c + ImVec2(size.x * 0.47f, 0), grid);
        dl->AddLine(c - ImVec2(0, size.y * 0.47f), c + ImVec2(0, size.y * 0.47f), grid);
        // Skin tone indicator (~123 degrees in the Cb/Cr plane).
        const float a = -2.15f;
        dl->AddLine(c, c + ImVec2(std::cos(a), std::sin(a)) * (size.x * 0.47f), IM_COL32(230, 160, 120, 120), 1.5f);
    }
}

}  // namespace avc::ui
