#include <doctest.h>

#include "core/strings.h"
#include "timeline/document.h"
#include "timeline/subtitles.h"

using namespace avc;

namespace {
Time S(double s) { return Time::fromSeconds(s); }
}  // namespace

TEST_CASE("subtitles: SRT with BOM, CRLF, numbers, tags and entities") {
    const std::string srt =
        "\xEF\xBB\xBF"
        "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>こんにちは</i>\r\n世界 &amp; 字幕\r\n\r\n"
        "2\r\n00:00:03,250 --> 00:00:04,000 X1:10 X2:20\r\n{\\an8}<font color=\"#ff0000\">上に表示</font>\r\n\r\n"
        "3\r\nbroken timing line\r\nignored\r\n\r\n"
        "4\r\n00:01:00,000 --> 01:00:00,001\r\nlong\r\n";
    auto r = subs::parse(srt);
    REQUIRE(r);
    REQUIRE(r->size() == 3);
    CHECK((*r)[0].start == Time::fromMilliseconds(1000));
    CHECK((*r)[0].end == Time::fromMilliseconds(2500));
    CHECK((*r)[0].text == "こんにちは\n世界 & 字幕");
    CHECK((*r)[1].text == "上に表示");
    CHECK((*r)[2].end == Time::fromMilliseconds(3600001));
}

TEST_CASE("subtitles: WebVTT with header, notes, styles, ids and short timestamps") {
    const std::string vtt =
        "WEBVTT - title\n\nNOTE this is a comment\nwith two lines\n\nSTYLE\n::cue { color: yellow }\n\n"
        "intro\n00:01.500 --> 00:03.000 align:start position:10%\n<v Narrator>Hello <c.yellow>there</c></v>\n\n"
        "00:00:05.000 --> 00:00:06.000\nA &lt;tag&gt; text\n";
    auto r = subs::parse(vtt);
    REQUIRE(r);
    REQUIRE(r->size() == 2);
    CHECK((*r)[0].start == Time::fromMilliseconds(1500));
    CHECK((*r)[0].text == "Hello there");
    CHECK((*r)[1].text == "A <tag> text");
    CHECK_FALSE(subs::parse("just text\nno cues"));
}

TEST_CASE("subtitles: Shift_JIS and UTF-16 files decode to UTF-8") {
    // "1\n00:00:01,000 --> 00:00:02,000\nこんにちは\n" in CP932.
    const std::string sjis = std::string("1\n00:00:01,000 --> 00:00:02,000\n") + "\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD" + "\n";
    CHECK_FALSE(isValidUtf8(sjis));
    auto text = decodeTextFile(sjis);
    REQUIRE(text);
    auto r = subs::parse(*text);
    REQUIRE(r);
    CHECK((*r)[0].text == "こんにちは");

    std::string utf16 = "\xFF\xFE";
    for (char32_t c : std::u32string_view(U"1\n00:00:01,000 --> 00:00:02,000\n字幕\n")) {
        utf16 += static_cast<char>(c & 0xFF);
        utf16 += static_cast<char>((c >> 8) & 0xFF);
    }
    auto t2 = decodeTextFile(utf16);
    REQUIRE(t2);
    auto r2 = subs::parse(*t2);
    REQUIRE(r2);
    CHECK((*r2)[0].text == "字幕");
    CHECK(isValidUtf8("日本語 ok"));
    CHECK_FALSE(isValidUtf8("\xC0\xAF"));  // overlong
}

TEST_CASE("subtitles: SRT and VTT round trip") {
    std::vector<subs::Cue> cues = {{Time::fromMilliseconds(0), Time::fromMilliseconds(1234), "一行目\n二行目"},
                                   {Time::fromMilliseconds(3723004), Time::fromMilliseconds(3725000), "a --> b & <c>"}};
    const std::string srt = subs::toSrt(cues);
    CHECK(srt.find("01:02:03,004 --> 01:02:05,000") != std::string::npos);
    auto back = subs::parse(srt);
    REQUIRE(back);
    REQUIRE(back->size() == 2);
    CHECK((*back)[0].text == cues[0].text);
    CHECK((*back)[1].end == cues[1].end);
    const std::string vtt = subs::toVtt(cues);
    CHECK(startsWith(vtt, "WEBVTT"));
    auto back2 = subs::parse(vtt);
    REQUIRE(back2);
    CHECK((*back2)[1].text == "a --> b & <c>");
    CHECK(subs::formatFromExtension("C:/x/movie.SRT") == subs::Format::Srt);
    CHECK(subs::formatFromExtension("a.vtt") == subs::Format::Vtt);
    CHECK_FALSE(subs::formatFromExtension("a.txt"));
}

TEST_CASE("subtitles: import onto the timeline and export a range") {
    Document doc(std::make_shared<Project>(makeProject("Subs", ProjectSettings{1920, 1080, {30, 1}, 48000})));
    const SequenceId sid = doc.project().activeSequence;
    std::vector<subs::Cue> cues = {{S(1), S(3), "A"}, {S(2), S(4), "B (overlaps A)"}, {S(5), S(6), "C"}};
    int added = 0;
    REQUIRE(doc.editSequence("Import Subtitles", sid, [&](SequenceEditor& e) {
        auto r = subs::addCues(e, cues, S(10), {});
        if (!r) return r.status();
        added = *r;
        return Status::ok();
    }));
    CHECK(added == 3);
    const Sequence& s = *doc.project().active();
    int subTracks = 0;
    for (const auto& t : s.tracks)
        if (t->kind == TrackKind::Subtitle) {
            ++subTracks;
            for (size_t i = 1; i < t->clips.size(); ++i) CHECK(t->clips[i - 1]->end() <= t->clips[i]->start);
        }
    CHECK(subTracks == 2);  // the overlapping cue went to a second track
    auto out = subs::cuesFromSequence(s);
    REQUIRE(out.size() == 3);
    CHECK(out[0].start == S(11));
    CHECK(out[1].text == "B (overlaps A)");
    // Export of a range: clipped and shifted to start at 0.
    auto part = subs::cuesFromSequence(s, TimeRange::fromStartEnd(S(12), S(15.5)));
    REQUIRE(part.size() == 3);
    CHECK(part[0].start == S(0));
    CHECK(part[0].end == S(1));
    CHECK(part[2].start == S(3));
    CHECK(part[2].end == S(3.5));
    CHECK(doc.undoHistory().size() == 1);
}
