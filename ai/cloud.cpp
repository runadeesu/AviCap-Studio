#include "ai/cloud.h"

#include <cstdio>

#include "effects/effects.h"
#include "export/export_settings.h"

namespace avc::ai {

namespace {

Json numberProp(const char* description) { return {{"type", "number"}, {"description", description}}; }
Json stringProp(const char* description) { return {{"type", "string"}, {"description", description}}; }

}  // namespace

Json cloudPlanSchema() {
    Json ops = Json::array();
    for (const auto& op : supportedOps()) ops.push_back(op);
    Json param = {{"type", "object"},
                  {"properties", {{"name", stringProp("effect parameter id")}, {"value", {{"type", "number"}}}}},
                  {"required", {"name", "value"}},
                  {"additionalProperties", false}};
    Json step = {
        {"type", "object"},
        {"properties",
         {{"op", {{"type", "string"}, {"enum", ops}}},
          {"start", numberProp("seconds")},
          {"end", numberProp("seconds")},
          {"time", numberProp("seconds")},
          {"duration", numberProp("seconds")},
          {"text", stringProp("title or subtitle text")},
          {"target", stringProp("see the operation list")},
          {"db", numberProp("volume in dB")},
          {"effect", stringProp("effect id")},
          {"params", {{"type", "array"}, {"items", param}}},
          {"factor", numberProp("speed factor, 1 = normal")},
          {"direction", {{"type", "string"}, {"enum", {"in", "out", "both"}}}},
          {"preset", stringProp("export preset id")},
          {"name", stringProp("marker name")},
          {"threshold_db", numberProp("silence threshold in dBFS (optional)")},
          {"min_silence", numberProp("shortest pause to remove, seconds (optional)")}}},
        {"required", {"op"}},
        {"additionalProperties", false}};
    return {{"type", "object"},
            {"properties",
             {{"steps", {{"type", "array"}, {"items", step}}},
              {"notes", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
            {"required", {"steps", "notes"}},
            {"additionalProperties", false}};
}

std::string cloudSystemPrompt() {
    // Stable text (no timestamps or per-request data) so it can be cached.
    std::string s =
        "You are the editing assistant of AviCap Studio, a desktop video editor. The user describes an edit in "
        "Japanese or English; you translate it into a plan of operations that the editor applies to its timeline. "
        "The user reviews the plan and a preview before anything changes, and the whole plan is undoable.\n\n"
        "The user message contains the instruction and a JSON description of the current timeline (tracks, clips "
        "with start/end in seconds on the timeline, text of titles and subtitles, markers, playhead). All times you "
        "output are timeline seconds.\n\n"
        "Operations (field names refer to the step object):\n"
        "- remove_silence: cut pauses in speech on all tracks (ripple). Optional threshold_db, min_silence.\n"
        "- delete_range: start, end. Removes that part of the timeline (ripple, all tracks).\n"
        "- keep_range: start, end. Removes everything outside it.\n"
        "- split_at: time. Splits every track.\n"
        "- add_text: text, start, duration. Adds a title on a free video track.\n"
        "- add_subtitle: text, start, duration. Adds a subtitle.\n"
        "- set_volume: target (\"music\" = audio-only clips such as BGM, \"voice\" = audio of video clips, \"all\" = "
        "master, \"selected\"), db.\n"
        "- apply_effect: effect (id from the list below), target (\"all_video\", \"selected\", \"all_audio\"), params "
        "(optional list of {name, value}).\n"
        "- set_speed: factor (0.05-20), target (\"selected\" or \"all_video\").\n"
        "- fade: direction (\"in\", \"out\", \"both\"), duration. Fades the start/end of the sequence.\n"
        "- add_beat_markers: adds markers on the beats of the music.\n"
        "- split_at_scenes: target (\"all_video\" or \"selected\"). Splits clips at detected scene changes.\n"
        "- add_marker: time, name.\n"
        "- export: preset (id from the list below). Queues an export after the edit.\n\n"
        "Rules: use only these operations and ids. Keep steps in the order the user asked for; note that "
        "delete_range and remove_silence shift everything after them, so when several ranges are deleted, list the "
        "later ones first. If part of the instruction cannot be expressed with these operations, or is ambiguous, "
        "do not guess: leave it out and explain it in notes, written in the user's language. Return an empty steps "
        "list if nothing applies.\n\n";
    s += "Video effects (id: name; parameters):\n";
    for (auto kind : {fx::EffectKind::Video, fx::EffectKind::Audio}) {
        if (kind == fx::EffectKind::Audio) s += "Audio effects:\n";
        for (const fx::EffectDef* d : fx::EffectRegistry::instance().list(kind)) {
            s += "- " + d->id + ": " + d->name;
            std::string params;
            for (const auto& p : d->params) {
                if (p.type != ParamType::Float) continue;
                char range[64];
                std::snprintf(range, sizeof range, " %g..%g", p.minValue, p.maxValue);
                params += (params.empty() ? "" : ", ") + p.id + range;
            }
            if (!params.empty()) s += "; " + params;
            s += "\n";
        }
    }
    s += "\nExport presets:\n";
    for (const auto& p : exp::presets()) s += "- " + p.id + ": " + p.name + "\n";
    return s;
}

Json buildCloudRequest(const std::string& instruction, const Json& timeline, const CloudOptions& opt) {
    Json user = {{"type", "text"},
                 {"text", "Instruction:\n" + instruction + "\n\nTimeline:\n" + timeline.dump()}};
    return {{"model", opt.model},
            {"max_tokens", opt.maxTokens},
            {"system", Json::array({{{"type", "text"}, {"text", cloudSystemPrompt()}, {"cache_control", {{"type", "ephemeral"}}}}})},
            {"messages", Json::array({{{"role", "user"}, {"content", Json::array({user})}}})},
            {"output_config", {{"effort", opt.effort}, {"format", {{"type", "json_schema"}, {"schema", cloudPlanSchema()}}}}},
            // A classifier decline is retried server-side on Anthropic's recommended model.
            {"fallbacks", "default"}};
}

Result<Plan> parseCloudResponse(int status, const std::string& body) {
    const Json j = Json::parse(body, nullptr, false);
    if (status != 200) {
        std::string msg = "HTTP " + std::to_string(status);
        if (j.is_object() && j.contains("error") && j["error"].is_object()) msg += ": " + j["error"].value("message", std::string());
        if (status == 401) msg += " (check the API key)";
        if (status == 429 || status == 529) msg += " (the service is busy - try again later)";
        return Result<Plan>::error(msg);
    }
    if (!j.is_object()) return Result<Plan>::error("invalid response");
    const std::string stop = j.value("stop_reason", std::string());
    if (stop == "refusal") return Result<Plan>::error("the request was declined by the model");
    if (stop == "max_tokens") return Result<Plan>::error("the reply was too long and was cut off");
    std::string text;
    if (j.contains("content") && j["content"].is_array())
        for (const auto& b : j["content"])
            if (b.is_object() && b.value("type", std::string()) == "text") text += b.value("text", std::string());
    const Json reply = Json::parse(text, nullptr, false);
    if (!reply.is_object() || !reply.contains("steps") || !reply["steps"].is_array())
        return Result<Plan>::error("the reply did not contain a plan");
    // Schema form -> engine form ({op, args}).
    Json plan = {{"steps", Json::array()}};
    for (const auto& st : reply["steps"]) {
        if (!st.is_object()) continue;
        Json args = Json::object();
        for (const auto& [k, v] : st.items()) {
            if (k == "op") continue;
            if (k == "params" && v.is_array()) {
                Json params = Json::object();
                for (const auto& p : v)
                    if (p.is_object() && p.contains("name") && p["name"].is_string() && p.contains("value") && p["value"].is_number())
                        params[p["name"].get<std::string>()] = p["value"];
                args["params"] = params;
            } else {
                args[k] = v;
            }
        }
        plan["steps"].push_back({{"op", st.value("op", std::string())}, {"args", args}});
    }
    auto p = planFromJson(plan, j.value("model", std::string("claude")));
    if (!p) return p;
    if (reply.contains("notes") && reply["notes"].is_array())
        for (const auto& n : reply["notes"])
            if (n.is_string() && !n.get<std::string>().empty()) p->notes.push_back(n.get<std::string>());
    return p;
}

Result<Plan> requestCloudPlan(const std::string& instruction, const Json& timeline, const CloudOptions& opt,
                              const HttpTransport& transport, const CancelToken& cancel) {
    if (opt.apiKey.empty()) return Result<Plan>::error("no API key");
    HttpRequest req;
    req.url = opt.endpoint;
    req.headers = {{"Content-Type", "application/json"},
                   {"x-api-key", opt.apiKey},
                   {"anthropic-version", "2023-06-01"},
                   {"anthropic-beta", "server-side-fallback-2026-07-01"}};
    req.body = buildCloudRequest(instruction, timeline, opt).dump();
    req.timeoutMs = 180000;
    const HttpResponse r = transport(req, cancel);
    if (!r.error.empty()) return Result<Plan>::error(r.error);
    return parseCloudResponse(r.status, r.body);
}

}  // namespace avc::ai
