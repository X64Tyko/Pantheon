#include <gtest/gtest.h>
#include "log/LogBuffer.h"
#include "log/RuntimeFlags.h"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

namespace
{
	// RAII: g_verbose_gateway_logs is process-global — restore it after each
	// test that touches it so later tests aren't affected by test order.
	struct VerboseFlagGuard
	{
		bool prev = g_verbose_gateway_logs.load();
		~VerboseFlagGuard() { g_verbose_gateway_logs.store(prev); }
	};

	std::vector<std::string> readLines(const std::string& path)
	{
		std::ifstream f(path);
		std::vector<std::string> out;
		std::string line;
		while (std::getline(f, line)) out.push_back(line);
		return out;
	}
}

// PushAndRecent/WaitAfter/WaitAfterTimeout/Overflow (plain LogBuffer
// ring-buffer/cursor mechanics, no Hermes-specific code involved at all)
// moved to momus/hephaestus/test_log_buffer.cpp — LogBuffer is shared/log/
// code compiled into both services, and having byte-identical copies of
// those four tests in both binaries tested nothing an extra CI run of one
// wouldn't already cover.

// Regression: Hermes's LogBuffer used to have no file-writing at all, unlike
// Kairos's — so nothing it printed was ever durably captured, only held in
// the in-memory ring buffer (and, for Kairos/Hephaestus's relayed lines,
// never even reached this process's memory to begin with).

TEST(LogBufferTest, SetFileWritesEveryLineRegardlessOfCategoryFilter)
{
	VerboseFlagGuard guard;
	g_verbose_gateway_logs.store(false);

	auto path = (std::filesystem::temp_directory_path() / "hermes_logbuf_all.log").string();
	std::filesystem::remove(path);

	LogBuffer buf;
	buf.setFile(path);
	buf.setFilter(hermesLogFilter);
	buf.push("[hermes] listening on :8000");
	buf.push("[hermes] GET /foo -> 404");               // gated category, flag off
	buf.push("[roku-ecp] launch 10.0.0.5 unreachable"); // gated category, flag off

	auto lines = readLines(path);
	ASSERT_EQ(lines.size(), 3u);
	EXPECT_EQ(lines[0], "[hermes] listening on :8000");
	EXPECT_EQ(lines[1], "[hermes] GET /foo -> 404");
	EXPECT_EQ(lines[2], "[roku-ecp] launch 10.0.0.5 unreachable");
}

TEST(LogBufferTest, GatewayCategoriesHiddenFromRecentWhenFlagOff)
{
	VerboseFlagGuard guard;
	g_verbose_gateway_logs.store(false);

	LogBuffer buf;
	buf.setFilter(hermesLogFilter);
	buf.push("[hermes] GET /foo -> 404");
	buf.push("[roku-ecp] launch 10.0.0.5 unreachable");

	auto [lines, seq] = buf.recent(10);
	EXPECT_TRUE(lines.empty());
}

TEST(LogBufferTest, GatewayCategoriesVisibleInRecentWhenFlagOn)
{
	VerboseFlagGuard guard;
	g_verbose_gateway_logs.store(true);

	LogBuffer buf;
	buf.setFilter(hermesLogFilter);
	buf.push("[hermes] GET /foo -> 404");
	buf.push("[roku-ecp] launch 10.0.0.5 unreachable");

	auto [lines, seq] = buf.recent(10);
	EXPECT_EQ(lines.size(), 2u);
}

TEST(LogBufferTest, NonGatewayCategoryAlwaysVisibleRegardlessOfFlag)
{
	VerboseFlagGuard guard;
	g_verbose_gateway_logs.store(false);

	LogBuffer buf;
	buf.setFilter(hermesLogFilter);
	buf.push("[kairos] GET /api/channels -> 200");

	auto [lines, seq] = buf.recent(10);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(lines[0], "[kairos] GET /api/channels -> 200");
}

// setForward: local (file-backed) lines that pass the filter should reach
// the combined buffer too, but the combined buffer itself must never write
// to a file — relaying someone else's already-logged lines through Hermes
// must not duplicate them into hermes.log.
TEST(LogBufferTest, ForwardedLinesReachCombinedBufferButNotItsOwnFile)
{
	VerboseFlagGuard guard;
	g_verbose_gateway_logs.store(true);

	auto local_path = (std::filesystem::temp_directory_path() / "hermes_logbuf_local.log").string();
	std::filesystem::remove(local_path);

	LogBuffer combined;
	LogBuffer local;
	local.setFile(local_path);
	local.setForward(&combined);
	local.setFilter(hermesLogFilter);

	local.push("[hermes] listening on :8000");
	// Simulates relayUpstreamLogs pushing a line from Kairos directly into
	// the combined buffer, bypassing local entirely.
	combined.push("[sync] pass complete");

	auto [combinedLines, seq] = combined.recent(10);
	ASSERT_EQ(combinedLines.size(), 2u);
	EXPECT_EQ(combinedLines[0], "[hermes] listening on :8000");
	EXPECT_EQ(combinedLines[1], "[sync] pass complete");

	// Only the line pushed directly to `local` should be in hermes.log —
	// the relayed [sync] line was never local's, and never should reach it.
	auto fileLines = readLines(local_path);
	ASSERT_EQ(fileLines.size(), 1u);
	EXPECT_EQ(fileLines[0], "[hermes] listening on :8000");
}

TEST(LogBufferTest, ForwardRespectsLocalFilterBeforeReachingCombined)
{
	VerboseFlagGuard guard;
	g_verbose_gateway_logs.store(false);

	LogBuffer combined;
	LogBuffer local;
	local.setForward(&combined);
	local.setFilter(hermesLogFilter);

	local.push("[hermes] GET /foo -> 404"); // gated off locally — must not forward

	auto [combinedLines, seq] = combined.recent(10);
	EXPECT_TRUE(combinedLines.empty());
}