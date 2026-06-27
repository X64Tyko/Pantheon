#include <gtest/gtest.h>
#include "db/Database.h"
#include "scheduler/RuleEngine.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/CursorState.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// Helper — write a string to a file next to the test binary and return the path
// ---------------------------------------------------------------------------

static std::string writeFile(const std::string& name, const std::string& content) {
    auto path = std::filesystem::current_path() / name;
    std::ofstream f(path);
    f << content;
    std::cout << "[EPGOutput] wrote " << path.string()
              << " (" << content.size() << " bytes)\n";
    return path.string();
}

// ---------------------------------------------------------------------------
// Fixture — two channels, a multi-episode show, and a movie
// ---------------------------------------------------------------------------

class EPGOutputTest : public ::testing::Test {
protected:
    Database        db{ ":memory:" };
    RuleEngine      engine{ db };
    EPGMaterializer materializer{ db, engine };

    void SetUp() override {
        auto& raw = db.get();

        // Channels
        raw.exec("INSERT INTO channel (channel_id, name, number, timezone)"
                 " VALUES ('ch1','Drama Central',1,'UTC')");
        raw.exec("INSERT INTO channel (channel_id, name, number, timezone)"
                 " VALUES ('ch2','Movie Night',2,'UTC')");

        // Show: "Breaking Chemistry" — 6 x 45-min episodes across 2 seasons
        raw.exec("INSERT INTO show (show_id, title, content_rating, overview)"
                 " VALUES ('s1','Breaking Chemistry','TV-MA',"
                 "'A chemistry teacher turns to a life of crime.')");

        const char* ep_sql =
            "INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms,overview)"
            " VALUES (?,?,?,?,?,?,?,?)";

        auto insertEp = [&](const char* id, int s, int e,
                             const char* title, const char* path, const char* overview) {
            SQLite::Statement q(raw, ep_sql);
            q.bind(1, id);  q.bind(2, "s1"); q.bind(3, s); q.bind(4, e);
            q.bind(5, title); q.bind(6, path);
            q.bind(7, static_cast<int64_t>(2700000)); // 45 min
            q.bind(8, overview);
            q.exec();
        };

        insertEp("s1e01", 1, 1, "Pilot",               "/media/bc/s01e01.mkv", "Walt meets Jesse.");
        insertEp("s1e02", 1, 2, "Cat's in the Bag",    "/media/bc/s01e02.mkv", "The duo deal with consequences.");
        insertEp("s1e03", 1, 3, "...And the Bag's in the River", "/media/bc/s01e03.mkv", "A decision must be made.");
        insertEp("s2e01", 2, 1, "Seven Thirty-Seven",  "/media/bc/s02e01.mkv", "A new threat emerges.");
        insertEp("s2e02", 2, 2, "Down",                "/media/bc/s02e02.mkv", "Walt struggles with his choices.");
        insertEp("s2e03", 2, 3, "Over",                "/media/bc/s02e03.mkv", "The situation escalates.");

        // Movie: "Galactic Voyage" — 2-hour film
        raw.exec("INSERT INTO movie (movie_id,title,file_path,duration_ms,year,overview,content_rating)"
                 " VALUES ('m1','Galactic Voyage','/media/galactic_voyage.mkv',7200000,2022,"
                 "'An astronaut crew ventures beyond the solar system.','PG-13')");

        // Channel 1 — all-day sequential episode block (no program_count cap)
        raw.exec("INSERT INTO block (block_id,channel_id,block_type,start_time,day_mask,priority,program_count)"
                 " VALUES ('b1','ch1','episode','00:00',127,0,0)");
        raw.exec("INSERT INTO block_content (block_id,content_type,content_id)"
                 " VALUES ('b1','show','s1')");

        // Channel 2 — all-day movie block
        raw.exec("INSERT INTO block (block_id,channel_id,block_type,start_time,day_mask,priority,program_count)"
                 " VALUES ('b2','ch2','movie','00:00',127,0,0)");
        raw.exec("INSERT INTO block_content (block_id,content_type,content_id)"
                 " VALUES ('b2','movie','m1')");
    }
};

// ---------------------------------------------------------------------------
// XMLTV output test
// ---------------------------------------------------------------------------

TEST_F(EPGOutputTest, XMLTV_StructureAndFileOutput) {
    std::string xml = materializer.generateXMLTV(12);

    // ── Print to stdout for quick terminal inspection ──────────────────────
    std::cout << "\n========== XMLTV OUTPUT (12h) ==========\n"
              << xml
              << "========================================\n\n";

    // ── Write to file for deeper analysis ─────────────────────────────────
    writeFile("kairos_sample.xmltv", xml);

    // ── Structural assertions ──────────────────────────────────────────────

    // Document header
    EXPECT_NE(xml.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"), std::string::npos)
        << "Must have XML declaration";
    EXPECT_NE(xml.find("<!DOCTYPE tv"), std::string::npos)
        << "Must have XMLTV DOCTYPE";
    EXPECT_NE(xml.find("<tv "), std::string::npos)
        << "Must have <tv> root element";
    EXPECT_NE(xml.find("</tv>"), std::string::npos)
        << "Must close <tv>";

    // Channel declarations
    EXPECT_NE(xml.find("kairos-1"), std::string::npos)
        << "Must have channel id kairos-1";
    EXPECT_NE(xml.find("kairos-2"), std::string::npos)
        << "Must have channel id kairos-2";
    EXPECT_NE(xml.find("Drama Central"), std::string::npos)
        << "Must include channel display name";
    EXPECT_NE(xml.find("Movie Night"), std::string::npos)
        << "Must include channel display name";

    // Programme elements
    EXPECT_NE(xml.find("<programme"), std::string::npos)
        << "Must have at least one <programme>";
    EXPECT_NE(xml.find("</programme>"), std::string::npos)
        << "Every <programme> must be closed";

    // Timestamp format: YYYYMMDDHHmmss +0000
    EXPECT_NE(xml.find("+0000"), std::string::npos)
        << "Timestamps must include UTC offset";

    // Title elements
    EXPECT_NE(xml.find("<title lang=\"en\">"), std::string::npos)
        << "Must have title element";

    // Show titles on channel 1 (episode block)
    EXPECT_NE(xml.find("Breaking Chemistry"), std::string::npos)
        << "Show title must appear in XMLTV output";

    // Movie title on channel 2
    EXPECT_NE(xml.find("Galactic Voyage"), std::string::npos)
        << "Movie title must appear in XMLTV output";

    // Episode numbering (XMLTV NS format: season-1.ep-1.0/1)
    EXPECT_NE(xml.find("xmltv_ns"), std::string::npos)
        << "Episodes must include xmltv_ns episode-num";
    EXPECT_NE(xml.find("onscreen"), std::string::npos)
        << "Episodes must include onscreen episode-num (S01E01 style)";

    // Sub-title for episodes (episode title within show)
    EXPECT_NE(xml.find("<sub-title"), std::string::npos)
        << "Episodes must have sub-title (individual episode name)";

    // Episode titles visible in output
    EXPECT_NE(xml.find("Pilot"), std::string::npos)
        << "First episode title should appear";

    // Description from overview field
    EXPECT_NE(xml.find("<desc"), std::string::npos)
        << "Items with overview must include <desc>";
}

TEST_F(EPGOutputTest, XMLTV_EscapesSpecialCharacters) {
    // Patch existing rows to contain XML-special characters — no FK changes needed
    db.get().exec("UPDATE show    SET title    = 'Science & Math'      WHERE show_id    = 's1'");
    db.get().exec("UPDATE episode SET title    = 'Pilot <Unaired>'     WHERE episode_id = 's1e01'");
    db.get().exec("UPDATE episode SET overview = 'Walt & Jesse > risk' WHERE episode_id = 's1e01'");

    std::string xml = materializer.generateXMLTV(4);

    // & in show/programme title → &amp;
    EXPECT_NE(xml.find("Science &amp; Math"), std::string::npos)
        << "Bare & in title must be escaped to &amp;";
    EXPECT_EQ(xml.find("Science & Math"), std::string::npos)
        << "Raw & must not appear unescaped in XML output";

    // < > in episode title in <sub-title> → &lt; &gt;
    EXPECT_NE(xml.find("Pilot &lt;Unaired&gt;"), std::string::npos)
        << "< > in episode title must be XML-escaped";
    EXPECT_EQ(xml.find("Pilot <Unaired>"), std::string::npos)
        << "Raw < > must not appear unescaped in XML output";

    // & and > in overview → &amp; &gt; inside <desc>
    EXPECT_NE(xml.find("Walt &amp; Jesse &gt; risk"), std::string::npos)
        << "& and > in overview must be XML-escaped in <desc>";
}

TEST_F(EPGOutputTest, XMLTV_IsWellFormedXML) {
    std::string xml = materializer.generateXMLTV(6);

    // Every opening tag we emit has a matching close — count occurrences
    auto count = [&](const std::string& needle) -> size_t {
        size_t n = 0, pos = 0;
        while ((pos = xml.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    };

    size_t open_prog  = count("<programme");
    size_t close_prog = count("</programme>");
    EXPECT_EQ(open_prog, close_prog)
        << "Every <programme> must have a closing tag";

    size_t open_ch  = count("<channel");
    size_t close_ch = count("</channel>");
    EXPECT_EQ(open_ch, close_ch)
        << "Every <channel> must have a closing tag";

    size_t open_title  = count("<title");
    size_t close_title = count("</title>");
    EXPECT_EQ(open_title, close_title)
        << "Every <title> must have a closing tag";
}

TEST_F(EPGOutputTest, XMLTV_ProgrammeTimesAreChronological) {
    std::string xml = materializer.generateXMLTV(6);

    // XMLTV groups all programmes by channel — ch1's full run then ch2's full run.
    // Both channels start from "now", so cross-channel timestamp comparison is not
    // meaningful.  Check ordering only within channel kairos-1's programme block.
    std::vector<std::string> starts;
    size_t pos = 0;
    while ((pos = xml.find("<programme", pos)) != std::string::npos) {
        size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) break;
        std::string opening = xml.substr(pos, tag_end - pos);

        if (opening.find("channel=\"kairos-1\"") != std::string::npos) {
            size_t s = opening.find("start=\"");
            if (s != std::string::npos) {
                s += 7;
                size_t e = opening.find('"', s);
                if (e != std::string::npos)
                    starts.push_back(opening.substr(s, e - s));
            }
        }
        pos = tag_end + 1;
    }

    ASSERT_FALSE(starts.empty()) << "Expected programme entries for kairos-1";
    // Lexicographic comparison is valid: format is "YYYYMMDDHHmmss +0000"
    for (size_t i = 1; i < starts.size(); ++i)
        EXPECT_GE(starts[i], starts[i - 1])
            << "Channel 1 programme start times must be non-decreasing at index " << i;
}

// ---------------------------------------------------------------------------
// M3U output test
// ---------------------------------------------------------------------------

TEST_F(EPGOutputTest, M3U_StructureAndFileOutput) {
    const std::string base = "http://kairos.local:8080";
    std::string m3u = materializer.generateM3U(base);

    // ── Print to stdout ────────────────────────────────────────────────────
    std::cout << "\n========== M3U OUTPUT ==========\n"
              << m3u
              << "================================\n\n";

    // ── Write to file ──────────────────────────────────────────────────────
    writeFile("kairos_channels.m3u", m3u);

    // ── Structural assertions ──────────────────────────────────────────────

    EXPECT_EQ(m3u.substr(0, 7), "#EXTM3U")
        << "M3U must start with #EXTM3U magic";

    // One EXTINF line per channel
    auto countStr = [&](const std::string& needle) -> size_t {
        size_t n = 0, pos = 0;
        while ((pos = m3u.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    };

    EXPECT_EQ(countStr("#EXTINF"), 2u)
        << "Must have one #EXTINF line per channel";

    // Channel 1
    EXPECT_NE(m3u.find("tvg-id=\"kairos-1\""), std::string::npos)
        << "Channel 1 must have tvg-id";
    EXPECT_NE(m3u.find("tvg-name=\"Drama Central\""), std::string::npos)
        << "Channel 1 must have tvg-name";
    EXPECT_NE(m3u.find("channel-number=\"1\""), std::string::npos)
        << "Channel 1 must have channel-number";
    EXPECT_NE(m3u.find(base + "/channels/ch1/stream"), std::string::npos)
        << "Channel 1 stream URL must use channel_id not number";

    // Channel 2
    EXPECT_NE(m3u.find("tvg-id=\"kairos-2\""), std::string::npos)
        << "Channel 2 must have tvg-id";
    EXPECT_NE(m3u.find("tvg-name=\"Movie Night\""), std::string::npos)
        << "Channel 2 must have tvg-name";
    EXPECT_NE(m3u.find(base + "/channels/ch2/stream"), std::string::npos)
        << "Channel 2 stream URL must be correct";

    // Group tag
    EXPECT_NE(m3u.find("group-title=\"Kairos\""), std::string::npos)
        << "All channels must belong to Kairos group";

    // Stream URLs contain the base URL
    EXPECT_NE(m3u.find(base), std::string::npos)
        << "Stream URLs must use the supplied base URL";
}

TEST_F(EPGOutputTest, M3U_ChannelsOrderedByNumber) {
    const std::string m3u = materializer.generateM3U("http://localhost");

    size_t pos1 = m3u.find("channel-number=\"1\"");
    size_t pos2 = m3u.find("channel-number=\"2\"");
    ASSERT_NE(pos1, std::string::npos);
    ASSERT_NE(pos2, std::string::npos);
    EXPECT_LT(pos1, pos2) << "Channel 1 must appear before channel 2";
}

TEST_F(EPGOutputTest, M3U_EmptyWhenNoChannels) {
    // Fresh DB with no channels
    Database   empty_db{ ":memory:" };
    RuleEngine empty_engine{ empty_db };
    EPGMaterializer empty_mat{ empty_db, empty_engine };
    std::string m3u = empty_mat.generateM3U("http://localhost");
    EXPECT_EQ(m3u, "#EXTM3U\n") << "Empty channel list should produce only the header";
}

// ---------------------------------------------------------------------------
// Combined round-trip: generate schedule → write XMLTV → check coherence
// ---------------------------------------------------------------------------

TEST_F(EPGOutputTest, RoundTrip_XmltvProgramCountMatchesProjectedItems) {
    constexpr int HOURS = 6;

    // generateXMLTV() queries from today_midnight (not now) so programs that
    // aired earlier today are included.  Mirror that lower bound here so the
    // DB reference count matches the XML output.
    std::time_t now_ts          = std::time(nullptr);
    std::time_t today_midnight  = (now_ts / 86400) * 86400;
    std::time_t horizon_ts      = now_ts + HOURS * 3600;

    // Run the full production pipeline: generateXMLTV() calls ensureScheduled()
    // which writes to scheduled_program, then renders XML from that table.
    std::string xml = materializer.generateXMLTV(HOURS);

    // Count <programme channel="kairos-1"> entries in the XMLTV output.
    size_t xml_prog_count = 0;
    size_t pos = 0;
    while ((pos = xml.find("channel=\"kairos-1\"", pos)) != std::string::npos) {
        ++xml_prog_count;
        pos += 18;
    }

    // Count scheduled_program rows using the same filters that generateXMLTV()
    // applies: non-filler, non-skipped, and within the [today_midnight, horizon) window.
    // ensureScheduled() may write a few overshoot rows past the horizon —
    // the window filter excludes those the same way the XML query does.
    SQLite::Statement q(db.get(), R"(
        SELECT COUNT(*) FROM scheduled_program
        WHERE channel_id='ch1'
          AND is_filler = 0
          AND status != 'skipped'
          AND wall_clock_end   > ?
          AND wall_clock_start < ?
    )");
    q.bind(1, static_cast<int64_t>(today_midnight));
    q.bind(2, static_cast<int64_t>(horizon_ts));
    q.executeStep();
    size_t db_prog_count = static_cast<size_t>(q.getColumn(0).getInt());

    EXPECT_GT(xml_prog_count, 0u) << "XMLTV must contain programmes for channel 1";
    EXPECT_EQ(xml_prog_count, db_prog_count)
        << "XMLTV programme count must match filtered scheduled_program rows for ch1";
}

// ---------------------------------------------------------------------------
// EPGMaterializer / generate() / ensureScheduled() integration
// ---------------------------------------------------------------------------

// 2024-01-01 00:00:00 UTC — Monday midnight that anchors kMonNoon's week.
static constexpr std::time_t kMon2024Midnight = 1704067200;
// 2024-01-01 12:00:00 UTC (same Monday, noon) — used as projection start.
static constexpr std::time_t kMon2024Noon     = 1704110400;

TEST_F(EPGOutputTest, Generate_OnPlayMode_DoesNotWriteCursorsToDB) {
    // on_play mode: ensureScheduled() must not persist cursor state to media_cursor
    // or block_state (the SAVEPOINT for cursor writes is rolled back implicitly by
    // not calling applyToDB).
    db.get().exec("UPDATE channel SET advance_mode='on_play' WHERE channel_id='ch1'");

    materializer.ensureScheduled("ch1", kMon2024Noon, 4);

    SQLite::Statement qc(db.get(),
        "SELECT COUNT(*) FROM media_cursor"
        " WHERE cursor_scope IN ('channel','block')"
        "   AND scope_id IN (SELECT block_id FROM block WHERE channel_id='ch1'"
        "                    UNION SELECT 'ch1')");
    qc.executeStep();
    EXPECT_EQ(qc.getColumn(0).getInt(), 0)
        << "on_play mode must not persist cursor rows to media_cursor";

    SQLite::Statement qb(db.get(),
        "SELECT COUNT(*) FROM block_state WHERE channel_id='ch1'");
    qb.executeStep();
    EXPECT_EQ(qb.getColumn(0).getInt(), 0)
        << "on_play mode must not persist block_state rows";
}

TEST_F(EPGOutputTest, Generate_OnPlayMode_WritesProgramRowsToScheduledProgram) {
    db.get().exec("UPDATE channel SET advance_mode='on_play' WHERE channel_id='ch1'");

    materializer.ensureScheduled("ch1", kMon2024Noon, 4);

    SQLite::Statement q(db.get(),
        "SELECT COUNT(*) FROM scheduled_program WHERE channel_id='ch1'");
    q.executeStep();
    EXPECT_GT(q.getColumn(0).getInt(), 0)
        << "on_play mode must still write scheduled_program rows";
}

TEST_F(EPGOutputTest, Generate_ScheduledMode_CursorAdvancedInDB) {
    // Default advance_mode='scheduled': commit() calls applyToDB() which must
    // persist at least one cursor row for the show block on ch1.
    materializer.ensureScheduled("ch1", kMon2024Noon, 4);

    // block b1 has default cursor_scope='block', so look for a block-scoped cursor.
    SQLite::Statement q(db.get(), R"(
        SELECT COUNT(*) FROM media_cursor
        WHERE cursor_scope='block'
          AND scope_id IN (SELECT block_id FROM block WHERE channel_id='ch1')
    )");
    q.executeStep();
    EXPECT_GT(q.getColumn(0).getInt(), 0)
        << "scheduled mode must persist block-scoped cursor after ensureScheduled";
}

TEST_F(EPGOutputTest, Generate_AnchorRestoration_LoadsCursorStateFromJson) {
    // Inject an anchor_hashes snapshot that places the show cursor at position 3
    // (i.e., episode index 3 = s2e01 in the 6-episode test fixture).
    // generate() must restore from it and produce s2e01 as the first item.
    using json = nlohmann::json;

    const std::string anchor_key = std::to_string(kMon2024Midnight);
    // block_states must be non-empty so projectDay sees hasBlockPosition("b1")==true
    // and skips the RNG-based seed-init that would overwrite the loaded cursor.
    json snap = {
        {"rng", "1 2 3 4"},   // arbitrary valid rng state
        {"cursors", json::array({{
            {"content_type", "show"},
            {"content_id",   "s1"},
            {"cursor_scope", "block"},
            {"scope_id",     "b1"},
            {"position",     3},
            {"episode_id",   ""}
        }})},
        {"block_states", json::array({{
            {"block_id",          "b1"},
            {"content_position",  0},
            {"runs_remaining",    1},
            {"consecutive_count", 0}
        }})}
    };
    json anchor_hashes = json::object();
    anchor_hashes[anchor_key] = snap;

    SQLite::Statement upd(db.get(),
        "UPDATE channel SET anchor_hashes=? WHERE channel_id='ch1'");
    upd.bind(1, anchor_hashes.dump());
    upd.exec();

    auto result = materializer.generate("ch1", kMon2024Noon, 2);
    ASSERT_FALSE(result.items.empty());

    // Filter to items that start at or after kMon2024Midnight so we skip any
    // leftover items projected before our query window.
    std::string first_id;
    for (const auto& item : result.items) {
        if (item.wall_clock_start_ms / 1000 >= kMon2024Midnight) {
            first_id = item.item_id;
            break;
        }
    }
    EXPECT_EQ(first_id, "s2e01")
        << "Anchor cursor at position=3 should restore to the 4th episode (s2e01)";
}

TEST_F(EPGOutputTest, EnsureScheduled_ClearsStaleHistoryBeforeReplay) {
    // In on_play mode: after markPlayed advances the cursor for s1e01, a subsequent
    // ensureScheduled() must schedule s1e02 first — not replay s1e01.
    db.get().exec("UPDATE channel SET advance_mode='on_play' WHERE channel_id='ch1'");

    // Pre-seed the cursor at position 0 so the RNG-based seed-init is bypassed.
    // Without this, projectDay randomises the starting episode on a fresh channel.
    db.get().exec(
        "INSERT INTO block_state (block_id,channel_id,content_position,"
        "runs_remaining,consecutive_count,updated_at)"
        " VALUES ('b1','ch1',0,1,0,strftime('%s','now'))");
    db.get().exec(
        "INSERT INTO media_cursor (content_type,content_id,cursor_scope,scope_id,"
        "position,episode_id,updated_at)"
        " VALUES ('show','s1','block','b1',0,'s1e01',strftime('%s','now'))");

    // Prime the schedule so we know what item 0 is.
    materializer.ensureScheduled("ch1", kMon2024Noon, 4);
    {
        SQLite::Statement q(db.get(),
            "SELECT item_id FROM scheduled_program"
            " WHERE channel_id='ch1' ORDER BY wall_clock_start LIMIT 1");
        ASSERT_TRUE(q.executeStep());
        EXPECT_EQ(q.getColumn(0).getString(), "s1e01")
            << "First scheduled item before any markPlayed should be s1e01";
    }

    // Advance the cursor via markPlayed (on_play mode: this is the sole cursor driver).
    engine.markPlayed("ch1", "b1", "episode", "s1e01", 2700000);

    // Regenerate — ensureScheduled deletes old rows and replans from cursor position.
    materializer.ensureScheduled("ch1", kMon2024Noon, 4);
    {
        SQLite::Statement q(db.get(),
            "SELECT item_id FROM scheduled_program"
            " WHERE channel_id='ch1' ORDER BY wall_clock_start LIMIT 1");
        ASSERT_TRUE(q.executeStep());
        EXPECT_EQ(q.getColumn(0).getString(), "s1e02")
            << "After markPlayed(s1e01), regenerated schedule must start from s1e02";
    }
}
