#include "Database.h"
#include <ctime>
#include <filesystem>
#include <iostream>

// ---------------------------------------------------------------------------
// Migrations — append-only. Never edit an existing entry; add a new one.
// Set requires_fk_off = true when the migration rebuilds tables that have
// FK references pointing at them (SQLite requires FK off for table drops).
// ---------------------------------------------------------------------------
namespace {

struct Migration {
    int         version;
    const char* sql;
    bool        requires_fk_off = false;
};

constexpr Migration kMigrations[] = {

// ── v1: base schema ─────────────────────────────────────────────────────────
{ 1, R"SQL(

    CREATE TABLE IF NOT EXISTS channel (
        channel_id  TEXT    PRIMARY KEY,
        name        TEXT    NOT NULL,
        number      INTEGER NOT NULL UNIQUE
    );

    CREATE TABLE IF NOT EXISTS show (
        show_id         TEXT PRIMARY KEY,
        title           TEXT NOT NULL,
        plex_rating_key TEXT,
        content_rating  TEXT
    );

    CREATE TABLE IF NOT EXISTS episode (
        episode_id  TEXT    PRIMARY KEY,
        show_id     TEXT    NOT NULL REFERENCES show(show_id),
        season      INTEGER NOT NULL,
        episode     INTEGER NOT NULL,
        title       TEXT    NOT NULL,
        file_path   TEXT    NOT NULL,
        duration_ms INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS block (
        block_id    TEXT    PRIMARY KEY,
        channel_id  TEXT    NOT NULL REFERENCES channel(channel_id),
        block_type  TEXT    NOT NULL CHECK(block_type IN ('episode','premier','filler')),
        day_mask    INTEGER NOT NULL DEFAULT 127,
        start_time  TEXT    NOT NULL DEFAULT '00:00',
        end_time    TEXT,
        priority    INTEGER NOT NULL DEFAULT 0,
        config_json TEXT    NOT NULL DEFAULT '{}'
    );

    CREATE TABLE IF NOT EXISTS block_show (
        block_id    TEXT    NOT NULL REFERENCES block(block_id),
        show_id     TEXT    NOT NULL REFERENCES show(show_id),
        position    INTEGER NOT NULL DEFAULT 0,
        advancement TEXT    NOT NULL DEFAULT 'sequential'
                             CHECK(advancement IN ('sequential','shuffle','rerun_shuffle')),
        PRIMARY KEY (block_id, show_id)
    );

    CREATE TABLE IF NOT EXISTS play_history (
        history_id  INTEGER PRIMARY KEY AUTOINCREMENT,
        episode_id  TEXT    NOT NULL REFERENCES episode(episode_id),
        channel_id  TEXT    NOT NULL REFERENCES channel(channel_id),
        block_id    TEXT    REFERENCES block(block_id),
        aired_at    INTEGER NOT NULL
    );

    CREATE INDEX IF NOT EXISTS idx_episode_show     ON episode(show_id);
    CREATE INDEX IF NOT EXISTS idx_block_channel    ON block(channel_id, priority);
    CREATE INDEX IF NOT EXISTS idx_block_show_block ON block_show(block_id, position);
    CREATE INDEX IF NOT EXISTS idx_history_episode  ON play_history(episode_id);
    CREATE INDEX IF NOT EXISTS idx_history_channel  ON play_history(channel_id, aired_at DESC);

)SQL" },

// ── v2: multi-source media, movie support, generalized block content ─────────
// Requires FK off: drops and rebuilds block and play_history.
{ 2, R"SQL(

    -- Extend channel with broadcast timezone
    ALTER TABLE channel ADD COLUMN timezone TEXT NOT NULL DEFAULT 'UTC';

    -- Remove plex-specific column (provider IDs move to source_mapping)
    ALTER TABLE show DROP COLUMN plex_rating_key;

    -- Rebuild block: add max_content_rating, add 'movie' to block_type check
    CREATE TABLE block_new (
        block_id           TEXT    PRIMARY KEY,
        channel_id         TEXT    NOT NULL REFERENCES channel(channel_id),
        block_type         TEXT    NOT NULL CHECK(block_type IN ('episode','premier','filler','movie')),
        day_mask           INTEGER NOT NULL DEFAULT 127,
        start_time         TEXT    NOT NULL DEFAULT '00:00',
        end_time           TEXT,
        priority           INTEGER NOT NULL DEFAULT 0,
        max_content_rating TEXT,
        config_json        TEXT    NOT NULL DEFAULT '{}'
    );

    INSERT INTO block_new
        (block_id, channel_id, block_type, day_mask, start_time, end_time, priority, config_json)
    SELECT block_id, channel_id, block_type, day_mask, start_time, end_time, priority, config_json
    FROM block;

    DROP TABLE block_show;
    DROP TABLE block;
    ALTER TABLE block_new RENAME TO block;

    -- Rebuild play_history: generalize from episode-only to episode + movie
    CREATE TABLE play_history_new (
        history_id  INTEGER PRIMARY KEY AUTOINCREMENT,
        item_type   TEXT    NOT NULL DEFAULT 'episode' CHECK(item_type IN ('episode','movie')),
        item_id     TEXT    NOT NULL,
        channel_id  TEXT    NOT NULL REFERENCES channel(channel_id),
        block_id    TEXT    REFERENCES block(block_id),
        aired_at    INTEGER NOT NULL
    );

    INSERT INTO play_history_new (history_id, item_type, item_id, channel_id, block_id, aired_at)
    SELECT history_id, 'episode', episode_id, channel_id, block_id, aired_at
    FROM play_history;

    DROP TABLE play_history;
    ALTER TABLE play_history_new RENAME TO play_history;

    -- Recreate indices lost in the table rebuilds
    CREATE INDEX IF NOT EXISTS idx_block_channel   ON block(channel_id, priority);
    CREATE INDEX IF NOT EXISTS idx_history_item    ON play_history(item_type, item_id, channel_id);
    CREATE INDEX IF NOT EXISTS idx_history_channel ON play_history(channel_id, aired_at DESC);

    -- Unified block content (replaces block_show)
    CREATE TABLE block_content (
        id           INTEGER PRIMARY KEY AUTOINCREMENT,
        block_id     TEXT    NOT NULL REFERENCES block(block_id) ON DELETE CASCADE,
        content_type TEXT    NOT NULL CHECK(content_type IN ('show','movie','playlist')),
        content_id   TEXT    NOT NULL,
        position     INTEGER NOT NULL DEFAULT 0,
        advancement  TEXT    NOT NULL DEFAULT 'sequential'
                              CHECK(advancement IN ('sequential','shuffle','rerun_shuffle')),
        cursor_scope TEXT    NOT NULL DEFAULT 'block'
                              CHECK(cursor_scope IN ('global','channel','block')),
        UNIQUE (block_id, content_type, content_id)
    );

    CREATE INDEX IF NOT EXISTS idx_block_content_block ON block_content(block_id, position);

    -- Media sources and user-selected libraries
    CREATE TABLE media_source (
        source_id    TEXT    PRIMARY KEY,
        source_type  TEXT    NOT NULL CHECK(source_type IN ('plex','jellyfin','emby','local')),
        display_name TEXT    NOT NULL,
        base_url     TEXT,
        enabled      INTEGER NOT NULL DEFAULT 1
    );

    CREATE TABLE media_library (
        library_id      TEXT    PRIMARY KEY,
        source_id       TEXT    NOT NULL REFERENCES media_source(source_id) ON DELETE CASCADE,
        external_lib_id TEXT    NOT NULL,
        display_name    TEXT    NOT NULL,
        library_type    TEXT    NOT NULL CHECK(library_type IN ('show','movie','mixed')),
        enabled         INTEGER NOT NULL DEFAULT 1,
        UNIQUE (source_id, external_lib_id)
    );

    CREATE INDEX IF NOT EXISTS idx_media_library_source ON media_library(source_id);

    -- Maps internal IDs to provider-specific IDs; one row per source the item appears in
    CREATE TABLE source_mapping (
        item_type   TEXT NOT NULL CHECK(item_type IN ('show','movie','episode')),
        kairos_id   TEXT NOT NULL,
        source_id   TEXT NOT NULL REFERENCES media_source(source_id),
        library_id  TEXT          REFERENCES media_library(library_id),
        external_id TEXT NOT NULL,
        PRIMARY KEY (item_type, kairos_id, source_id)
    );

    CREATE INDEX IF NOT EXISTS idx_source_mapping_lookup
        ON source_mapping(source_id, item_type, external_id);

    -- Standalone movies
    CREATE TABLE movie (
        movie_id       TEXT    PRIMARY KEY,
        title          TEXT    NOT NULL,
        content_rating TEXT,
        file_path      TEXT    NOT NULL,
        duration_ms    INTEGER NOT NULL,
        year           INTEGER
    );

    -- Playlists (Plex/Jellyfin synced or custom)
    CREATE TABLE playlist (
        playlist_id TEXT PRIMARY KEY,
        title       TEXT NOT NULL,
        source      TEXT NOT NULL DEFAULT 'custom'
                         CHECK(source IN ('plex','jellyfin','emby','custom'))
    );

    CREATE TABLE playlist_item (
        playlist_id TEXT    NOT NULL REFERENCES playlist(playlist_id) ON DELETE CASCADE,
        position    INTEGER NOT NULL,
        item_type   TEXT    NOT NULL CHECK(item_type IN ('episode','movie')),
        item_id     TEXT    NOT NULL,
        PRIMARY KEY (playlist_id, position)
    );

    -- Multi-level show/movie/playlist cursors (global / per-channel / per-block)
    CREATE TABLE media_cursor (
        content_type TEXT    NOT NULL CHECK(content_type IN ('show','movie','playlist')),
        content_id   TEXT    NOT NULL,
        cursor_scope TEXT    NOT NULL CHECK(cursor_scope IN ('global','channel','block')),
        scope_id     TEXT    NOT NULL DEFAULT '',
        episode_id   TEXT    REFERENCES episode(episode_id),
        movie_id     TEXT    REFERENCES movie(movie_id),
        position     INTEGER,
        updated_at   INTEGER NOT NULL,
        PRIMARY KEY (content_type, content_id, cursor_scope, scope_id)
    );

)SQL", true /* requires_fk_off */ }

,

// ── v3: rich metadata fields + locked flag for user overrides ────────────────
{ 3, R"SQL(

    ALTER TABLE show ADD COLUMN overview                TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN studio                  TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN status                  TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN genres                  TEXT    NOT NULL DEFAULT '[]';
    ALTER TABLE show ADD COLUMN thumb                   TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN art                     TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN imdb_id                 TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN tvdb_id                 TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN tmdb_id                 TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN originally_available_at TEXT    NOT NULL DEFAULT '';
    ALTER TABLE show ADD COLUMN year                    INTEGER;
    ALTER TABLE show ADD COLUMN audience_rating         REAL;
    ALTER TABLE show ADD COLUMN locked                  INTEGER NOT NULL DEFAULT 0;

    ALTER TABLE movie ADD COLUMN overview               TEXT    NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN tagline                TEXT    NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN studio                 TEXT    NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN director               TEXT    NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN genres                 TEXT    NOT NULL DEFAULT '[]';
    ALTER TABLE movie ADD COLUMN thumb                  TEXT    NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN art                    TEXT    NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN imdb_id                TEXT    NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN tmdb_id                TEXT    NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN audience_rating        REAL;
    ALTER TABLE movie ADD COLUMN locked                 INTEGER NOT NULL DEFAULT 0;

    ALTER TABLE episode ADD COLUMN overview             TEXT    NOT NULL DEFAULT '';
    ALTER TABLE episode ADD COLUMN air_date             TEXT    NOT NULL DEFAULT '';
    ALTER TABLE episode ADD COLUMN thumb                TEXT    NOT NULL DEFAULT '';

)SQL" }

}; // kMigrations

} // namespace

// ---------------------------------------------------------------------------

Database::Database(const std::string& path)
    : db_([&]() -> std::string {
          auto parent = std::filesystem::path(path).parent_path();
          if (!parent.empty())
              std::filesystem::create_directories(parent);
          return path;
      }(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
{
    configure();
    runMigrations();
    std::cout << "[db] opened " << path << '\n';
}

void Database::configure() {
    db_.exec("PRAGMA journal_mode = WAL");
    db_.exec("PRAGMA foreign_keys = ON");
    db_.exec("PRAGMA synchronous  = NORMAL");
    db_.exec("PRAGMA cache_size   = -8000");
}

void Database::runMigrations() {
    db_.exec(R"(
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version    INTEGER PRIMARY KEY,
            applied_at INTEGER NOT NULL
        )
    )");

    for (const auto& m : kMigrations) {
        {
            SQLite::Statement q(db_, "SELECT 1 FROM schema_migrations WHERE version = ?");
            q.bind(1, m.version);
            if (q.executeStep()) continue;
        }

        const auto recordMigration = [&] {
            SQLite::Statement ins(db_,
                "INSERT INTO schema_migrations (version, applied_at) VALUES (?, ?)");
            ins.bind(1, m.version);
            ins.bind(2, static_cast<int64_t>(std::time(nullptr)));
            ins.exec();
        };

        if (m.requires_fk_off) {
            // PRAGMA foreign_keys cannot be set inside a transaction, so we
            // manage BEGIN/COMMIT manually here to get both FK-off and atomicity.
            db_.exec("PRAGMA foreign_keys = OFF");
            db_.exec("BEGIN IMMEDIATE");
            try {
                db_.exec(m.sql);
                recordMigration();
                db_.exec("COMMIT");
            } catch (...) {
                db_.exec("ROLLBACK");
                db_.exec("PRAGMA foreign_keys = ON");
                throw;
            }
            db_.exec("PRAGMA foreign_keys = ON");
        } else {
            SQLite::Transaction txn(db_);
            db_.exec(m.sql);
            recordMigration();
            txn.commit();
        }

        std::cout << "[db] applied migration " << m.version << '\n';
    }
}
