#include <gtest/gtest.h>
#include "source/MediaProbe.h"
#include "detect/ChapterDetector.h"
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// Regression coverage for the "Kairos stuck on 2GB RAM after a full sync"
// investigation: probe/chapter/detection passes all shape their work as a
// short-lived std::thread pool spun up over a batch of files and joined
// before the next pass starts (see SyncManager::syncMediaProbeFromFiles and
// ChapterDetectionManager::runShowDetect). These tests exercise that exact
// pool-per-pass shape against a real (committed) fixture file so that
// use-after-free / double-free / leaked-allocation bugs in MediaProbe.cpp
// and ChapterDetector.cpp surface here instead of only in production.
//
// A normal build does not catch leaks by itself — these tests only *fail*
// on a real leak when the whole build is instrumented with
// AddressSanitizer+LeakSanitizer:
//
//   cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPANTHEON_SANITIZE=ON
//   cmake --build build-asan --target momus_kairos -j
//   ctest --test-dir build-asan -R MediaProbeResourceRelease --output-on-failure
//
// Without -DPANTHEON_SANITIZE=ON these still run and validate correctness
// (probed values match the fixture), just without the leak check.

namespace
{
	std::string fixturePath()
	{
		return std::string(MOMUS_KAIROS_FIXTURES_DIR) + "/sample_chapters.mp4";
	}

	bool haveBinary(const std::string& name)
	{
		return std::system(("command -v " + name + " > /dev/null 2>&1").c_str()) == 0;
	}

	// Runs `iterations` calls to `fn` spread across a small worker pool, joined
	// before returning — the same shape as SyncManager's and
	// ChapterDetectionManager's per-pass thread pools.
	template <typename Fn>
	void runWorkerPool(int worker_count, int iterations, Fn&& fn)
	{
		std::atomic<int> next{0};
		std::vector<std::thread> workers;
		workers.reserve(static_cast<size_t>(worker_count));
		for (int w = 0; w < worker_count; ++w)
		{
			workers.emplace_back([&]()
			{
				for (int i = next.fetch_add(1); i < iterations; i = next.fetch_add(1)) fn(i);
			});
		}
		for (auto& t : workers) t.join();
	}
} // namespace

TEST(MediaProbeResourceRelease, FixtureProbesCorrectly)
{
	// Sanity check that the fixture and native libavformat path both work
	// before trusting the multi-pass tests below.
	FileProbeInfo info = probeFileInfo(fixturePath());
	EXPECT_GE(info.duration_ms, 5500);
	EXPECT_LE(info.duration_ms, 6500);
	EXPECT_EQ(info.video.width, 160);
	EXPECT_EQ(info.video.height, 120);

	auto chapters = probeChapters(fixturePath());
	ASSERT_EQ(chapters.size(), 2u);
	EXPECT_EQ(chapters[0].title, "Intro");
	EXPECT_EQ(chapters[1].title, "Main");

	VideoInfo vinfo = probeVideoInfo(fixturePath());
	EXPECT_EQ(vinfo.width, 160);
	EXPECT_EQ(vinfo.height, 120);
}

// Mirrors SyncManager's probe-pass worker pool: N threads pulling from a
// shared index, each opening+closing the same file repeatedly via
// avformat_open_input/avformat_close_input. If openFormat's FmtCtxPtr ever
// failed to close on an error path (or a fallback path leaked its FILE*
// pipe), running this under LSan would catch it.
TEST(MediaProbeResourceRelease, ProbePassWorkerPoolAcrossMultiplePasses)
{
	const std::string path = fixturePath();
	for (int pass = 0; pass < 3; ++pass)
	{
		runWorkerPool(4, 40, [&](int)
		{
			FileProbeInfo info = probeFileInfo(path);
			auto chapters      = probeChapters(path);
			VideoInfo vinfo    = probeVideoInfo(path);
			EXPECT_GT(info.duration_ms, 0);
			EXPECT_EQ(chapters.size(), 2u);
			EXPECT_EQ(vinfo.width, 160);
		});
	}
}

// Mirrors ChapterDetectionManager::runShowDetect's worker pool: scene-cut
// timeline + audio fingerprint per "episode" (all pointed at the same
// fixture — content doesn't matter for resource-release checking), followed
// by the sequential detectRecurringSegments() cross-episode pass. Requires
// the real ffmpeg/fpcalc binaries (shelled out via popen, same as
// production); skips rather than failing when they're not installed, since
// momus's CI runner currently only installs libavformat-dev, not the ffmpeg
// CLI package.
TEST(MediaProbeResourceRelease, DetectionPassWorkerPool)
{
	if (!haveBinary("ffmpeg") || !haveBinary("fpcalc"))
	{
		GTEST_SKIP() << "ffmpeg/fpcalc CLI not installed — skipping detection-pass leak check";
	}
	const std::string path      = fixturePath();
	constexpr int kEpisodeCount = 6;

	std::vector<EpisodeFingerprint> fps(kEpisodeCount);
	runWorkerPool(3, kEpisodeCount, [&](int i)
	{
		auto cuts      = sceneChangeTimeline(path);
		auto ad_breaks = detectAdBreaks(path, 6000, "episode");
		auto fp        = computeAudioFingerprint(path);
		EXPECT_FALSE(fp.empty()) << "fpcalc produced no fingerprint items for the fixture";
		fps[static_cast<size_t>(i)] = {std::to_string(i), 6000, std::move(fp)};
		(void)cuts;
		(void)ad_breaks;
	});

	// Sequential cross-episode pass, same as production — every "episode" is
	// the identical fixture file, so this should corroborate trivially.
	auto recurring = detectRecurringSegments(fps);
	(void)recurring;
}