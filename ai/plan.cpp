#include "ai/plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <regex>

#include "core/i18n.h"
#include "core/strings.h"
#include "effects/effects.h"
#include "timeline/edit_ops.h"

namespace avc::ai {

namespace {

double num(const Json& j, const char* key, double def) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<double>();
}

std::string str(const Json& j, const char* key, const std::string& def = {}) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

std::string secs(double s) {
    char buf[32];
    const int m = static_cast<int>(s / 60);
    std::snprintf(buf, sizeof buf, "%d:%05.2f", m, s - m * 60);
    return buf;
}

std::string fmt(const char* key, const std::string& a = {}, const std::string& b = {}) {
    // tr() keys use %1 / %2 placeholders.
    std::string s = tr(key);
    auto rep = [&](const char* ph, const std::string& v) {
        const size_t p = s.find(ph);
        if (p != std::string::npos) s.replace(p, 2, v);
    };
    rep("%1", a);
    rep("%2", b);
    return s;
}

// --------------------------------------------------------- text normalisation

std::string normalize(const std::string& in) {
    std::u32string u = utf8ToUtf32(in);
    for (char32_t& c : u) {
        if (c >= 0xFF01 && c <= 0xFF5E) c = c - 0xFF01 + 0x21;  // full-width ASCII
        else if (c == 0x3000) c = U' ';
        else if (c == U'〜' || c == U'～') c = U'~';
        if (c >= U'A' && c <= U'Z') c = c - U'A' + U'a';
    }
    return utf32ToUtf8(u);
}

std::vector<std::string> splitClauses(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    const std::u32string u = utf8ToUtf32(text);
    std::u32string buf;
    auto flush = [&] {
        std::string s = utf32ToUtf8(buf);
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) out.push_back(s.substr(a));
        buf.clear();
    };
    int quote = 0;
    for (size_t i = 0; i < u.size(); ++i) {
        const char32_t c = u[i];
        if (c == U'「' || c == U'『' || c == U'"' || c == U'“') ++quote;
        if (c == U'」' || c == U'』' || c == U'”') quote = std::max(0, quote - 1);
        if (quote == 0 && (c == U'。' || c == U'、' || c == U'\n' || c == U';' || c == U',' || c == U'.')) {
            // Keep decimal points ("2.5秒").
            if (c == U'.' && i > 0 && i + 1 < u.size() && u[i - 1] >= U'0' && u[i - 1] <= U'9' && u[i + 1] >= U'0' && u[i + 1] <= U'9') {
                buf.push_back(c);
                continue;
            }
            flush();
            continue;
        }
        buf.push_back(c);
    }
    flush();
    // Split on conjunctions too.
    std::vector<std::string> final;
    static const std::regex conj("\\s*(?:そして|それから|その後|and then|then|and also| and )\\s*", std::regex::icase);
    static const std::regex bw("black and white", std::regex::icase);
    for (std::string c : out) {
        c = std::regex_replace(c, bw, "black & white");
        std::sregex_token_iterator it(c.begin(), c.end(), conj, -1), end;
        for (; it != end; ++it)
            if (!it->str().empty()) final.push_back(it->str());
    }
    return final;
}

// Time expressions: 1:23, 1分30秒, 2.5秒, 90s, 3 sec.
std::vector<double> findTimes(const std::string& s) {
    std::vector<double> out;
    static const std::regex re(
        "(\\d+):(\\d+(?:\\.\\d+)?)|(\\d+)\\s*分\\s*(?:(\\d+(?:\\.\\d+)?)\\s*秒)?|(\\d+(?:\\.\\d+)?)\\s*(?:秒|s\\b|sec\\b|secs\\b|seconds?\\b)");
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) {
        const auto& m = *it;
        if (m[1].matched) out.push_back(std::stod(m[1]) * 60 + std::stod(m[2]));
        else if (m[3].matched) out.push_back(std::stod(m[3]) * 60 + (m[4].matched ? std::stod(m[4]) : 0.0));
        else if (m[5].matched) out.push_back(std::stod(m[5]));
    }
    return out;
}

std::optional<std::string> quoted(const std::string& s) {
    // Manual scan: std::regex bracket expressions work on bytes, not UTF-8.
    const std::u32string u = utf8ToUtf32(s);
    static const std::u32string open = U"「『\"“'", close = U"」』\"”'";
    for (size_t i = 0; i < u.size(); ++i) {
        const size_t k = open.find(u[i]);
        if (k == std::u32string::npos) continue;
        const size_t j = u.find(close[k], i + 1);
        if (j != std::u32string::npos && j > i + 1) return utf32ToUtf8(u.substr(i + 1, j - i - 1));
    }
    return std::nullopt;
}

bool has(const std::string& s, const char* pattern) {
    thread_local std::map<const char*, std::regex> cache;
    auto it = cache.find(pattern);
    if (it == cache.end()) it = cache.emplace(pattern, std::regex(pattern)).first;
    return std::regex_search(s, it->second);
}

std::string targetFor(const std::string& s) {
    return has(s, "選択|selected|this clip|このクリップ") ? "selected" : "all_video";
}


}  // namespace

// ------------------------------------------------------------------ plan json

const std::vector<std::string>& supportedOps() {
    static const std::vector<std::string> ops = {"remove_silence", "delete_range", "keep_range", "split_at", "add_text", "add_subtitle",
                                                 "set_volume", "apply_effect", "set_speed", "fade", "add_beat_markers",
                                                 "split_at_scenes", "add_marker", "export"};
    return ops;
}

Json Plan::toJson() const {
    Json j;
    j["steps"] = Json::array();
    for (const auto& s : steps) j["steps"].push_back({{"op", s.op}, {"args", s.args}});
    return j;
}

Result<Plan> planFromJson(const Json& j, const std::string& source) {
    if (!j.is_object() || !j.contains("steps") || !j["steps"].is_array())
        return Result<Plan>::error("plan must be an object with a \"steps\" array");
    Plan p;
    p.source = source;
    if (j["steps"].size() > 50) return Result<Plan>::error("too many steps (max 50)");
    for (const auto& s : j["steps"]) {
        if (!s.is_object() || !s.contains("op") || !s["op"].is_string()) return Result<Plan>::error("each step needs an \"op\"");
        PlanStep st;
        st.op = s["op"].get<std::string>();
        st.args = s.contains("args") && s["args"].is_object() ? s["args"] : Json::object();
        const auto& ops = supportedOps();
        if (std::find(ops.begin(), ops.end(), st.op) == ops.end()) return Result<Plan>::error("unsupported operation: " + st.op);
        auto needNum = [&](const char* k, double lo, double hi) -> Status {
            if (!st.args.contains(k) || !st.args[k].is_number()) return Status::error(st.op + ": missing number '" + k + "'");
            const double v = st.args[k].get<double>();
            if (!(v >= lo && v <= hi)) return Status::error(st.op + ": '" + k + "' out of range");
            return Status::ok();
        };
        auto needStr = [&](const char* k) -> Status {
            if (!st.args.contains(k) || !st.args[k].is_string() || st.args[k].get<std::string>().empty())
                return Status::error(st.op + ": missing text '" + k + "'");
            return Status::ok();
        };
        Status ok;
        if (st.op == "delete_range" || st.op == "keep_range") {
            ok = needNum("start", 0, 1e7);
            if (ok) ok = needNum("end", 0, 1e7);
            if (ok && st.args["end"].get<double>() <= st.args["start"].get<double>()) ok = Status::error(st.op + ": end must be after start");
        } else if (st.op == "split_at") {
            ok = needNum("time", 0, 1e7);
        } else if (st.op == "add_text" || st.op == "add_subtitle") {
            ok = needStr("text");
        } else if (st.op == "set_volume") {
            ok = needNum("db", -60, 24);
        } else if (st.op == "apply_effect") {
            ok = needStr("effect");
            if (ok && !fx::EffectRegistry::instance().find(st.args["effect"].get<std::string>()))
                ok = Status::error("unknown effect: " + st.args["effect"].get<std::string>());
        } else if (st.op == "set_speed") {
            ok = needNum("factor", 0.05, 20);
        } else if (st.op == "add_marker") {
            ok = needNum("time", 0, 1e7);
        } else if (st.op == "export") {
            ok = needStr("preset");
        }
        if (!ok) return ok;
        st.description = describeStep(st);
        p.steps.push_back(std::move(st));
    }
    return p;
}

std::string describeStep(const PlanStep& s) {
    const Json& a = s.args;
    if (s.op == "remove_silence") return tr("Remove silent parts (ripple, all tracks stay in sync)");
    if (s.op == "delete_range") return fmt("Delete %1 - %2", secs(num(a, "start", 0)), secs(num(a, "end", 0)));
    if (s.op == "keep_range") return fmt("Keep only %1 - %2", secs(num(a, "start", 0)), secs(num(a, "end", 0)));
    if (s.op == "split_at") return fmt("Split all tracks at %1", secs(num(a, "time", 0)));
    if (s.op == "add_text") return fmt("Add title \"%1\"", str(a, "text"));
    if (s.op == "add_subtitle") return fmt("Add subtitle \"%1\"", str(a, "text"));
    if (s.op == "set_volume") {
        char db[16];
        std::snprintf(db, sizeof db, "%+.1f dB", num(a, "db", 0));
        return fmt("Set volume of %1 to %2", tr(str(a, "target", "all").c_str()), db);
    }
    if (s.op == "apply_effect") {
        const fx::EffectDef* d = fx::EffectRegistry::instance().find(str(a, "effect"));
        return fmt("Apply %1 to %2", d ? tr(d->name.c_str()) : str(a, "effect"), tr(str(a, "target", "all_video").c_str()));
    }
    if (s.op == "set_speed") {
        char f[16];
        std::snprintf(f, sizeof f, "%.0f%%", num(a, "factor", 1) * 100.0);
        return fmt("Change speed to %1 (%2)", f, tr(str(a, "target", "selected").c_str()));
    }
    if (s.op == "fade") return fmt("Fade %1", tr(str(a, "direction", "both").c_str()));
    if (s.op == "add_beat_markers") return tr("Add markers on the music beats");
    if (s.op == "split_at_scenes") return fmt("Split %1 at scene changes", tr(str(a, "target", "all_video").c_str()));
    if (s.op == "add_marker") return fmt("Add marker at %1", secs(num(a, "time", 0)));
    if (s.op == "export") return fmt("Export with preset %1", str(a, "preset"));
    return s.op;
}

// ------------------------------------------------------------------ rules

const std::vector<const char*>& exampleInstructions() {
    static const std::vector<const char*> v = {
        trNoop("Cut the silent parts"),    trNoop("Make the BGM quieter (-12 dB)"), trNoop("Cut the first 5 seconds"),
        trNoop("Black and white"),         trNoop("Cinematic look"),                trNoop("Add the title \"Title\""),
        trNoop("Add fades"),               trNoop("Add markers on the beats"),      trNoop("Split at scene changes"),
        trNoop("Export for YouTube"),
    };
    return v;
}

namespace {

// Parses one clause; returns the steps it understood (possibly none).
std::vector<PlanStep> parseClause(const std::string& raw, const PlanContext& ctx) {
    std::vector<PlanStep> out;
    const std::string s = normalize(raw);
    const auto times = findTimes(s);
    const double dur = ctx.sequenceDuration.seconds();
    const double now = ctx.playhead.seconds();
    const bool removeVerb = has(s, "削除|カット|消し|消す|消して|除去|取り除|切り取|delete|remove|cut|trim|drop");
    const bool head = has(s, "最初|冒頭|先頭|頭の|first|beginning|\\bstart\\b|opening");
    const bool tail = has(s, "最後|末尾|終わり|おわり|ラスト|\\blast\\b|ending|\\bend\\b|tail");
    static const std::regex dbre("(-?\\d+(?:\\.\\d+)?)\\s*db");
    std::smatch m;

    if (has(s, "無音|沈黙|間を詰|空白|silence|silent|dead air|\\bpauses?\\b")) {
        Json args = Json::object();
        if (std::regex_search(s, m, dbre)) args["threshold_db"] = std::stod(m[1]);
        if (times.size() == 1) args["min_silence"] = times[0];
        out.push_back({"remove_silence", args, {}});
        return out;
    }
    if (times.size() >= 2 && removeVerb && head && tail) {
        // "最初の10秒と最後の5秒をカット"
        out.push_back({"delete_range", {{"start", 0.0}, {"end", times[0]}}, {}});
        if (dur > 0) out.push_back({"delete_range", {{"start", std::max(0.0, dur - times[1])}, {"end", dur}}, {}});
        return out;
    }
    if (times.size() >= 2 && has(s, "だけ|のみ|以外を|残し|残す|keep|\\bonly\\b")) {
        out.push_back({"keep_range", {{"start", std::min(times[0], times[1])}, {"end", std::max(times[0], times[1])}}, {}});
        return out;
    }
    if (times.size() >= 2 && removeVerb) {
        out.push_back({"delete_range", {{"start", std::min(times[0], times[1])}, {"end", std::max(times[0], times[1])}}, {}});
        return out;
    }
    if (times.size() == 1 && removeVerb && head) {
        out.push_back({"delete_range", {{"start", 0.0}, {"end", times[0]}}, {}});
        return out;
    }
    if (times.size() == 1 && removeVerb && tail) {
        if (dur > 0) out.push_back({"delete_range", {{"start", std::max(0.0, dur - times[0])}, {"end", dur}}, {}});
        return out;
    }
    if (has(s, "シーン|場面|scene") && (removeVerb || has(s, "分割|切り|区切|split"))) {
        out.push_back({"split_at_scenes", {{"target", targetFor(s)}}, {}});
        return out;
    }
    if (has(s, "分割|split")) {
        out.push_back({"split_at", {{"time", times.empty() ? now : times[0]}}, {}});
        return out;
    }
    if (auto q = quoted(raw)) {
        const bool sub = has(s, "字幕|テロップ|subtitle|caption");
        if (sub || has(s, "タイトル|テキスト|文字|title|text")) {
            Json a = {{"text", *q}, {"start", times.empty() ? now : times[0]}};
            if (times.size() >= 2) a["duration"] = std::max(0.5, times[1] - times[0]);
            out.push_back({sub ? "add_subtitle" : "add_text", a, {}});
            return out;
        }
    }

    // Non-exclusive rules (one clause may ask for several looks).
    const bool audioNoun = has(s, "音量|ボリューム|volume|bgm|音楽|ミュージック|music|曲|声|ボイス|ナレーション|セリフ|voice|dialog|narration");
    const bool hasDb = std::regex_search(s, m, dbre);
    const bool quieter = has(s, "小さく|下げ|絞|quieter|lower|softer|turn down|reduce");
    const bool louder = has(s, "大きく|上げ|louder|raise|boost|turn up");
    if ((audioNoun || has(s, "loud|quiet")) && (hasDb || quieter || louder) && !has(s, "ノイズ|noise")) {
        const std::string target = has(s, "bgm|音楽|ミュージック|music|曲") ? "music"
                                   : has(s, "声|ボイス|ナレーション|セリフ|voice|dialog|narration") ? "voice"
                                   : has(s, "選択|selected") ? "selected" : "all";
        double db = hasDb ? std::stod(m[1]) : quieter ? -6.0 : 4.0;
        out.push_back({"set_volume", {{"target", target}, {"db", std::clamp(db, -60.0, 24.0)}}, {}});
    }
    auto effect = [&](const char* id, Json params = Json::object()) {
        out.push_back({"apply_effect", {{"effect", id}, {"target", targetFor(s)}, {"params", std::move(params)}}, {}});
    };
    if (has(s, "白黒|モノクロ|black & white|grayscale|greyscale|b&w|monochrome")) effect("color.basic", {{"saturation", 0.0}});
    if (has(s, "明るく|明るい|brighter|brighten|lighter")) effect("color.basic", {{"exposure", 0.5}});
    if (has(s, "暗く|暗い|darker|darken")) effect("color.basic", {{"exposure", -0.5}});
    if (has(s, "鮮やか|ビビッド|vivid|saturated|colorful|colourful")) effect("color.basic", {{"saturation", 1.3}, {"vibrance", 0.3}});
    if (has(s, "暖か|温か|あたたか|\\bwarm")) effect("color.basic", {{"temperature", 0.3}});
    if (has(s, "涼し|寒色|クール|\\bcool|\\bcold")) effect("color.basic", {{"temperature", -0.3}});
    if (has(s, "シネマ|映画風|映画っぽ|cinematic|movie look")) {
        effect("color.basic", {{"contrast", 0.2}, {"saturation", 0.85}, {"temperature", 0.1}});
        effect("stylize.vignette", {{"amount", 0.35}});
    } else if (has(s, "ビネット|周辺を暗く|周囲を暗く|vignette")) {
        effect("stylize.vignette");
    }
    if (has(s, "ぼかし|ぼかす|ボケ|blur")) effect("blur.gaussian");
    if (has(s, "グロー|ブルーム|glow|bloom")) effect("stylize.bloom");
    if (has(s, "フィルム|グレイン|grain|film look")) effect("stylize.film_grain");
    if (has(s, "ノイズ(を)?(除去|消|カット|取|減)|ノイズリダクション|noise reduction|denoise|remove (the )?noise|reduce noise"))
        out.push_back({"apply_effect", {{"effect", "audio.noise_reduction"}, {"target", "all_audio"}, {"params", Json::object()}}, {}});
    static const std::regex speedre("(\\d+(?:\\.\\d+)?)\\s*(?:倍速|倍|x\\b|times)");
    const std::string speedTarget = has(s, "全体|全部|すべて|全て|whole|entire|all") ? "all_video" : "selected";
    if (std::regex_search(s, m, speedre)) out.push_back({"set_speed", {{"factor", std::stod(m[1])}, {"target", speedTarget}}, {}});
    else if (has(s, "スロー|slow motion|slow-mo|slowmo")) out.push_back({"set_speed", {{"factor", 0.5}, {"target", speedTarget}}, {}});
    else if (has(s, "早送り|fast forward|speed up")) out.push_back({"set_speed", {{"factor", 2.0}, {"target", speedTarget}}, {}});
    const bool fin = has(s, "フェードイン|fade in|fade-in"), fout = has(s, "フェードアウト|fade out|fade-out");
    if (fin || fout || has(s, "フェード|fade"))
        out.push_back({"fade", {{"direction", fin && !fout ? "in" : fout && !fin ? "out" : "both"}, {"duration", times.empty() ? 1.0 : times[0]}}, {}});
    const bool beat = has(s, "ビート|リズム|拍|beat|bpm|tempo|rhythm");
    if (beat) out.push_back({"add_beat_markers", Json::object(), {}});
    if (!beat && has(s, "マーカー|目印|marker")) out.push_back({"add_marker", {{"time", times.empty() ? now : times[0]}}, {}});
    if (has(s, "書き出|エクスポート|出力|export|render out")) {
        const std::string preset = has(s, "4k") ? "youtube-4k"
                                   : has(s, "tiktok|ティックトック|ティクトク") ? "tiktok"
                                   : has(s, "shorts|ショート") ? "youtube-shorts"
                                   : has(s, "reels|リール|instagram|インスタ") ? "instagram-reels"
                                   : has(s, "prores|master|マスター") ? "master-prores"
                                   : "youtube-1080p";
        out.push_back({"export", {{"preset", preset}}, {}});
    }
    return out;
}

}  // namespace

Plan planFromText(const std::string& text, const PlanContext& ctx) {
    Plan p;
    p.source = "local";
    const auto clauses = splitClauses(text);
    for (size_t i = 0; i < clauses.size(); ++i) {
        auto steps = parseClause(clauses[i], ctx);
        // Commas also appear inside one instruction ("BGMの音量を、-10dBに"):
        // when a fragment means nothing alone, try it joined with the next one.
        if (steps.empty() && i + 1 < clauses.size()) {
            auto joined = parseClause(clauses[i] + " " + clauses[i + 1], ctx);
            if (!joined.empty()) {
                steps = std::move(joined);
                ++i;
            }
        }
        if (steps.empty()) p.notes.push_back(std::string(tr("Not understood:")) + " " + clauses[i]);
        for (auto& st : steps) p.steps.push_back(std::move(st));
    }
    for (auto& st : p.steps) st.description = describeStep(st);
    return p;
}

// ------------------------------------------------------------------ apply helpers

namespace {

bool audioOnlyMedia(const Project& prj, const Clip& c) {
    const MediaItem* m = c.kind == ClipKind::Media ? prj.findMedia(c.media) : nullptr;
    return m && m->info.hasAudio() && !m->info.hasVideo();
}

// Track used for silence detection: the audio track carrying the most
// dialogue (audio from video files), falling back to the first audio track.
int voiceTrack(const Project& prj, const Sequence& seq, const std::string& name) {
    int best = -1;
    Time bestCover{-1};
    for (int i : seq.audioTrackIndices()) {
        const Track& t = *seq.tracks[static_cast<size_t>(i)];
        if (!name.empty() && t.name == name) return i;
        Time cover{0};
        for (const auto& c : t.clips)
            if (c->kind == ClipKind::Media && !audioOnlyMedia(prj, *c)) cover += c->duration;
        if (cover > bestCover) {
            bestCover = cover;
            best = i;
        }
    }
    return best;
}

std::vector<const Clip*> targetClips(const Project& prj, const Sequence& seq, const std::string& target, const std::set<ClipId>& sel,
                                     bool audio) {
    std::vector<const Clip*> out;
    for (const auto& tp : seq.tracks) {
        if (tp->locked) continue;
        const bool isAudio = !isVisualTrack(tp->kind);
        for (const auto& c : tp->clips) {
            if (target == "selected") {
                if (sel.count(c->id) && isAudio == audio) out.push_back(c.get());
            } else if (audio) {
                if (!isAudio) continue;
                if (target == "music" && !audioOnlyMedia(prj, *c)) continue;
                if (target == "voice" && audioOnlyMedia(prj, *c)) continue;
                out.push_back(c.get());
            } else if (!isAudio && (c->kind == ClipKind::Media || c->kind == ClipKind::Compound)) {
                out.push_back(c.get());
            }
        }
    }
    return out;
}

const Clip* musicClip(const Project& prj, const Sequence& seq, const std::set<ClipId>& sel) {
    const Clip* first = nullptr;
    for (int i : seq.audioTrackIndices())
        for (const auto& c : seq.tracks[static_cast<size_t>(i)]->clips)
            if (audioOnlyMedia(prj, *c)) {
                if (sel.count(c->id)) return c.get();
                if (!first) first = c.get();
            }
    return first;
}

int freeVisualTrack(SequenceEditor& e, TimeRange r) {
    const auto vis = e.seq().visualTrackIndices();
    int highestUsed = -1;
    for (size_t i = 0; i < vis.size(); ++i)
        if (!edit::rangeIsEmpty(e.trackAt(vis[i]), r)) highestUsed = static_cast<int>(i);
    for (size_t i = static_cast<size_t>(highestUsed + 1); i < vis.size(); ++i) {
        const Track& t = e.trackAt(vis[i]);
        if (!t.locked && (t.kind == TrackKind::Video || t.kind == TrackKind::Text)) return vis[i];
    }
    auto r2 = edit::addTrack(e, TrackKind::Video);
    return r2 ? e.seq().trackIndex(*r2) : -1;
}

}  // namespace

PlanNeeds analysisNeeded(const Plan& plan, const Project& prj, SequenceId seqId, const std::set<ClipId>& sel) {
    PlanNeeds n;
    const Sequence* seq = prj.findSequence(seqId);
    if (!seq) return n;
    for (const auto& st : plan.steps) {
        if (st.op == "remove_silence") {
            const int t = voiceTrack(prj, *seq, str(st.args, "track"));
            if (t >= 0)
                for (const auto& c : seq->tracks[static_cast<size_t>(t)]->clips)
                    if (c->kind == ClipKind::Media) n.silence.insert(c->media);
        } else if (st.op == "add_beat_markers") {
            if (const Clip* c = musicClip(prj, *seq, sel)) n.beats.insert(c->media);
        } else if (st.op == "split_at_scenes") {
            for (const Clip* c : targetClips(prj, *seq, str(st.args, "target", "all_video"), sel, false))
                if (c->kind == ClipKind::Media) n.scenes.insert(c->media);
        }
    }
    return n;
}

Status applyPlan(ProjectEditor& pe, SequenceId seqId, const Plan& plan, const PlanInputs& in, ApplyReport* report) {
    const Project& prj0 = pe.current();
    const Sequence* seq0 = prj0.findSequence(seqId);
    if (!seq0) return Status::error("No sequence");
    if (report) {
        report->durationBefore = seq0->duration();
        report->clipsBefore = seq0->clipCount();
    }
    auto log = [&](const std::string& line) {
        if (report) report->log.push_back(line);
    };
    for (const PlanStep& st : plan.steps) {
        const Json& a = st.args;
        SequenceEditor e = pe.sequence(seqId);
        const Project& prj = pe.current();
        const Sequence& seq = e.seq();
        Status ok;
        if (st.op == "remove_silence") {
            const int t = voiceTrack(prj, seq, str(a, "track"));
            if (t < 0) return Status::error(tr("There is no audio to analyse"));
            std::vector<TimeRange> ranges;
            for (const auto& c : seq.tracks[static_cast<size_t>(t)]->clips) {
                auto it = in.silences.find(c->media);
                if (c->kind != ClipKind::Media || it == in.silences.end()) continue;
                auto r = sourceRangesToTimeline(*c, it->second.silences);
                ranges.insert(ranges.end(), r.begin(), r.end());
            }
            ranges = normalizeRanges(ranges, Time::fromMilliseconds(100));
            Time total{0};
            for (const auto& r : ranges) total += r.duration;
            ok = rippleRemoveRanges(e, ranges);
            char buf[96];
            std::snprintf(buf, sizeof buf, "%zu / %.1f s", ranges.size(), total.seconds());
            log(describeStep(st) + ": " + buf);
        } else if (st.op == "delete_range" || st.op == "keep_range") {
            const Time s = Time::fromSeconds(num(a, "start", 0)), en = Time::fromSeconds(num(a, "end", 0));
            if (st.op == "delete_range") {
                ok = rippleRemoveRanges(e, {TimeRange::fromStartEnd(s, en)});
            } else {
                const Time d = seq.duration();
                std::vector<TimeRange> rm;
                if (s.ticks > 0) rm.push_back(TimeRange::fromStartEnd(Time{0}, s));
                if (en < d) rm.push_back(TimeRange::fromStartEnd(en, d));
                ok = rippleRemoveRanges(e, rm);
            }
            log(describeStep(st));
        } else if (st.op == "split_at") {
            ok = edit::splitAtTime(e, Time::fromSeconds(num(a, "time", 0)).snappedToFrame(seq.frameRate), {}).status();
            log(describeStep(st));
        } else if (st.op == "add_text" || st.op == "add_subtitle") {
            const bool sub = st.op == "add_subtitle";
            const Time start = Time::fromSeconds(num(a, "start", in.playhead.seconds())).snappedToFrame(seq.frameRate);
            const Time dur = Time::fromSeconds(num(a, "duration", sub ? 3.0 : 5.0));
            Clip c = makeTextClip(str(a, "text"), start, dur, sub ? ClipKind::Subtitle : ClipKind::Text);
            int track = -1;
            if (sub) {
                for (int i = 0; i < e.trackCount(); ++i)
                    if (e.trackAt(i).kind == TrackKind::Subtitle && !e.trackAt(i).locked) track = i;
                if (track < 0) {
                    auto r = edit::addTrack(e, TrackKind::Subtitle, tr("Subtitles"));
                    if (!r) return r.status();
                    track = e.seq().trackIndex(*r);
                }
                c.textStyle.fontSize = std::max(36.0f, static_cast<float>(seq.height) * 0.05f);
                c.textStyle.fontWeight = 600;
                c.textParams.setStatic("strokeColor", pv(0, 0, 0, 1));
                c.textParams.setStatic("strokeWidth", pv(c.textStyle.fontSize * 0.08f));
            } else {
                track = freeVisualTrack(e, {start, dur});
            }
            if (track < 0) return Status::error("No track for the text");
            ok = edit::placeClips(e, {{track, c}}, edit::PlaceMode::Overwrite);
            log(describeStep(st));
        } else if (st.op == "set_volume") {
            const std::string target = str(a, "target", "all");
            const float db = static_cast<float>(num(a, "db", 0));
            if (target == "all") {
                e.props().masterVolumeDb = db;
            } else {
                std::vector<ClipId> ids;
                for (const Clip* c : targetClips(prj, seq, target, in.selection, true)) ids.push_back(c->id);
                if (ids.empty()) return Status::error(fmt("No clips for \"%1\"", tr(target.c_str())));
                for (ClipId id : ids) e.mutableClip(id).audio.setStatic("volume", pv(db));
            }
            log(describeStep(st));
        } else if (st.op == "apply_effect") {
            const std::string effectId = str(a, "effect");
            const fx::EffectDef* def = fx::EffectRegistry::instance().find(effectId);
            if (!def) return Status::error("unknown effect: " + effectId);
            const bool audio = def->kind == fx::EffectKind::Audio;
            std::string target = str(a, "target", audio ? "all_audio" : "all_video");
            if (audio && target == "all_audio") target = "all";
            std::vector<ClipId> ids;
            for (const Clip* c : targetClips(prj, seq, target, in.selection, audio)) ids.push_back(c->id);
            if (ids.empty()) return Status::error(tr("No clips to apply the effect to"));
            for (ClipId id : ids) {
                EffectInstance inst = fx::EffectRegistry::instance().instantiate(effectId);
                if (auto pit = a.find("params"); pit != a.end() && pit->is_object())
                    for (const auto& [k, v] : pit->items()) {
                        if (v.is_number()) inst.params.setStatic(k, pv(v.get<float>()));
                        else if (v.is_array() && v.size() >= 1 && v.size() <= 4) {
                            ParamValue pvv{};
                            for (size_t i = 0; i < v.size(); ++i) pvv[i] = v[i].is_number() ? v[i].get<float>() : 0.0f;
                            inst.params.setStatic(k, pvv);
                        }
                    }
                // Merge with an existing instance of the same effect instead of stacking duplicates.
                Clip& c = e.mutableClip(id);
                auto existing = std::find_if(c.effects.begin(), c.effects.end(), [&](const EffectInstance& x) { return x.effectId == effectId; });
                if (existing != c.effects.end()) {
                    for (const auto& [k, p] : inst.params.items())
                        if (a.contains("params") && a["params"].contains(k)) existing->params.set(k, p);
                } else {
                    c.effects.push_back(std::move(inst));
                }
            }
            log(describeStep(st));
        } else if (st.op == "set_speed") {
            const double f = num(a, "factor", 1);
            const Rational r{static_cast<int64_t>(std::lround(f * 1000)), 1000};
            std::vector<ClipId> ids;
            for (const Clip* c : targetClips(prj, seq, str(a, "target", "selected"), in.selection, false))
                if (c->hasLimitedSource()) ids.push_back(c->id);
            if (ids.empty()) return Status::error(tr("Select the clips to change the speed of"));
            for (ClipId id : ids)
                if (auto s2 = edit::setClipSpeed(e, id, r.reduced(), true); !s2) return s2;
            log(describeStep(st));
        } else if (st.op == "fade") {
            const std::string dir = str(a, "direction", "both");
            const Time d = Time::fromSeconds(std::clamp(num(a, "duration", 1.0), 0.1, 10.0));
            const Time end = seq.duration();
            bool any = false;
            for (int ti = 0; ti < e.trackCount(); ++ti) {
                const Track& t = e.trackAt(ti);
                if (t.clips.empty() || t.locked || t.kind == TrackKind::Subtitle) continue;
                const ClipId firstId = t.clips.front()->id, lastId = t.clips.back()->id;
                const bool atStart = t.clips.front()->start.ticks == 0, atEnd = t.clips.back()->end() == end;
                TransitionSpec spec;
                spec.type = "dip-black";
                if ((dir == "in" || dir == "both") && atStart) {
                    Clip& c = e.mutableClip(firstId);
                    spec.duration = minTime(d, Time{c.duration.ticks / 2});
                    c.transitionIn = spec;
                    any = true;
                }
                if ((dir == "out" || dir == "both") && atEnd) {
                    Clip& c = e.mutableClip(lastId);
                    spec.duration = minTime(d, Time{c.duration.ticks / 2});
                    c.transitionOut = spec;
                    any = true;
                }
            }
            if (!any) return Status::error(tr("No clip starts or ends the sequence"));
            log(describeStep(st));
        } else if (st.op == "add_beat_markers") {
            const Clip* m = musicClip(prj, seq, in.selection);
            if (!m) return Status::error(tr("Add music (BGM) first"));
            auto it = in.beats.find(m->media);
            if (it == in.beats.end()) return Status::error(tr("The music has not been analysed"));
            edit::removeMarkersOfKind(e, MarkerKind::Beat);
            int count = 0;
            for (Time b : it->second.beats)
                if (auto tl = sourceTimeToTimeline(*m, b)) {
                    Marker mk;
                    mk.time = tl->snappedToFrame(seq.frameRate);
                    mk.kind = MarkerKind::Beat;
                    mk.color = 0xFF40C0FF;
                    mk.name = "Beat";
                    edit::addMarker(e, mk);
                    ++count;
                }
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d (%.1f BPM)", count, it->second.bpm);
            log(describeStep(st) + ": " + buf);
        } else if (st.op == "split_at_scenes") {
            std::vector<std::pair<ClipId, Time>> cuts;
            for (const Clip* c : targetClips(prj, seq, str(a, "target", "all_video"), in.selection, false)) {
                auto it = in.scenes.find(c->media);
                if (c->kind != ClipKind::Media || it == in.scenes.end()) continue;
                for (Time t : it->second.cuts)
                    if (auto tl = sourceTimeToTimeline(*c, t); tl && *tl > c->start && *tl < c->end()) cuts.emplace_back(c->id, *tl);
            }
            // Later cuts first so earlier clip ids stay valid.
            std::sort(cuts.begin(), cuts.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            for (const auto& [id, t] : cuts)
                if (e.clip(id) && e.clip(id)->range().contains(t))
                    if (auto r = edit::splitClip(e, id, t.snappedToFrame(seq.frameRate), true); !r) return r.status();
            log(describeStep(st) + ": " + std::to_string(cuts.size()));
        } else if (st.op == "add_marker") {
            Marker mk;
            mk.time = Time::fromSeconds(num(a, "time", 0)).snappedToFrame(seq.frameRate);
            mk.name = str(a, "name", "AI");
            edit::addMarker(e, mk);
            log(describeStep(st));
        } else if (st.op == "export") {
            if (report) report->exports.push_back(str(a, "preset", "youtube-1080p"));
            log(describeStep(st));
        } else {
            return Status::error("unsupported operation: " + st.op);
        }
        if (!ok) return ok;
    }
    if (report) {
        const Sequence* s = pe.current().findSequence(seqId);
        report->durationAfter = s ? s->duration() : Time{0};
        report->clipsAfter = s ? s->clipCount() : 0;
    }
    return Status::ok();
}

Json describeTimeline(const Project& prj, SequenceId seqId, Time playhead, const std::set<ClipId>& sel) {
    Json j;
    const Sequence* s = prj.findSequence(seqId);
    if (!s) return j;
    j["width"] = s->width;
    j["height"] = s->height;
    j["fps"] = s->frameRate.toDouble();
    j["duration_sec"] = s->duration().seconds();
    j["playhead_sec"] = playhead.seconds();
    j["tracks"] = Json::array();
    size_t budget = 400;
    for (const auto& t : s->tracks) {
        Json tj = {{"name", t->name}, {"kind", trackKindName(t->kind)}, {"muted", t->muted}, {"locked", t->locked}, {"clips", Json::array()}};
        for (const auto& c : t->clips) {
            if (budget == 0) break;
            --budget;
            Json cj = {{"id", c->id}, {"kind", clipKindName(c->kind)}, {"start", c->start.seconds()}, {"end", c->end().seconds()},
                       {"selected", sel.count(c->id) != 0}};
            if (c->kind == ClipKind::Media) {
                if (const MediaItem* m = prj.findMedia(c->media)) {
                    cj["name"] = m->name;  // file name only, never the path
                    cj["media"] = m->info.hasVideo() ? (m->info.hasAudio() ? "video+audio" : "video") : "audio";
                }
            } else if (c->kind == ClipKind::Text || c->kind == ClipKind::Subtitle) {
                cj["text"] = c->text.substr(0, 200);
            }
            if (c->speed != Rational{1, 1}) cj["speed"] = c->speed.toDouble();
            if (!c->effects.empty()) {
                cj["effects"] = Json::array();
                for (const auto& e : c->effects) cj["effects"].push_back(e.effectId);
            }
            tj["clips"].push_back(cj);
        }
        j["tracks"].push_back(tj);
    }
    j["markers"] = Json::array();
    for (const auto& m : s->markers) j["markers"].push_back({{"time", m.time.seconds()}, {"name", m.name}, {"kind", markerKindName(m.kind)}});
    return j;
}

}  // namespace avc::ai
