#include <gtest/gtest.h>
#include "log/LogBuffer.h"
#include "log/RuntimeFlags.h"
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>

namespace
{
	// RAII: g_verbose_transcode_logs is process-global — restore it after each
	// test that touches it so later tests aren't affected by test order.
	struct VerboseFlagGuard
	{
		bool prev = g_verbose_transcode_logs.load();
		~VerboseFlagGuard() { g_verbose_transcode_logs.store(prev); }
	};

	std::vector<std::string> readLines(const std::string& path)
	{
		std::ifstream f(path);
		std::vector<std::string> out;
		std::string line;
		while (std::getline(f, line)) out.push_back(line);
		return out;
	}

	// LogBuffer::push() now prepends a wall-clock timestamp (see its own
	// comment) to every stored/written line, so exact-match assertions against
	// pushed content need to check the suffix rather than the whole line.
	::testing::AssertionResult EndsWithLine(const std::string& actual, const std::string& expectedSuffix)
	{
		if (actual.size() >= expectedSuffix.size() &&
			actual.compare(actual.size() - expectedSuffix.size(), expectedSuffix.size(), expectedSuffix) == 0)
			return ::testing::AssertionSuccess();
		return ::testing::AssertionFailure() << "\"" << actual << "\" does not end with \"" << expectedSuffix << "\"";
	}
}

TEST(LogBufferTest, PushAndRecent)
{
	LogBuffer buf;
	buf.push("line 1");
	buf.push("line 2");
	buf.push("line 3");

	auto [lines, seq] = buf.recent(2);
	ASSERT_EQ(lines.size(), 2);
	EXPECT_TRUE(EndsWithLine(lines[0], "line 2"));
	EXPECT_TRUE(EndsWithLine(lines[1], "line 3"));
	EXPECT_EQ(seq, 3);
}

TEST(LogBufferTest, WaitAfter)
{
	LogBuffer buf;
	buf.push("line 1");

	// Start a thread to push a line after a short delay
	std::thread t([&buf]()
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		buf.push("line 2");
	});

	auto [lines, seq] = buf.waitAfter(1, std::chrono::milliseconds(500));
	ASSERT_EQ(lines.size(), 1);
	EXPECT_TRUE(EndsWithLine(lines[0], "line 2"));
	EXPECT_EQ(seq, 2);

	t.join();
}

TEST(LogBufferTest, WaitAfterTimeout)
{
	LogBuffer buf;
	buf.push("line 1");

	auto [lines, seq] = buf.waitAfter(1, std::chrono::milliseconds(50));
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(seq, 1);
}

TEST(LogBufferTest, Overflow)
{
	LogBuffer buf;
	// kMax is 2000
	for (int i = 0; i < 2100; ++i)
	{
		buf.push("line " + std::to_string(i));
	}

	auto [lines, seq] = buf.recent(5000);
	EXPECT_EQ(lines.size(), 2000);
	EXPECT_TRUE(EndsWithLine(lines.front(), "line 100"));
	EXPECT_TRUE(EndsWithLine(lines.back(), "line 2099"));
	EXPECT_EQ(seq, 2100); // seq_ starts at 1, so 2100 pushes end at seq 2100
}

// Regression: Hephaestus's LogBuffer used to have no file-writing at all,
// unlike Kairos's — so nothing it printed was ever durably captured, only
// held in the in-memory ring buffer. These mirror Kairos's equivalent
// FilterExpr-adjacent LogBuffer coverage.

TEST(LogBufferTest, SetFileWritesEveryLineRegardlessOfCategoryFilter)
{
	VerboseFlagGuard guard;
	g_verbose_transcode_logs.store(false);

	auto path = (std::filesystem::temp_directory_path() / "heph_logbuf_all.log").string();
	std::filesystem::remove(path);

	LogBuffer buf;
	buf.setFile(path);
	buf.setFilter(hephaestusLogFilter);
	buf.push("[hephaestus] startup");
	buf.push("[ffmpeg] spawning: ...");     // gated category, flag off
	buf.push("[sessions] channel started"); // gated category, flag off

	auto lines = readLines(path);
	ASSERT_EQ(lines.size(), 3u);
	EXPECT_TRUE(EndsWithLine(lines[0], "[hephaestus] startup"));
	EXPECT_TRUE(EndsWithLine(lines[1], "[ffmpeg] spawning: ..."));
	EXPECT_TRUE(EndsWithLine(lines[2], "[sessions] channel started"));
}

TEST(LogBufferTest, VerboseCategoriesHiddenFromRecentWhenFlagOff)
{
	VerboseFlagGuard guard;
	g_verbose_transcode_logs.store(false);

	LogBuffer buf;
	buf.setFilter(hephaestusLogFilter);
	buf.push("[hephaestus] startup");
	buf.push("[ffmpeg] spawning: ...");
	buf.push("[sessions] channel started");

	auto [lines, seq] = buf.recent(10);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_TRUE(EndsWithLine(lines[0], "[hephaestus] startup"));
}

TEST(LogBufferTest, VerboseCategoriesVisibleInRecentWhenFlagOn)
{
	VerboseFlagGuard guard;
	g_verbose_transcode_logs.store(true);

	LogBuffer buf;
	buf.setFilter(hephaestusLogFilter);
	buf.push("[hephaestus] startup");
	buf.push("[ffmpeg] spawning: ...");
	buf.push("[sessions] channel started");

	auto [lines, seq] = buf.recent(10);
	ASSERT_EQ(lines.size(), 3u);
}

TEST(LogBufferTest, NonVerboseCategoriesAlwaysVisibleRegardlessOfFlag)
{
	VerboseFlagGuard guard;
	g_verbose_transcode_logs.store(false);

	LogBuffer buf;
	buf.setFilter(hephaestusLogFilter);
	buf.push("[hwprobe] decode ok");
	buf.push("[kairos] GET /api/channels -> 200");
	buf.push("[probe] checking vaapi");

	auto [lines, seq] = buf.recent(10);
	EXPECT_EQ(lines.size(), 3u);
}