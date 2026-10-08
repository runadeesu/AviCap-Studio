#pragma once
// Subtitle files: SubRip (.srt) and WebVTT (.vtt) import/export.
//
// Import accepts what real-world files contain: BOMs, CRLF, UTF-16 and
// Shift_JIS text (decoded by the caller with decodeTextFile), cue numbers or
// identifiers, "MM:SS.mmm" short timestamps, cue settings, formatting tags
// and HTML entities. Overlapping cues are kept on extra subtitle tracks.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "timeline/editor.h"

namespace avc::subs {

struct Cue {
    Time start;
    Time end;
    std::string text;  // lines separated by '\n', tags removed
};

enum class Format { Srt, Vtt };

std::optional<Format> formatFromExtension(std::string_view pathOrExt);
// Parses UTF-8 text (format detected from a "WEBVTT" header).
Result<std::vector<Cue>> parse(std::string_view utf8);
std::string toSrt(const std::vector<Cue>& cues);
std::string toVtt(const std::vector<Cue>& cues);
std::string serialize(const std::vector<Cue>& cues, Format f);

// Subtitle clips of every enabled subtitle track, in time order. With a
// range, cues are clipped to it and shifted so the range starts at 0.
std::vector<Cue> cuesFromSequence(const Sequence& seq, std::optional<TimeRange> range = std::nullopt);

struct ImportStyle {
    float fontSize = 0;  // 0 = 5% of the sequence height (min 36)
    std::string trackName = "Subtitles";
};
// Adds the cues (shifted by `offset`) as subtitle clips on new subtitle
// track(s). Returns the number of clips added.
Result<int> addCues(SequenceEditor& e, const std::vector<Cue>& cues, Time offset, const ImportStyle& style);

}  // namespace avc::subs
