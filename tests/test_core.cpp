#include <doctest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

#include "core/file_io.h"
#include "core/ids.h"
#include "core/jobs.h"
#include "core/settings.h"
#include "core/strings.h"
#include "core/time.h"
#include "tests/test_support.h"

using namespace avc;

TEST_CASE("mulDiv rounding and overflow safety") {
    CHECK(mulDiv(10, 3, 4, Rounding::Down) == 7);
    CHECK(mulDiv(10, 3, 4, Rounding::Up) == 8);
    CHECK(mulDiv(10, 3, 4, Rounding::Nearest) == 8);  // 7.5 -> 8
    CHECK(mulDiv(-10, 3, 4, Rounding::Down) == -8);
    CHECK(mulDiv(-10, 3, 4, Rounding::Up) == -7);
    CHECK(mulDiv(-10, 3, 4, Rounding::Nearest) == -8);
    // Large intermediate: 3 hours in ticks * 60000
    const int64_t big = kTicksPerSecond * 3600 * 3;
    CHECK(mulDiv(big, 60000, 1001 * kTicksPerSecond / kTicksPerSecond, Rounding::Down) ==
          static_cast<int64_t>((static_cast<long double>(big) * 60000) / 1001));
}

TEST_CASE("tick base is exact for common frame and sample rates") {
    const Rational rates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48, 1},
                              {50, 1},       {60000, 1001}, {60, 1}, {120, 1}};
    for (auto r : rates) {
        const Time one = Time::frameDuration(r);
        // frame duration * frames per 1001 seconds must be exact
        CHECK((kTicksPerSecond * r.den) % r.num == 0);
        CHECK(Time::fromFrames(1000000, r).ticks == one.ticks * 1000000);
    }
    for (int sr : {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000}) {
        CHECK(kTicksPerSecond % sr == 0);
    }
}

TEST_CASE("no drift over long timelines at 29.97") {
    const Rational ntsc{30000, 1001};
    // 3 hours of frames
    const int64_t frames = 3 * 3600 * 30;
    Time accumulated{0};
    const Time frame = Time::frameDuration(ntsc);
    for (int64_t i = 0; i < frames; ++i) accumulated += frame;
    CHECK(accumulated == Time::fromFrames(frames, ntsc));
    CHECK(accumulated.toFrames(ntsc) == frames);
    // audio samples at the same instant are exact
    const int64_t samples = accumulated.toSamples(48000, Rounding::Nearest);
    CHECK(Time::fromSamples(samples, 48000) == accumulated);
}

TEST_CASE("frame conversion roundtrip") {
    for (Rational r : {Rational{24000, 1001}, Rational{25, 1}, Rational{60000, 1001}}) {
        for (int64_t f : {0LL, 1LL, 2LL, 1000LL, 107892LL, 999999LL}) {
            CHECK(Time::fromFrames(f, r).toFrames(r) == f);
        }
    }
}

TEST_CASE("timebase conversion") {
    CHECK(Time::fromTimebase(90000, {1, 90000}) == Time{kTicksPerSecond});
    CHECK(Time::fromTimebase(1001, {1, 30000}).toFrames({30000, 1001}) == 1);
    CHECK(Time{kTicksPerSecond}.toTimebase({1, 1000}) == 1000);
    CHECK(Time::fromSeconds(1.5).toTimebase({1, 90000}) == 135000);
}

TEST_CASE("Rational from frame rate snaps NTSC") {
    CHECK(Rational::fromFrameRate(29.97) == Rational{30000, 1001});
    CHECK(Rational::fromFrameRate(23.976) == Rational{24000, 1001});
    CHECK(Rational::fromFrameRate(59.94) == Rational{60000, 1001});
    CHECK(Rational::fromFrameRate(25.0) == Rational{25, 1});
    CHECK(Rational::parse("30000/1001").value() == Rational{30000, 1001});
    CHECK(Rational{60, 2} == Rational{30, 1});
}

TEST_CASE("drop-frame timecode") {
    const Rational ntsc{30000, 1001};
    CHECK(formatTimecode(Time::fromFrames(0, ntsc), ntsc) == "00:00:00;00");
    CHECK(formatTimecode(Time::fromFrames(1799, ntsc), ntsc) == "00:00:59;29");
    CHECK(formatTimecode(Time::fromFrames(1800, ntsc), ntsc) == "00:01:00;02");
    CHECK(formatTimecode(Time::fromFrames(17982, ntsc), ntsc) == "00:10:00;00");
    CHECK(formatTimecode(Time::fromFrames(107892, ntsc), ntsc) == "01:00:00;00");
    CHECK(parseTimecode("00:01:00;02", ntsc)->toFrames(ntsc) == 1800);
    CHECK(parseTimecode("01:00:00;00", ntsc)->toFrames(ntsc) == 107892);
    const Rational pal{25, 1};
    CHECK(formatTimecode(Time::fromFrames(25 * 61 + 3, pal), pal) == "00:01:01:03");
    CHECK(parseTimecode("00:01:01:03", pal)->toFrames(pal) == 25 * 61 + 3);
    CHECK(parseTimecode("48", pal)->toFrames(pal) == 48);
    CHECK_FALSE(parseTimecode("ab:cd", pal).has_value());
}

TEST_CASE("TimeRange") {
    TimeRange a{Time{10}, Time{10}}, b{Time{15}, Time{10}}, c{Time{20}, Time{5}};
    CHECK(a.overlaps(b));
    CHECK_FALSE(a.overlaps(c));
    CHECK(a.intersection(b) == TimeRange{Time{15}, Time{5}});
    CHECK(a.contains(Time{19}));
    CHECK_FALSE(a.contains(Time{20}));
}

TEST_CASE("UTF-8 helpers and paths") {
    const std::string jp = "動画_テスト.mp4";
    auto p = pathFromUtf8(jp);
    CHECK(pathToUtf8(p) == jp);
    CHECK(utf8Length(jp) == 10);
    CHECK(utf32ToUtf8(utf8ToUtf32(jp)) == jp);
    CHECK(sanitizeFileName("a:b*c?") == "a_b_c_");
    CHECK(icontains("Hello World", "wORLD"));
    uint64_t v = 0;
    CHECK(parseHex64(hex64(0xdeadbeefcafe1234ull), v));
    CHECK(v == 0xdeadbeefcafe1234ull);
}

TEST_CASE("atomic file write replaces content and handles unicode names") {
    auto dir = test::makeTempDir("atomic");
    auto file = dir / pathFromUtf8("プロジェクト.avicap");
    REQUIRE(writeFileAtomic(file, "first"));
    CHECK(*readFileBytes(file) == "first");
    AtomicWriteOptions opt;
    opt.keepBackup = true;
    REQUIRE(writeFileAtomic(file, "second", opt));
    CHECK(*readFileBytes(file) == "second");
    auto bak = file;
    bak += ".bak";
    CHECK(*readFileBytes(bak) == "first");
    // No temp files left behind.
    int count = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) {
        (void)e;
        ++count;
    }
    CHECK(count == 2);
    auto id1 = fileIdentity(file);
    CHECK(id1.exists);
    CHECK(id1.size == 6);
}

TEST_CASE("ids are unique and roundtrip") {
    std::set<Id> ids;
    for (int i = 0; i < 10000; ++i) ids.insert(newId());
    CHECK(ids.size() == 10000);
    const Id id = newId();
    CHECK(idFromString(idToString(id)) == id);
}

TEST_CASE("thread pool runs by priority and supports cancellation") {
    ThreadPool pool("test", 1);
    std::vector<int> order;
    std::mutex m;
    std::atomic<bool> release{false};
    // Block the single worker so subsequent jobs queue up.
    auto blocker = pool.submit("block", JobPriority::Highest, [&](JobContext&) {
        while (!release) std::this_thread::yield();
    });
    auto low = pool.submit("low", JobPriority::Low, [&](JobContext&) {
        std::lock_guard l(m);
        order.push_back(3);
    });
    auto high = pool.submit("high", JobPriority::High, [&](JobContext&) {
        std::lock_guard l(m);
        order.push_back(1);
    });
    auto medium = pool.submit("medium", JobPriority::Medium, [&](JobContext&) {
        std::lock_guard l(m);
        order.push_back(2);
    });
    auto cancelled = pool.submit("cancelled", JobPriority::High, [&](JobContext&) {
        std::lock_guard l(m);
        order.push_back(99);
    });
    cancelled.cancel();
    release = true;
    pool.waitIdle();
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK(cancelled.status() == JobStatus::Cancelled);
    CHECK(high.status() == JobStatus::Succeeded);
}

TEST_CASE("group cancellation reaches running jobs") {
    ThreadPool pool("test2", 2);
    CancelToken group = CancelToken::create();
    std::atomic<int> observed{0};
    std::vector<JobHandle> handles;
    for (int i = 0; i < 4; ++i) {
        handles.push_back(pool.submit("loop", JobPriority::Medium, [&](JobContext& ctx) {
            while (!ctx.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++observed;
        }, group));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    group.cancel();
    for (auto& h : handles) CHECK(h.waitFor(std::chrono::seconds(5)));
    for (auto& h : handles) CHECK(h.status() == JobStatus::Cancelled);
}

TEST_CASE("failed job reports error") {
    ThreadPool pool("test3", 1);
    auto h = pool.submit("throws", JobPriority::Medium, [](JobContext&) { throw std::runtime_error("boom"); });
    h.wait();
    CHECK(h.status() == JobStatus::Failed);
    CHECK(h.state()->error() == "boom");
}

TEST_CASE("settings roundtrip with defaults for unknown fields") {
    AppSettings s;
    s.general.language = "ja";
    s.cache.maxGB = 50;
    s.keyboard.overrides["timeline.split"] = "Ctrl+K";
    s.addRecentProject("C:/a.avicap");
    s.addRecentProject("C:/b.avicap");
    s.addRecentProject("C:/a.avicap");
    AppSettings r = AppSettings::fromJson(s.toJson());
    CHECK(r.general.language == "ja");
    CHECK(r.cache.maxGB == doctest::Approx(50));
    CHECK(r.keyboard.overrides["timeline.split"] == "Ctrl+K");
    CHECK(r.general.recentProjects == std::vector<std::string>{"C:/a.avicap", "C:/b.avicap"});
    AppSettings broken = AppSettings::fromJson("{\"general\": {\"autosaveIntervalSec\": \"oops\"}, \"cache\": 5}");
    CHECK(broken.general.autosaveIntervalSec == 60);
    AppSettings garbage = AppSettings::fromJson("not json");
    CHECK(garbage.general.language == "auto");
}
