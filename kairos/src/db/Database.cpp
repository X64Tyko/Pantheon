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

,

// ── v4: add program_count — block stops after N programs (0=no limit); end_time
//        remains as an optional hard time cutoff; either condition stops the block.
{ 4, R"SQL(
    ALTER TABLE block ADD COLUMN program_count INTEGER NOT NULL DEFAULT 0;
)SQL" }

,

// ── v5: advancement/cursor_scope move to block level; season_filter + expanded
//        content types on block_content; filler_list tables; playlist_item id.
{ 5, R"SQL(

    -- Advancement now lives on the block, not on individual content rows
    ALTER TABLE block ADD COLUMN advancement  TEXT NOT NULL DEFAULT 'sequential'
        CHECK(advancement IN ('sequential','shuffle','rerun_shuffle'));
    ALTER TABLE block ADD COLUMN cursor_scope TEXT NOT NULL DEFAULT 'block'
        CHECK(cursor_scope IN ('global','channel','block'));

    -- Rebuild block_content: drop advancement/cursor_scope, add season_filter,
    -- expand content_type to include 'episode' and 'filler_list'.
    CREATE TABLE block_content_v5 (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        block_id      TEXT    NOT NULL REFERENCES block(block_id) ON DELETE CASCADE,
        content_type  TEXT    NOT NULL
                      CHECK(content_type IN ('show','movie','episode','playlist','filler_list')),
        content_id    TEXT    NOT NULL,
        position      INTEGER NOT NULL DEFAULT 0,
        season_filter INTEGER,
        UNIQUE (block_id, content_type, content_id)
    );

    INSERT INTO block_content_v5 (id, block_id, content_type, content_id, position)
    SELECT id, block_id, content_type, content_id, position
    FROM block_content;

    DROP TABLE block_content;
    ALTER TABLE block_content_v5 RENAME TO block_content;

    CREATE INDEX IF NOT EXISTS idx_block_content_block ON block_content(block_id, position);

    -- Rebuild playlist_item to add auto-increment id for REST delete operations
    CREATE TABLE playlist_item_v5 (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        playlist_id TEXT    NOT NULL REFERENCES playlist(playlist_id) ON DELETE CASCADE,
        position    INTEGER NOT NULL,
        item_type   TEXT    NOT NULL CHECK(item_type IN ('episode','movie')),
        item_id     TEXT    NOT NULL,
        UNIQUE (playlist_id, position)
    );

    INSERT INTO playlist_item_v5 (playlist_id, position, item_type, item_id)
    SELECT playlist_id, position, item_type, item_id FROM playlist_item;

    DROP TABLE playlist_item;
    ALTER TABLE playlist_item_v5 RENAME TO playlist_item;

    CREATE INDEX IF NOT EXISTS idx_playlist_item ON playlist_item(playlist_id, position);

    -- Filler lists: reusable pools of short content for gap-filling
    CREATE TABLE filler_list (
        filler_list_id TEXT    PRIMARY KEY,
        title          TEXT    NOT NULL,
        advancement    TEXT    NOT NULL DEFAULT 'shuffle'
                               CHECK(advancement IN ('sequential','shuffle'))
    );

    CREATE TABLE filler_list_item (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        filler_list_id TEXT    NOT NULL REFERENCES filler_list(filler_list_id) ON DELETE CASCADE,
        item_type      TEXT    NOT NULL CHECK(item_type IN ('episode','movie')),
        item_id        TEXT    NOT NULL,
        position       INTEGER NOT NULL DEFAULT 0,
        UNIQUE (filler_list_id, item_type, item_id)
    );

    CREATE INDEX IF NOT EXISTS idx_filler_list_item ON filler_list_item(filler_list_id, position);

)SQL", true /* requires_fk_off */ }

,

// ── v6: late_start_mins — block can start up to N minutes past start_time if
//        preempted by higher-priority content that finishes within that window.
{ 6, R"SQL(
    ALTER TABLE block ADD COLUMN late_start_mins INTEGER NOT NULL DEFAULT 0;
)SQL" }

,

// ── v7: plex_list_link — tracks which playlists/filler-lists mirror a Plex
//        playlist or collection so they can be re-synced automatically.
{ 7, R"SQL(
    CREATE TABLE IF NOT EXISTS plex_list_link (
        list_type      TEXT    NOT NULL CHECK(list_type IN ('playlist','filler_list')),
        list_id        TEXT    NOT NULL,
        source_id      TEXT    NOT NULL REFERENCES media_source(source_id) ON DELETE CASCADE,
        external_id    TEXT    NOT NULL,
        plex_type      TEXT    NOT NULL CHECK(plex_type IN ('playlist','collection')),
        last_synced_at INTEGER,
        PRIMARY KEY (list_type, list_id)
    );
)SQL" }

,

// ── v8: block filler assignments, channel filler defaults, and new block
//        scheduling fields (align_to_mins, inter_filler, early_start_secs,
//        filler_selection).
{ 8, R"SQL(
    ALTER TABLE block ADD COLUMN align_to_mins    INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE block ADD COLUMN inter_filler     INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE block ADD COLUMN early_start_secs INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE block ADD COLUMN filler_selection TEXT    NOT NULL DEFAULT 'round_robin';

    ALTER TABLE channel ADD COLUMN default_filler_selection TEXT NOT NULL DEFAULT 'round_robin';

    CREATE TABLE block_filler_entry (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        block_id       TEXT    NOT NULL REFERENCES block(block_id) ON DELETE CASCADE,
        filler_list_id TEXT    NOT NULL REFERENCES filler_list(filler_list_id) ON DELETE CASCADE,
        advancement    TEXT    NOT NULL DEFAULT 'sequential'
                               CHECK(advancement IN ('sequential','shuffle','sized')),
        weight         INTEGER NOT NULL DEFAULT 1,
        position       INTEGER NOT NULL DEFAULT 0,
        UNIQUE (block_id, filler_list_id)
    );

    CREATE INDEX IF NOT EXISTS idx_block_filler_entry ON block_filler_entry(block_id, position);

    CREATE TABLE channel_filler_entry (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        channel_id     TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        filler_list_id TEXT    NOT NULL REFERENCES filler_list(filler_list_id) ON DELETE CASCADE,
        advancement    TEXT    NOT NULL DEFAULT 'sequential'
                               CHECK(advancement IN ('sequential','shuffle','sized')),
        weight         INTEGER NOT NULL DEFAULT 1,
        position       INTEGER NOT NULL DEFAULT 0,
        UNIQUE (channel_id, filler_list_id)
    );

    CREATE INDEX IF NOT EXISTS idx_channel_filler_entry ON channel_filler_entry(channel_id, position);
)SQL" }

,

// ── v9: block_state — tracks round-robin content position per (block, channel).
//        Needed by the RuleEngine to cycle through block_content slots without
//        depending on media_cursor (which tracks per-show episode position).
{ 9, R"SQL(
    CREATE TABLE IF NOT EXISTS block_state (
        block_id         TEXT    NOT NULL REFERENCES block(block_id) ON DELETE CASCADE,
        channel_id       TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        content_position INTEGER NOT NULL DEFAULT 0,
        updated_at       INTEGER NOT NULL,
        PRIMARY KEY (block_id, channel_id)
    );
)SQL" }

,

// ── v10: scheduled_program — persisted materialized schedule.
//         Each row stores the SimState cursor snapshot after that entry was
//         scheduled, so the EPGMaterializer can extend the schedule forward
//         from exactly where it left off without re-deriving state from play
//         history. status tracks actual playback: scheduled → aired | skipped.
{ 10, R"SQL(
    CREATE TABLE IF NOT EXISTS scheduled_program (
        id               INTEGER PRIMARY KEY AUTOINCREMENT,
        channel_id       TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        block_id         TEXT    REFERENCES block(block_id) ON DELETE SET NULL,
        item_type        TEXT    NOT NULL CHECK(item_type IN ('episode','movie')),
        item_id          TEXT    NOT NULL,
        wall_clock_start INTEGER NOT NULL,
        wall_clock_end   INTEGER NOT NULL,
        status           TEXT    NOT NULL DEFAULT 'scheduled'
                         CHECK(status IN ('scheduled','aired','skipped')),
        cursor_json      TEXT    NOT NULL DEFAULT '{}',
        created_at       INTEGER NOT NULL,
        UNIQUE(channel_id, wall_clock_start)
    );
    CREATE INDEX IF NOT EXISTS idx_sched_channel_time
        ON scheduled_program(channel_id, wall_clock_start);
    CREATE INDEX IF NOT EXISTS idx_sched_channel_end
        ON scheduled_program(channel_id, wall_clock_end DESC);
)SQL" }

,

// ── v11: per-channel seed for deterministic EPG preview projection.
{ 11, R"SQL(
    ALTER TABLE channel ADD COLUMN seed INTEGER NOT NULL DEFAULT 12345;
)SQL" }

,

// ── v12: add ON DELETE CASCADE to block.channel_id.
//         v2 rebuilt block without CASCADE, so deleting a channel left orphaned
//         blocks behind (FK violation). Full table rebuild required — SQLite
//         does not support ALTER FOREIGN KEY.
{ 12, R"SQL(
    CREATE TABLE block_v12 (
        block_id           TEXT    PRIMARY KEY,
        channel_id         TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        block_type         TEXT    NOT NULL CHECK(block_type IN ('episode','premier','filler','movie')),
        day_mask           INTEGER NOT NULL DEFAULT 127,
        start_time         TEXT    NOT NULL DEFAULT '00:00',
        end_time           TEXT,
        priority           INTEGER NOT NULL DEFAULT 0,
        max_content_rating TEXT,
        config_json        TEXT    NOT NULL DEFAULT '{}',
        program_count      INTEGER NOT NULL DEFAULT 0,
        advancement        TEXT    NOT NULL DEFAULT 'sequential'
                           CHECK(advancement IN ('sequential','shuffle','rerun_shuffle')),
        cursor_scope       TEXT    NOT NULL DEFAULT 'block'
                           CHECK(cursor_scope IN ('global','channel','block')),
        late_start_mins    INTEGER NOT NULL DEFAULT 0,
        align_to_mins      INTEGER NOT NULL DEFAULT 0,
        inter_filler       INTEGER NOT NULL DEFAULT 0,
        early_start_secs   INTEGER NOT NULL DEFAULT 0,
        filler_selection   TEXT    NOT NULL DEFAULT 'round_robin'
    );

    INSERT INTO block_v12
        SELECT block_id, channel_id, block_type, day_mask, start_time, end_time,
               priority, max_content_rating, config_json, program_count,
               advancement, cursor_scope, late_start_mins, align_to_mins,
               inter_filler, early_start_secs, filler_selection
        FROM block;

    DROP TABLE block;
    ALTER TABLE block_v12 RENAME TO block;

    CREATE INDEX IF NOT EXISTS idx_block_channel ON block(channel_id, priority);
)SQL", true /* requires_fk_off */ }

,

// ── v13: five advancement modes; weight + run_count on block_content;
//         runs_remaining on block_state; fix media_cursor CHECK to include
//         show_rerun and filler_list; smart_pct on block;
//         episode_group / episode_group_member for multipart markup.
{ 13, R"SQL(

    -- Rebuild block: expand advancement CHECK, add smart_pct.
    CREATE TABLE block_v13 (
        block_id           TEXT    PRIMARY KEY,
        channel_id         TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        block_type         TEXT    NOT NULL CHECK(block_type IN ('episode','premier','filler','movie')),
        day_mask           INTEGER NOT NULL DEFAULT 127,
        start_time         TEXT    NOT NULL DEFAULT '00:00',
        end_time           TEXT,
        priority           INTEGER NOT NULL DEFAULT 0,
        max_content_rating TEXT,
        config_json        TEXT    NOT NULL DEFAULT '{}',
        program_count      INTEGER NOT NULL DEFAULT 0,
        advancement        TEXT    NOT NULL DEFAULT 'sequential'
                           CHECK(advancement IN (
                               'sequential','shuffle','smart_shuffle',
                               'rerun_shuffle','rerun_smart')),
        cursor_scope       TEXT    NOT NULL DEFAULT 'block'
                           CHECK(cursor_scope IN ('global','channel','block')),
        late_start_mins    INTEGER NOT NULL DEFAULT 0,
        align_to_mins      INTEGER NOT NULL DEFAULT 0,
        inter_filler       INTEGER NOT NULL DEFAULT 0,
        early_start_secs   INTEGER NOT NULL DEFAULT 0,
        filler_selection   TEXT    NOT NULL DEFAULT 'round_robin',
        smart_pct          INTEGER NOT NULL DEFAULT 30
    );

    INSERT INTO block_v13
        SELECT block_id, channel_id, block_type, day_mask, start_time, end_time,
               priority, max_content_rating, config_json, program_count,
               advancement, cursor_scope, late_start_mins, align_to_mins,
               inter_filler, early_start_secs, filler_selection, 30
        FROM block;

    DROP TABLE block;
    ALTER TABLE block_v13 RENAME TO block;

    CREATE INDEX IF NOT EXISTS idx_block_channel ON block(channel_id, priority);

    -- weight: weighted random show selection (rerun modes).
    -- run_count: episodes to play sequentially per selection (rerun modes).
    ALTER TABLE block_content ADD COLUMN weight    INTEGER NOT NULL DEFAULT 1;
    ALTER TABLE block_content ADD COLUMN run_count INTEGER NOT NULL DEFAULT 1;

    -- runs_remaining: plays left in the current show's run (rerun modes).
    ALTER TABLE block_state ADD COLUMN runs_remaining INTEGER NOT NULL DEFAULT 0;

    -- Rebuild media_cursor: expand content_type CHECK to include show_rerun and filler_list.
    CREATE TABLE media_cursor_v13 (
        content_type TEXT    NOT NULL
                     CHECK(content_type IN ('show','show_rerun','movie','playlist','filler_list')),
        content_id   TEXT    NOT NULL,
        cursor_scope TEXT    NOT NULL CHECK(cursor_scope IN ('global','channel','block')),
        scope_id     TEXT    NOT NULL DEFAULT '',
        episode_id   TEXT    REFERENCES episode(episode_id),
        movie_id     TEXT    REFERENCES movie(movie_id),
        position     INTEGER NOT NULL DEFAULT 0,
        updated_at   INTEGER NOT NULL,
        PRIMARY KEY (content_type, content_id, cursor_scope, scope_id)
    );

    INSERT INTO media_cursor_v13
        SELECT content_type, content_id, cursor_scope, scope_id,
               episode_id, movie_id, COALESCE(position, 0), updated_at
        FROM media_cursor
        WHERE content_type IN ('show','movie','playlist');

    DROP TABLE media_cursor;
    ALTER TABLE media_cursor_v13 RENAME TO media_cursor;

    -- Episode groups: link multipart episodes so the scheduler keeps them together.
    CREATE TABLE episode_group (
        group_id   TEXT PRIMARY KEY,
        show_id    TEXT NOT NULL REFERENCES show(show_id) ON DELETE CASCADE,
        name       TEXT NOT NULL,
        group_type TEXT NOT NULL DEFAULT 'multipart'
                   CHECK(group_type IN ('multipart'))
    );

    CREATE TABLE episode_group_member (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        group_id   TEXT    NOT NULL REFERENCES episode_group(group_id) ON DELETE CASCADE,
        episode_id TEXT    NOT NULL REFERENCES episode(episode_id) ON DELETE CASCADE,
        part_num   INTEGER NOT NULL DEFAULT 1,
        UNIQUE (group_id, episode_id),
        UNIQUE (group_id, part_num)
    );

    CREATE INDEX IF NOT EXISTS idx_episode_group_show ON episode_group(show_id);
    CREATE INDEX IF NOT EXISTS idx_episode_group_member_ep ON episode_group_member(episode_id);

)SQL", true /* requires_fk_off */ }

,
// ── v14: start_scope — whether align/early/late apply to the block or each episode ──
{ 14, R"SQL(
    ALTER TABLE block ADD COLUMN start_scope TEXT NOT NULL DEFAULT 'block';
)SQL" }

,

// ── v15: no_history_behavior — per-block policy for shows in a rerun block that
//         have no play history on this channel yet.
{ 15, R"SQL(
    ALTER TABLE block ADD COLUMN no_history_behavior TEXT NOT NULL DEFAULT 'normal'
        CHECK(no_history_behavior IN ('normal','fallback_all','exclude','filler','skip'));
)SQL" }

,

// ── v16: max_consecutive_episodes on block; consecutive_count on block_state.
//         Controls how many episodes from the same show can play in a row before
//         a random restart is forced (0 = unlimited). consecutive_count tracks the
//         running tally so show-switches always reset to a fresh random start.
{ 16, R"SQL(
    ALTER TABLE block ADD COLUMN max_consecutive_episodes INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE block_state ADD COLUMN consecutive_count INTEGER NOT NULL DEFAULT 0;
)SQL" }

,

// ── v17: is_scheduled on play_history — distinguishes EPG-generated entries
//         (is_scheduled=1, written at schedule time) from Tunarr-confirmed plays
//         (is_scheduled=0, written by markPlayed). Scheduled entries are purged
//         when clearScheduleCache regenerates a channel's EPG.
{ 17, R"SQL(
    ALTER TABLE play_history ADD COLUMN is_scheduled INTEGER NOT NULL DEFAULT 0;
)SQL" }

,

// ── v18: per-channel advance mode. 'scheduled' (default) means the EPG
//         projection drives cursor advancement; 'on_play' means cursors only
//         move on confirmed playback, suitable for pause-while-not-streaming.
{ 18, R"SQL(
    ALTER TABLE channel ADD COLUMN advance_mode TEXT NOT NULL DEFAULT 'scheduled';
)SQL" }

,

// ── v19: 'filler' was a NoHistoryBehavior that was never implemented — it
//         collapsed to Skip. Convert any stored values to 'skip'.
{ 19, R"SQL(
    UPDATE block SET no_history_behavior = 'skip' WHERE no_history_behavior = 'filler';
)SQL" }

,

// ── v20: default filler advancement changes from 'sequential' to 'sized'
//         (best-fit duration selection). Update existing entries that are
//         still using the old default — users who explicitly chose 'sequential'
//         will see their entries updated, but 'sized' is the correct default
//         for dead-air gap filling.
{ 20, R"SQL(
    UPDATE block_filler_entry   SET advancement = 'sized' WHERE advancement = 'sequential';
    UPDATE channel_filler_entry SET advancement = 'sized' WHERE advancement = 'sequential';
)SQL" }

,

// ── v21: expand scheduled_program item_type to allow 'filler' so merged
//         filler blocks are persisted alongside content items.
//         Old cached rows are intentionally dropped — they lack filler entries
//         and would show gaps in the preview until regenerated.
{ 21, R"SQL(
    CREATE TABLE scheduled_program_new (
        id               INTEGER PRIMARY KEY AUTOINCREMENT,
        channel_id       TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        block_id         TEXT    REFERENCES block(block_id) ON DELETE SET NULL,
        item_type        TEXT    NOT NULL CHECK(item_type IN ('episode','movie','filler')),
        item_id          TEXT    NOT NULL,
        wall_clock_start INTEGER NOT NULL,
        wall_clock_end   INTEGER NOT NULL,
        status           TEXT    NOT NULL DEFAULT 'scheduled'
                         CHECK(status IN ('scheduled','aired','skipped')),
        cursor_json      TEXT    NOT NULL DEFAULT '{}',
        created_at       INTEGER NOT NULL,
        UNIQUE(channel_id, wall_clock_start)
    );
    DROP TABLE scheduled_program;
    ALTER TABLE scheduled_program_new RENAME TO scheduled_program;
    CREATE INDEX IF NOT EXISTS idx_sched_channel_time
        ON scheduled_program(channel_id, wall_clock_start);
    CREATE INDEX IF NOT EXISTS idx_sched_channel_end
        ON scheduled_program(channel_id, wall_clock_end DESC);
)SQL", true /* requires_fk_off */ }

,

// ── v22: fix missing ON DELETE actions on play_history FKs.
//         v2 created play_history without CASCADE/SET NULL, so any channel or
//         block with history entries was undeletable (SQLite defaults to RESTRICT).
//         channel_id → CASCADE, block_id → SET NULL (mirrors scheduled_program).
{ 22, R"SQL(
    CREATE TABLE play_history_v22 (
        history_id   INTEGER PRIMARY KEY AUTOINCREMENT,
        item_type    TEXT    NOT NULL DEFAULT 'episode' CHECK(item_type IN ('episode','movie')),
        item_id      TEXT    NOT NULL,
        channel_id   TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        block_id     TEXT             REFERENCES block(block_id)     ON DELETE SET NULL,
        aired_at     INTEGER NOT NULL,
        is_scheduled INTEGER NOT NULL DEFAULT 0
    );

    INSERT INTO play_history_v22
        SELECT history_id, item_type, item_id, channel_id, block_id, aired_at, is_scheduled
        FROM play_history;

    DROP TABLE play_history;
    ALTER TABLE play_history_v22 RENAME TO play_history;

    CREATE INDEX IF NOT EXISTS idx_history_item
        ON play_history(item_type, item_id, channel_id);
    CREATE INDEX IF NOT EXISTS idx_history_channel
        ON play_history(channel_id, aired_at DESC);
)SQL", true /* requires_fk_off */ },

{ 23, R"SQL(
    ALTER TABLE playlist ADD COLUMN mode TEXT NOT NULL DEFAULT 'sequential';
)SQL", false }

,

{ 24, R"SQL(
    ALTER TABLE block ADD COLUMN name TEXT NOT NULL DEFAULT '';
)SQL", false },

{ 25, R"SQL(
    ALTER TABLE episode      ADD COLUMN absolute_index   INTEGER;
    ALTER TABLE block_content ADD COLUMN include_specials INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE block_content ADD COLUMN episode_order   TEXT    NOT NULL DEFAULT 'season';
)SQL", false }

,

// ── v26: per-channel offline fallback. When no content and no filler are
//         available, Kairos can serve a looping video or a static image
//         (optionally paired with an audio track from the library).
{ 26, R"SQL(
    ALTER TABLE channel ADD COLUMN offline_video_path  TEXT NOT NULL DEFAULT '';
    ALTER TABLE channel ADD COLUMN offline_image_path  TEXT NOT NULL DEFAULT '';
    ALTER TABLE channel ADD COLUMN offline_audio_id    TEXT NOT NULL DEFAULT '';
    ALTER TABLE channel ADD COLUMN offline_audio_type  TEXT NOT NULL DEFAULT '';
    ALTER TABLE channel ADD COLUMN offline_audio_title TEXT NOT NULL DEFAULT '';
    ALTER TABLE channel ADD COLUMN logo_path           TEXT NOT NULL DEFAULT '';
)SQL", false }

,

// ── v27: block intro/outro/interstitial content slots.
//         intro  : plays once when the block starts (before first content item).
//         outro  : plays once when program_count is hit (after last content item).
//         interstitial: plays between show transitions at a configurable frequency.
//         interstitial_every_n: fire interstitial after every N show transitions (0=disabled).
{ 27, R"SQL(
    ALTER TABLE block ADD COLUMN intro_content_type        TEXT NOT NULL DEFAULT '';
    ALTER TABLE block ADD COLUMN intro_content_id          TEXT NOT NULL DEFAULT '';
    ALTER TABLE block ADD COLUMN outro_content_type        TEXT NOT NULL DEFAULT '';
    ALTER TABLE block ADD COLUMN outro_content_id          TEXT NOT NULL DEFAULT '';
    ALTER TABLE block ADD COLUMN interstitial_content_type TEXT NOT NULL DEFAULT '';
    ALTER TABLE block ADD COLUMN interstitial_content_id   TEXT NOT NULL DEFAULT '';
    ALTER TABLE block ADD COLUMN interstitial_every_n      INTEGER NOT NULL DEFAULT 1;
)SQL" }

,

// ── v28: channel_bumper — channel-wide content injected between programs.
//         mode='between': fire after every N content programs (every_n).
//         mode='filler' : weave bumper content into filler gap filling.
{ 28, R"SQL(
    CREATE TABLE IF NOT EXISTS channel_bumper (
        id           INTEGER PRIMARY KEY AUTOINCREMENT,
        channel_id   TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        content_type TEXT    NOT NULL CHECK(content_type IN ('show','episode','playlist')),
        content_id   TEXT    NOT NULL,
        mode         TEXT    NOT NULL DEFAULT 'between'
                             CHECK(mode IN ('between','filler')),
        every_n      INTEGER NOT NULL DEFAULT 3,
        position     INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX IF NOT EXISTS idx_channel_bumper ON channel_bumper(channel_id, position);
)SQL" }

,

// ── v29: add is_filler column to scheduled_program. Inter-filler clips are now
//         stored individually with their real item_type/item_id and is_filler=1
//         instead of a merged 'filler' sentinel with an empty file_path.
//         Cache is cleared so it regenerates with the new layout.
{ 29, R"SQL(
    ALTER TABLE scheduled_program ADD COLUMN is_filler INTEGER NOT NULL DEFAULT 0;
    DELETE FROM scheduled_program;
    DELETE FROM play_history WHERE is_scheduled = 1;
)SQL", false }

,

// ── v30: anchor_hashes on channel — stores the mutated seed at each weekly
//         anchor (Monday midnight UTC). Value = initial_seed + cumulative items
//         scheduled through that anchor, encoded as JSON {ts_str: int}.
//         Used by the EPG preview to detect meaningful schedule changes.
{ 30, R"SQL(
    ALTER TABLE channel ADD COLUMN anchor_hashes TEXT;
)SQL", false },

// ── v31: anchor_hashes format change — now stores full RNG + cursor snapshot
//         per week: {ts_str: {rng: "s0 s1 s2 s3", cursors: [...], block_states: [...]}}.
//         Old cumulative-int format is incompatible; clear to force fresh generation.
{ 31, R"SQL(
    UPDATE channel SET anchor_hashes = NULL WHERE anchor_hashes IS NOT NULL;
)SQL", false },

// ── v32: snap_to_group_start on block — controls whether rerun mode snaps a
//         random mid-group pick back to Part 1. Defaults to true (1).
//         Premier blocks are unaffected (they never enter snap code paths).
{ 32, R"SQL(
    ALTER TABLE block ADD COLUMN snap_to_group_start INTEGER NOT NULL DEFAULT 1;
)SQL", false },

// ── v33: replace filler_list_id with content_type + content_id in filler
//         entry tables, enabling shows, movies, and playlists as filler.
{ 33, R"SQL(
    ALTER TABLE block_filler_entry    ADD COLUMN content_type TEXT NOT NULL DEFAULT 'filler_list';
    ALTER TABLE block_filler_entry    ADD COLUMN content_id   TEXT;
    UPDATE block_filler_entry    SET content_id = filler_list_id;
    ALTER TABLE channel_filler_entry  ADD COLUMN content_type TEXT NOT NULL DEFAULT 'filler_list';
    ALTER TABLE channel_filler_entry  ADD COLUMN content_id   TEXT;
    UPDATE channel_filler_entry  SET content_id = filler_list_id;
)SQL", false },

// ── v34: add extended metadata columns for labels, network, actors, countries, collections
{ 34, R"SQL(
    ALTER TABLE show  ADD COLUMN labels      TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE show  ADD COLUMN network     TEXT NOT NULL DEFAULT '';
    ALTER TABLE show  ADD COLUMN actors      TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE show  ADD COLUMN countries   TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE show  ADD COLUMN collections TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE movie ADD COLUMN labels      TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE movie ADD COLUMN actors      TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE movie ADD COLUMN countries   TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE movie ADD COLUMN collections TEXT NOT NULL DEFAULT '[]';
)SQL", false },

// ── v35: drop legacy NOT NULL filler_list_id from channel_filler_entry so
//         shows, movies, and playlists can be added as channel default filler.
//         Also normalise the uniqueness constraint to (channel_id, content_type, content_id).
{ 35, R"SQL(
    CREATE TABLE channel_filler_entry_new (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        channel_id     TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        filler_list_id TEXT    REFERENCES filler_list(filler_list_id) ON DELETE CASCADE,
        advancement    TEXT    NOT NULL DEFAULT 'sequential'
                               CHECK(advancement IN ('sequential','shuffle','sized')),
        weight         INTEGER NOT NULL DEFAULT 1,
        position       INTEGER NOT NULL DEFAULT 0,
        content_type   TEXT    NOT NULL DEFAULT 'filler_list',
        content_id     TEXT,
        UNIQUE (channel_id, content_type, content_id)
    );
    INSERT INTO channel_filler_entry_new
        SELECT id, channel_id, filler_list_id, advancement, weight, position, content_type, content_id
        FROM channel_filler_entry;
    DROP TABLE channel_filler_entry;
    ALTER TABLE channel_filler_entry_new RENAME TO channel_filler_entry;
    CREATE INDEX IF NOT EXISTS idx_channel_filler_entry ON channel_filler_entry(channel_id, position);
)SQL", false },

// ── v36: drop legacy NOT NULL filler_list_id from block_filler_entry so
//         shows, movies, and playlists can be added as block-level filler.
//         Normalises the uniqueness constraint to (block_id, content_type, content_id).
{ 36, R"SQL(
    CREATE TABLE block_filler_entry_new (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        block_id       TEXT    NOT NULL REFERENCES block(block_id) ON DELETE CASCADE,
        filler_list_id TEXT,
        advancement    TEXT    NOT NULL DEFAULT 'sequential'
                               CHECK(advancement IN ('sequential','shuffle','sized')),
        weight         INTEGER NOT NULL DEFAULT 1,
        position       INTEGER NOT NULL DEFAULT 0,
        content_type   TEXT    NOT NULL DEFAULT 'filler_list',
        content_id     TEXT,
        UNIQUE (block_id, content_type, content_id)
    );
    INSERT INTO block_filler_entry_new
        SELECT id, block_id, filler_list_id, advancement, weight, position, content_type, content_id
        FROM block_filler_entry;
    DROP TABLE block_filler_entry;
    ALTER TABLE block_filler_entry_new RENAME TO block_filler_entry;
    CREATE INDEX IF NOT EXISTS idx_block_filler_entry ON block_filler_entry(block_id, position);
)SQL", false },

// ── v37: add season_filter to filler entries and bumpers so specific seasons
//         can be targeted (NULL = all seasons, N = season N only).
{ 37, R"SQL(
    ALTER TABLE channel_filler_entry ADD COLUMN season_filter INTEGER;
    ALTER TABLE block_filler_entry   ADD COLUMN season_filter INTEGER;
    ALTER TABLE channel_bumper       ADD COLUMN season_filter INTEGER;
)SQL", false }

,

// ── v38: app_config key-value table for service integrations (Sonarr, Radarr).
{ 38, R"SQL(
    CREATE TABLE app_config (
        key   TEXT PRIMARY KEY,
        value TEXT NOT NULL DEFAULT ''
    );
)SQL", false }

,

// ── v39: named seasons — stores the season title from the media server
//         (e.g. "Specials", "Season 1", or custom Plex names).
{ 39, R"SQL(
    CREATE TABLE show_season (
        show_id     TEXT    NOT NULL REFERENCES show(show_id) ON DELETE CASCADE,
        season      INTEGER NOT NULL,
        season_name TEXT    NOT NULL DEFAULT '',
        PRIMARY KEY (show_id, season)
    );
)SQL", false }

,

// ── v40: user accounts and sessions for API authentication.
{ 40, R"SQL(
    CREATE TABLE user (
        user_id       TEXT    PRIMARY KEY,
        username      TEXT    NOT NULL UNIQUE,
        password_hash TEXT    NOT NULL,
        role          TEXT    NOT NULL DEFAULT 'viewer'
                              CHECK(role IN ('admin','viewer')),
        created_at    INTEGER NOT NULL
    );

    CREATE TABLE session (
        token         TEXT    PRIMARY KEY,
        user_id       TEXT    NOT NULL REFERENCES user(user_id) ON DELETE CASCADE,
        created_at    INTEGER NOT NULL,
        expires_at    INTEGER NOT NULL,
        last_seen     INTEGER NOT NULL
    );

    CREATE INDEX idx_session_user ON session(user_id);
)SQL", false }

,

// ── v41: chapter table for per-media segment metadata (intros, ad breaks,
//         credits, chapter points). source tracks where data came from;
//         locked=1 means a manual edit that survives re-syncs.
{ 41, R"SQL(
    CREATE TABLE chapter (
        chapter_id   TEXT    PRIMARY KEY,
        media_type   TEXT    NOT NULL,
        media_id     TEXT    NOT NULL,
        chapter_type TEXT    NOT NULL DEFAULT 'unclassified',
        title        TEXT    NOT NULL DEFAULT '',
        start_ms     INTEGER NOT NULL,
        end_ms       INTEGER NOT NULL,
        position     INTEGER NOT NULL,
        source       TEXT    NOT NULL DEFAULT 'manual',
        locked       INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX idx_chapter_media ON chapter(media_type, media_id, position);
)SQL", false }

,

// ── v42: Block model refactor.
//   • Drop max_content_rating (unused) and config_json (legacy).
//   • Add play_style (standard|rerun) — splits the old compound advancement values.
//   • Remap advancement: smart_shuffle→smart, rerun_shuffle→shuffle, rerun_smart→smart.
//   • Collapse block_type 'premier' → 'episode' (Premier had no remaining unique behaviour).
{ 42, R"SQL(
    CREATE TABLE block_v42 (
        block_id                  TEXT    PRIMARY KEY,
        channel_id                TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        block_type                TEXT    NOT NULL DEFAULT 'episode'
                                  CHECK(block_type IN ('episode','filler','movie')),
        day_mask                  INTEGER NOT NULL DEFAULT 127,
        start_time                TEXT    NOT NULL DEFAULT '00:00',
        end_time                  TEXT,
        priority                  INTEGER NOT NULL DEFAULT 0,
        program_count             INTEGER NOT NULL DEFAULT 0,
        play_style                TEXT    NOT NULL DEFAULT 'standard'
                                  CHECK(play_style IN ('standard','rerun')),
        advancement               TEXT    NOT NULL DEFAULT 'sequential'
                                  CHECK(advancement IN ('sequential','shuffle','smart')),
        cursor_scope              TEXT    NOT NULL DEFAULT 'block'
                                  CHECK(cursor_scope IN ('global','channel','block')),
        late_start_mins           INTEGER NOT NULL DEFAULT 0,
        align_to_mins             INTEGER NOT NULL DEFAULT 0,
        inter_filler              INTEGER NOT NULL DEFAULT 0,
        early_start_secs          INTEGER NOT NULL DEFAULT 0,
        filler_selection          TEXT    NOT NULL DEFAULT 'round_robin',
        smart_pct                 INTEGER NOT NULL DEFAULT 30,
        start_scope               TEXT    NOT NULL DEFAULT 'block',
        no_history_behavior       TEXT    NOT NULL DEFAULT 'normal'
                                  CHECK(no_history_behavior IN ('normal','fallback_all','exclude','filler','skip')),
        max_consecutive_episodes  INTEGER NOT NULL DEFAULT 0,
        name                      TEXT    NOT NULL DEFAULT '',
        intro_content_type        TEXT    NOT NULL DEFAULT '',
        intro_content_id          TEXT    NOT NULL DEFAULT '',
        outro_content_type        TEXT    NOT NULL DEFAULT '',
        outro_content_id          TEXT    NOT NULL DEFAULT '',
        interstitial_content_type TEXT    NOT NULL DEFAULT '',
        interstitial_content_id   TEXT    NOT NULL DEFAULT '',
        interstitial_every_n      INTEGER NOT NULL DEFAULT 1,
        snap_to_group_start       INTEGER NOT NULL DEFAULT 1
    );

    INSERT INTO block_v42 (
        block_id, channel_id,
        block_type,
        day_mask, start_time, end_time, priority, program_count,
        play_style,
        advancement,
        cursor_scope, late_start_mins, align_to_mins, inter_filler,
        early_start_secs, filler_selection, smart_pct, start_scope,
        no_history_behavior, max_consecutive_episodes, name,
        intro_content_type, intro_content_id,
        outro_content_type, outro_content_id,
        interstitial_content_type, interstitial_content_id, interstitial_every_n,
        snap_to_group_start
    )
    SELECT
        block_id, channel_id,
        CASE WHEN block_type = 'premier' THEN 'episode' ELSE block_type END,
        day_mask, start_time, end_time, priority, program_count,
        CASE WHEN advancement IN ('rerun_shuffle','rerun_smart') THEN 'rerun' ELSE 'standard' END,
        CASE
            WHEN advancement = 'smart_shuffle' THEN 'smart'
            WHEN advancement = 'rerun_shuffle' THEN 'shuffle'
            WHEN advancement = 'rerun_smart'   THEN 'smart'
            ELSE advancement
        END,
        cursor_scope, late_start_mins, align_to_mins, inter_filler,
        early_start_secs, filler_selection, smart_pct, start_scope,
        no_history_behavior, max_consecutive_episodes, name,
        intro_content_type, intro_content_id,
        outro_content_type, outro_content_id,
        interstitial_content_type, interstitial_content_id, interstitial_every_n,
        snap_to_group_start
    FROM block;

    DROP TABLE block;
    ALTER TABLE block_v42 RENAME TO block;
    CREATE INDEX idx_block_channel ON block(channel_id, priority);
)SQL", true }

,

// ── v43: Timeslot block type.
//   • Extend block_type CHECK to allow 'timeslot'.
//   • Add timeslot_slot and timeslot_slot_queue tables.
{ 43, R"SQL(
    CREATE TABLE block_v43 (
        block_id                  TEXT    PRIMARY KEY,
        channel_id                TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        block_type                TEXT    NOT NULL DEFAULT 'episode'
                                  CHECK(block_type IN ('episode','filler','movie','timeslot')),
        day_mask                  INTEGER NOT NULL DEFAULT 127,
        start_time                TEXT    NOT NULL DEFAULT '00:00',
        end_time                  TEXT,
        priority                  INTEGER NOT NULL DEFAULT 0,
        program_count             INTEGER NOT NULL DEFAULT 0,
        play_style                TEXT    NOT NULL DEFAULT 'standard'
                                  CHECK(play_style IN ('standard','rerun')),
        advancement               TEXT    NOT NULL DEFAULT 'sequential'
                                  CHECK(advancement IN ('sequential','shuffle','smart')),
        cursor_scope              TEXT    NOT NULL DEFAULT 'block'
                                  CHECK(cursor_scope IN ('global','channel','block')),
        late_start_mins           INTEGER NOT NULL DEFAULT 0,
        align_to_mins             INTEGER NOT NULL DEFAULT 0,
        inter_filler              INTEGER NOT NULL DEFAULT 0,
        early_start_secs          INTEGER NOT NULL DEFAULT 0,
        filler_selection          TEXT    NOT NULL DEFAULT 'round_robin',
        smart_pct                 INTEGER NOT NULL DEFAULT 30,
        start_scope               TEXT    NOT NULL DEFAULT 'block',
        no_history_behavior       TEXT    NOT NULL DEFAULT 'normal'
                                  CHECK(no_history_behavior IN ('normal','fallback_all','exclude','filler','skip')),
        max_consecutive_episodes  INTEGER NOT NULL DEFAULT 0,
        name                      TEXT    NOT NULL DEFAULT '',
        intro_content_type        TEXT    NOT NULL DEFAULT '',
        intro_content_id          TEXT    NOT NULL DEFAULT '',
        outro_content_type        TEXT    NOT NULL DEFAULT '',
        outro_content_id          TEXT    NOT NULL DEFAULT '',
        interstitial_content_type TEXT    NOT NULL DEFAULT '',
        interstitial_content_id   TEXT    NOT NULL DEFAULT '',
        interstitial_every_n      INTEGER NOT NULL DEFAULT 1,
        snap_to_group_start       INTEGER NOT NULL DEFAULT 1
    );

    INSERT INTO block_v43 SELECT * FROM block;
    DROP TABLE block;
    ALTER TABLE block_v43 RENAME TO block;
    CREATE INDEX idx_block_channel ON block(channel_id, priority);

    CREATE TABLE timeslot_slot (
        slot_id            TEXT    PRIMARY KEY,
        block_id           TEXT    NOT NULL REFERENCES block(block_id) ON DELETE CASCADE,
        slot_index         INTEGER NOT NULL,
        slot_offset_mins   INTEGER NOT NULL DEFAULT 0,
        slot_duration_mins INTEGER NOT NULL DEFAULT 60,
        overflow           TEXT    NOT NULL DEFAULT 'cutoff' CHECK(overflow IN ('cutoff','finish')),
        late_start_mins    INTEGER NOT NULL DEFAULT 5,
        early_start_secs   INTEGER NOT NULL DEFAULT 0,
        align_to_mins      INTEGER NOT NULL DEFAULT 0,
        start_scope        TEXT    NOT NULL DEFAULT 'block' CHECK(start_scope IN ('block','episode')),
        queue_pos          INTEGER NOT NULL DEFAULT 0,
        episode_pos        INTEGER NOT NULL DEFAULT 0,
        UNIQUE(block_id, slot_index)
    );

    CREATE TABLE timeslot_slot_queue (
        entry_id               TEXT    PRIMARY KEY,
        slot_id                TEXT    NOT NULL REFERENCES timeslot_slot(slot_id) ON DELETE CASCADE,
        queue_index            INTEGER NOT NULL,
        content_type           TEXT    NOT NULL CHECK(content_type IN ('show','movie')),
        content_id             TEXT    NOT NULL,
        premiere_date          TEXT,
        pre_premiere_behavior  TEXT    NOT NULL DEFAULT 'replay_previous'
                               CHECK(pre_premiere_behavior IN ('replay_previous','filler','skip')),
        UNIQUE(slot_id, queue_index)
    );

    CREATE INDEX idx_timeslot_slot_block ON timeslot_slot(block_id, slot_index);
    CREATE INDEX idx_timeslot_queue_slot ON timeslot_slot_queue(slot_id, queue_index);
)SQL", true }

,

// ── v44: include season_filter in filler entry unique constraints so the same
//         show can appear multiple times with different season filters.
//         Partial unique index on season_filter IS NULL prevents exact duplicates
//         for unfiltered entries (SQLite treats NULLs as distinct in UNIQUE).
{ 44, R"SQL(
    CREATE TABLE block_filler_entry_new (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        block_id       TEXT    NOT NULL REFERENCES block(block_id) ON DELETE CASCADE,
        filler_list_id TEXT,
        advancement    TEXT    NOT NULL DEFAULT 'sequential'
                               CHECK(advancement IN ('sequential','shuffle','sized')),
        weight         INTEGER NOT NULL DEFAULT 1,
        position       INTEGER NOT NULL DEFAULT 0,
        content_type   TEXT    NOT NULL DEFAULT 'filler_list',
        content_id     TEXT,
        season_filter  INTEGER,
        UNIQUE (block_id, content_type, content_id, season_filter)
    );
    INSERT INTO block_filler_entry_new
        SELECT id, block_id, filler_list_id, advancement, weight, position, content_type, content_id, season_filter
        FROM block_filler_entry;
    DROP TABLE block_filler_entry;
    ALTER TABLE block_filler_entry_new RENAME TO block_filler_entry;
    CREATE INDEX IF NOT EXISTS idx_block_filler_entry ON block_filler_entry(block_id, position);
    CREATE UNIQUE INDEX IF NOT EXISTS idx_block_filler_no_season
        ON block_filler_entry (block_id, content_type, content_id) WHERE season_filter IS NULL;

    CREATE TABLE channel_filler_entry_new (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        channel_id     TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        filler_list_id TEXT    REFERENCES filler_list(filler_list_id) ON DELETE CASCADE,
        advancement    TEXT    NOT NULL DEFAULT 'sequential'
                               CHECK(advancement IN ('sequential','shuffle','sized')),
        weight         INTEGER NOT NULL DEFAULT 1,
        position       INTEGER NOT NULL DEFAULT 0,
        content_type   TEXT    NOT NULL DEFAULT 'filler_list',
        content_id     TEXT,
        season_filter  INTEGER,
        UNIQUE (channel_id, content_type, content_id, season_filter)
    );
    INSERT INTO channel_filler_entry_new
        SELECT id, channel_id, filler_list_id, advancement, weight, position, content_type, content_id, season_filter
        FROM channel_filler_entry;
    DROP TABLE channel_filler_entry;
    ALTER TABLE channel_filler_entry_new RENAME TO channel_filler_entry;
    CREATE INDEX IF NOT EXISTS idx_channel_filler_entry ON channel_filler_entry(channel_id, position);
    CREATE UNIQUE INDEX IF NOT EXISTS idx_channel_filler_no_season
        ON channel_filler_entry (channel_id, content_type, content_id) WHERE season_filter IS NULL;
)SQL", false }

// ── v45: episode external IDs ────────────────────────────────────────────────
,{ 45, R"SQL(
    ALTER TABLE episode ADD COLUMN tvdb_id TEXT NOT NULL DEFAULT '';
    ALTER TABLE episode ADD COLUMN tmdb_id TEXT NOT NULL DEFAULT '';
    ALTER TABLE episode ADD COLUMN imdb_id TEXT NOT NULL DEFAULT '';
)SQL" }

// ── v46: scraper infrastructure ──────────────────────────────────────────────
,{ 46, R"SQL(
    CREATE TABLE IF NOT EXISTS scraper_job (
        job_id       TEXT PRIMARY KEY,
        job_type     TEXT NOT NULL CHECK(job_type IN ('scan','match','fetch')),
        status       TEXT NOT NULL DEFAULT 'pending'
                         CHECK(status IN ('pending','running','done','error')),
        target_id    TEXT,
        error_msg    TEXT,
        created_at   TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
        completed_at TEXT
    );

    CREATE TABLE IF NOT EXISTS scanned_file (
        file_id      TEXT PRIMARY KEY,
        file_path    TEXT NOT NULL UNIQUE,
        size_bytes   INTEGER NOT NULL DEFAULT 0,
        duration_ms  INTEGER,
        matched_id   TEXT,
        match_status TEXT NOT NULL DEFAULT 'unmatched'
                         CHECK(match_status IN ('unmatched','matched','skipped','conflict'))
    );

    CREATE TABLE IF NOT EXISTS match_candidate (
        candidate_id TEXT PRIMARY KEY,
        file_id      TEXT NOT NULL REFERENCES scanned_file(file_id) ON DELETE CASCADE,
        source       TEXT NOT NULL CHECK(source IN ('tmdb','tvdb','local')),
        external_id  TEXT NOT NULL,
        title        TEXT NOT NULL,
        year         INTEGER,
        score        REAL NOT NULL DEFAULT 0,
        accepted     INTEGER,
        UNIQUE(file_id, source, external_id)
    );
)SQL" }

// ── v47: metadata overrides ───────────────────────────────────────────────────
,{ 47, R"SQL(
    CREATE TABLE IF NOT EXISTS metadata_override (
        id          INTEGER PRIMARY KEY,
        entity_type TEXT NOT NULL CHECK(entity_type IN ('show','movie','episode')),
        entity_id   TEXT NOT NULL,
        field_name  TEXT NOT NULL,
        value       TEXT NOT NULL,
        set_by      TEXT NOT NULL DEFAULT 'user',
        created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
        UNIQUE(entity_type, entity_id, field_name)
    );
)SQL" }

// ── v48: per-item match status + confidence from metadata scrapers ─────────────
,{ 48, R"SQL(
    ALTER TABLE show  ADD COLUMN match_status TEXT NOT NULL DEFAULT 'unscraped';
    ALTER TABLE show  ADD COLUMN match_score  REAL;
    ALTER TABLE movie ADD COLUMN match_status TEXT NOT NULL DEFAULT 'unscraped';
    ALTER TABLE movie ADD COLUMN match_score  REAL;

    CREATE TABLE IF NOT EXISTS item_match_candidate (
        candidate_id TEXT    PRIMARY KEY,
        item_type    TEXT    NOT NULL CHECK(item_type IN ('show','movie')),
        kairos_id    TEXT    NOT NULL,
        source       TEXT    NOT NULL CHECK(source IN ('tmdb','tvdb')),
        external_id  TEXT    NOT NULL,
        title        TEXT    NOT NULL,
        year         INTEGER,
        score        REAL    NOT NULL DEFAULT 0,
        accepted     INTEGER,
        poster_url   TEXT    NOT NULL DEFAULT '',
        overview     TEXT    NOT NULL DEFAULT '',
        UNIQUE(item_type, kairos_id, source, external_id)
    );
    CREATE INDEX IF NOT EXISTS idx_item_match_kairos
        ON item_match_candidate(item_type, kairos_id);
    CREATE INDEX IF NOT EXISTS idx_item_match_pending
        ON item_match_candidate(accepted) WHERE accepted IS NULL;
)SQL" }

// ── v49: content requests from viewers ────────────────────────────────────────
,{ 49, R"SQL(
    CREATE TABLE IF NOT EXISTS content_request (
        request_id   TEXT    PRIMARY KEY,
        user_id      TEXT    NOT NULL,
        content_type TEXT    NOT NULL CHECK(content_type IN ('show','movie')),
        source       TEXT    NOT NULL CHECK(source IN ('tmdb','tvdb')),
        external_id  TEXT    NOT NULL,
        title        TEXT    NOT NULL DEFAULT '',
        year         INTEGER,
        poster_url   TEXT    NOT NULL DEFAULT '',
        status       TEXT    NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','approved','rejected')),
        created_at   INTEGER NOT NULL,
        UNIQUE(user_id, content_type, source, external_id)
    );
    CREATE INDEX IF NOT EXISTS idx_request_status ON content_request(status);
    CREATE INDEX IF NOT EXISTS idx_request_user   ON content_request(user_id);
)SQL" }

,{ 50, R"SQL(
    ALTER TABLE episode ADD COLUMN locked INTEGER NOT NULL DEFAULT 0;
)SQL" }

// ── v51: add 'anidb' to source CHECK on item_match_candidate and content_request ─
// SQLite cannot alter CHECK constraints in-place; tables must be rebuilt.
,{ 51, R"SQL(
    CREATE TABLE item_match_candidate_v51 (
        candidate_id TEXT    PRIMARY KEY,
        item_type    TEXT    NOT NULL CHECK(item_type IN ('show','movie')),
        kairos_id    TEXT    NOT NULL,
        source       TEXT    NOT NULL CHECK(source IN ('tmdb','tvdb','anidb')),
        external_id  TEXT    NOT NULL,
        title        TEXT    NOT NULL,
        year         INTEGER,
        score        REAL    NOT NULL DEFAULT 0,
        accepted     INTEGER,
        poster_url   TEXT    NOT NULL DEFAULT '',
        overview     TEXT    NOT NULL DEFAULT '',
        UNIQUE(item_type, kairos_id, source, external_id)
    );
    INSERT INTO item_match_candidate_v51
        SELECT * FROM item_match_candidate;
    DROP TABLE item_match_candidate;
    ALTER TABLE item_match_candidate_v51 RENAME TO item_match_candidate;
    CREATE INDEX IF NOT EXISTS idx_item_match_kairos
        ON item_match_candidate(item_type, kairos_id);
    CREATE INDEX IF NOT EXISTS idx_item_match_pending
        ON item_match_candidate(accepted) WHERE accepted IS NULL;

    CREATE TABLE content_request_v51 (
        request_id   TEXT    PRIMARY KEY,
        user_id      TEXT    NOT NULL,
        content_type TEXT    NOT NULL CHECK(content_type IN ('show','movie')),
        source       TEXT    NOT NULL CHECK(source IN ('tmdb','tvdb','anidb')),
        external_id  TEXT    NOT NULL,
        title        TEXT    NOT NULL DEFAULT '',
        year         INTEGER,
        poster_url   TEXT    NOT NULL DEFAULT '',
        status       TEXT    NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','approved','rejected')),
        created_at   INTEGER NOT NULL,
        UNIQUE(user_id, content_type, source, external_id)
    );
    INSERT INTO content_request_v51
        SELECT * FROM content_request;
    DROP TABLE content_request;
    ALTER TABLE content_request_v51 RENAME TO content_request;
    CREATE INDEX IF NOT EXISTS idx_request_status ON content_request(status);
    CREATE INDEX IF NOT EXISTS idx_request_user   ON content_request(user_id);
)SQL" }

// ── v52: preferred scraper per library ─────────────────────────────────────
,{ 52, R"SQL(
    ALTER TABLE media_library ADD COLUMN preferred_scraper TEXT NOT NULL DEFAULT '';
)SQL" }

// ── v53: per-channel audio / subtitle language ──────────────────────────────
,{ 53, R"SQL(
    ALTER TABLE channel ADD COLUMN audio_lang    TEXT NOT NULL DEFAULT '';
    ALTER TABLE channel ADD COLUMN subtitle_lang TEXT NOT NULL DEFAULT '';
)SQL" }

// ── v54: preferred metadata language per library and per item ────────────────
,{ 54, R"SQL(
    ALTER TABLE media_library ADD COLUMN preferred_language TEXT NOT NULL DEFAULT '';
    ALTER TABLE show          ADD COLUMN preferred_language TEXT NOT NULL DEFAULT '';
    ALTER TABLE movie         ADD COLUMN preferred_language TEXT NOT NULL DEFAULT '';
)SQL" }

// ── v55: per-channel transcode quality settings ───────────────────────────────
,{ 55, R"SQL(
    ALTER TABLE channel ADD COLUMN stream_resolution     TEXT    NOT NULL DEFAULT 'source';
    ALTER TABLE channel ADD COLUMN stream_video_bitrate  INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE channel ADD COLUMN stream_audio_bitrate  INTEGER NOT NULL DEFAULT 192;
)SQL" }

// ── v56: rerun cooldown (min minutes before a repeat play is eligible) ───────
,{ 56, R"SQL(
    ALTER TABLE channel ADD COLUMN rerun_min_time_mins INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE block   ADD COLUMN rerun_min_time_mins INTEGER NOT NULL DEFAULT 0;
)SQL" }

// ── v57: filler play history — filler picks were never recorded anywhere,
//         so the "sized" LRU pick and smart_pct cooldown in pickFillerSim had
//         no recency data to work with and always fell back to the same clip.
//         Kept separate from play_history so filler plays never pollute the
//         content-side rerun/smart cooldown queries (getHotMovieIds etc).
,{ 57, R"SQL(
    CREATE TABLE filler_play_history (
        history_id  INTEGER PRIMARY KEY AUTOINCREMENT,
        item_id     TEXT    NOT NULL,
        channel_id  TEXT    NOT NULL REFERENCES channel(channel_id) ON DELETE CASCADE,
        block_id    TEXT    REFERENCES block(block_id) ON DELETE SET NULL,
        aired_at    INTEGER NOT NULL
    );
    CREATE INDEX idx_filler_history_channel ON filler_play_history(channel_id, aired_at DESC);
)SQL" }

// ── v58: watch progress for VOD resume / continue-watching. One row per
//         user+item; upserted by periodic position pings from the player.
,{ 58, R"SQL(
    CREATE TABLE watch_progress (
        user_id      TEXT    NOT NULL REFERENCES user(user_id) ON DELETE CASCADE,
        content_type TEXT    NOT NULL CHECK(content_type IN ('movie','episode')),
        content_id   TEXT    NOT NULL,
        position_ms  INTEGER NOT NULL DEFAULT 0,
        duration_ms  INTEGER NOT NULL DEFAULT 0,
        updated_at   INTEGER NOT NULL,
        PRIMARY KEY (user_id, content_type, content_id)
    );
    CREATE INDEX idx_watch_progress_user ON watch_progress(user_id, updated_at DESC);
)SQL" }

// ── v59: real movie release date (movie previously only stored a coarse
//         `year` integer) + an index supporting "most-recently-aired
//         episode per show" queries (Home's Recently Released/Recently
//         Aired shelves).
,{ 59, R"SQL(
    ALTER TABLE movie ADD COLUMN release_date TEXT NOT NULL DEFAULT '';
    CREATE INDEX idx_episode_show_airdate ON episode(show_id, air_date);
)SQL" }

// ── v60: backfill release_date for movies scraped before v59 existed —
//         without this, every existing movie has release_date='' and the
//         Recently Released shelf's "AND release_date != ''" filter (see
//         ContentRepository::searchMovies) excludes the entire library,
//         leaving the shelf empty. Jan 1 of the already-known `year` is a
//         coarse but immediately-usable placeholder; it's overwritten with
//         the real precise date the next time a movie is normally re-
//         scraped/matched (acceptMatch's UPDATE always sets release_date
//         when the row isn't locked), so this is a one-time bootstrap, not
//         a permanent approximation.
,{ 60, R"SQL(
    UPDATE movie SET release_date = printf('%04d-01-01', year)
    WHERE release_date = '' AND year IS NOT NULL;
)SQL" }

// ── v61: match_confirmed — distinguishes a human-confirmed match from an
//         auto-accepted one. match_status='matched' alone doesn't tell these
//         apart: the auto-match path (ScraperManager's threshold-clearing
//         branch) sets it directly with no human involved, same as
//         acceptCandidate() (reached only via the review-queue accept route
//         and manual match search). Metadata writeback to Plex/Jellyfin must
//         gate on this, not on match_status — see acceptCandidate().
,{ 61, R"SQL(
    ALTER TABLE show  ADD COLUMN match_confirmed INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE movie ADD COLUMN match_confirmed INTEGER NOT NULL DEFAULT 0;
)SQL" }

// ── v62: media_library.include_anidb — AniDB is an anime-only database, but
//         wantScraper() previously queried it for every library with no
//         preferred_scraper set (the default), letting a general movie/show
//         library get cross-matched against completely unrelated anime
//         titles on nothing but a title-string coincidence. Requiring an
//         explicit per-library opt-in (default off) closes that off; an
//         admin only enables it for a library that actually contains anime.
,{ 62, R"SQL(
    ALTER TABLE media_library ADD COLUMN include_anidb INTEGER NOT NULL DEFAULT 0;
)SQL" }

// ── v63: parental controls — restricted accounts, across media and channels.
//   Two-layer model: a per-content-type severity ceiling as the base rule
//   (see RatingSeverity.h), with an explicit per-title/per-channel allow/block
//   override on top of it. Unrestricted by default (restricted=0); an admin
//   opts a specific account into restriction via the Users page.
,{ 63, R"SQL(
    ALTER TABLE user ADD COLUMN restricted         INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE user ADD COLUMN max_tv_rating      TEXT    NOT NULL DEFAULT 'TV-Y';
    ALTER TABLE user ADD COLUMN max_movie_rating   TEXT    NOT NULL DEFAULT 'G';
    ALTER TABLE user ADD COLUMN max_channel_rating TEXT    NOT NULL DEFAULT 'TV-Y';

    ALTER TABLE channel ADD COLUMN content_tag TEXT NOT NULL DEFAULT '';

    CREATE TABLE user_content_override (
        id          INTEGER PRIMARY KEY,
        user_id     TEXT NOT NULL REFERENCES user(user_id) ON DELETE CASCADE,
        entity_type TEXT NOT NULL CHECK(entity_type IN ('show','movie','channel')),
        entity_id   TEXT NOT NULL,
        mode        TEXT NOT NULL CHECK(mode IN ('allow','block')),
        UNIQUE(user_id, entity_type, entity_id)
    );
)SQL" }

// ── v64: session.purpose / session_id — lets a session be tagged as a
//   viewer-scoped Cast-receiver token distinct from a normal login
//   ('login' vs 'cast'; enforced at AuthStore::validate() time, not just by
//   convention). session_id is a separate, non-secret identifier the client
//   can be given to list/revoke a cast session by — the raw token itself is
//   never sent back down after the session is created.
,{ 64, R"SQL(
    ALTER TABLE session ADD COLUMN purpose    TEXT NOT NULL DEFAULT 'login';
    ALTER TABLE session ADD COLUMN session_id TEXT;
    CREATE UNIQUE INDEX idx_session_session_id ON session(session_id);
)SQL" }

// ── v65: per-library Home-shelf visibility (filler/bumper libraries opt out) ─
,{ 65, R"SQL(
    ALTER TABLE media_library ADD COLUMN show_on_home INTEGER NOT NULL DEFAULT 1;
)SQL" }

// ── v66: multi-scraper IDs and alternate titles ──────────────────────────────
,{ 66, R"SQL(
    -- Multi-scraper IDs with priority
    CREATE TABLE IF NOT EXISTS item_external_id (
        item_type   TEXT    NOT NULL CHECK(item_type IN ('show','movie')),
        kairos_id   TEXT    NOT NULL,
        source      TEXT    NOT NULL,  -- 'tmdb', 'tvdb', 'anidb', 'imdb', etc.
        external_id TEXT    NOT NULL,
        priority    INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (item_type, kairos_id, source)
    );
    CREATE INDEX idx_item_ext_kairos ON item_external_id(item_type, kairos_id);

    -- Alternate titles for search and matching
    CREATE TABLE IF NOT EXISTS item_alternate_title (
        item_type   TEXT    NOT NULL CHECK(item_type IN ('show','movie')),
        kairos_id   TEXT    NOT NULL,
        title       TEXT    NOT NULL,
        PRIMARY KEY (item_type, kairos_id, title)
    );
    CREATE INDEX idx_item_alt_title ON item_alternate_title(title);

    -- Migrate existing IDs from show and movie tables
    -- Show: tmdb, tvdb, imdb
    INSERT OR IGNORE INTO item_external_id (item_type, kairos_id, source, external_id, priority)
    SELECT 'show', show_id, 'tmdb', tmdb_id, 1 FROM show WHERE tmdb_id != '';
    INSERT OR IGNORE INTO item_external_id (item_type, kairos_id, source, external_id, priority)
    SELECT 'show', show_id, 'tvdb', tvdb_id, 2 FROM show WHERE tvdb_id != '';
    INSERT OR IGNORE INTO item_external_id (item_type, kairos_id, source, external_id, priority)
    SELECT 'show', show_id, 'imdb', imdb_id, 3 FROM show WHERE imdb_id != '';

    -- Movie: tmdb, imdb
    INSERT OR IGNORE INTO item_external_id (item_type, kairos_id, source, external_id, priority)
    SELECT 'movie', movie_id, 'tmdb', tmdb_id, 1 FROM movie WHERE tmdb_id != '';
    INSERT OR IGNORE INTO item_external_id (item_type, kairos_id, source, external_id, priority)
    SELECT 'movie', movie_id, 'imdb', imdb_id, 2 FROM movie WHERE imdb_id != '';
)SQL" }

// ── v67: roku_device — a user's paired Roku channels. Persists across the
//   channel being closed/reopened and across Hermes restarts (Hermes's own
//   live device-session registry is in-memory/ephemeral); pairing_token is
//   cleared once a channel confirms pairing, ip_address/app_id are what let
//   Hermes ECP-launch the channel when no live session is registered.
,{ 67, R"SQL(
    CREATE TABLE roku_device (
        id                 TEXT PRIMARY KEY,
        user_id            TEXT NOT NULL REFERENCES user(user_id) ON DELETE CASCADE,
        name               TEXT NOT NULL,
        ip_address         TEXT NOT NULL,
        app_id             TEXT NOT NULL DEFAULT 'dev',
        pairing_token      TEXT,
        pairing_expires_at INTEGER,
        created_at         INTEGER NOT NULL
    );
    CREATE INDEX idx_roku_device_user ON roku_device(user_id);
)SQL" }

// ── v68: per-library scraper priority order, split by item_type ─────────────
//   Replaces the single media_library.preferred_scraper "tie-break only"
//   field with a real ordered list, so a mixed (show+movie) library can rank
//   its scrapers differently for each. preferred_scraper is left in place
//   (some old code paths still display it) but matching no longer reads it —
//   this table is seeded from it once so existing preferences carry over.
,{ 68, R"SQL(
    CREATE TABLE IF NOT EXISTS library_scraper_priority (
        library_id TEXT    NOT NULL,
        item_type  TEXT    NOT NULL CHECK(item_type IN ('show','movie')),
        source     TEXT    NOT NULL,
        priority   INTEGER NOT NULL,
        PRIMARY KEY (library_id, item_type, source)
    );
    CREATE INDEX idx_lib_scraper_priority ON library_scraper_priority(library_id, item_type);

    INSERT OR IGNORE INTO library_scraper_priority (library_id, item_type, source, priority)
    SELECT library_id, 'show', preferred_scraper, 1 FROM media_library WHERE preferred_scraper != '';
    INSERT OR IGNORE INTO library_scraper_priority (library_id, item_type, source, priority)
    SELECT library_id, 'movie', preferred_scraper, 1 FROM media_library WHERE preferred_scraper != '';
)SQL" }

// ── v69: scrape exemption — library-level and per-item ───────────────────────
//   For content that should never be sent to TMDB/TVDB at all (filler/bumper
//   libraries, individual home videos mixed into an otherwise normal library).
//   Deliberately not reusing match_status (conflates "matching progress" with
//   "opted out") or locked (only guards per-field overwrite on sync — doesn't
//   stop an item from being queried against scrapers at all).
,{ 69, R"SQL(
    ALTER TABLE media_library ADD COLUMN skip_scraping INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE show          ADD COLUMN skip_scraping INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE movie         ADD COLUMN skip_scraping INTEGER NOT NULL DEFAULT 0;
)SQL" }

// ── v70: show specials linked to a movie-library file ────────────────────────
//   A show's special/OVA/TV-movie is often filed as a standalone file in a
//   Movies library rather than under the show's own folder. linked_movie_id
//   lets a season-0 episode row resolve playback through that movie's file
//   while the movie stays fully independent (still browsable/playable on its
//   own) — see PlaybackService's live join and SyncManager's orphan-cleanup
//   guard, both of which depend on this column existing.
//   find_specials is a per-show opt-in so the scan only runs (automatically,
//   during normal sync) for shows the user actually wants this for.
//   episode_display_order controls how season-0 episodes are grouped for VOD
//   browsing: 'season' (default, unchanged) buckets them into one "Specials"
//   group; 'aired' interleaves them between numbered seasons by air date
//   (e.g. Gundam movies/OVAs between Season 1 and Season 2).
//   special_link_candidate is deliberately a new table, not item_match_candidate
//   — that table keys on (item_type, kairos_id, source, external_id), modelling
//   alternate identities of ONE item; this is a cross-item link (show + episode
//   number -> movie) with its own tri-state per candidate.
,{ 70, R"SQL(
    ALTER TABLE episode ADD COLUMN linked_movie_id TEXT REFERENCES movie(movie_id) ON DELETE SET NULL;
    ALTER TABLE show    ADD COLUMN find_specials INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE show    ADD COLUMN episode_display_order TEXT NOT NULL DEFAULT 'season'
                         CHECK(episode_display_order IN ('season','aired'));

    CREATE TABLE IF NOT EXISTS special_link_candidate (
        candidate_id      TEXT    PRIMARY KEY,
        show_id           TEXT    NOT NULL REFERENCES show(show_id) ON DELETE CASCADE,
        episode_number    INTEGER NOT NULL,
        special_title     TEXT    NOT NULL,
        special_overview  TEXT    NOT NULL DEFAULT '',
        special_air_date  TEXT    NOT NULL DEFAULT '',
        special_thumb     TEXT    NOT NULL DEFAULT '',
        source            TEXT    NOT NULL CHECK(source IN ('tmdb','tvdb','anidb')),
        source_episode_id TEXT    NOT NULL DEFAULT '',
        movie_id          TEXT    NOT NULL REFERENCES movie(movie_id) ON DELETE CASCADE,
        score             REAL    NOT NULL DEFAULT 0,
        accepted          INTEGER,
        created_at        TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
        decided_at        TEXT,
        UNIQUE(show_id, episode_number, movie_id)
    );
    CREATE INDEX IF NOT EXISTS idx_special_candidate_show    ON special_link_candidate(show_id);
    CREATE INDEX IF NOT EXISTS idx_special_candidate_pending ON special_link_candidate(show_id, accepted) WHERE accepted IS NULL;
)SQL" }

// ── v71: source-side user discovery + opt-in watch-state sync target ─────────
//   source_user is populated by IMediaSource::listServerUsers() during sync —
//   every account/profile that exists on a Plex/Jellyfin/Emby server, so
//   Pantheon can flag ones with no corresponding local account. imported_user_id
//   is the local user this discovered account maps to, however that mapping
//   was made: an admin importing it as a brand-new local account (original
//   meaning, source import route), or later linking it to an existing local
//   user (SourceRepository::setImportedUserId / SourceService's
//   PUT .../link) so that account's own watch history gets pulled too — see
//   SyncManager::syncLinkedUserWatchState + IMediaSource::fetchWatchState.
//   Either way, a row with it set is excluded from the "unregistered" list
//   instead of being re-offered every sync.
//   media_source.synced_user_id is a separate, narrower link: which local
//   user (if any) should have watch/resume state pulled from this source's
//   already-configured primary account during sync. Unset = feature off.
,{ 71, R"SQL(
    CREATE TABLE source_user (
        source_id        TEXT    NOT NULL REFERENCES media_source(source_id) ON DELETE CASCADE,
        external_user_id TEXT    NOT NULL,
        display_name     TEXT    NOT NULL DEFAULT '',
        email            TEXT    NOT NULL DEFAULT '',
        last_seen_at     INTEGER NOT NULL,
        imported_user_id TEXT    REFERENCES user(user_id) ON DELETE SET NULL,
        PRIMARY KEY (source_id, external_user_id)
    );

    ALTER TABLE media_source ADD COLUMN synced_user_id TEXT REFERENCES user(user_id) ON DELETE SET NULL;
)SQL" }

// ── v72: invite-based account creation ────────────────────────────────────────
//   must_change_password gates a fresh account created without the owner
//   having chosen their own password yet — either a temp password an admin
//   relays out-of-band, or (before the invite link is claimed) an unguessable
//   placeholder hash nobody knows. Cleared by any successful password change,
//   whether self-service or admin-initiated (see AuthStore::updateUser).
//   user_invite backs the email-invite path only (temp_password logs in with
//   the existing /api/auth/login and needs no token): a single-use, expiring
//   claim link since there's no password yet to authenticate with.
,{ 72, R"SQL(
    ALTER TABLE user ADD COLUMN must_change_password INTEGER NOT NULL DEFAULT 0;

    CREATE TABLE user_invite (
        invite_id  TEXT    PRIMARY KEY,
        user_id    TEXT    NOT NULL REFERENCES user(user_id) ON DELETE CASCADE,
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        used_at    INTEGER
    );
    CREATE INDEX idx_user_invite_user ON user_invite(user_id);
)SQL" }

// ── v73: durable "played" state on watch_progress. Previously, crossing the
//         95%-watched threshold deleted the row outright — that made "played"
//         indistinguishable from "never started" once the row was gone, which
//         broke series-continuation (resuming a show after finishing an
//         episode fell back to episode 1 instead of the next one). Finished
//         items now stay as a row with completed=1 (position_ms clamped to
//         duration_ms) instead of being removed; Continue Watching queries
//         add "AND completed = 0" to keep the externally-visible behavior
//         identical to before.
,{ 73, R"SQL(
    ALTER TABLE watch_progress ADD COLUMN completed INTEGER NOT NULL DEFAULT 0;
)SQL" }

// ── v74: folder-path + fuzzy-title sync-time dedup. show.folder_path lets
//         cross-source dedup compare on-disk location the same way movies
//         already do via file_path — no backfill for existing rows (would
//         require reimplementing the season-folder-stripping heuristic in
//         SQL); they simply stay '' until their next normal sync, and
//         getShowDetail()'s existing episode-derived fallback covers the gap
//         until then. duplicate_candidate holds "uncertain" cross-source
//         matches for human review; ON CONFLICT DO NOTHING on the insert is
//         the entire remembered-dismissal mechanism — a dismissed pair is
//         never resurrected by a later sync re-detecting it.
,{ 74, R"SQL(
    ALTER TABLE show ADD COLUMN folder_path TEXT NOT NULL DEFAULT '';

    CREATE TABLE IF NOT EXISTS duplicate_candidate (
        candidate_id      TEXT PRIMARY KEY,
        item_type         TEXT NOT NULL CHECK(item_type IN ('show','movie')),
        kairos_id_a       TEXT NOT NULL,
        kairos_id_b       TEXT NOT NULL,
        trigger           TEXT NOT NULL CHECK(trigger IN ('fuzzy_title','folder_uncertain','both')),
        reason            TEXT NOT NULL DEFAULT '',
        title_similarity  REAL NOT NULL DEFAULT 0,
        folder_a          TEXT NOT NULL DEFAULT '',
        folder_b          TEXT NOT NULL DEFAULT '',
        status            TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','dismissed')),
        created_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
        decided_at        TEXT
    );
    CREATE INDEX idx_dup_candidate_status ON duplicate_candidate(item_type, status);
)SQL" }

// ── v75: user-discovery diagnostics. IMediaSource::listServerUsers() has
//         always swallowed failures to a stderr log line only — an admin
//         staring at an empty "unmapped users" list has no way to tell "this
//         server genuinely has no other accounts" from "the request got
//         rejected" without shelling into the container and grepping logs.
//         user_sync_error is empty on success (or a source that's never
//         synced yet); non-empty holds the last failure's HTTP status/message,
//         cleared back to '' the next time discovery succeeds. Set by
//         SyncManager right after calling listServerUsers(), read by
//         GET /api/sources so the admin sees it inline in Hades.
,{ 75, R"SQL(
    ALTER TABLE media_source ADD COLUMN user_sync_error TEXT NOT NULL DEFAULT '';
    ALTER TABLE media_source ADD COLUMN user_sync_checked_at INTEGER;
)SQL" }

// ── v76: profile-switch PIN — optional per-user PIN so a device that's
//         already passed the real username/password login can hop between
//         profiles (Netflix/Plex "Home" style) without re-entering
//         credentials. NULL pin_hash means no PIN set. fail_count/locked_until
//         throttle brute-forcing the short numeric PIN (see AuthStore::switchProfile).
,{ 76, R"SQL(
    ALTER TABLE user ADD COLUMN pin_hash         TEXT;
    ALTER TABLE user ADD COLUMN pin_fail_count   INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE user ADD COLUMN pin_locked_until INTEGER NOT NULL DEFAULT 0;
)SQL" }

// ── v77: sync priority for sources ──────────────────────────────────────
,{ 77, R"SQL(
    ALTER TABLE media_source ADD COLUMN sync_priority INTEGER NOT NULL DEFAULT 0;
)SQL" }

// ── v78: cursor state becomes fully anchor-self-sufficient — no more live
//         play_history/scheduled_program derivation during project().
//         - media_cursor: episode_order joins the key (two blocks playing the
//           same show in different orders now track independent progress),
//           plus an `ahead` column (JSON array of episode ids aired out of
//           order, ahead of the `episode_id` watermark — see CursorState).
//         - timeslot_slot: episode_pos (a raw, unreconciled index) is
//           replaced by the same watermark+ahead pair; left in place unused
//           rather than dropped, since nothing else reads it.
//         - anchor_hashes format changes shape (episode_order/ahead in
//           cursors, plus now includes slot_cursors/filler_positions/recency
//           that were previously silently dropped) — old snapshots are
//           incompatible, so clear them and let channels regenerate fresh,
//           same as v31.
,{ 78, R"SQL(
    CREATE TABLE media_cursor_v78 (
        content_type  TEXT    NOT NULL
                      CHECK(content_type IN ('show','show_rerun','movie','playlist','filler_list')),
        content_id    TEXT    NOT NULL,
        cursor_scope  TEXT    NOT NULL CHECK(cursor_scope IN ('global','channel','block')),
        scope_id      TEXT    NOT NULL DEFAULT '',
        episode_order TEXT    NOT NULL DEFAULT '',
        episode_id    TEXT    REFERENCES episode(episode_id),
        movie_id      TEXT    REFERENCES movie(movie_id),
        position      INTEGER NOT NULL DEFAULT 0,
        ahead         TEXT    NOT NULL DEFAULT '[]',
        updated_at    INTEGER NOT NULL,
        PRIMARY KEY (content_type, content_id, cursor_scope, scope_id, episode_order)
    );

    INSERT INTO media_cursor_v78
        (content_type, content_id, cursor_scope, scope_id, episode_order,
         episode_id, movie_id, position, ahead, updated_at)
        SELECT content_type, content_id, cursor_scope, scope_id, '',
               episode_id, movie_id, position, '[]', updated_at
        FROM media_cursor;

    DROP TABLE media_cursor;
    ALTER TABLE media_cursor_v78 RENAME TO media_cursor;

    ALTER TABLE timeslot_slot ADD COLUMN episode_watermark_id TEXT;
    ALTER TABLE timeslot_slot ADD COLUMN episode_ahead        TEXT NOT NULL DEFAULT '[]';

    UPDATE channel SET anchor_hashes = NULL WHERE anchor_hashes IS NOT NULL;
)SQL" }

// ── v79: Library filtering completeness pass — real per-item "date added"
//         (from each source's own signal, not a sync-time proxy), a
//         per-library priority order to resolve it when an item is matched
//         across multiple sources (mirrors library_scraper_priority/v68),
//         plus persisted writer (movies) and resolution (movies+episodes,
//         probed once at sync time instead of only-on-demand/memory-cached).
,{ 79, R"SQL(
    ALTER TABLE show  ADD COLUMN added_at        INTEGER;
    ALTER TABLE show  ADD COLUMN added_at_source TEXT NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN added_at        INTEGER;
    ALTER TABLE movie ADD COLUMN added_at_source TEXT NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN writer           TEXT NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN resolution_label TEXT NOT NULL DEFAULT '';
    ALTER TABLE episode ADD COLUMN resolution_label TEXT NOT NULL DEFAULT '';

    CREATE TABLE IF NOT EXISTS library_source_priority (
        library_id TEXT    NOT NULL,
        item_type  TEXT    NOT NULL CHECK(item_type IN ('show','movie')),
        source     TEXT    NOT NULL,
        priority   INTEGER NOT NULL,
        PRIMARY KEY (library_id, item_type, source)
    );
    CREATE INDEX idx_lib_source_priority ON library_source_priority(library_id, item_type);
)SQL" }

// ── v80: source priority now actually does something — superseded the v79
//         per-library/per-item-type library_source_priority table (which only
//         ever gated added_at, was never surfaced anywhere editable, and made
//         "priority" mean something different per library) with the already-
//         existing but functionally dead media_source.sync_priority (v77 —
//         collected in the add-source form, never read server-side, never
//         shown again after creation). One priority per source now governs
//         both sync order (SyncManager::loadSources) and, via the new
//         primary_source column, which source's data wins on every field —
//         not just added_at — when the same show/movie is matched across
//         multiple sources, with lower-priority sources backfilling whatever
//         the current owner left empty instead of being locked out entirely.
,{ 80, R"SQL(
    DROP TABLE IF EXISTS library_source_priority;
    ALTER TABLE show  ADD COLUMN primary_source TEXT NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN primary_source TEXT NOT NULL DEFAULT '';
)SQL" }

// ── v81: TV manifest — GET /api/tv/manifest's backing store. tv_shelf holds
//         Home's row composition, tv_zone holds Library/Detail/Guide's zone
//         layout. Both are DB-backed (not a hardcoded constant) specifically
//         so a future "customize your Home/Library" feature is additive (new
//         CRUD endpoints + a settings screen writing to these same tables)
//         rather than a rework of where TvManifestService reads from — see
//         hades/src/tv/TvHome.tsx et al for the hardcoded shape this seed
//         data reproduces exactly. Zones never carry behavior, only
//         placement + a data pointer (config_json), same discipline as
//         tv_shelf's params_json — the closed itemAction vocabulary
//         (open-detail/play-direct-with-position/play-latest-episode/
//         play-resolved/navigate-library/watch-live) is enforced client-side,
//         not by this schema.
,{ 81, R"SQL(
    CREATE TABLE tv_shelf (
        id             TEXT    PRIMARY KEY,
        row_order      INTEGER NOT NULL,
        row_type       TEXT    NOT NULL,           -- 'hero' | 'shelf' | 'guide'
        title          TEXT    NOT NULL DEFAULT '',
        endpoint       TEXT    NOT NULL DEFAULT '', -- '' for hero/guide — hero's dual shows+movies sources are fixed row-type behavior, not per-row data
        params_json    TEXT    NOT NULL DEFAULT '{}',
        item_action    TEXT    NOT NULL DEFAULT '', -- '' = client falls back to its own default ('open-detail')
        end_tile       TEXT    NOT NULL DEFAULT '',
        empty_behavior TEXT    NOT NULL DEFAULT 'hide',
        enabled        INTEGER NOT NULL DEFAULT 1
    );

    CREATE TABLE tv_zone (
        id          TEXT    PRIMARY KEY,
        screen      TEXT    NOT NULL,   -- 'library' | 'detail' | 'guide'
        zone_id     TEXT    NOT NULL,
        zone_order  INTEGER NOT NULL,
        config_json TEXT    NOT NULL DEFAULT '{}',
        enabled     INTEGER NOT NULL DEFAULT 1
    );

    INSERT INTO tv_shelf (id, row_order, row_type, title, endpoint, params_json, item_action, end_tile, empty_behavior) VALUES
        ('hero',             0, 'hero',  '',                        '',             '{}', '', '', 'hide'),
        ('continue-watching',1, 'shelf', 'Continue Watching',       '/api/watch-progress', '{}', 'play-direct-with-position', '', 'hide'),
        ('recent-shows',     2, 'shelf', 'Recently Added Shows',    '/api/shows',   '{"limit":16,"sort":"recently_added","home":true,"hide_empty":true}',   'open-detail',       'navigate-library', 'hide'),
        ('recent-movies',    3, 'shelf', 'Recently Added Movies',   '/api/movies',  '{"limit":16,"sort":"recently_added","home":true,"hide_empty":true}',   'open-detail',       'navigate-library', 'hide'),
        ('recent-released',  4, 'shelf', 'Recently Released',       '/api/movies',  '{"limit":16,"sort":"recently_released","home":true,"hide_empty":true}','open-detail',       'navigate-library', 'hide'),
        ('recent-aired',     5, 'shelf', 'Recently Aired',          '/api/shows',   '{"limit":16,"sort":"recently_aired","home":true,"hide_empty":true}',   'play-latest-episode','navigate-library', 'hide'),
        ('guide',            6, 'guide', '',                        '',             '{}', '', '', 'hide');

    INSERT INTO tv_zone (id, screen, zone_id, zone_order, config_json) VALUES
        ('library-search-bar',    'library', 'search-bar',    0, '{"dataSource":{"endpoint":"/api/shows|/api/movies","queryParam":"q"}}'),
        ('library-library-pills', 'library', 'library-pills', 1, '{"dataSource":{"endpoint":"/api/libraries"}}'),
        ('library-filter-pills',  'library', 'filter-pills',  2, '{"filterFields":["genre"]}'),
        ('library-tile-grid',     'library', 'tile-grid',     3, '{"dataSource":{"endpoints":["/api/shows","/api/movies"]},"itemAction":"open-detail"}'),

        ('detail-hero-backdrop',  'detail',  'hero-backdrop', 0, '{}'),
        ('detail-meta-block',     'detail',  'meta-block',    1, '{}'),
        ('detail-genre-chips',    'detail',  'genre-chips',   2, '{}'),
        ('detail-play-button',    'detail',  'play-button',   3, '{"itemAction":"play-resolved"}'),
        ('detail-episode-shelves','detail',  'episode-shelves',4,'{"showOnly":true}'),

        ('guide-channel-header',  'guide',   'channel-header',0, '{}'),
        ('guide-time-grid',       'guide',   'time-grid',     1, '{}'),
        ('guide-preview-panel',   'guide',   'preview-panel', 2, '{"itemAction":"watch-live"}');
)SQL" }

// ── v82: filter-pills zone's filterFields expanded past just "genre" — real
//         usage feedback on the native Android client asked for a real
//         multi-field filter builder ("just like the web version"), and per
//         this whole manifest design's own founding principle, *which*
//         fields are filterable is server-owned data a client renders, not
//         something to hardcode per-platform (see TvManifestService's own
//         header comment). Limited to fields ContentRepository::
//         getMetadataValues actually backs with real distinct-value queries
//         (genre/content_rating/studio/network/actor/director/country/
//         collection) plus a few numeric/enum fields every client's own
//         field registry (FIELD_DEFS in hades/src/components/media/
//         filterFields.ts, mirrored client-side per platform) already knows
//         how to render without a values fetch (year/resolution/decade/
//         audience_rating). Excludes library/source/title — those already
//         have their own dedicated library-pills/search-bar zones.
,{ 82, R"SQL(
    UPDATE tv_zone
    SET config_json = '{"filterFields":["genre","content_rating","studio","network","actor","director","country","collection","year","resolution","decade","audience_rating"]}'
    WHERE id = 'library-filter-pills';
)SQL" }

// ── v83: filter-pills zone gains sortOptions — the native Android client had
//         no sort control at all (unlike hades' own LibraryFilters.tsx,
//         which has always had one, just hardcoded client-side). Rather than
//         hardcode a second copy of that list in Kotlin, this follows the
//         same v82 principle: *which* sort modes are offered is server-owned
//         data a client renders, not something to hardcode per-platform.
//         Values are exactly the backend sort vocabulary ContentRepository
//         already implements (ContentRepository.h's ShowQueryParams/
//         MovieQueryParams sort field, ContentRepository.cpp's dirFor/mdirFor
//         ORDER BY switches) — "recently_released_or_aired" is the one
//         client-side convenience alias (hades' LibraryStore.showSort()/
//         movieSort() split it into recently_aired for shows vs
//         recently_released for movies, since the two content types don't
//         share a single backend sort mode for it).
,{ 83, R"SQL(
    UPDATE tv_zone
    SET config_json = '{"filterFields":["genre","content_rating","studio","network","actor","director","country","collection","year","resolution","decade","audience_rating"],"sortOptions":["title","recently_added","year","audience_rating","duration","recently_released_or_aired","random"]}'
    WHERE id = 'library-filter-pills';
)SQL" }


// ── v84: original_title — the title in a show/movie's original language,
//         distinct from the (possibly localized/English) `title` field.
//         Was never tracked anywhere: no column, no scraper population, no
//         writeback mapping. TMDB's `original_name`/`original_title` are the
//         only scraper source populating it for now (see TmdbScraper.cpp);
//         other scrapers leave it blank rather than guess.
,{ 84, R"SQL(
    ALTER TABLE show  ADD COLUMN original_title TEXT NOT NULL DEFAULT '';
    ALTER TABLE movie ADD COLUMN original_title TEXT NOT NULL DEFAULT '';
)SQL" }


// ── v85: per-source writeback settings. auto_writeback defaults OFF —
//         writeback pushing to a real external library is exactly the kind
//         of thing that shouldn't newly start happening for existing
//         sources just because the column now exists; an admin has to
//         explicitly opt each source in. The three per-field toggles
//         default ON, preserving today's existing manual/bulk-writeback
//         behavior (which has always sent art and, as of this same
//         session, external IDs) — they're an opt-OUT for a specific
//         source that doesn't want a given field touched, not a new
//         opt-in gate on top of the existing single-item/"Writeback All"
//         triggers.
,{ 85, R"SQL(
    ALTER TABLE media_source ADD COLUMN auto_writeback INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE media_source ADD COLUMN writeback_update_art INTEGER NOT NULL DEFAULT 1;
    ALTER TABLE media_source ADD COLUMN writeback_update_external_ids INTEGER NOT NULL DEFAULT 1;
    ALTER TABLE media_source ADD COLUMN writeback_update_collections INTEGER NOT NULL DEFAULT 1;
)SQL" }

,

// ── v86: persisted audio/subtitle language filter fields + external subtitle
//         sidecar cataloging. audio_languages/embedded_subtitle_languages
//         mirror the genres/labels JSON-array convention (default '[]', not
//         ''), probed once at sync time via a single combined ffprobe call
//         — see MediaProbe::probeFileInfo and SyncManager::
//         syncMediaProbeFromFiles. embedded_subtitle_languages deliberately
//         holds only what's embedded in the container — the filterable
//         "subtitle_language" field (FilterExpr.cpp, ContentRepository::
//         getMetadataValues) unions this with subtitle_track.language at
//         query time rather than maintaining a pre-merged column, since
//         embedded probing (conditional/cached) and the external sidecar
//         scan (unconditional every sync) fire independently and a
//         denormalized union can't reliably stay in sync across the two.
//         subtitle_track holds only external sidecar files (.srt/.ass/.ssa/
//         .vtt next to the video) — embedded tracks stay ephemeral/probe-
//         time-only in Hephaestus's own MediaProbe, same as before this
//         migration. No `locked` column: no manual-edit UI exists for these
//         rows yet.
{ 86, R"SQL(
    ALTER TABLE episode ADD COLUMN audio_languages             TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE episode ADD COLUMN embedded_subtitle_languages TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE movie   ADD COLUMN audio_languages             TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE movie   ADD COLUMN embedded_subtitle_languages TEXT NOT NULL DEFAULT '[]';

    CREATE TABLE subtitle_track (
        subtitle_id TEXT    PRIMARY KEY,
        media_type  TEXT    NOT NULL,
        media_id    TEXT    NOT NULL,
        file_path   TEXT    NOT NULL,
        language    TEXT    NOT NULL DEFAULT '',
        forced      INTEGER NOT NULL DEFAULT 0,
        sdh         INTEGER NOT NULL DEFAULT 0,
        title       TEXT    NOT NULL DEFAULT '',
        source      TEXT    NOT NULL DEFAULT 'file'
    );
    CREATE INDEX idx_subtitle_track_media ON subtitle_track(media_type, media_id);
)SQL" }

,{ 87, R"SQL(
    -- Smart playlists: membership computed periodically from a stored canon
    -- filter-syntax string (compileFilterExpr, FilterExpr.h) into real
    -- playlist_item rows via the same replaceListItems() path source-linked
    -- playlists already use (SyncManager::syncPlexLinks) — not a live
    -- per-read query, so channel/block cursors (which point at
    -- playlist_item.position) keep working completely unchanged; a smart
    -- playlist is structurally "linked to a local filter" the same way a
    -- Plex-linked playlist is "linked to a remote list."
    -- 'shuffle' extends the mode CHECK in the same migration since both
    -- touch playlist — a per-playlist shuffle option to match FillerList's
    -- existing advancement:'shuffle'|'sequential'.
    ALTER TABLE playlist ADD COLUMN membership  TEXT NOT NULL DEFAULT 'static' CHECK(membership IN ('static','smart'));
    ALTER TABLE playlist ADD COLUMN filter_expr TEXT NOT NULL DEFAULT '';
    ALTER TABLE playlist ADD COLUMN smart_type  TEXT NOT NULL DEFAULT 'movie' CHECK(smart_type IN ('show','movie'));
    ALTER TABLE playlist ADD COLUMN smart_sort  TEXT NOT NULL DEFAULT 'title';
    ALTER TABLE playlist ADD COLUMN smart_limit INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE playlist ADD COLUMN last_smart_refresh_at INTEGER;
)SQL" }

// Home shelves are just playlists with show_on_home=1 — reusing a playlist's
// existing title/membership/filter_expr/smart_type/smart_sort (migration 87)
// as the shelf's definition instead of a separate shelf-definition table/CRUD
// surface. home_tile_limit is a *display* cap (how many tiles before the
// "Continue in Library" end tile on the Home page) — independent of
// smart_limit, which caps actual playlist membership/scheduling; a playlist
// can have 200 members and still only show 16 on its home shelf.
// home_active_start/end (MM-DD, both empty = always shown) gate a shelf to a
// date window — e.g. a Halloween shelf only appearing Oct 1-31 — checked in
// C++ (wraparound-safe for a Dec-to-Jan window) rather than in SQL.
,{ 88, R"SQL(
    ALTER TABLE playlist ADD COLUMN show_on_home      INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE playlist ADD COLUMN home_order        INTEGER NOT NULL DEFAULT 0;
    ALTER TABLE playlist ADD COLUMN home_tile_limit   INTEGER NOT NULL DEFAULT 16;
    ALTER TABLE playlist ADD COLUMN home_active_start TEXT NOT NULL DEFAULT '';
    ALTER TABLE playlist ADD COLUMN home_active_end   TEXT NOT NULL DEFAULT '';
)SQL" }

// Playlist tile poster for the Library's new Playlists section — a plain
// pasted URL (same "Custom Poster URL" override convention shows/movies
// already use, served through the existing generic /api/images/proxy, no
// new upload infra). Empty = the frontend falls back to a client-side
// collage of the playlist's first few items' own posters.
,{ 89, R"SQL(
    ALTER TABLE playlist ADD COLUMN poster_source TEXT NOT NULL DEFAULT '';
)SQL" }

// ── v90: scraper-derived tags (TMDB keywords, AniList tags, etc.) — a JSON
//         array column mirroring `genres`, but distinct from `labels`
//         (source-synced Plex/Jellyfin user tags). Owned by ScraperManager's
//         applyShowMetadata/applyMovieMetadata like genres, filterable via
//         the generic isJsonField() path in FilterExpr.cpp.
,{ 90, R"SQL(
    ALTER TABLE show  ADD COLUMN tags TEXT NOT NULL DEFAULT '[]';
    ALTER TABLE movie ADD COLUMN tags TEXT NOT NULL DEFAULT '[]';
)SQL" }

}; // kMigrations

} // namespace

// ---------------------------------------------------------------------------

Database::Database(const std::string& path)
    : path_(path),
      db_([&]() -> std::string {
          auto parent = std::filesystem::path(path).parent_path();
          if (!parent.empty())
              std::filesystem::create_directories(parent);
          return path;
      }(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
{
    configure(db_);
    runMigrations();
    std::cout << "[db] opened " << path << '\n';
}

void Database::configure() { configure(db_); }

void Database::configure(SQLite::Database& db) const {
    db.exec("PRAGMA journal_mode = WAL");
    db.exec("PRAGMA foreign_keys = ON");
    db.exec("PRAGMA synchronous  = NORMAL");
    db.exec("PRAGMA cache_size   = -8000");
    // Long-running sync writes many rows on its own connection (see
    // openConnection's 60s timeout) while API-triggered writers (scraper
    // match/refresh, etc.) share this connection — 5s was too tight and
    // sync's post-pass would throw "database is locked" under contention.
    db.exec("PRAGMA busy_timeout = 20000");
}

SQLite::Database Database::openConnection(int busy_timeout_ms) const {
    SQLite::Database conn(path_, SQLite::OPEN_READWRITE);
    // Don't re-issue journal_mode=WAL on a secondary connection: the mode is
    // already set by the primary, and this pragma briefly requests a lock even
    // when it's a no-op — which can race with an active write transaction.
    conn.exec("PRAGMA foreign_keys = ON");
    conn.exec("PRAGMA synchronous  = NORMAL");
    conn.exec("PRAGMA cache_size   = -8000");
    conn.setBusyTimeout(busy_timeout_ms);
    return conn;
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
