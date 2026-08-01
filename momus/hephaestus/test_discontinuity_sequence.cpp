#include <gtest/gtest.h>
#include "stream/ChannelSession.h"
#include "kairos/KairosClient.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

// Fresh temp hls_root + a ChannelSession pointed at it, without ever calling
// start() (no real ffmpeg/Kairos involved) — patchDiscontinuitySequenceForTest()
// only ever touches the playlist.m3u8 file directly.
class DiscontinuitySequenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() / ("momus_hls_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + testName());
        // Must match ChannelSession::hlsDir()'s real layout (hls_root/live/
        // channel_id/bucket) — the session below is constructed with the
        // default bucket ("default"), so the fixture needs that same
        // subfolder or patchDiscontinuitySequence()'s ifstream silently
        // fails to open a nonexistent path and every test vacuously passes.
        channelDir = root / "live" / "ch-1" / ChannelSession::kDefaultBucket;
        std::filesystem::create_directories(channelDir);

        kairos = std::make_unique<KairosClient>("http://unused.invalid");
        StreamOptions opts;
        opts.hls_root = root.string();
        session = std::make_unique<ChannelSession>("ch-1", *kairos, "ffmpeg", opts);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::string testName() const {
        return ::testing::UnitTest::GetInstance()->current_test_info()->name();
    }

    std::string playlistPath() const { return (channelDir / "playlist.m3u8").string(); }

    std::filesystem::path root, channelDir;
    std::unique_ptr<KairosClient>   kairos;
    std::unique_ptr<ChannelSession> session;
};

} // namespace

TEST_F(DiscontinuitySequenceTest, InsertsSequenceWhenDiscontinuityHasRolledOff) {
    // Exactly the reported bug: a discontinuity happened once (count=1) but
    // its #EXT-X-DISCONTINUITY tag has already scrolled out of the current
    // 6-segment window — none of these segments carry it anymore.
    writeFile(playlistPath(),
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:2\n"
        "#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:2.000,\n"
        "seg-00010.ts\n"
        "#EXTINF:2.000,\n"
        "seg-00011.ts\n");

    session->setDiscontinuityCountForTest(1);
    session->patchDiscontinuitySequenceForTest();

    std::string result = readFile(playlistPath());
    EXPECT_NE(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:1"), std::string::npos);
    // Placement: after #EXT-X-VERSION, still before the first segment.
    auto versionPos = result.find("#EXT-X-VERSION");
    auto seqPos      = result.find("#EXT-X-DISCONTINUITY-SEQUENCE");
    auto firstSegPos = result.find("#EXTINF");
    EXPECT_LT(versionPos, seqPos);
    EXPECT_LT(seqPos, firstSegPos);
}

TEST_F(DiscontinuitySequenceTest, ZeroWhenDiscontinuityStillVisible) {
    // Right after a transition — the tag is still inside the window, so the
    // *cumulative rolled-off* count is correctly 0 even though one has
    // happened in this session's lifetime.
    writeFile(playlistPath(),
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:2\n"
        "#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:2.000,\n"
        "seg-00010.ts\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:2.000,\n"
        "seg-00011.ts\n");

    session->setDiscontinuityCountForTest(1);
    session->patchDiscontinuitySequenceForTest();

    std::string result = readFile(playlistPath());
    EXPECT_NE(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:0"), std::string::npos);
    // The still-visible tag itself must survive the rewrite untouched.
    EXPECT_NE(result.find("#EXT-X-DISCONTINUITY\n"), std::string::npos);
}

TEST_F(DiscontinuitySequenceTest, CorrectsStaleExistingValue) {
    // A second discontinuity has since rolled off too — an existing (now
    // stale) sequence line must be updated, not left alone or duplicated.
    writeFile(playlistPath(),
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-DISCONTINUITY-SEQUENCE:1\n"
        "#EXT-X-TARGETDURATION:2\n"
        "#EXTINF:2.000,\n"
        "seg-00020.ts\n");

    session->setDiscontinuityCountForTest(2);
    session->patchDiscontinuitySequenceForTest();

    std::string result = readFile(playlistPath());
    EXPECT_EQ(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:1"), std::string::npos);
    EXPECT_NE(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:2"), std::string::npos);
    // Never two sequence lines.
    size_t first = result.find("#EXT-X-DISCONTINUITY-SEQUENCE:");
    EXPECT_EQ(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:", first + 1), std::string::npos);
}

TEST_F(DiscontinuitySequenceTest, IsIdempotentWhenAlreadyCorrect) {
    writeFile(playlistPath(),
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-DISCONTINUITY-SEQUENCE:1\n"
        "#EXTINF:2.000,\n"
        "seg-00020.ts\n");

    session->setDiscontinuityCountForTest(1);
    session->patchDiscontinuitySequenceForTest();
    std::string once = readFile(playlistPath());
    session->patchDiscontinuitySequenceForTest();
    std::string twice = readFile(playlistPath());

    EXPECT_EQ(once, twice);
}

TEST_F(DiscontinuitySequenceTest, SkipsGarbledMidWriteRead) {
    // No #EXTM3U header — simulates catching ffmpeg mid-rewrite. Must leave
    // the file untouched rather than "fixing" a partial read into a broken one.
    writeFile(playlistPath(), "#EXTINF:2.000,\nseg-00099.ts\n");

    session->setDiscontinuityCountForTest(3);
    session->patchDiscontinuitySequenceForTest();

    std::string result = readFile(playlistPath());
    EXPECT_EQ(result, "#EXTINF:2.000,\nseg-00099.ts\n");
}
