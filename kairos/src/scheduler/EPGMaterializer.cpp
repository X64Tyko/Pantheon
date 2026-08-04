#include "EPGMaterializer.h"
#include "CursorState.h"
#include "Rng.h"
#include "../db/ChannelRepository.h"
#include "../db/CursorRepository.h"
#include "../db/Database.h"
#include "../db/ScheduleRepository.h"
#include "metrics/OperationMetrics.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>

#include "RuntimeFlags.h"

EPGMaterializer::EPGMaterializer(Database& db, RuleEngine& engine)
	: db_(db)
	, engine_(engine)
{
}

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string EPGMaterializer::xmlEscape(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
	{
		switch (c)
		{
			case '&': out += "&amp;";
				break;
			case '<': out += "&lt;";
				break;
			case '>': out += "&gt;";
				break;
			case '"': out += "&quot;";
				break;
			default: out += c;
		}
	}
	return out;
}

std::string EPGMaterializer::fmtXMLTVTime(std::time_t t)
{
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	char buf[32];
	strftime(buf, sizeof(buf), "%Y%m%d%H%M%S +0000", &tm);
	return buf;
}

// ── Schedule cache management ─────────────────────────────────────────────────

GenerateResult EPGMaterializer::generate(
	const std::string& channel_id, std::time_t from, int horizon_hours, int seed)
{
	using json = nlohmann::json;

	const std::time_t horizon = from + static_cast<std::time_t>(horizon_hours) * 3600;
	// Timezone-aware — must agree exactly with project()'s own week-walking and
	// week-boundary anchor captures (RuleEngine::weekMondayForChannel). A naive
	// UTC-calendar-week boundary can land days away from this for a channel not
	// on UTC, silently missing (or colliding with) anchors project() wrote.
	const std::time_t week_monday = engine_.weekMondayForChannel(channel_id, from);

	int init_seed = seed;
	if (init_seed < 0)
	{
		SQLite::Statement qs(db_.get(), "SELECT seed FROM channel WHERE channel_id=?");
		qs.bind(1, channel_id);
		if (qs.executeStep()) init_seed = qs.getColumn(0).getInt();
	}

	Xoshiro256 rng(init_seed >= 0 ? static_cast<uint64_t>(init_seed) : 0ULL);
	CursorState cs;
	GenerateResult result;

	bool has_anchor = false;
	{
		SQLite::Statement qa(db_.get(),
							 "SELECT anchor_hashes FROM channel WHERE channel_id=?");
		qa.bind(1, channel_id);
		if (qa.executeStep() && !qa.getColumn(0).isNull())
		{
			try
			{
				auto aj  = json::parse(qa.getColumn(0).getString());
				auto key = std::to_string(week_monday);
				if (aj.contains(key) && aj[key].is_object())
				{
					const auto& snap = aj[key];
					has_anchor       = true;
					if (snap.contains("rng")) rng = Xoshiro256::deserialize(snap["rng"].get<std::string>());
					cs = CursorState::deserializeCursors(snap.dump());
				}
			}
			catch (...)
			{
			}
		}
	}

	// Bootstrap anchor captured in result.anchors for commit() to persist.
	if (!has_anchor)
	{
		result.anchors[week_monday] = json{
			{"rng", rng.serialize()},
			{"cursors", json::array()},
			{"block_states", json::array()}
		}.dump();
	}

	const int proj_hours = static_cast<int>((horizon - week_monday) / 3600) + 2;
	result.items         = engine_.project(channel_id, week_monday, proj_hours, cs, rng,
										   &result.anchors, &result.play_records, &result.filler_records);
	result.cursor_state = std::move(cs);

	// Detect divergences: new items that differ from what is currently committed.
	// Compares by time overlap rather than exact wall_clock_start equality — a
	// timing shift (block priority/window changes, different content picked
	// upstream, etc.) must still surface as a divergence rather than silently
	// missing because nothing lines up at the exact same instant. Both lists are
	// chronological (existing via ORDER BY, new items by construction from
	// project()), so a single forward-advancing pointer over `existing` is enough.
	{
		struct ExistingRow
		{
			std::time_t start, end;
			std::string type, id;
		};
		std::vector<ExistingRow> existing;
		SQLite::Statement eq(db_.get(), R"(
            SELECT wall_clock_start, wall_clock_end, item_type, item_id
            FROM scheduled_program
            WHERE channel_id = ?
              AND wall_clock_end   > ?
              AND wall_clock_start < ?
            ORDER BY wall_clock_start ASC
        )");
		eq.bind(1, channel_id);
		eq.bind(2, static_cast<int64_t>(from));
		eq.bind(3, static_cast<int64_t>(horizon));
		while (eq.executeStep())
			existing.push_back({
				static_cast<std::time_t>(eq.getColumn(0).getInt64()),
				static_cast<std::time_t>(eq.getColumn(1).getInt64()),
				eq.getColumn(2).getString(), eq.getColumn(3).getString()
			});

		size_t ei = 0;
		for (const auto& item : result.items)
		{
			std::time_t ws = item.wall_clock_start_ms / 1000;
			std::time_t we = item.wall_clock_end_ms / 1000;
			if (ws < from) continue;
			if (ws >= horizon) break;

			// Drop existing rows that ended before this item starts — both lists
			// are chronological, so they're never relevant to any later item either.
			while (ei < existing.size() && existing[ei].end <= ws) ++ei;

			const bool overlaps = ei < existing.size() &&
				existing[ei].start < we && existing[ei].end > ws;
			if (!overlaps)
			{
				// Nothing existing covers this stretch at all — a shifted timeline
				// or genuinely new content; still worth surfacing, not silence.
				result.divergences.push_back({
					ws, we, item.block_id,
					"", "", item.item_type, item.item_id
				});
			}
			else if (existing[ei].type != item.item_type || existing[ei].id != item.item_id)
			{
				result.divergences.push_back({
					ws, we, item.block_id,
					existing[ei].type, existing[ei].id,
					item.item_type, item.item_id
				});
			}
		}
	}

	if (epgDebug())
		std::cout << "[epg] generate() channel=" << channel_id
			<< " items=" << result.items.size()
			<< " divergences=" << result.divergences.size() << '\n';

	return result;
}

std::optional<bool> EPGMaterializer::checkAnchorDivergence(const std::string& channel_id,
														   std::time_t reference_time)
{
	using json = nlohmann::json;

	// Timezone-aware — see the comment in generate() on week_monday.
	std::time_t week_monday = engine_.weekMondayForChannel(channel_id, reference_time);
	std::time_t prev_monday = engine_.weekMondayForChannel(channel_id, week_monday - 86400);

	json anchors = json::object();
	{
		SQLite::Statement qa(db_.get(), "SELECT anchor_hashes FROM channel WHERE channel_id=?");
		qa.bind(1, channel_id);
		if (qa.executeStep() && !qa.getColumn(0).isNull())
		{
			try { anchors = json::parse(qa.getColumn(0).getString()); }
			catch (...) { return std::nullopt; }
		}
	}

	auto cur_key  = std::to_string(week_monday);
	auto prev_key = std::to_string(prev_monday);
	if (!anchors.contains(cur_key) || !anchors.contains(prev_key)) return std::nullopt;

	// Re-derive week_monday's anchor by projecting forward from prev_monday's
	// stored state — generate() is pure/read-only, so this is safe to call as a
	// probe with no side effects.
	auto probe = generate(channel_id, prev_monday, 7 * 24 + 2);
	auto it    = probe.anchors.find(week_monday);
	if (it == probe.anchors.end()) return std::nullopt;

	try
	{
		return json::parse(it->second) == anchors[cur_key];
	}
	catch (...)
	{
		return std::nullopt;
	}
}

void EPGMaterializer::commit(
	const std::string& channel_id, std::time_t from, std::time_t horizon, GenerateResult& result)
{
	using json = nlohmann::json;
	auto now   = static_cast<int64_t>(std::time(nullptr));

	auto persistAnchors = [&]()
	{
		if (result.anchors.empty()) return;
		json existing = json::object();
		{
			SQLite::Statement qa(db_.get(),
								 "SELECT anchor_hashes FROM channel WHERE channel_id=?");
			qa.bind(1, channel_id);
			if (qa.executeStep() && !qa.getColumn(0).isNull())
			{
				try { existing = json::parse(qa.getColumn(0).getString()); }
				catch (...)
				{
				}
			}
		}
		for (auto& [ts, snap_str] : result.anchors)
		{
			try { existing[std::to_string(ts)] = json::parse(snap_str); }
			catch (...)
			{
			}
		}
		SQLite::Statement upd(db_.get(),
							  "UPDATE channel SET anchor_hashes=? WHERE channel_id=?");
		upd.bind(1, existing.dump());
		upd.bind(2, channel_id);
		upd.exec();
	};

	db_.get().exec("SAVEPOINT sp_commit");

	// Everything below runs inside sp_commit. project() (in generate(), before
	// commit() is even called) reads blocks fresh from the DB, but a block can
	// still be deleted by a concurrent request in the gap between that read and
	// this insert — the block_id FK then aborts the statement. Without this
	// try/catch that exception unwound straight past RELEASE SAVEPOINT below,
	// leaving sp_commit open on this connection forever (SQLite has no
	// destructor-based savepoint guard), so every later commit on any channel
	// would start failing too. Roll back and release before rethrowing so a
	// stale reference only ever costs this one request.
	try
	{
		// Floored at `now`, not `from` — never touch something already airing or past.
		{
			std::time_t clear_from = std::max(from, static_cast<std::time_t>(now));
			SQLite::Statement del(db_.get(), R"(
                DELETE FROM scheduled_program
                 WHERE channel_id = ? AND status != 'aired'
                   AND wall_clock_start >= ? AND wall_clock_start < ?
            )");
			del.bind(1, channel_id);
			del.bind(2, static_cast<int64_t>(clear_from));
			del.bind(3, static_cast<int64_t>(horizon + 7200));
			del.exec();
		}

		SQLite::Statement ins(db_.get(), R"(
            INSERT OR IGNORE INTO scheduled_program
                (channel_id, block_id, item_type, item_id,
                 wall_clock_start, wall_clock_end, cursor_json, created_at, is_filler)
            VALUES (?,?,?,?,?,?,?,?,?)
        )");

		int inserted = 0, skipped = 0;
		for (const auto& item : result.items)
		{
			std::time_t item_end = item.wall_clock_end_ms / 1000;
			if (item_end > horizon + 7200) break;
			ins.bind(1, channel_id);
			if (item.block_id.empty()) ins.bind(2);
			else ins.bind(2, item.block_id);
			ins.bind(3, item.item_type);
			ins.bind(4, item.item_id);
			ins.bind(5, item.wall_clock_start_ms / 1000);
			ins.bind(6, item_end);
			ins.bind(7, item.cursor_json);
			ins.bind(8, now);
			ins.bind(9, item.is_filler ? 1 : 0);
			try
			{
				ins.exec();
				if (db_.get().getChanges() > 0) ++inserted;
				else ++skipped;
			}
			catch (const std::exception& e)
			{
				// Most likely a block deleted out from under this projection —
				// drop just this item rather than failing the whole channel's EPG.
				std::cerr << "[epg] commit() skipping item channel=" << channel_id
					<< " block=" << item.block_id << " item=" << item.item_id
					<< ": " << e.what() << '\n';
				++skipped;
			}
			// tryReset(), not reset() — after a failed exec(), sqlite3_reset()
			// re-surfaces that same error code, and Statement::reset() throws
			// on a non-OK code, which would silently defeat the catch above
			// by re-throwing right past it once every loop iteration hit a
			// real constraint violation instead of just resetting cleanly.
			ins.tryReset();
		}
		CursorRepository(db_).apply(channel_id, result.cursor_state);

		ScheduleRepository sched_repo(db_);
		for (const auto& r : result.play_records) sched_repo.recordScheduledPlayHistory(r.item_type, r.item_id, r.channel_id, r.block_id, r.aired_at);
		for (const auto& r : result.filler_records) sched_repo.recordScheduledFillerHistory(r.item_id, r.channel_id, r.block_id, r.aired_at);

		db_.get().exec("RELEASE SAVEPOINT sp_commit");

		if (epgDebug())
			std::cout << "[epg] commit() channel=" << channel_id
				<< " inserted=" << inserted << " skipped=" << skipped << '\n';
	}
	catch (...)
	{
		try { db_.get().exec("ROLLBACK TO SAVEPOINT sp_commit; RELEASE SAVEPOINT sp_commit"); }
		catch (...)
		{
		}
		throw;
	}

	persistAnchors();
}

void EPGMaterializer::preSeed(const std::string& channel_id, int weeks)
{
	using json = nlohmann::json;

	if (weeks <= 0) return;

	std::lock_guard<std::mutex> channel_guard(channelLock(channel_id));

	auto now   = std::time(nullptr);
	auto start = now - static_cast<std::time_t>(weeks) * 7 * 24 * 3600;
	// Slightly over-horizon so the final anchor snapshot is captured.
	int horizon_hours = weeks * 7 * 24 + 2;

	auto result = generate(channel_id, start, horizon_hours);

	// Persist only cursor state and anchors — no scheduled_program rows.
	CursorRepository(db_).apply(channel_id, result.cursor_state);

	if (!result.anchors.empty())
	{
		json existing = json::object();
		{
			SQLite::Statement qa(db_.get(),
								 "SELECT anchor_hashes FROM channel WHERE channel_id=?");
			qa.bind(1, channel_id);
			if (qa.executeStep() && !qa.getColumn(0).isNull())
			{
				try { existing = json::parse(qa.getColumn(0).getString()); }
				catch (...) {}
			}
		}
		for (auto& [ts, snap_str] : result.anchors)
		{
			try { existing[std::to_string(ts)] = json::parse(snap_str); }
			catch (...) {}
		}
		SQLite::Statement upd(db_.get(),
							  "UPDATE channel SET anchor_hashes=? WHERE channel_id=?");
		upd.bind(1, existing.dump());
		upd.bind(2, channel_id);
		upd.exec();
	}

	if (epgDebug())
		std::cout << "[epg] preSeed() channel=" << channel_id
			<< " weeks=" << weeks << '\n';
}

std::mutex& EPGMaterializer::channelLock(const std::string& channel_id)
{
	std::lock_guard<std::mutex> g(channel_locks_mtx_);
	auto& slot = channel_locks_[channel_id];
	if (!slot) slot = std::make_unique<std::mutex>();
	return *slot;
}

void EPGMaterializer::ensureScheduled(const std::string& channel_id,
									  std::time_t from, int horizon_hours,
									  int seed)
{
	// Serializes concurrent callers for this one channel — see
	// channelLock()'s own comment. A caller that loses the race just finds
	// the horizon already covered (scheduled mode) or re-does a now-cheap
	// regenerate against the cursor state the winner just committed
	// (on_play mode) once it gets the lock, instead of duplicating the work
	// in parallel.
	std::lock_guard<std::mutex> channel_guard(channelLock(channel_id));

	// ── on_play mode: regenerate from current cursor position on every call. ──
	{
		bool on_play = ChannelRepository(db_).getAdvanceMode(channel_id) == "on_play";
		if (on_play)
		{
			auto now_ts = static_cast<int64_t>(std::time(nullptr));
			{
				SQLite::Statement d1(db_.get(),
									 "DELETE FROM scheduled_program WHERE channel_id=?");
				d1.bind(1, channel_id);
				d1.exec();
			}
			CursorState cs = CursorRepository(db_).load(channel_id);
			Xoshiro256 onplay_rng(seed >= 0 ? static_cast<uint64_t>(seed) : 0);
			auto items = engine_.project(channel_id, from, horizon_hours, cs, onplay_rng);

			db_.get().exec("SAVEPOINT sp_ens");
			SQLite::Statement ins(db_.get(), R"(
                INSERT OR IGNORE INTO scheduled_program
                    (channel_id, block_id, item_type, item_id,
                     wall_clock_start, wall_clock_end, cursor_json, created_at, is_filler)
                VALUES (?,?,?,?,?,?,?,?,?)
            )");
			for (const auto& item : items)
			{
				ins.bind(1, channel_id);
				if (item.block_id.empty()) ins.bind(2);
				else ins.bind(2, item.block_id);
				ins.bind(3, item.item_type);
				ins.bind(4, item.item_id);
				ins.bind(5, item.wall_clock_start_ms / 1000);
				ins.bind(6, item.wall_clock_end_ms / 1000);
				ins.bind(7, item.cursor_json);
				ins.bind(8, now_ts);
				ins.bind(9, item.is_filler ? 1 : 0);
				ins.exec();
				ins.reset();
			}
			db_.get().exec("RELEASE SAVEPOINT sp_ens");

			if (epgDebug())
				std::cout << "[epg] ensureScheduled on_play channel=" << channel_id
					<< " => " << items.size() << " items\n";
			return;
		}
	}

	// ── scheduled mode ───────────────────────────────────────────────────────
	const std::time_t horizon = from + static_cast<std::time_t>(horizon_hours) * 3600;

	// generate() always re-projects the whole week from Monday through
	// `horizon` (see below), not just this call's small window — cheap early
	// in the week, increasingly expensive by Thursday/Friday. ensureScheduled
	// is called on every /now poll (live playback, previews), so without this
	// check a channel that's already fully scheduled through `horizon` pays
	// that full re-projection cost on every single poll for no reason —
	// commit() would just re-INSERT OR IGNORE rows that already exist.
	{
		SQLite::Statement chk(db_.get(),
							  "SELECT 1 FROM scheduled_program WHERE channel_id=? AND wall_clock_end >= ? LIMIT 1");
		chk.bind(1, channel_id);
		chk.bind(2, static_cast<int64_t>(horizon));
		if (chk.executeStep())
		{
			if (epgDebug())
				std::cout << "[epg] ensureScheduled channel=" << channel_id
					<< " already covers horizon=" << horizon << ", skipping generate()\n";
			return;
		}
	}

	if (epgDebug())
		std::cout << "[epg] ensureScheduled channel=" << channel_id
			<< " from=" << from << " horizon_hours=" << horizon_hours << '\n';

	// Only this branch — the actual regenerate — is worth structured timing.
	// The horizon-covered fast path above (the common case, hit on every
	// /now poll) returns before this point, so this recorder only ever fires
	// on a genuine cache miss, never on the hot poll path.
	OperationRecorder op_rec("epg.generate");
	auto result = generate(channel_id, from, horizon_hours, seed);
	if (result.items.empty())
	{
		std::cout << "[epg] WARNING: generate() returned 0 items for channel="
			<< channel_id << " — EPG will be empty\n";
		return;
	}
	commit(channel_id, from, horizon, result);
}

void EPGMaterializer::notifyPlayed(const std::string& channel_id,
								   const std::string& item_id)
{
	// Mark the earliest scheduled occurrence of this item as aired.
	SQLite::Statement q(db_.get(), R"(
        UPDATE scheduled_program SET status = 'aired'
        WHERE id = (
            SELECT id FROM scheduled_program
            WHERE channel_id = ? AND item_id = ? AND status = 'scheduled'
            ORDER BY wall_clock_start ASC LIMIT 1
        )
    )");
	q.bind(1, channel_id);
	q.bind(2, item_id);
	q.exec();
}

// ── XMLTV ─────────────────────────────────────────────────────────────────────

std::string EPGMaterializer::generateXMLTV(int horizon_hours, const std::string& base_url)
{
	struct Chan
	{
		std::string id, name, logo_path;
		int number;
	};
	std::vector<Chan> channels;
	{
		// Always-public/unauthenticated route (real IPTV/XMLTV clients) — a
		// guest's throwaway demo channel (is_demo=1) must never appear in the
		// real lineup; a real viewer's channel (is_demo=0) is a full lineup
		// citizen, included here same as an admin-created one.
		SQLite::Statement q(db_.get(),
							"SELECT channel_id, name, number, logo_path FROM channel WHERE is_demo = 0 ORDER BY number");
		while (q.executeStep())
			channels.push_back({
				q.getColumn(0).getString(),
				q.getColumn(1).getString(),
				q.getColumn(3).getString(),
				q.getColumn(2).getInt()
			});
	}

	std::time_t now            = std::time(nullptr);
	std::time_t today_midnight = (now / 86400) * 86400;
	std::time_t horizon        = now + static_cast<std::time_t>(horizon_hours) * 3600;

	if (epgDebug())
		std::cout << "[epg] generateXMLTV: " << channels.size()
			<< " channel(s), horizon_hours=" << horizon_hours << '\n';

	// Extend cache from today midnight so programs that already aired today are
	// present in scheduled_program and appear in the XMLTV output.  Callers that
	// schedule from `now` leave a gap from midnight to the current moment.
	int hours_from_midnight = static_cast<int>((horizon - today_midnight) / 3600) + 1;
	for (const auto& ch : channels) ensureScheduled(ch.id, today_midnight, hours_from_midnight);

	std::ostringstream xml;
	xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		<< "<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n"
		<< "<tv source-info-name=\"Kairos\" generator-info-name=\"Kairos\">\n";

	for (const auto& ch : channels)
	{
		xml << "  <channel id=\"kairos-" << ch.number << "\">\n"
			<< "    <display-name>" << xmlEscape(ch.name) << "</display-name>\n";
		if (!ch.logo_path.empty() && !base_url.empty()) xml << "    <icon src=\"" << xmlEscape(base_url + "/api/channels/" + ch.id + "/logo") << "\"/>\n";
		xml << "  </channel>\n";
	}

	for (const auto& ch : channels)
	{
		// Filler items (is_filler=1) are excluded from XMLTV output; instead,
		// each content item's stop time is extended to the next content item's
		// start time (absorbing the filler gap). LEAD() finds that next start.
		// The +7200s cap prevents runaway expansion across long inter-block gaps.
		SQLite::Statement q(db_.get(), R"(
            WITH content AS (
                SELECT sp.item_type, sp.item_id,
                       sp.wall_clock_start,
                       sp.wall_clock_end,
                       LEAD(sp.wall_clock_start) OVER (
                           PARTITION BY sp.channel_id ORDER BY sp.wall_clock_start
                       ) AS next_content_start
                FROM scheduled_program sp
                WHERE sp.channel_id = ?
                  AND sp.is_filler = 0
                  AND sp.wall_clock_end   >  ?
                  AND sp.wall_clock_start <  ?
                  AND sp.status != 'skipped'
            )
            SELECT c.item_type,
                   c.wall_clock_start,
                   CASE WHEN c.next_content_start IS NOT NULL
                             AND c.next_content_start <= c.wall_clock_end + 7200
                        THEN c.next_content_start
                        ELSE c.wall_clock_end
                   END AS stop_time,
                   COALESCE(e.title,    m.title,    '')  AS item_title,
                   COALESCE(s.title,    '')              AS show_title,
                   COALESCE(e.season,   0)               AS season,
                   COALESCE(e.episode,  0)               AS ep_num,
                   COALESCE(e.overview, m.overview, '')  AS description
            FROM content c
            LEFT JOIN episode e ON c.item_type = 'episode' AND c.item_id = e.episode_id
            LEFT JOIN show    s ON c.item_type = 'episode' AND e.show_id  = s.show_id
            LEFT JOIN movie   m ON c.item_type = 'movie'   AND c.item_id = m.movie_id
            ORDER BY c.wall_clock_start
        )");
		q.bind(1, ch.id);
		q.bind(2, static_cast<int64_t>(today_midnight));
		q.bind(3, static_cast<int64_t>(horizon));

		int prog_count = 0;
		while (q.executeStep())
		{
			++prog_count;
			std::time_t start       = static_cast<std::time_t>(q.getColumn(1).getInt64());
			std::time_t stop        = static_cast<std::time_t>(q.getColumn(2).getInt64());
			std::string item_title  = q.getColumn(3).getString();
			std::string show_title  = q.getColumn(4).getString();
			int season              = q.getColumn(5).getInt();
			int ep_num              = q.getColumn(6).getInt();
			std::string description = q.getColumn(7).getString();

			std::string display_title = show_title.empty() ? item_title : show_title;

			xml << "  <programme"
				<< " start=\"" << fmtXMLTVTime(start) << "\""
				<< " stop=\"" << fmtXMLTVTime(stop) << "\""
				<< " channel=\"kairos-" << ch.number << "\">\n"
				<< "    <title lang=\"en\">" << xmlEscape(display_title) << "</title>\n";

			if (!show_title.empty())
			{
				xml << "    <sub-title lang=\"en\">"
					<< xmlEscape(item_title) << "</sub-title>\n";
				if (season > 0 && ep_num > 0)
				{
					xml << "    <episode-num system=\"xmltv_ns\">"
						<< (season - 1) << "." << (ep_num - 1) << ".0/1"
						<< "</episode-num>\n"
						<< "    <episode-num system=\"onscreen\">"
						<< "S" << season << "E" << ep_num
						<< "</episode-num>\n";
				}
			}

			if (!description.empty()) xml << "    <desc lang=\"en\">" << xmlEscape(description) << "</desc>\n";

			xml << "  </programme>\n";
		}
		if (epgDebug())
			std::cout << "[epg]   XMLTV channel=" << ch.id
				<< " programmes=" << prog_count << '\n';
		else if (prog_count == 0)
			std::cout << "[epg] WARNING: channel=" << ch.id
				<< " (" << ch.name << ") has 0 programmes in XMLTV window\n";
	}

	xml << "</tv>\n";
	return xml.str();
}

// ── M3U ──────────────────────────────────────────────────────────────────────

std::string EPGMaterializer::generateM3U(const std::string& base_url)
{
	// See generateXMLTV's identical filter above — a demo channel must never
	// reach a real IPTV client's lineup.
	SQLite::Statement q(db_.get(),
						"SELECT channel_id, name, number, logo_path FROM channel WHERE is_demo = 0 ORDER BY number");

	std::ostringstream m3u;
	m3u << "#EXTM3U\n";

	while (q.executeStep())
	{
		std::string id        = q.getColumn(0).getString();
		std::string name      = q.getColumn(1).getString();
		int num               = q.getColumn(2).getInt();
		std::string logo_path = q.getColumn(3).getString();
		std::string logo_url  = logo_path.empty() ? "" : base_url + "/api/channels/" + id + "/logo";

		m3u << "#EXTINF:-1"
			<< " tvg-id=\"kairos-" << num << "\""
			<< " tvg-name=\"" << name << "\""
			<< " tvg-logo=\"" << logo_url << "\""
			<< " group-title=\"Kairos\""
			<< " channel-number=\"" << num << "\""
			<< "," << name << "\n"
			<< base_url << "/channels/" << id << "/stream\n";
	}
	return m3u.str();
}