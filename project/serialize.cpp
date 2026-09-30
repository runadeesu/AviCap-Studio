#include "project/serialize.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "avicap_build_info.h"
#include "core/file_io.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "project/migrate.h"

namespace avc {

namespace {

template <typename T>
T val(const Json& j, const char* key, T def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}

Json idJ(Id id) { return idToString(id); }
Id idOf(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return kInvalidId;
    return idFromString(it->get<std::string>());
}

Json timeJ(Time t) { return t.ticks; }
Time timeOf(const Json& j, const char* key, Time def = Time{0}) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return def;
    return Time{it->get<int64_t>()};
}

Json rationalJ(Rational r) { return r.toString(); }
Rational rationalOf(const Json& j, const char* key, Rational def) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    auto r = Rational::parse(it->get<std::string>());
    return r ? *r : def;
}

Json valueJ(const ParamValue& v) { return Json::array({v[0], v[1], v[2], v[3]}); }
ParamValue valueOf(const Json& j) {
    ParamValue v{};
    if (!j.is_array()) return v;
    for (size_t i = 0; i < 4 && i < j.size(); ++i) v[i] = j[i].is_number() ? j[i].get<float>() : 0.0f;
    return v;
}


Json effectsJ(const std::vector<EffectInstance>& fx) {
    Json a = Json::array();
    for (auto& e : fx)
        a.push_back({{"id", idJ(e.id)}, {"effect", e.effectId}, {"enabled", e.enabled}, {"params", paramSetToJson(e.params)}});
    return a;
}

std::vector<EffectInstance> effectsOf(const Json& j) {
    std::vector<EffectInstance> out;
    if (!j.is_array()) return out;
    for (auto& e : j) {
        EffectInstance fx;
        fx.id = idOf(e, "id");
        if (fx.id == kInvalidId) fx.id = newId();
        fx.effectId = val<std::string>(e, "effect", "");
        fx.enabled = val<bool>(e, "enabled", true);
        if (auto it = e.find("params"); it != e.end()) fx.params = paramSetFromJson(*it);
        if (!fx.effectId.empty()) out.push_back(std::move(fx));
    }
    return out;
}

Json transitionJ(const TransitionSpec& t) {
    return {{"type", t.type}, {"duration", timeJ(t.duration)}, {"params", paramSetToJson(t.params)}};
}

std::optional<TransitionSpec> transitionOf(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return std::nullopt;
    TransitionSpec t;
    t.type = val<std::string>(*it, "type", "cross-dissolve");
    t.duration = timeOf(*it, "duration");
    if (auto p = it->find("params"); p != it->end()) t.params = paramSetFromJson(*p);
    if (t.duration.ticks <= 0) return std::nullopt;
    return t;
}

const char* alignName(TextAlign a) {
    switch (a) {
    case TextAlign::Left: return "left";
    case TextAlign::Right: return "right";
    default: return "center";
    }
}
TextAlign alignOf(const std::string& s) {
    if (s == "left") return TextAlign::Left;
    if (s == "right") return TextAlign::Right;
    return TextAlign::Center;
}

const char* mediaKindName(MediaKind k) {
    switch (k) {
    case MediaKind::Video: return "video";
    case MediaKind::Audio: return "audio";
    case MediaKind::Image: return "image";
    default: return "unknown";
    }
}
MediaKind mediaKindOf(const std::string& s) {
    if (s == "video") return MediaKind::Video;
    if (s == "audio") return MediaKind::Audio;
    if (s == "image") return MediaKind::Image;
    return MediaKind::Unknown;
}

}  // namespace

std::string timeToString(Time t) { return std::to_string(t.ticks); }

// ---------------------------------------------------------------- params

Json paramSetToJson(const ParamSet& p) {
    Json o = Json::object();
    for (auto& [id, param] : p.items()) {
        Json e;
        e["v"] = valueJ(param.staticValue());
        if (param.animated()) {
            Json keys = Json::array();
            for (auto& k : param.keys()) {
                Json kj{{"t", timeJ(k.time)}, {"v", valueJ(k.value)}, {"i", interpName(k.interp)}};
                if (k.interp == Interp::Bezier) kj["b"] = Json::array({k.x1, k.y1, k.x2, k.y2});
                keys.push_back(std::move(kj));
            }
            e["k"] = std::move(keys);
        }
        o[id] = std::move(e);
    }
    return o;
}

ParamSet paramSetFromJson(const Json& j) {
    ParamSet p;
    if (!j.is_object()) return p;
    for (auto it = j.begin(); it != j.end(); ++it) {
        AnimatedParam ap(valueOf(it.value().value("v", Json::array())));
        if (auto k = it.value().find("k"); k != it.value().end() && k->is_array()) {
            std::vector<Keyframe> keys;
            for (auto& kj : *k) {
                Keyframe kf;
                kf.time = timeOf(kj, "t");
                kf.value = valueOf(kj.value("v", Json::array()));
                kf.interp = interpFromName(val<std::string>(kj, "i", "linear"));
                if (auto b = kj.find("b"); b != kj.end() && b->is_array() && b->size() == 4) {
                    kf.x1 = (*b)[0].get<float>();
                    kf.y1 = (*b)[1].get<float>();
                    kf.x2 = (*b)[2].get<float>();
                    kf.y2 = (*b)[3].get<float>();
                }
                keys.push_back(kf);
            }
            ap.setKeys(std::move(keys));
        }
        p.set(it.key(), std::move(ap));
    }
    return p;
}

// ---------------------------------------------------------------- clip

Json clipToJson(const Clip& c) {
    Json j{{"id", idJ(c.id)},
           {"kind", clipKindName(c.kind)},
           {"name", c.name},
           {"start", timeJ(c.start)},
           {"duration", timeJ(c.duration)},
           {"sourceIn", timeJ(c.sourceIn)}};
    if (c.media) j["media"] = idJ(c.media);
    if (c.nested) j["nested"] = idJ(c.nested);
    if (c.streamIndex >= 0) j["stream"] = c.streamIndex;
    if (!(c.speed == Rational{1, 1})) j["speed"] = rationalJ(c.speed);
    if (c.reverse) j["reverse"] = true;
    if (c.freezeFrame) j["freeze"] = true;
    if (!c.enabled) j["enabled"] = false;
    if (c.linkGroup) j["link"] = idJ(c.linkGroup);
    if (c.group) j["group"] = idJ(c.group);
    if (c.colorLabel) j["color"] = c.colorLabel;
    if (!c.transform.empty()) j["transform"] = paramSetToJson(c.transform);
    if (c.flipH) j["flipH"] = true;
    if (c.flipV) j["flipV"] = true;
    if (c.blend != BlendMode::Normal) j["blend"] = blendModeName(c.blend);
    if (!c.audio.empty()) j["audio"] = paramSetToJson(c.audio);
    if (c.gainDb != 0.0f) j["gainDb"] = c.gainDb;
    if (!c.effects.empty()) j["effects"] = effectsJ(c.effects);
    if (c.transitionIn) j["transitionIn"] = transitionJ(*c.transitionIn);
    if (c.transitionOut) j["transitionOut"] = transitionJ(*c.transitionOut);
    if (c.kind == ClipKind::Text || c.kind == ClipKind::Subtitle) {
        j["text"] = c.text;
        const TextStyle& s = c.textStyle;
        j["textStyle"] = {{"font", s.fontFamily}, {"size", s.fontSize},     {"weight", s.fontWeight},
                          {"italic", s.italic},   {"align", alignName(s.align)}, {"tracking", s.tracking},
                          {"lineSpacing", s.lineSpacing}, {"kerning", s.kerningEnabled}};
        j["textParams"] = paramSetToJson(c.textParams);
    }
    if (c.kind == ClipKind::Solid) j["solidColor"] = valueJ(c.solidColor);
    return j;
}

Clip clipFromJson(const Json& j) {
    Clip c;
    c.id = idOf(j, "id");
    if (c.id == kInvalidId) c.id = newId();
    c.kind = clipKindFromName(val<std::string>(j, "kind", "media"));
    c.name = val<std::string>(j, "name", "");
    c.start = timeOf(j, "start");
    c.duration = timeOf(j, "duration");
    c.sourceIn = timeOf(j, "sourceIn");
    c.media = idOf(j, "media");
    c.nested = idOf(j, "nested");
    c.streamIndex = val<int>(j, "stream", -1);
    c.speed = rationalOf(j, "speed", Rational{1, 1});
    if (c.speed.num <= 0 || c.speed.den <= 0) c.speed = {1, 1};
    c.reverse = val<bool>(j, "reverse", false);
    c.freezeFrame = val<bool>(j, "freeze", false);
    c.enabled = val<bool>(j, "enabled", true);
    c.linkGroup = idOf(j, "link");
    c.group = idOf(j, "group");
    c.colorLabel = val<uint32_t>(j, "color", 0);
    if (auto it = j.find("transform"); it != j.end()) c.transform = paramSetFromJson(*it);
    c.flipH = val<bool>(j, "flipH", false);
    c.flipV = val<bool>(j, "flipV", false);
    c.blend = blendModeFromName(val<std::string>(j, "blend", "normal"));
    if (auto it = j.find("audio"); it != j.end()) c.audio = paramSetFromJson(*it);
    c.gainDb = val<float>(j, "gainDb", 0.0f);
    if (auto it = j.find("effects"); it != j.end()) c.effects = effectsOf(*it);
    c.transitionIn = transitionOf(j, "transitionIn");
    c.transitionOut = transitionOf(j, "transitionOut");
    c.text = val<std::string>(j, "text", "");
    if (auto it = j.find("textStyle"); it != j.end() && it->is_object()) {
        TextStyle& s = c.textStyle;
        s.fontFamily = val<std::string>(*it, "font", s.fontFamily);
        s.fontSize = val<float>(*it, "size", s.fontSize);
        s.fontWeight = val<int>(*it, "weight", s.fontWeight);
        s.italic = val<bool>(*it, "italic", s.italic);
        s.align = alignOf(val<std::string>(*it, "align", "center"));
        s.tracking = val<float>(*it, "tracking", s.tracking);
        s.lineSpacing = val<float>(*it, "lineSpacing", s.lineSpacing);
        s.kerningEnabled = val<float>(*it, "kerning", s.kerningEnabled);
    }
    if (auto it = j.find("textParams"); it != j.end()) c.textParams = paramSetFromJson(*it);
    if (auto it = j.find("solidColor"); it != j.end()) c.solidColor = valueOf(*it);
    return c;
}

// ---------------------------------------------------------------- track

Json trackPropsToJson(const Track& t) {
    Json j{{"id", idJ(t.id)}, {"kind", trackKindName(t.kind)}, {"name", t.name}};
    if (t.locked) j["locked"] = true;
    if (t.hidden) j["hidden"] = true;
    if (t.muted) j["muted"] = true;
    if (t.solo) j["solo"] = true;
    if (!t.syncLock) j["syncLock"] = false;
    if (t.height > 0) j["height"] = t.height;
    if (t.volumeDb != 0.0f) j["volumeDb"] = t.volumeDb;
    if (t.pan != 0.0f) j["pan"] = t.pan;
    if (!t.effects.empty()) j["effects"] = effectsJ(t.effects);
    return j;
}

Track trackPropsFromJson(const Json& j) {
    Track t;
    t.id = idOf(j, "id");
    if (t.id == kInvalidId) t.id = newId();
    t.kind = trackKindFromName(val<std::string>(j, "kind", "video"));
    t.name = val<std::string>(j, "name", "");
    t.locked = val<bool>(j, "locked", false);
    t.hidden = val<bool>(j, "hidden", false);
    t.muted = val<bool>(j, "muted", false);
    t.solo = val<bool>(j, "solo", false);
    t.syncLock = val<bool>(j, "syncLock", true);
    t.height = val<float>(j, "height", 0.0f);
    t.volumeDb = val<float>(j, "volumeDb", 0.0f);
    t.pan = val<float>(j, "pan", 0.0f);
    if (auto it = j.find("effects"); it != j.end()) t.effects = effectsOf(*it);
    return t;
}

Json trackToJson(const Track& t) {
    Json j = trackPropsToJson(t);
    Json clips = Json::array();
    for (auto& c : t.clips) clips.push_back(clipToJson(*c));
    j["clips"] = std::move(clips);
    return j;
}

Track trackFromJson(const Json& j) {
    Track t = trackPropsFromJson(j);
    if (auto it = j.find("clips"); it != j.end() && it->is_array()) {
        for (auto& cj : *it) {
            Clip c = clipFromJson(cj);
            if (c.duration.ticks <= 0) continue;
            t.clips.push_back(std::make_shared<Clip>(std::move(c)));
        }
    }
    std::stable_sort(t.clips.begin(), t.clips.end(), [](const ClipPtr& a, const ClipPtr& b) { return a->start < b->start; });
    return t;
}

// ---------------------------------------------------------------- sequence

Json sequencePropsToJson(const Sequence& s) {
    Json j{{"id", idJ(s.id)},
           {"name", s.name},
           {"width", s.width},
           {"height", s.height},
           {"frameRate", rationalJ(s.frameRate)},
           {"sampleRate", s.sampleRate},
           {"audioChannels", s.audioChannels},
           {"masterVolumeDb", s.masterVolumeDb},
           {"backgroundColor", valueJ(s.backgroundColor)}};
    if (!s.masterEffects.empty()) j["masterEffects"] = effectsJ(s.masterEffects);
    if (s.workArea) j["workArea"] = {{"start", timeJ(s.workArea->start)}, {"duration", timeJ(s.workArea->duration)}};
    Json markers = Json::array();
    for (auto& m : s.markers) {
        Json mj{{"id", idJ(m.id)}, {"time", timeJ(m.time)}, {"kind", markerKindName(m.kind)}, {"color", m.color}};
        if (m.duration.ticks) mj["duration"] = timeJ(m.duration);
        if (!m.name.empty()) mj["name"] = m.name;
        if (!m.comment.empty()) mj["comment"] = m.comment;
        if (m.score != 0.0f) mj["score"] = m.score;
        markers.push_back(std::move(mj));
    }
    j["markers"] = std::move(markers);
    return j;
}

Sequence sequencePropsFromJson(const Json& j) {
    Sequence s;
    s.id = idOf(j, "id");
    if (s.id == kInvalidId) s.id = newId();
    s.name = val<std::string>(j, "name", "Sequence");
    s.width = std::clamp(val<int>(j, "width", 1920), 16, 16384);
    s.height = std::clamp(val<int>(j, "height", 1080), 16, 16384);
    s.frameRate = rationalOf(j, "frameRate", Rational{30, 1});
    if (s.frameRate.num <= 0 || s.frameRate.den <= 0) s.frameRate = {30, 1};
    s.sampleRate = val<int>(j, "sampleRate", 48000);
    s.audioChannels = val<int>(j, "audioChannels", 2);
    s.masterVolumeDb = val<float>(j, "masterVolumeDb", 0.0f);
    if (auto it = j.find("backgroundColor"); it != j.end()) s.backgroundColor = valueOf(*it);
    if (auto it = j.find("masterEffects"); it != j.end()) s.masterEffects = effectsOf(*it);
    if (auto it = j.find("workArea"); it != j.end() && it->is_object())
        s.workArea = TimeRange{timeOf(*it, "start"), timeOf(*it, "duration")};
    if (auto it = j.find("markers"); it != j.end() && it->is_array()) {
        for (auto& mj : *it) {
            Marker m;
            m.id = idOf(mj, "id");
            if (m.id == kInvalidId) m.id = newId();
            m.time = timeOf(mj, "time");
            m.duration = timeOf(mj, "duration");
            m.kind = markerKindFromName(val<std::string>(mj, "kind", "standard"));
            m.color = val<uint32_t>(mj, "color", m.color);
            m.name = val<std::string>(mj, "name", "");
            m.comment = val<std::string>(mj, "comment", "");
            m.score = val<float>(mj, "score", 0.0f);
            s.markers.push_back(std::move(m));
        }
        std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.time < b.time; });
    }
    return s;
}

Json sequenceToJson(const Sequence& s) {
    Json j = sequencePropsToJson(s);
    Json tracks = Json::array();
    for (auto& t : s.tracks) tracks.push_back(trackToJson(*t));
    j["tracks"] = std::move(tracks);
    return j;
}

Sequence sequenceFromJson(const Json& j) {
    Sequence s = sequencePropsFromJson(j);
    std::vector<TrackPtr> visual, audio;
    if (auto it = j.find("tracks"); it != j.end() && it->is_array()) {
        for (auto& tj : *it) {
            auto t = std::make_shared<Track>(trackFromJson(tj));
            (isVisualTrack(t->kind) ? visual : audio).push_back(std::move(t));
        }
    }
    s.tracks = std::move(visual);
    s.tracks.insert(s.tracks.end(), audio.begin(), audio.end());
    return s;
}

// ---------------------------------------------------------------- media

Json mediaInfoToJson(const MediaInfo& m) {
    Json j{{"kind", mediaKindName(m.kind)},
           {"container", m.container},
           {"duration", timeJ(m.duration)},
           {"startTime", timeJ(m.startTime)},
           {"bitRate", m.bitRate}};
    if (!m.timecode.empty()) j["timecode"] = m.timecode;
    Json v = Json::array();
    for (auto& s : m.video)
        v.push_back({{"index", s.index},          {"width", s.width},           {"height", s.height},
                     {"frameRate", rationalJ(s.frameRate)}, {"sar", rationalJ(s.sampleAspect)},
                     {"codec", s.codec},          {"pixfmt", s.pixelFormat},    {"bitDepth", s.bitDepth},
                     {"rotation", s.rotation},    {"colorSpace", s.colorSpace}, {"transfer", s.colorTransfer},
                     {"primaries", s.colorPrimaries}, {"fullRange", s.fullRange}, {"alpha", s.hasAlpha},
                     {"frames", s.frameCount}});
    j["video"] = std::move(v);
    Json a = Json::array();
    for (auto& s : m.audio)
        a.push_back({{"index", s.index},
                     {"sampleRate", s.sampleRate},
                     {"channels", s.channels},
                     {"codec", s.codec},
                     {"layout", s.channelLayout},
                     {"language", s.language}});
    j["audio"] = std::move(a);
    return j;
}

MediaInfo mediaInfoFromJson(const Json& j) {
    MediaInfo m;
    if (!j.is_object()) return m;
    m.kind = mediaKindOf(val<std::string>(j, "kind", "unknown"));
    m.container = val<std::string>(j, "container", "");
    m.duration = timeOf(j, "duration");
    m.startTime = timeOf(j, "startTime");
    m.bitRate = val<int64_t>(j, "bitRate", 0);
    m.timecode = val<std::string>(j, "timecode", "");
    if (auto it = j.find("video"); it != j.end() && it->is_array())
        for (auto& s : *it) {
            VideoStreamInfo v;
            v.index = val<int>(s, "index", -1);
            v.width = val<int>(s, "width", 0);
            v.height = val<int>(s, "height", 0);
            v.frameRate = rationalOf(s, "frameRate", Rational{30, 1});
            v.sampleAspect = rationalOf(s, "sar", Rational{1, 1});
            v.codec = val<std::string>(s, "codec", "");
            v.pixelFormat = val<std::string>(s, "pixfmt", "");
            v.bitDepth = val<int>(s, "bitDepth", 8);
            v.rotation = val<int>(s, "rotation", 0);
            v.colorSpace = val<std::string>(s, "colorSpace", "");
            v.colorTransfer = val<std::string>(s, "transfer", "");
            v.colorPrimaries = val<std::string>(s, "primaries", "");
            v.fullRange = val<bool>(s, "fullRange", false);
            v.hasAlpha = val<bool>(s, "alpha", false);
            v.frameCount = val<int64_t>(s, "frames", 0);
            m.video.push_back(std::move(v));
        }
    if (auto it = j.find("audio"); it != j.end() && it->is_array())
        for (auto& s : *it) {
            AudioStreamInfo a;
            a.index = val<int>(s, "index", -1);
            a.sampleRate = val<int>(s, "sampleRate", 0);
            a.channels = val<int>(s, "channels", 0);
            a.codec = val<std::string>(s, "codec", "");
            a.channelLayout = val<std::string>(s, "layout", "");
            a.language = val<std::string>(s, "language", "");
            m.audio.push_back(std::move(a));
        }
    return m;
}

Json mediaToJson(const MediaItem& m) {
    Json j{{"id", idJ(m.id)},
           {"name", m.name},
           {"path", m.path},
           {"relativePath", m.relativePath},
           {"fileSize", m.fileSize},
           {"fileModifiedNs", m.fileModifiedNs},
           {"info", mediaInfoToJson(m.info)}};
    if (m.bin) j["bin"] = idJ(m.bin);
    if (m.favorite) j["favorite"] = true;
    if (!m.tags.empty()) j["tags"] = m.tags;
    if (!m.notes.empty()) j["notes"] = m.notes;
    return j;
}

MediaItem mediaFromJson(const Json& j) {
    MediaItem m;
    m.id = idOf(j, "id");
    if (m.id == kInvalidId) m.id = newId();
    m.name = val<std::string>(j, "name", "");
    m.path = val<std::string>(j, "path", "");
    m.relativePath = val<std::string>(j, "relativePath", "");
    m.fileSize = val<uint64_t>(j, "fileSize", 0);
    m.fileModifiedNs = val<int64_t>(j, "fileModifiedNs", 0);
    if (auto it = j.find("info"); it != j.end()) m.info = mediaInfoFromJson(*it);
    m.bin = idOf(j, "bin");
    m.favorite = val<bool>(j, "favorite", false);
    m.tags = val<std::vector<std::string>>(j, "tags", {});
    m.notes = val<std::string>(j, "notes", "");
    return m;
}

// ---------------------------------------------------------------- project

Json projectPropsToJson(const Project& p) {
    Json bins = Json::array();
    for (auto& b : p.bins) bins.push_back({{"id", idJ(b.id)}, {"parent", idJ(b.parent)}, {"name", b.name}});
    return {{"id", idJ(p.id)},
            {"name", p.name},
            {"created", p.createdUtc},
            {"settings",
             {{"width", p.settings.width},
              {"height", p.settings.height},
              {"frameRate", rationalJ(p.settings.frameRate)},
              {"sampleRate", p.settings.sampleRate}}},
            {"bins", std::move(bins)},
            {"activeSequence", idJ(p.activeSequence)}};
}

void projectPropsFromJson(const Json& j, Project& p) {
    p.id = idOf(j, "id");
    if (p.id == kInvalidId) p.id = newId();
    p.name = val<std::string>(j, "name", "Untitled");
    p.createdUtc = val<std::string>(j, "created", "");
    if (auto it = j.find("settings"); it != j.end() && it->is_object()) {
        p.settings.width = val<int>(*it, "width", 1920);
        p.settings.height = val<int>(*it, "height", 1080);
        p.settings.frameRate = rationalOf(*it, "frameRate", Rational{30, 1});
        p.settings.sampleRate = val<int>(*it, "sampleRate", 48000);
    }
    p.bins.clear();
    if (auto it = j.find("bins"); it != j.end() && it->is_array())
        for (auto& bj : *it) p.bins.push_back(Bin{idOf(bj, "id"), idOf(bj, "parent"), val<std::string>(bj, "name", "Bin")});
    p.activeSequence = idOf(j, "activeSequence");
}

Json projectToJson(const Project& p, const SaveOptions& opt) {
    Json pj = projectPropsToJson(p);
    Json media = Json::array();
    for (auto& m : p.media) {
        Json mj = mediaToJson(*m);
        if (!opt.projectDir.empty() && !m->path.empty()) {
            std::error_code ec;
            auto rel = std::filesystem::relative(pathFromUtf8(m->path), opt.projectDir, ec);
            if (!ec && !rel.empty()) mj["relativePath"] = pathToUtf8(rel.generic_u8string());
        }
        media.push_back(std::move(mj));
    }
    pj["media"] = std::move(media);
    Json seqs = Json::array();
    for (auto& s : p.sequences) seqs.push_back(sequenceToJson(*s));
    pj["sequences"] = std::move(seqs);
    return {{"format", "avicap-project"},
            {"schemaVersion", kProjectSchemaVersion},
            {"appVersion", AVICAP_VERSION_STRING},
            {"savedUtc", utcNowIso8601()},
            {"project", std::move(pj)}};
}

std::string projectToString(const Project& p, const SaveOptions& opt) {
    return projectToJson(p, opt).dump(opt.pretty ? 1 : -1, '\t');
}

Result<LoadResult> projectFromJson(Json j, const std::filesystem::path& projectDir) {
    if (!j.is_object()) return Result<LoadResult>::error("Not a project file");
    if (val<std::string>(j, "format", "") != "avicap-project") return Result<LoadResult>::error("Not an AviCap project file");
    LoadResult result;
    result.loadedSchemaVersion = val<int>(j, "schemaVersion", 0);
    if (result.loadedSchemaVersion > kProjectSchemaVersion)
        return Result<LoadResult>::error("This project was created by a newer version of AviCap Studio (schema " +
                                         std::to_string(result.loadedSchemaVersion) + "). Please update the application.");
    if (result.loadedSchemaVersion < kProjectSchemaVersion) {
        Status st = migrateProjectJson(j, result.loadedSchemaVersion, result.warnings);
        if (!st) return st;
        result.migrated = true;
    }
    auto pit = j.find("project");
    if (pit == j.end() || !pit->is_object()) return Result<LoadResult>::error("Project data missing");
    const Json& pj = *pit;
    auto project = std::make_shared<Project>();
    projectPropsFromJson(pj, *project);
    project->appVersion = val<std::string>(j, "appVersion", "");
    if (auto it = pj.find("media"); it != pj.end() && it->is_array()) {
        for (auto& mj : *it) {
            MediaItem m = mediaFromJson(mj);
            // Relative path wins when the absolute path is gone (project folder moved).
            std::error_code ec;
            if (!projectDir.empty() && !m.relativePath.empty() && !std::filesystem::exists(pathFromUtf8(m.path), ec)) {
                auto candidate = projectDir / pathFromUtf8(m.relativePath);
                if (std::filesystem::exists(candidate, ec)) {
                    result.warnings.push_back("Media '" + m.name + "' found relative to the project and relinked.");
                    m.path = pathToUtf8(candidate.lexically_normal());
                }
            }
            project->media.push_back(std::make_shared<MediaItem>(std::move(m)));
        }
    }
    if (auto it = pj.find("sequences"); it != pj.end() && it->is_array())
        for (auto& sj : *it) project->sequences.push_back(std::make_shared<Sequence>(sequenceFromJson(sj)));
    if (project->sequences.empty()) {
        auto s = std::make_shared<Sequence>(makeSequence("Sequence 1", project->settings.width, project->settings.height,
                                                          project->settings.frameRate));
        project->sequences.push_back(s);
        result.warnings.push_back("Project had no sequence; an empty one was created.");
    }
    if (!project->findSequence(project->activeSequence)) project->activeSequence = project->sequences.front()->id;

    // Repair instead of refusing to open: drop overlapping clips, report it.
    for (auto& sp : project->sequences) {
        std::string err = validateSequence(*sp);
        if (err.empty()) continue;
        auto s = std::make_shared<Sequence>(*sp);
        for (auto& tp : s->tracks) {
            auto t = std::make_shared<Track>(*tp);
            std::vector<ClipPtr> kept;
            Time prevEnd{std::numeric_limits<int64_t>::min()};
            for (auto& c : t->clips) {
                if (c->start < prevEnd || c->duration.ticks <= 0 || c->start.ticks < 0) {
                    result.warnings.push_back("Removed invalid clip '" + c->name + "' in track " + t->name);
                    continue;
                }
                kept.push_back(c);
                prevEnd = c->end();
            }
            t->clips = std::move(kept);
            tp = t;
        }
        sp = s;
    }
    const std::string perr = validateProject(*project);
    if (!perr.empty()) {
        // Clips referencing missing media/sequences are removed.
        for (auto& sp : project->sequences) {
            auto s = std::make_shared<Sequence>(*sp);
            for (auto& tp : s->tracks) {
                auto t = std::make_shared<Track>(*tp);
                std::vector<ClipPtr> kept;
                for (auto& c : t->clips) {
                    const bool bad = (c->kind == ClipKind::Media && !project->findMedia(c->media)) ||
                                     (c->kind == ClipKind::Compound && !project->findSequence(c->nested));
                    if (bad) result.warnings.push_back("Removed clip '" + c->name + "' with a missing reference");
                    else kept.push_back(c);
                }
                t->clips = std::move(kept);
                tp = t;
            }
            sp = s;
        }
        const std::string again = validateProject(*project);
        if (!again.empty()) return Result<LoadResult>::error("Project is damaged: " + again);
    }
    result.project = project;
    return result;
}

Result<LoadResult> projectFromString(const std::string& text, const std::filesystem::path& projectDir) {
    Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded()) return Result<LoadResult>::error("Project file is not valid JSON (damaged or truncated)");
    return projectFromJson(std::move(j), projectDir);
}

Status saveProjectFile(const Project& p, const std::filesystem::path& file) {
    SaveOptions opt;
    opt.projectDir = file.parent_path();
    const std::string text = projectToString(p, opt);
    AtomicWriteOptions w;
    w.keepBackup = true;
    Status st = writeFileAtomic(file, text, w);
    if (st) AVC_INFO("project", "Saved project '{}' ({} bytes)", pathToUtf8(file), text.size());
    else AVC_ERROR("project", "Saving '{}' failed: {}", pathToUtf8(file), st.message());
    return st;
}

Result<LoadResult> loadProjectFile(const std::filesystem::path& file) {
    auto data = readFileBytes(file);
    if (!data) return Result<LoadResult>::error("Cannot read project file: " + pathToUtf8(file));
    auto r = projectFromString(*data, file.parent_path());
    if (!r) {
        // Fall back to the backup written by the previous save.
        auto bak = file;
        bak += ".bak";
        if (auto b = readFileBytes(bak)) {
            auto rb = projectFromString(*b, file.parent_path());
            if (rb) {
                rb->warnings.insert(rb->warnings.begin(),
                                    "The project file was damaged; the previous backup (.bak) was loaded instead.");
                AVC_WARN("project", "Loaded backup for '{}': {}", pathToUtf8(file), r.errorMessage());
                return rb;
            }
        }
        return r;
    }
    AVC_INFO("project", "Loaded project '{}' (schema {}, {} media, {} sequences)", pathToUtf8(file),
             r->loadedSchemaVersion, r->project->media.size(), r->project->sequences.size());
    return r;
}

}  // namespace avc
