#include <doctest.h>

#include "ai/plan.h"
#include "core/i18n.h"
#include "media/probe.h"
#include "tests/test_support.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"

using namespace avc;

namespace {
Time S(double s) { return Time::fromSeconds(s); }

ai::PlanContext ctx60() {
    ai::PlanContext c;
    c.sequenceDuration = S(60);
    c.playhead = S(12);
    return c;
}

std::vector<std::string> ops(const ai::Plan& p) {
    std::vector<std::string> v;
    for (const auto& s : p.steps) v.push_back(s.op);
    return v;
}
}  // namespace

TEST_CASE("plan: Japanese instructions become steps") {
    auto p = ai::planFromText("無音をカットして、BGMの音量を-12dBにして。白黒にして、YouTube用に書き出して", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"remove_silence", "set_volume", "apply_effect", "export"});
    CHECK(p.notes.empty());
    CHECK(p.steps[1].args["target"] == "music");
    CHECK(p.steps[1].args["db"].get<double>() == doctest::Approx(-12));
    CHECK(p.steps[2].args["effect"] == "color.basic");
    CHECK(p.steps[2].args["params"]["saturation"].get<double>() == doctest::Approx(0));
    CHECK(p.steps[3].args["preset"] == "youtube-1080p");
    for (const auto& s : p.steps) CHECK_FALSE(s.description.empty());
}

TEST_CASE("plan: time expressions and ranges") {
    auto p = ai::planFromText("最初の10秒と最後の5秒をカット", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"delete_range", "delete_range"});
    CHECK(p.steps[0].args["end"].get<double>() == doctest::Approx(10));
    CHECK(p.steps[1].args["start"].get<double>() == doctest::Approx(55));
    CHECK(p.steps[1].args["end"].get<double>() == doctest::Approx(60));

    p = ai::planFromText("1:30から1分の部分を削除", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"delete_range"});
    CHECK(p.steps[0].args["start"].get<double>() == doctest::Approx(60));
    CHECK(p.steps[0].args["end"].get<double>() == doctest::Approx(90));

    p = ai::planFromText("０：０５〜０：２０だけ残して", ctx60());  // full-width digits
    REQUIRE(ops(p) == std::vector<std::string>{"keep_range"});
    CHECK(p.steps[0].args["start"].get<double>() == doctest::Approx(5));
    CHECK(p.steps[0].args["end"].get<double>() == doctest::Approx(20));

    p = ai::planFromText("2.5秒で分割", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"split_at"});
    CHECK(p.steps[0].args["time"].get<double>() == doctest::Approx(2.5));

    p = ai::planFromText("ここで分割", ctx60());  // playhead
    REQUIRE(ops(p) == std::vector<std::string>{"split_at"});
    CHECK(p.steps[0].args["time"].get<double>() == doctest::Approx(12));
}

TEST_CASE("plan: quoted text, subtitles and titles") {
    auto p = ai::planFromText("5秒に「こんにちは、世界」という字幕を入れて", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"add_subtitle"});
    CHECK(p.steps[0].args["text"] == "こんにちは、世界");  // the comma inside quotes does not split
    CHECK(p.steps[0].args["start"].get<double>() == doctest::Approx(5));

    p = ai::planFromText("Add a title \"My Trip\" at 0:02", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"add_text"});
    CHECK(p.steps[0].args["text"] == "My Trip");
    CHECK(p.steps[0].args["start"].get<double>() == doctest::Approx(2));
}

TEST_CASE("plan: English, merged fragments and unknown text") {
    auto p = ai::planFromText("Remove the silences, make it black and white and export for TikTok", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"remove_silence", "apply_effect", "export"});
    CHECK(p.steps[2].args["preset"] == "tiktok");

    // A comma inside one instruction: the fragment is joined with the next one.
    p = ai::planFromText("BGMの音量を、-10dBにして", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"set_volume"});
    CHECK(p.steps[0].args["target"] == "music");
    CHECK(p.steps[0].args["db"].get<double>() == doctest::Approx(-10));

    p = ai::planFromText("声を大きく、シネマ風にして、フェードアウト3秒", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"set_volume", "apply_effect", "apply_effect", "fade"});
    CHECK(p.steps[0].args["target"] == "voice");
    CHECK(p.steps[0].args["db"].get<double>() > 0);
    CHECK(p.steps[3].args["direction"] == "out");
    CHECK(p.steps[3].args["duration"].get<double>() == doctest::Approx(3));

    p = ai::planFromText("ビートにマーカーを打って、シーンごとに分割", ctx60());
    REQUIRE(ops(p) == std::vector<std::string>{"add_beat_markers", "split_at_scenes"});

    p = ai::planFromText("今日はいい天気ですね", ctx60());
    CHECK(p.steps.empty());
    REQUIRE(p.notes.size() == 1);
}

TEST_CASE("plan: every UI example is understood in Japanese and English") {
    for (Language lang : {Language::Japanese, Language::English}) {
        setLanguage(lang);
        for (const char* ex : ai::exampleInstructions()) {
            const std::string text = tr(ex);
            auto p = ai::planFromText(text, ctx60());
            CHECK_MESSAGE(!p.steps.empty(), text);
            CHECK_MESSAGE(p.notes.empty(), text);
        }
    }
    setLanguage(Language::Japanese);
}

TEST_CASE("plan: JSON from a cloud model is validated") {
    using J = ai::Json;
    auto ok = ai::planFromJson(J::parse(R"({"steps":[{"op":"delete_range","args":{"start":1,"end":2}},
        {"op":"apply_effect","args":{"effect":"color.basic","target":"all_video","params":{"saturation":0}}}]})"),
                               "cloud");
    REQUIRE(ok);
    CHECK(ok->steps.size() == 2);
    CHECK(ok->source == "cloud");
    CHECK_FALSE(ok->steps[0].description.empty());

    CHECK_FALSE(ai::planFromJson(J::parse(R"({"steps":[{"op":"rm -rf","args":{}}]})"), "cloud"));
    CHECK_FALSE(ai::planFromJson(J::parse(R"({"steps":[{"op":"delete_range","args":{"start":5,"end":2}}]})"), "cloud"));
    CHECK_FALSE(ai::planFromJson(J::parse(R"({"steps":[{"op":"apply_effect","args":{"effect":"no.such"}}]})"), "cloud"));
    CHECK_FALSE(ai::planFromJson(J::parse(R"({"steps":[{"op":"set_volume","args":{"db":1000}}]})"), "cloud"));
    CHECK_FALSE(ai::planFromJson(J::parse(R"({"nope":1})"), "cloud"));
    // Round trip.
    auto again = ai::planFromJson(ok->toJson(), "cloud");
    REQUIRE(again);
    CHECK(again->steps.size() == 2);
}

TEST_CASE("plan: apply is one undoable edit and fails atomically") {
    const std::string f = test::testMedia("av_1080p30.mp4");
    if (f.empty()) return;
    auto media = createMediaItem(f);
    REQUIRE(media);
    Document doc(std::make_shared<Project>(makeProject("Plan", ProjectSettings{1280, 720, {30, 1}, 48000})));
    REQUIRE(doc.edit("import", [&](ProjectEditor& pe) {
        pe.addMedia(*media);
        return Status::ok();
    }));
    const SequenceId sid = doc.project().activeSequence;
    REQUIRE(doc.editSequence("add", sid, [&](SequenceEditor& e) { return edit::addMediaClip(e, *media, S(0), 0, 3, edit::PlaceMode::Overwrite).status(); }));
    const Time before = doc.project().active()->duration();
    REQUIRE(before.seconds() == doctest::Approx(5.0).epsilon(0.01));
    const size_t undoBefore = doc.undoHistory().size();

    ai::PlanContext pc;
    pc.sequenceDuration = before;
    auto plan = ai::planFromText("1秒から2秒を削除して、「テスト」という字幕を0.5秒に入れて。白黒にして、全体の音量を-3dBに。フェードイン、3秒にマーカー", pc);
    REQUIRE(ops(plan) == std::vector<std::string>{"delete_range", "add_subtitle", "apply_effect", "set_volume", "fade", "add_marker"});

    ai::PlanInputs in;
    ai::ApplyReport rep;
    REQUIRE(doc.edit("AI Edit", [&](ProjectEditor& pe) { return ai::applyPlan(pe, sid, plan, in, &rep); }));
    CHECK(doc.undoHistory().size() == undoBefore + 1);
    CHECK(rep.log.size() == plan.steps.size());
    const Sequence& s = *doc.project().active();
    CHECK(s.duration().seconds() == doctest::Approx(4.0).epsilon(0.01));
    CHECK(rep.durationAfter == s.duration());
    CHECK(s.masterVolumeDb == doctest::Approx(-3.0f));
    bool subtitle = false, effect = false, fadeIn = false;
    for (const auto& t : s.tracks)
        for (const auto& c : t->clips) {
            if (c->kind == ClipKind::Subtitle && c->text == "テスト") subtitle = true;
            if (t->kind == TrackKind::Video && c->kind == ClipKind::Media) {
                for (const auto& e : c->effects) effect |= e.effectId == "color.basic";
                if (c->start == Time{0}) fadeIn |= c->transitionIn.has_value();
            }
        }
    CHECK(subtitle);
    CHECK(effect);
    CHECK(fadeIn);
    REQUIRE(s.markers.size() == 1);
    CHECK(s.markers[0].time.seconds() == doctest::Approx(3.0).epsilon(0.01));

    // A failing step leaves the project untouched (no partial plan).
    const ProjectPtr snapshot = doc.current();
    auto bad = ai::planFromText("2秒から3秒を削除して、2倍速にして", pc);  // nothing selected -> speed fails
    REQUIRE(ops(bad) == std::vector<std::string>{"delete_range", "set_speed"});
    CHECK_FALSE(doc.edit("AI Edit", [&](ProjectEditor& pe) { return ai::applyPlan(pe, sid, bad, in, nullptr); }));
    CHECK(doc.current() == snapshot);

    // Undo restores the original timeline in one step.
    REQUIRE(doc.undo());
    CHECK(doc.project().active()->duration() == before);
}

TEST_CASE("plan: silence removal uses the analysis of the voice track") {
    const std::string f = test::testMedia("av_1080p30.mp4");
    if (f.empty()) return;
    auto media = createMediaItem(f);
    REQUIRE(media);
    Document doc(std::make_shared<Project>(makeProject("Plan", ProjectSettings{1280, 720, {30, 1}, 48000})));
    MediaId mid = kInvalidId;
    REQUIRE(doc.edit("import", [&](ProjectEditor& pe) {
        mid = media->id;
        pe.addMedia(*media);
        return Status::ok();
    }));
    const SequenceId sid = doc.project().activeSequence;
    REQUIRE(doc.editSequence("add", sid, [&](SequenceEditor& e) { return edit::addMediaClip(e, *media, S(0), 0, 3, edit::PlaceMode::Overwrite).status(); }));

    auto plan = ai::planFromText("無音を削除", {});
    auto needs = ai::analysisNeeded(plan, doc.project(), sid, {});
    REQUIRE(needs.silence.size() == 1);
    CHECK(*needs.silence.begin() == mid);

    ai::PlanInputs in;
    ai::SilenceResult sr;
    sr.silences = {TimeRange::fromStartEnd(S(1), S(1.5)), TimeRange::fromStartEnd(S(3), S(4))};
    in.silences[mid] = sr;
    ai::ApplyReport rep;
    REQUIRE(doc.edit("AI Edit", [&](ProjectEditor& pe) { return ai::applyPlan(pe, sid, plan, in, &rep); }));
    CHECK(doc.project().active()->duration().seconds() == doctest::Approx(3.5).epsilon(0.01));
    const Sequence& s = *doc.project().active();
    const Track& v = *s.tracks[0];
    const Track& a = *s.tracks[3];
    REQUIRE(v.clips.size() == a.clips.size());
    for (size_t i = 0; i < v.clips.size(); ++i) CHECK(v.clips[i]->sourceIn == a.clips[i]->sourceIn);
}

TEST_CASE("plan: timeline description never contains file paths") {
    const std::string f = test::testMedia("日本語クリップ.mp4");
    if (f.empty()) return;
    auto media = createMediaItem(f);
    REQUIRE(media);
    Document doc(std::make_shared<Project>(makeProject("Plan")));
    REQUIRE(doc.edit("import", [&](ProjectEditor& pe) {
        pe.addMedia(*media);
        return Status::ok();
    }));
    const SequenceId sid = doc.project().activeSequence;
    REQUIRE(doc.editSequence("add", sid, [&](SequenceEditor& e) { return edit::addMediaClip(e, *media, S(0), 0, 3, edit::PlaceMode::Overwrite).status(); }));
    const std::string j = ai::describeTimeline(doc.project(), sid, S(1), {}).dump();
    CHECK(j.find("日本語クリップ") != std::string::npos);
    CHECK(j.find(test::testMediaDir().string()) == std::string::npos);
    CHECK(j.find('/') == std::string::npos);
}

// ------------------------------------------------------------------ cloud assistant (mock transport, no network)

#include "ai/cloud.h"

TEST_CASE("cloud plan: request contents, privacy and response parsing") {
    ai::CloudOptions opt;
    opt.apiKey = "test-key";
    const ai::Json timeline = {{"duration_sec", 30.0}, {"tracks", ai::Json::array({{{"name", "V1"}, {"clips", ai::Json::array({{{"name", "clip.mp4"}}})}}})}};

    std::string sentBody;
    std::vector<std::pair<std::string, std::string>> sentHeaders;
    std::string sentUrl;
    auto transport = [&](const HttpRequest& r, const CancelToken&) {
        sentBody = r.body;
        sentHeaders = r.headers;
        sentUrl = r.url;
        // Structured output: the plan arrives as JSON text in a text block.
        const ai::Json plan = {{"steps", ai::Json::array({{{"op", "delete_range"}, {"start", 1.0}, {"end", 2.5}},
                                                          {{"op", "apply_effect"}, {"effect", "color.basic"}, {"target", "all_video"},
                                                           {"params", ai::Json::array({{{"name", "saturation"}, {"value", 0.0}}})}}})},
                               {"notes", ai::Json::array({"字幕の色は変更できません"})}};
        const ai::Json resp = {{"model", "claude-opus-5-5"},
                               {"stop_reason", "end_turn"},
                               {"content", ai::Json::array({{{"type", "thinking"}, {"thinking", ""}}, {{"type", "text"}, {"text", plan.dump()}}})}};
        return HttpResponse{200, resp.dump(), {}};
    };
    auto p = ai::requestCloudPlan("1秒から2.5秒を削除して白黒に", timeline, opt, transport);
    REQUIRE(p);
    REQUIRE(p->steps.size() == 2);
    CHECK(p->source == "claude-opus-5-5");
    CHECK(p->steps[0].op == "delete_range");
    CHECK(p->steps[1].args["params"]["saturation"].get<double>() == doctest::Approx(0.0));
    REQUIRE(p->notes.size() == 1);

    // The request: model, structured output schema, fallbacks, headers, endpoint.
    CHECK(sentUrl == "https://api.anthropic.com/v1/messages");
    const ai::Json body = ai::Json::parse(sentBody);
    CHECK(body["model"] == "claude-opus-5-5");
    CHECK(body["output_config"]["format"]["type"] == "json_schema");
    CHECK(body["output_config"]["format"]["schema"]["additionalProperties"] == false);
    CHECK(body["fallbacks"] == "default");
    CHECK_FALSE(body.contains("thinking"));
    bool key = false, version = false, beta = false;
    for (const auto& [k, v] : sentHeaders) {
        key |= k == "x-api-key" && v == "test-key";
        version |= k == "anthropic-version" && v == "2023-06-01";
        beta |= k == "anthropic-beta" && v == "server-side-fallback-2026-07-01";
    }
    CHECK(key);
    CHECK(version);
    CHECK(beta);
    // Only the instruction and the timeline description are sent.
    const std::string userText = body["messages"][0]["content"][0]["text"];
    CHECK(userText.find("1秒から2.5秒を削除して白黒に") != std::string::npos);
    CHECK(userText.find("clip.mp4") != std::string::npos);
    // The system prompt is stable (cacheable) and lists the effects.
    CHECK(ai::cloudSystemPrompt() == ai::cloudSystemPrompt());
    CHECK(ai::cloudSystemPrompt().find("color.basic") != std::string::npos);
}

TEST_CASE("cloud plan: errors, refusals and invalid plans are reported, never applied") {
    using ai::Json;
    CHECK_FALSE(ai::parseCloudResponse(401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})"));
    auto e = ai::parseCloudResponse(401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    CHECK(e.errorMessage().find("invalid x-api-key") != std::string::npos);
    CHECK_FALSE(ai::parseCloudResponse(200, R"({"stop_reason":"refusal","content":[]})"));
    CHECK_FALSE(ai::parseCloudResponse(200, R"({"stop_reason":"max_tokens","content":[{"type":"text","text":"{\"steps\":["}]})"));
    CHECK_FALSE(ai::parseCloudResponse(200, "not json"));
    // Unknown operations or effects are rejected by the same validation as local plans.
    const Json bad = {{"stop_reason", "end_turn"},
                      {"content", Json::array({{{"type", "text"}, {"text", R"({"steps":[{"op":"apply_effect","effect":"no.such"}],"notes":[]})"}}})}};
    CHECK_FALSE(ai::parseCloudResponse(200, bad.dump()));
    // Transport failures and missing keys.
    ai::CloudOptions opt;
    CHECK_FALSE(ai::requestCloudPlan("x", Json::object(), opt, [](const HttpRequest&, const CancelToken&) { return HttpResponse{}; }));
    opt.apiKey = "k";
    auto t = ai::requestCloudPlan("x", Json::object(), opt, [](const HttpRequest&, const CancelToken&) {
        HttpResponse r;
        r.error = "timeout";
        return r;
    });
    REQUIRE_FALSE(t);
    CHECK(t.errorMessage() == "timeout");
}
