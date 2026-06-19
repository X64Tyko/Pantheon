#include <gtest/gtest.h>
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <set>
#include <string>
#include <vector>

// Bump this when a new migration is added. Every test that cares about the
// total count reads from here — no other number to update.
static constexpr int kMigrationCount = 23;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::set<std::string> tableNames(SQLite::Database& db) {
    std::set<std::string> out;
    SQLite::Statement q(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'");
    while (q.executeStep())
        out.insert(q.getColumn(0).getString());
    return out;
}

static std::set<std::string> columnNames(SQLite::Database& db, const std::string& table) {
    std::set<std::string> out;
    SQLite::Statement q(db, "PRAGMA table_info(" + table + ")");
    while (q.executeStep())
        out.insert(q.getColumn(1).getString());
    return out;
}

static bool indexExists(SQLite::Database& db, const std::string& name) {
    SQLite::Statement q(db, "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?");
    q.bind(1, name);
    return q.executeStep();
}

// ---------------------------------------------------------------------------
// Fixture — fresh in-memory DB per test
// ---------------------------------------------------------------------------

class MigrationTest : public ::testing::Test {
protected:
    Database db{ ":memory:" };
};

// ---------------------------------------------------------------------------
// Migration count & sequencing
// ---------------------------------------------------------------------------

TEST_F(MigrationTest, AllMigrationsApplied) {
    SQLite::Statement q(db.get(), "SELECT COUNT(*) FROM schema_migrations");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), kMigrationCount);
}

TEST_F(MigrationTest, MigrationVersionsAreContiguousFrom1) {
    std::vector<int> versions;
    SQLite::Statement q(db.get(),
        "SELECT version FROM schema_migrations ORDER BY version");
    while (q.executeStep())
        versions.push_back(q.getColumn(0).getInt());
    ASSERT_EQ(versions.size(), static_cast<size_t>(kMigrationCount));
    for (int i = 0; i < kMigrationCount; ++i)
        EXPECT_EQ(versions[i], i + 1) << "Gap at migration " << (i + 1);
}

// ---------------------------------------------------------------------------
// Table existence (all 20 domain tables + schema_migrations)
// ---------------------------------------------------------------------------

TEST_F(MigrationTest, AllExpectedTablesExist) {
    auto tables = tableNames(db.get());
    const std::vector<std::string> expected = {
        "channel", "show", "episode", "movie",
        "block", "block_content",
        "play_history",
        "media_source", "media_library", "source_mapping",
        "playlist", "playlist_item",
        "filler_list", "filler_list_item",
        "plex_list_link",
        "block_filler_entry", "channel_filler_entry",
        "block_state",
        "media_cursor",
        "scheduled_program",
        "schema_migrations",
    };
    for (const auto& t : expected)
        EXPECT_GT(tables.count(t), 0u) << "Missing table: " << t;
}

// ---------------------------------------------------------------------------
// Column presence — one test per table with non-trivial evolution
// ---------------------------------------------------------------------------

TEST_F(MigrationTest, ChannelColumnsIncludeAllMigrationAdditions) {
    auto cols = columnNames(db.get(), "channel");
    EXPECT_GT(cols.count("channel_id"),              0u);
    EXPECT_GT(cols.count("name"),                    0u);
    EXPECT_GT(cols.count("number"),                  0u);
    EXPECT_GT(cols.count("timezone"),                0u) << "added v2";
    EXPECT_GT(cols.count("default_filler_selection"),0u) << "added v8";
    EXPECT_GT(cols.count("seed"),                    0u) << "added v11";
}

TEST_F(MigrationTest, ShowColumnsPresentAndPlexRatingKeyDropped) {
    auto cols = columnNames(db.get(), "show");
    EXPECT_GT(cols.count("show_id"),        0u);
    EXPECT_GT(cols.count("title"),          0u);
    EXPECT_GT(cols.count("content_rating"), 0u);
    // Rich metadata from v3
    EXPECT_GT(cols.count("overview"),       0u) << "added v3";
    EXPECT_GT(cols.count("genres"),         0u) << "added v3";
    EXPECT_GT(cols.count("thumb"),          0u) << "added v3";
    EXPECT_GT(cols.count("imdb_id"),        0u) << "added v3";
    EXPECT_GT(cols.count("tvdb_id"),        0u) << "added v3";
    EXPECT_GT(cols.count("tmdb_id"),        0u) << "added v3";
    EXPECT_GT(cols.count("locked"),         0u) << "added v3";
    // Dropped in v2
    EXPECT_EQ(cols.count("plex_rating_key"), 0u) << "must be absent after v2 drop";
}

TEST_F(MigrationTest, BlockColumnsIncludeAllMigrationAdditions) {
    auto cols = columnNames(db.get(), "block");
    EXPECT_GT(cols.count("block_id"),        0u);
    EXPECT_GT(cols.count("channel_id"),      0u);
    EXPECT_GT(cols.count("block_type"),      0u);
    EXPECT_GT(cols.count("day_mask"),        0u);
    EXPECT_GT(cols.count("start_time"),      0u);
    EXPECT_GT(cols.count("priority"),        0u);
    EXPECT_GT(cols.count("program_count"),   0u) << "added v4";
    EXPECT_GT(cols.count("advancement"),     0u) << "added v5";
    EXPECT_GT(cols.count("cursor_scope"),    0u) << "added v5";
    EXPECT_GT(cols.count("late_start_mins"), 0u) << "added v6";
    EXPECT_GT(cols.count("align_to_mins"),   0u) << "added v8";
    EXPECT_GT(cols.count("inter_filler"),    0u) << "added v8";
    EXPECT_GT(cols.count("early_start_secs"),0u) << "added v8";
    EXPECT_GT(cols.count("filler_selection"),0u) << "added v8";
    EXPECT_GT(cols.count("smart_pct"),       0u) << "added v13";
    EXPECT_GT(cols.count("start_scope"),         0u) << "added v14";
    EXPECT_GT(cols.count("no_history_behavior"), 0u) << "added v15";
}

TEST_F(MigrationTest, ScheduledProgramColumnsFromV10) {
    auto cols = columnNames(db.get(), "scheduled_program");
    EXPECT_GT(cols.count("id"),              0u);
    EXPECT_GT(cols.count("channel_id"),      0u);
    EXPECT_GT(cols.count("block_id"),        0u);
    EXPECT_GT(cols.count("item_type"),       0u);
    EXPECT_GT(cols.count("item_id"),         0u);
    EXPECT_GT(cols.count("wall_clock_start"),0u);
    EXPECT_GT(cols.count("wall_clock_end"),  0u);
    EXPECT_GT(cols.count("status"),          0u);
    EXPECT_GT(cols.count("cursor_json"),     0u);
    EXPECT_GT(cols.count("created_at"),      0u);
}

TEST_F(MigrationTest, EpisodeColumnsIncludeV3Additions) {
    auto cols = columnNames(db.get(), "episode");
    EXPECT_GT(cols.count("episode_id"),  0u);
    EXPECT_GT(cols.count("show_id"),     0u);
    EXPECT_GT(cols.count("season"),      0u);
    EXPECT_GT(cols.count("episode"),     0u);
    EXPECT_GT(cols.count("duration_ms"), 0u);
    EXPECT_GT(cols.count("overview"),    0u) << "added v3";
    EXPECT_GT(cols.count("air_date"),    0u) << "added v3";
    EXPECT_GT(cols.count("thumb"),       0u) << "added v3";
}

TEST_F(MigrationTest, BlockContentHasSeasonFilterFromV5) {
    auto cols = columnNames(db.get(), "block_content");
    EXPECT_GT(cols.count("id"),           0u);
    EXPECT_GT(cols.count("block_id"),     0u);
    EXPECT_GT(cols.count("content_type"), 0u);
    EXPECT_GT(cols.count("content_id"),   0u);
    EXPECT_GT(cols.count("position"),     0u);
    EXPECT_GT(cols.count("season_filter"),0u) << "added v5";
    EXPECT_GT(cols.count("weight"),       0u) << "added v13";
    EXPECT_GT(cols.count("run_count"),    0u) << "added v13";
    // advancement and cursor_scope were moved to block level in v5
    EXPECT_EQ(cols.count("advancement"),   0u) << "moved to block in v5";
    EXPECT_EQ(cols.count("cursor_scope"),  0u) << "moved to block in v5";
}

// ---------------------------------------------------------------------------
// Index existence
// ---------------------------------------------------------------------------

TEST_F(MigrationTest, AllKeyIndicesExist) {
    const std::vector<std::string> indices = {
        "idx_episode_show",
        "idx_block_channel",
        "idx_block_content_block",
        "idx_history_channel",
        "idx_media_library_source",
        "idx_source_mapping_lookup",
        "idx_playlist_item",
        "idx_filler_list_item",
        "idx_block_filler_entry",
        "idx_channel_filler_entry",
        "idx_sched_channel_time",
        "idx_sched_channel_end",
    };
    for (const auto& idx : indices)
        EXPECT_TRUE(indexExists(db.get(), idx)) << "Missing index: " << idx;
}

// ---------------------------------------------------------------------------
// Idempotency — second Database open on same :memory: can't easily reuse the
// same connection, so we verify no rows exist beyond kMigrationCount (no
// double-apply happened during the single-pass startup).
// ---------------------------------------------------------------------------

TEST_F(MigrationTest, NoDoubleMigrationsDuringStartup) {
    SQLite::Statement q(db.get(),
        "SELECT COUNT(*) FROM schema_migrations WHERE version > ?");
    q.bind(1, kMigrationCount);
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 0) << "Unexpected extra migration row";
}
