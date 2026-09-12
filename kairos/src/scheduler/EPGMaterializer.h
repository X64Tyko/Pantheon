#pragma once
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "CursorState.h"
#include "RuleEngine.h"

class Database;

// Divergence: an item in the new projection that differs from what is currently
// committed in scheduled_program at the same wall-clock start time.
struct Divergence
{
	std::time_t wall_clock_start;
	std::time_t wall_clock_end;
	std::string block_id;
	std::string prev_item_type;
	std::string prev_item_id;
	std::string new_item_type;
	std::string new_item_id;
};

// Result of a generate() call. Passed to commit() to write to the database.
struct GenerateResult
{
	std::vector<ScheduledItem> items;
	std::vector<Divergence> divergences;
	CursorState cursor_state;
	std::map<std::time_t, std::string> anchors;
	std::vector<PlayRecord> play_records;
	std::vector<PlayRecord> filler_records;
};

class EPGMaterializer
{
public:
	EPGMaterializer(Database& db, RuleEngine& engine);

	// Pure in-memory projection from the Monday midnight anchor of `from`.
	// No DB writes. Divergences are populated by comparing new items against
	// what is currently in scheduled_program for the same channel/time range.
	GenerateResult generate(const std::string& channel_id,
							std::time_t from, int horizon_hours,
							int seed = -1);

	// Cheap divergence signal: re-derives the anchor for the week containing
	// `reference_time` by projecting forward from the *previous* week's stored
	// anchor, and compares it against what's currently stored for that week. A
	// mismatch means something changed (blocks, filler, the scheduling algorithm
	// itself) since that anchor was captured — before paying for a full
	// projection + item-by-item diff, this catches "has anything changed at all"
	// from just two small JSON snapshots. Read-only; no DB writes.
	// Returns nullopt when there's no previous-week anchor to verify against yet
	// (nothing generated for this channel that far back) — not itself a signal
	// of divergence, just "can't tell, go generate."
	std::optional<bool> checkAnchorDivergence(const std::string& channel_id,
											  std::time_t reference_time);

	// Commit a GenerateResult to scheduled_program. Replaces (deletes then
	// inserts), not appends, any not-yet-aired row from max(from, now) through horizon.
	void commit(const std::string& channel_id,
				std::time_t from,
				std::time_t horizon,
				GenerateResult& result);

	// Run a virtual projection over the past N weeks and commit only the
	// resulting cursor state and anchor snapshots — no scheduled_program rows
	// are written. Designed for new-channel seeding (called automatically on
	// create when pre_seed_weeks > 0) and explicit re-seed resets (triggered
	// via PATCH trigger_pre_seed=true after a hardReset). No-op if weeks <= 0.
	void preSeed(const std::string& channel_id, int weeks);

	// Ensure the schedule for `channel_id` is populated from `from` through
	// `from + horizon_hours`. Calls generate() + commit(), skipping the
	// re-projection if the horizon is already covered.
	void ensureScheduled(const std::string& channel_id,
						 std::time_t from, int horizon_hours,
						 int seed = -1);

	// Mark the earliest matching scheduled entry as aired.
	void notifyPlayed(const std::string& channel_id, const std::string& item_id);

	// XMLTV XML for all channels. Calls ensureScheduled() then reads the cache.
	// base_url (e.g. "http://host:port") is used to build each channel's
	// <icon src="base_url/api/channels/{id}/logo"/> when it has a logo_path
	// configured; pass "" to omit icons entirely.
	std::string generateXMLTV(int horizon_hours = 24, const std::string& base_url = "");

	// M3U channel list. Stream URLs point to base_url/channels/{id}/stream.
	std::string generateM3U(const std::string& base_url);

private:
	static std::string xmlEscape(const std::string& s);
	static std::string fmtXMLTVTime(std::time_t t);

	// Per-channel lock for ensureScheduled() — without it, concurrent
	// callers for the same channel (multiple simultaneous viewers hitting
	// /now, /next, /epg right as the horizon needs extending) can each
	// independently see "not yet covered" and all pay the full generate()
	// re-projection cost (increasingly expensive by Thursday/Friday — see
	// ensureScheduled()'s own comment) instead of one doing the work and the
	// rest finding it already covered once they get the lock. Keyed by
	// channel so unrelated channels never serialize against each other.
	// Entries are never removed — cardinality is bounded by the number of
	// channels that exist, not request volume.
	std::mutex& channelLock(const std::string& channel_id);
	std::mutex channel_locks_mtx_;
	std::map<std::string, std::unique_ptr<std::mutex>> channel_locks_;

	Database& db_;
	RuleEngine& engine_;
};