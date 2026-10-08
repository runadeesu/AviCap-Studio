#include "timeline/subtitles.h"

#include <algorithm>
#include <cstdio>

#include "core/strings.h"
#include "timeline/edit_ops.h"

namespace avc::subs {

namespace {

// "HH:MM:SS,mmm", "HH:MM:SS.mmm", "MM:SS.mmm" (VTT short form), "H:MM:SS.m".
std::optional<Time> parseTimestamp(std::string_view s) {
    s = std::string_view(s.data(), s.size());
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    std::vector<std::string> parts = split(s, ':', false);
    if (parts.size() < 2 || parts.size() > 3) return std::nullopt;
    auto toInt = [](const std::string& p, int64_t& out) {
        if (p.empty() || p.size() > 6) return false;
        out = 0;
        for (char c : p) {
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        return true;
    };
    int64_t h = 0, m = 0;
    if (parts.size() == 3 && !toInt(parts[0], h)) return std::nullopt;
    if (!toInt(parts[parts.size() - 2], m)) return std::nullopt;
    std::string sec = parts.back();
    std::replace(sec.begin(), sec.end(), ',', '.');
    const size_t dot = sec.find('.');
    int64_t whole = 0, ms = 0;
    if (!toInt(sec.substr(0, dot), whole)) return std::nullopt;
    if (dot != std::string::npos) {
        std::string frac = sec.substr(dot + 1);
        if (frac.empty() || frac.size() > 9) return std::nullopt;
        for (char c : frac)
            if (c < '0' || c > '9') return std::nullopt;
        while (frac.size() < 3) frac += '0';
        ms = std::stoll(frac.substr(0, 3));
    }
    if (m >= 60 || whole >= 60) return std::nullopt;
    return Time::fromMilliseconds(((h * 60 + m) * 60 + whole) * 1000 + ms);
}

// "start --> end [settings]"
bool parseTiming(std::string_view line, Time& a, Time& b) {
    const size_t arrow = line.find("-->");
    if (arrow == std::string_view::npos) return false;
    auto start = parseTimestamp(line.substr(0, arrow));
    std::string_view rest = line.substr(arrow + 3);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    const size_t sp = rest.find_first_of(" \t");
    auto end = parseTimestamp(rest.substr(0, sp));
    if (!start || !end) return false;
    a = *start;
    b = *end;
    return true;
}

std::string decodeEntities(std::string s) {
    static const std::pair<const char*, const char*> ents[] = {{"&amp;", "&"},   {"&lt;", "<"},     {"&gt;", ">"},   {"&quot;", "\""},
                                                               {"&#39;", "'"},   {"&apos;", "'"},   {"&nbsp;", " "}, {"&lrm;", ""},
                                                               {"&rlm;", ""}};
    for (const auto& [from, to] : ents) s = replaceAll(std::move(s), from, to);
    return s;
}

// Removes <i>, </b>, <font ...>, <c.yellow>, <v Speaker>, <00:00:01.000> and {\an8}.
std::string stripTags(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '<') {
            const size_t close = in.find('>', i);
            if (close != std::string::npos && close - i < 200) {
                i = close;
                continue;
            }
        } else if (in[i] == '{' && i + 1 < in.size() && in[i + 1] == '\\') {
            const size_t close = in.find('}', i);
            if (close != std::string::npos) {
                i = close;
                continue;
            }
        }
        out += in[i];
    }
    return decodeEntities(out);
}

std::string formatTs(Time t, char sep) {
    int64_t ms = (t.ticks * 1000 + kTicksPerSecond / 2) / kTicksPerSecond;  // rounded
    if (ms < 0) ms = 0;
    const int64_t h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, f = ms % 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld%c%03lld", static_cast<long long>(h), static_cast<long long>(m),
                  static_cast<long long>(s), sep, static_cast<long long>(f));
    return buf;
}

}  // namespace

std::optional<Format> formatFromExtension(std::string_view p) {
    const std::string s = toLower(p);
    if (endsWith(s, "srt")) return Format::Srt;
    if (endsWith(s, "vtt")) return Format::Vtt;
    return std::nullopt;
}

Result<std::vector<Cue>> parse(std::string_view text) {
    std::string norm = replaceAll(std::string(text), "\r\n", "\n");
    std::replace(norm.begin(), norm.end(), '\r', '\n');
    std::erase(norm, '\0');  // stray NULs (padded UTF-16 files)
    if (startsWith(norm, "\xEF\xBB\xBF")) norm.erase(0, 3);
    const std::vector<std::string> lines = split(norm, '\n', false);
    const bool vtt = !lines.empty() && startsWith(trim(lines[0]), "WEBVTT");

    std::vector<Cue> cues;
    size_t i = vtt ? 1 : 0;
    while (i < lines.size()) {
        // Skip blank lines between blocks.
        while (i < lines.size() && trim(lines[i]).empty()) ++i;
        if (i >= lines.size()) break;
        // Collect the block.
        std::vector<std::string> block;
        while (i < lines.size() && !trim(lines[i]).empty()) block.push_back(lines[i++]);
        if (vtt && (startsWith(block[0], "NOTE") || startsWith(block[0], "STYLE") || startsWith(block[0], "REGION"))) continue;
        // Timing line is the first or second line (after a number / identifier).
        size_t t = 0;
        Time a, b;
        bool ok = parseTiming(block[0], a, b);
        if (!ok && block.size() > 1) {
            ok = parseTiming(block[1], a, b);
            t = 1;
        }
        if (!ok) continue;  // stray text: ignore like players do
        std::string txt;
        for (size_t k = t + 1; k < block.size(); ++k) {
            std::string line = trim(stripTags(block[k]));
            if (line.empty()) continue;
            if (!txt.empty()) txt += '\n';
            txt += line;
        }
        if (b <= a || txt.empty()) continue;
        cues.push_back({a, b, std::move(txt)});
    }
    if (cues.empty()) return Result<std::vector<Cue>>::error("no subtitles found in the file");
    std::stable_sort(cues.begin(), cues.end(), [](const Cue& x, const Cue& y) { return x.start < y.start; });
    return cues;
}

std::string toSrt(const std::vector<Cue>& cues) {
    std::string out;
    int n = 0;
    for (const auto& c : cues) {
        out += std::to_string(++n) + "\r\n";
        out += formatTs(c.start, ',') + " --> " + formatTs(c.end, ',') + "\r\n";
        out += replaceAll(c.text, "\n", "\r\n") + "\r\n\r\n";
    }
    return out;
}

std::string toVtt(const std::vector<Cue>& cues) {
    std::string out = "WEBVTT\n\n";
    for (const auto& c : cues) {
        std::string txt = replaceAll(replaceAll(replaceAll(c.text, "&", "&amp;"), "<", "&lt;"), ">", "&gt;");
        // "-->" may not appear in cue text.
        txt = replaceAll(txt, "-->", "--&gt;");
        out += formatTs(c.start, '.') + " --> " + formatTs(c.end, '.') + "\n" + txt + "\n\n";
    }
    return out;
}

std::string serialize(const std::vector<Cue>& cues, Format f) { return f == Format::Srt ? toSrt(cues) : toVtt(cues); }

std::vector<Cue> cuesFromSequence(const Sequence& seq, std::optional<TimeRange> range) {
    std::vector<Cue> out;
    for (const auto& t : seq.tracks) {
        if (t->kind != TrackKind::Subtitle || t->hidden) continue;
        for (const auto& c : t->clips) {
            if (c->kind != ClipKind::Subtitle || !c->enabled || c->text.empty()) continue;
            Time a = c->start, b = c->end();
            if (range) {
                a = maxTime(a, range->start);
                b = minTime(b, range->end());
                if (b <= a) continue;
                a = a - range->start;
                b = b - range->start;
            }
            out.push_back({a, b, c->text});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Cue& x, const Cue& y) { return x.start < y.start; });
    return out;
}

Result<int> addCues(SequenceEditor& e, const std::vector<Cue>& cues, Time offset, const ImportStyle& style) {
    const float fontSize = style.fontSize > 0 ? style.fontSize : std::max(36.0f, static_cast<float>(e.seq().height) * 0.05f);
    // Greedy lane assignment: overlapping cues go to extra tracks.
    std::vector<std::pair<TrackId, Time>> lanes;  // track, end of its last clip
    std::vector<std::pair<TrackId, Clip>> placed;
    for (const Cue& cue : cues) {
        const Time a = (cue.start + offset).snappedToFrame(e.seq().frameRate);
        Time b = (cue.end + offset).snappedToFrame(e.seq().frameRate);
        if (b <= a) b = a + Time::fromFrames(1, e.seq().frameRate);
        if (a < Time{0}) continue;
        int lane = -1;
        for (size_t l = 0; l < lanes.size(); ++l)
            if (lanes[l].second <= a) {
                lane = static_cast<int>(l);
                break;
            }
        if (lane < 0) {
            auto tr = edit::addTrack(e, TrackKind::Subtitle, lanes.empty() ? style.trackName : style.trackName + " " + std::to_string(lanes.size() + 1));
            if (!tr) return tr.status();
            lanes.emplace_back(*tr, Time{0});
            lane = static_cast<int>(lanes.size()) - 1;
        }
        Clip c = makeTextClip(cue.text, a, b - a, ClipKind::Subtitle);
        c.textStyle.fontSize = fontSize;
        c.textStyle.fontWeight = 600;
        c.textParams.setStatic("strokeColor", pv(0, 0, 0, 1));
        c.textParams.setStatic("strokeWidth", pv(fontSize * 0.08f));
        lanes[static_cast<size_t>(lane)].second = b;
        placed.emplace_back(lanes[static_cast<size_t>(lane)].first, std::move(c));
    }
    if (placed.empty()) return Result<int>::error("no subtitles to add");
    // Track indices shift while tracks are added: resolve them at the end.
    std::vector<edit::Placement> fixed;
    for (auto& [track, clip] : placed) fixed.push_back({e.seq().trackIndex(track), std::move(clip)});
    const int n = static_cast<int>(fixed.size());
    if (auto st = edit::placeClips(e, std::move(fixed), edit::PlaceMode::Overwrite); !st) return st;
    return n;
}

}  // namespace avc::subs
