#include <doctest.h>

#include <chrono>
#include <fstream>

#include "cache/cache.h"
#include "core/strings.h"
#include "media/probe.h"
#include "tests/test_support.h"

using namespace avc;
namespace fs = std::filesystem;

TEST_CASE("cache store: read/write, collisions, usage, cleanup") {
    auto dir = test::makeTempDir("cachestore");
    cache::CacheStore store(dir);
    REQUIRE(store.write("thumbnails", "a|1", "img", "hello"));
    REQUIRE(store.write("waveforms", "b|2", "peaks", std::string(1000, 'x')));
    CHECK(store.read("thumbnails", "a|1", "img") == std::optional<std::string>("hello"));
    CHECK_FALSE(store.read("thumbnails", "missing", "img"));
    // An entry is only returned for its exact key, even if the file name matched.
    fs::copy_file(store.pathFor("thumbnails", "a|1", "img"), store.pathFor("thumbnails", "other", "img"));
    CHECK_FALSE(store.read("thumbnails", "other", "img"));
    CHECK_FALSE(store.write("x", "bad\nkey", "img", "v"));

    auto usage = store.usage();
    REQUIRE(usage.size() == 2);
    CHECK(usage[0].category == "waveforms");
    CHECK(store.totalBytes() > 1000);

    // Age-based cleanup removes only old files.
    const fs::path old = store.pathFor("waveforms", "b|2", "peaks");
    fs::last_write_time(old, fs::file_time_type::clock::now() - std::chrono::hours(24 * 40));
    CHECK(store.cleanup(UINT64_MAX, 30) > 1000);
    CHECK_FALSE(fs::exists(old));
    CHECK(store.read("thumbnails", "a|1", "img"));

    // Size-based cleanup removes least recently used first.
    for (int i = 0; i < 10; ++i) REQUIRE(store.write("t", "k" + std::to_string(i), "bin", std::string(100, 'z')));
    for (int i = 0; i < 10; ++i)
        fs::last_write_time(store.pathFor("t", "k" + std::to_string(i), "bin"),
                            fs::file_time_type::clock::now() - std::chrono::minutes(100 - i));
    store.cleanup(600, 0);
    CHECK(store.totalBytes() <= 600);
    CHECK_FALSE(store.read("t", "k0", "bin"));
    CHECK(store.read("t", "k9", "bin"));

    CHECK(store.clearCategory("t") > 0);
    CHECK_FALSE(fs::exists(dir / "t"));
    CHECK(store.clearCategory("..") == 0);  // never escapes the cache root
}

TEST_CASE("thumbnail cache: decode, memory hit, disk hit, invalidation") {
    const std::string src = test::testMedia("scenes.mp4");
    if (src.empty()) return;
    auto dir = test::makeTempDir("thumbs");
    auto media = createMediaItem(src);
    REQUIRE(media);
    cache::CacheStore store(dir / "cache");
    {
        cache::ThumbnailCache thumbs(store);
        CHECK_FALSE(thumbs.peek(*media, Time::fromSeconds(1.0)));
        auto t = thumbs.get(*media, Time::fromSeconds(1.0), 160);
        REQUIRE_MESSAGE(t, t.errorMessage());
        CHECK((*t)->width == 160);
        CHECK((*t)->height == 90);
        // scenes.mp4 is red for 0-2 s, green 2-4 s, blue 4-6 s.
        CHECK((*t)->rgba[(45 * 160 + 80) * 4 + 0] > 200);
        CHECK(thumbs.peek(*media, Time::fromSeconds(1.1)) == *t);  // same quantized step
        auto g = thumbs.get(*media, Time::fromSeconds(3.0), 160);
        REQUIRE(g);
        CHECK((*g)->rgba[(45 * 160 + 80) * 4 + 1] > 100);  // lavfi "green" is #008000
        CHECK((*g)->rgba[(45 * 160 + 80) * 4 + 0] < 60);
        // Past the end clamps to the last frame instead of failing.
        auto end = thumbs.get(*media, Time::fromSeconds(100), 160);
        REQUIRE(end);
        CHECK((*end)->rgba[(45 * 160 + 80) * 4 + 2] > 200);
        thumbs.trimMemory(0);
        CHECK_FALSE(thumbs.peek(*media, Time::fromSeconds(1.0)));
    }
    CHECK(store.usage().at(0).files == 3);
    // A new cache instance reads from disk (JPEG round trip keeps the colour).
    cache::ThumbnailCache again(store);
    auto t = again.get(*media, Time::fromSeconds(1.0), 160);
    REQUIRE(t);
    CHECK((*t)->rgba[(45 * 160 + 80) * 4 + 0] > 200);
    // A changed source identity gives a different key.
    MediaItem changed = *media;
    changed.path = src + ".missing";
    CHECK(cache::mediaKey(changed) != cache::mediaKey(*media));
    CHECK_FALSE(again.get(changed, Time::fromSeconds(1.0), 160));
}

TEST_CASE("waveform cache persists peaks") {
    const std::string src = test::testMedia("speech_silence.wav");
    if (src.empty()) return;
    auto dir = test::makeTempDir("wavecache");
    auto media = createMediaItem(src);
    REQUIRE(media);
    cache::CacheStore store(dir);
    cache::WaveformCache wc(store);
    CHECK_FALSE(wc.peek(*media));
    auto w = wc.get(*media);
    REQUIRE_MESSAGE(w, w.errorMessage());
    CHECK((*w)->durationSeconds() == doctest::Approx(9.0).epsilon(0.01));
    CHECK(wc.peek(*media) == *w);
    cache::WaveformCache fresh(store);
    auto w2 = fresh.get(*media);
    REQUIRE(w2);
    CHECK((*w2)->frames == (*w)->frames);
    const auto cols = (*w2)->query(0, 9, 18);  // 0.5 s columns
    REQUIRE(cols.size() == 18);
    CHECK(cols[0].second > 0.4f);               // tone (0.5 amplitude) at 0-1 s
    CHECK(std::abs(cols[3].second) < 0.01f);    // silence at 1.5-2 s
    CHECK(cols[6].second > 0.4f);               // tone again at 3-3.5 s
}
