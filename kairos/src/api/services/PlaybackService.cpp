#include "PlaybackService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../conf/ConfStore.h"
#include "../../db/Database.h"
#include "../../db/WatchProgressRepository.h"
#include "../../db/PlaybackHistoryRepository.h"
#include "../../db/ContentRepository.h"
#include "../../db/SubtitleTrackRepository.h"
#include "../../db/ShowTrackPreferenceRepository.h"
#include "../../db/MovieTrackPreferenceRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <vector>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

PlaybackService::PlaybackService(const ServiceContext& ctx)
	: db_(ctx.db)
	, conf_(ctx.conf)
{
}

namespace
{
	bool validContentType(const std::string& t) { return t == "movie" || t == "episode"; }

	// Best-effort title lookup for the playback_history row — kept a plain
	// inline query here (not a ContentRepository method) since it's only ever
	// needed by this one write path and movie/episode titles don't share a
	// column name the way GET /api/watch-progress's own two separate lookups
	// above already don't either. Empty on any failure; a missing title in the
	// history view is cosmetic, never worth failing the ping over.
	std::string lookupTitle(Database& db, const std::string& content_type, const std::string& content_id)
	{
		try
		{
			const char* sql = content_type == "movie"
								  ? "SELECT title FROM movie WHERE movie_id = ?"
								  : content_type == "channel"
								  ? "SELECT name FROM channel WHERE channel_id = ?"
								  : "SELECT title FROM episode WHERE episode_id = ?";
			SQLite::Statement q(db.get(), sql);
			q.bind(1, content_id);
			if (q.executeStep()) return q.getColumn(0).getString();
		}
		catch (...)
		{
		}
		return "";
	}
} // namespace

void PlaybackService::registerRoutes(httplib::Server& svr)
{
	// ── GET /api/playback/:content_type/:id ───────────────────────────────────
	// Internal — called by Hephaestus (not the browser) to resolve a library
	// item to a playable file. Exempted from auth in Router.cpp's isPublicPath,
	// same bucket as /now and /played, but if Hephaestus forwards the original
	// caller's bearer token, currentUser() still resolves it (see Router.cpp's
	// set_pre_routing_handler) — used below to surface this user's sticky
	// per-show track preference (see migration v92), so a viewer's own client
	// never has to pass show_id or a saved preference through itself.
	svr.Get(R"(/api/playback/([^/]+)/(.+))", [this](const Req& req, Res& res)
	{
		auto content_type = req.matches[1].str();
		auto id           = req.matches[2].str();
		if (!validContentType(content_type))
		{
			route::err(res, 400, "content_type must be movie or episode");
			return;
		}

		try
		{
			// A linked special (episode.linked_movie_id set — see ScraperManager's
			// specials linking) has no file of its own; its file_path/duration_ms
			// must live-resolve through the movie it's linked to, never a stale
			// copy, so a re-scanned/moved movie file is always reflected here.
			const char* sql = content_type == "movie"
								  ? "SELECT file_path, duration_ms, title, '', '', keyframes_ms, keyframes_size, keyframes_mtime "
								  "FROM movie WHERE movie_id = ?"
								  : R"(
					SELECT COALESCE(m.file_path, e.file_path),
					       COALESCE(m.duration_ms, e.duration_ms),
					       e.title,
					       COALESCE(e.linked_movie_id, ''),
					       e.show_id,
					       COALESCE(m.keyframes_ms, e.keyframes_ms),
					       COALESCE(m.keyframes_size, e.keyframes_size),
					       COALESCE(m.keyframes_mtime, e.keyframes_mtime)
					FROM episode e LEFT JOIN movie m ON m.movie_id = e.linked_movie_id
					WHERE e.episode_id = ?
				)";
			SQLite::Statement q(db_.get(), sql);
			q.bind(1, id);
			if (!q.executeStep())
			{
				route::err(res, 404, "not found");
				return;
			}

			auto file_path = q.getColumn(0).getString();
			if (file_path.empty())
			{
				route::err(res, 404, "no file for this item");
				return;
			}
			// Only file_path.empty() was ever checked here — a non-empty path
			// that doesn't actually resolve (bad/absent path_map, unmounted
			// share) got handed to Hephaestus with total confidence,
			// guaranteeing a downstream ffmpeg spawn failure with no
			// diagnostic short of its own stderr. mapped is reused below
			// instead of calling applyPathMap a second time.
			std::string mapped = conf_.applyPathMap(file_path);
			if (!std::filesystem::exists(mapped))
			{
				std::cerr << "[playback] file missing on disk for " << content_type << " " << id
					<< ": " << mapped << "\n";
				route::err(res, 404, "media file not found on disk");
				return;
			}

			// subtitle_track rows don't share a column across movie/episode the
			// way file_path/duration_ms do above (COALESCE works there because
			// both sides read the same column name) — this is a key switch
			// instead: a linked special's external subtitles live under the
			// movie it's linked to, not the episode row itself.
			const auto linked_movie_id       = q.getColumn(3).getString();
			const std::string sub_media_type = (content_type == "movie" || !linked_movie_id.empty()) ? "movie" : "episode";
			const std::string sub_media_id   = content_type == "movie" ? id : (!linked_movie_id.empty() ? linked_movie_id : id);

			json external_subtitles = json::array();
			for (const auto& t : SubtitleTrackRepository(db_).get(sub_media_type, sub_media_id))
			{
				external_subtitles.push_back({
					{"file_path", conf_.applyPathMap(t.file_path)},
					{"language", t.language},
					{"forced", t.forced},
					{"sdh", t.sdh},
					{"title", t.title},
				});
			}

			// Resolution order: item-specific preference (movie_track_preference
			// / show_track_preference, settable from the detail page or by
			// switching tracks mid-playback) first, then the user's library-wide
			// default (migration v94) fills in whichever side that left unset —
			// e.g. a user who's only ever set a global "always Danish audio"
			// default, never touched this specific show, still gets it honored.
			std::string preferred_audio_lang, preferred_subtitle_lang;
			const auto show_id = q.getColumn(4).getString();
			if (auto user = currentUser())
			{
				if (content_type == "movie")
				{
					if (auto pref = MovieTrackPreferenceRepository(db_).get(user->user_id, id))
					{
						preferred_audio_lang    = pref->audio_lang;
						preferred_subtitle_lang = pref->subtitle_lang;
					}
				}
				else if (!show_id.empty())
				{
					if (auto pref = ShowTrackPreferenceRepository(db_).get(user->user_id, show_id))
					{
						preferred_audio_lang    = pref->audio_lang;
						preferred_subtitle_lang = pref->subtitle_lang;
					}
				}
				if (preferred_audio_lang.empty()) preferred_audio_lang = user->default_audio_lang;
				if (preferred_subtitle_lang.empty()) preferred_subtitle_lang = user->default_subtitle_lang;
			}

			// Cached direct-stream keyframe data from the sync-time probe (see
			// Database.cpp's v98 migration and SyncManager::syncMediaProbeFromFiles)
			// — lets Hephaestus skip its own full-file ffprobe scan on playback
			// start when this is present and still matches the file's current
			// size/mtime. keyframes_ms is stored as a JSON array string; an empty/
			// unparseable value (never probed yet) surfaces as [], which
			// Hephaestus treats the same as "no cached data."
			json keyframes_ms = json::array();
			try { keyframes_ms = json::parse(q.getColumn(5).getString()); }
			catch (...)
			{
			}

			route::ok(res, json{
						  {"file_path", mapped},
						  {"duration_ms", q.getColumn(1).getInt64()},
						  {"title", q.getColumn(2).getString()},
						  {"external_subtitles", external_subtitles},
						  {"preferred_audio_lang", preferred_audio_lang},
						  {"preferred_subtitle_lang", preferred_subtitle_lang},
						  {"keyframes_ms", keyframes_ms},
						  {"keyframes_size", q.getColumn(6).getInt64()},
						  {"keyframes_mtime", q.getColumn(7).getInt64()},
					  }.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/playback/:content_type/:id", e);
			route::err(res, 500, e.what());
		}
	});

	// ── PUT /api/playback/:content_type/:id/keyframes ─────────────────────────
	// Internal — Hephaestus calls this after its own fallback ffprobe keyframe
	// scan (the GET route above returned no cached keyframes_ms, or Hephaestus's
	// own re-stat of the file no longer matched keyframes_size/keyframes_mtime
	// — see VodSession::computeSegmentBoundaries()), so the next direct-stream
	// session on this file doesn't pay for that scan again. Gated on the
	// shared internal secret in Router.cpp's pre-routing handler, same bucket
	// as /played — this mutates library metadata with no end-user session to
	// check, and isPublicPath's blanket "/api/playback/" exemption would
	// otherwise leave it wide open. Body: {"keyframes_ms": [0, 4004, ...],
	// "size": 123, "mtime": 456}.
	svr.Put(R"(/api/playback/([^/]+)/(.+)/keyframes)", [this](const Req& req, Res& res)
	{
		auto content_type = req.matches[1].str();
		auto id           = req.matches[2].str();
		if (!validContentType(content_type))
		{
			route::err(res, 400, "content_type must be movie or episode");
			return;
		}

		try
		{
			json body = json::parse(req.body);
			if (!body.contains("keyframes_ms") || !body["keyframes_ms"].is_array())
			{
				route::err(res, 400, "keyframes_ms must be an array");
				return;
			}
			const std::string keyframes_ms = body["keyframes_ms"].dump();
			const int64_t size             = body.value("size", int64_t(0));
			const int64_t mtime            = body.value("mtime", int64_t(0));

			// A linked special (episode.linked_movie_id set) has no file of its
			// own — its keyframe data lives on the movie it's linked to, same
			// resolution the GET handler above does for file_path/duration_ms.
			std::string target_type = content_type, target_id = id;
			if (content_type == "episode")
			{
				SQLite::Statement lq(db_.get(), "SELECT linked_movie_id FROM episode WHERE episode_id = ?");
				lq.bind(1, id);
				if (lq.executeStep())
				{
					auto linked = lq.getColumn(0).getString();
					if (!linked.empty())
					{
						target_type = "movie";
						target_id   = linked;
					}
				}
			}

			const char* sql = target_type == "movie"
								  ? "UPDATE movie SET keyframes_ms = ?, keyframes_size = ?, keyframes_mtime = ? WHERE movie_id = ?"
								  : "UPDATE episode SET keyframes_ms = ?, keyframes_size = ?, keyframes_mtime = ? WHERE episode_id = ?";
			SQLite::Statement upd(db_.get(), sql);
			upd.bind(1, keyframes_ms);
			upd.bind(2, size);
			upd.bind(3, mtime);
			upd.bind(4, target_id);
			upd.exec();

			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PUT /api/playback/:content_type/:id/keyframes", e);
			route::err(res, 500, e.what());
		}
	});

	// ── PUT /api/episodes/:id/track-preference ────────────────────────────────
	// Saves a Plex-style sticky per-show audio/subtitle language — resolves
	// episode->show_id server-side (see ContentRepository::getEpisode) so
	// neither client needs to know or plumb show_id. Body: any of
	// {"audio_lang": "eng", "subtitle_lang": "spa"} (a field absent/omitted
	// leaves that side untouched — see ShowTrackPreferenceRepository::set).
	svr.Put(R"(/api/episodes/(.+)/track-preference)", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}

		try
		{
			auto episode = ContentRepository(db_).getEpisode(req.matches[1].str());
			if (!episode)
			{
				route::err(res, 404, "not found");
				return;
			}

			auto b = json::parse(req.body);
			std::optional<std::string> audio_lang, subtitle_lang;
			if (b.contains("audio_lang")) audio_lang = b.at("audio_lang").get<std::string>();
			if (b.contains("subtitle_lang")) subtitle_lang = b.at("subtitle_lang").get<std::string>();

			auto now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
			ShowTrackPreferenceRepository(db_).set(user->user_id, episode->show_id, audio_lang, subtitle_lang, now_ms);

			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PUT /api/episodes/:id/track-preference", e);
			route::err(res, 400, e.what());
		}
	});

	// ── Show/movie track preference, direct by id ─────────────────────────────
	// The episode-indirection route above is what a switch mid-playback
	// actually calls (all it has on hand is the episode being watched); these
	// let the detail page read/write the same show_track_preference row
	// directly by show_id, so a preference can be set before ever pressing
	// play at all — the whole reason this pair exists.
	svr.Get(R"(/api/shows/(.+)/track-preference)", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		auto pref = ShowTrackPreferenceRepository(db_).get(user->user_id, req.matches[1].str());
		route::ok(res, json{
					  {"audio_lang", pref ? pref->audio_lang : ""},
					  {"subtitle_lang", pref ? pref->subtitle_lang : ""},
				  }.dump());
	});

	svr.Put(R"(/api/shows/(.+)/track-preference)", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		try
		{
			auto b = json::parse(req.body);
			std::optional<std::string> audio_lang, subtitle_lang;
			if (b.contains("audio_lang")) audio_lang = b.at("audio_lang").get<std::string>();
			if (b.contains("subtitle_lang")) subtitle_lang = b.at("subtitle_lang").get<std::string>();
			auto now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
			ShowTrackPreferenceRepository(db_).set(user->user_id, req.matches[1].str(), audio_lang, subtitle_lang, now_ms);
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PUT /api/shows/:id/track-preference", e);
			route::err(res, 400, e.what());
		}
	});

	// Movie counterpart — see MovieTrackPreferenceRepository's own comment
	// for why this is a separate table from show_track_preference rather
	// than a media_type column on one shared table (matches this codebase's
	// existing show/movie split everywhere else, e.g. subtitle_track uses
	// media_type+media_id instead — the two conventions coexist for
	// different reasons, this one following the narrower, older precedent).
	svr.Get(R"(/api/movies/(.+)/track-preference)", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		auto pref = MovieTrackPreferenceRepository(db_).get(user->user_id, req.matches[1].str());
		route::ok(res, json{
					  {"audio_lang", pref ? pref->audio_lang : ""},
					  {"subtitle_lang", pref ? pref->subtitle_lang : ""},
				  }.dump());
	});

	svr.Put(R"(/api/movies/(.+)/track-preference)", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		try
		{
			auto b = json::parse(req.body);
			std::optional<std::string> audio_lang, subtitle_lang;
			if (b.contains("audio_lang")) audio_lang = b.at("audio_lang").get<std::string>();
			if (b.contains("subtitle_lang")) subtitle_lang = b.at("subtitle_lang").get<std::string>();
			auto now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
			MovieTrackPreferenceRepository(db_).set(user->user_id, req.matches[1].str(), audio_lang, subtitle_lang, now_ms);
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PUT /api/movies/:id/track-preference", e);
			route::err(res, 400, e.what());
		}
	});

	// ── GET /api/watch-progress ────────────────────────────────────────────────
	// Movies: one card per in-progress movie, same as always. Shows: one card
	// per show, from that show's most-recently-touched episode. If that
	// episode was completed (finished naturally, skipped, or auto-advanced
	// past via the player's Up Next), the show only stays in Continue
	// Watching when there's a next episode to resume into — surfaced as a
	// fresh, unstarted "up next" card (position_ms=0, up_next=true) rather
	// than the show just vanishing until the viewer manually starts it again.
	// Mirrors resolvePlayTarget.ts's client-side resume logic, computed here
	// once per show for the shelf instead of on demand.
	svr.Get("/api/watch-progress", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}

		int limit = 24;
		if (auto it = req.params.find("limit"); it != req.params.end())
		{
			try { limit = std::clamp(std::stoi(it->second), 1, 100); }
			catch (...)
			{
			}
		}

		try
		{
			json out = json::array();

			// completed is the rewatch count (see WatchProgressRepository.h), not a
			// 0/1 flag that resets when a rewatch starts — "still in progress" is
			// instead read straight off position vs duration, which is correct
			// whether this is a first watch or a currently-mid rewatch of
			// something finished before.
			SQLite::Statement movies(db_.get(), R"SQL(
				SELECT content_id, position_ms, duration_ms, updated_at
				FROM watch_progress WHERE user_id = ? AND content_type = 'movie'
					AND duration_ms > 0 AND position_ms < duration_ms
			)SQL");
			movies.bind(1, user->user_id);
			while (movies.executeStep())
			{
				auto content_id = movies.getColumn(0).getString();
				SQLite::Statement m(db_.get(), "SELECT title FROM movie WHERE movie_id = ?");
				m.bind(1, content_id);
				if (!m.executeStep()) continue; // stale reference (deleted item)

				json r;
				r["content_type"] = "movie";
				r["content_id"]   = content_id;
				r["position_ms"]  = movies.getColumn(1).getInt64();
				r["duration_ms"]  = movies.getColumn(2).getInt64();
				r["updated_at"]   = movies.getColumn(3).getInt64();
				r["title"]        = m.getColumn(0).getString();
				out.push_back(std::move(r));
			}

			SQLite::Statement showsQ(db_.get(), R"SQL(
				SELECT DISTINCT e.show_id
				FROM watch_progress wp JOIN episode e ON e.episode_id = wp.content_id
				WHERE wp.user_id = ? AND wp.content_type = 'episode'
			)SQL");
			showsQ.bind(1, user->user_id);
			std::vector<std::string> show_ids;
			while (showsQ.executeStep()) show_ids.push_back(showsQ.getColumn(0).getString());

			WatchProgressRepository wpRepo(db_);
			ContentRepository contentRepo(db_);
			for (auto& show_id : show_ids)
			{
				auto state = wpRepo.getLatestForShow(user->user_id, show_id);
				if (!state) continue;

				auto content_id  = state->content_id;
				auto position_ms = state->position_ms;
				auto duration_ms = state->duration_ms;
				bool up_next     = false;

				if (state->completed)
				{
					auto next = contentRepo.getNextEpisode(state->content_id);
					if (!next) continue; // finale watched — nothing to continue into
					content_id  = next->episode_id;
					position_ms = 0;
					duration_ms = next->duration_ms;
					up_next     = true;
				}

				SQLite::Statement e(db_.get(), "SELECT title, season, episode FROM episode WHERE episode_id = ?");
				e.bind(1, content_id);
				if (!e.executeStep()) continue;

				json r;
				r["content_type"] = "episode";
				r["content_id"]   = content_id;
				r["position_ms"]  = position_ms;
				r["duration_ms"]  = duration_ms;
				// Kept as the completed episode's timestamp (not "now") when
				// synthesizing an up_next card, so shelf ordering still
				// reflects how recently the show was actually watched.
				r["updated_at"] = state->updated_at;
				r["title"]      = e.getColumn(0).getString();
				r["season"]     = e.getColumn(1).getInt();
				r["episode"]    = e.getColumn(2).getInt();
				r["show_id"]    = show_id;
				if (up_next) r["up_next"] = true;

				SQLite::Statement s(db_.get(), "SELECT title FROM show WHERE show_id = ?");
				s.bind(1, show_id);
				r["show_title"] = s.executeStep() ? s.getColumn(0).getString() : "";

				out.push_back(std::move(r));
			}

			std::sort(out.begin(), out.end(), [](const json& a, const json& b)
			{
				return a["updated_at"].get<int64_t>() > b["updated_at"].get<int64_t>();
			});
			if (out.size() > static_cast<size_t>(limit)) out.erase(out.begin() + limit, out.end());

			route::ok(res, out.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/watch-progress", e);
			route::err(res, 500, e.what());
		}
	});

	// ── PUT /api/watch-progress/:content_type/:id ─────────────────────────────
	// Upserts position. An item finished (>=95% through, or explicit
	// {"completed":true} — used by the player's skip-credits/up-next action to
	// mark an episode played before it's actually reached 95%) is stored with
	// completed=1 and position clamped to duration, rather than deleted, so
	// "played" survives as a durable fact (see migration v73). Ordinary pings
	// with no "completed" field behave exactly as before.
	//
	// device_type/direct_stream (both optional, client already knows both from
	// its own session-start response) additionally feed PlaybackHistoryRepository
	// — see migration v91's comment for why that's a separate append-only
	// table rather than something bolted onto watch_progress itself.
	svr.Put(R"(/api/watch-progress/([^/]+)/(.+))", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}

		auto content_type = req.matches[1].str();
		auto content_id   = req.matches[2].str();

		// Live channels have no position/duration/resume concept at all —
		// telemetry previously stopped here entirely for them (validContentType
		// rejects "channel", same as every other route in this file, since none
		// of movie/episode's position-tracking or resolve-to-file logic applies
		// to a live schedule). This branch only ever feeds
		// PlaybackHistoryRepository (the "who's watching what" Activity view),
		// deliberately skipping WatchProgressRepository so a channel never
		// pollutes Continue Watching or "played" state the way a movie/episode
		// ping does — recordPing's own (user, content_type, content_id) keying
		// already does the right thing here unmodified: continuous pings on the
		// same channel extend one sitting, switching channels starts a new one.
		if (content_type == "channel")
		{
			try
			{
				auto b      = json::parse(req.body);
				auto now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
				PlaybackHistoryRepository(db_).recordPing(
					user->user_id, "channel", content_id,
					lookupTitle(db_, "channel", content_id),
					b.value("device_type", ""), b.value("direct_stream", false),
					0, 0, now_ms, false);
				route::ok(res, json{{"ok", true}}.dump());
			}
			catch (const std::exception& e)
			{
				route::logErr("PUT /api/watch-progress/channel/:id", e);
				route::err(res, 400, e.what());
			}
			return;
		}

		if (!validContentType(content_type))
		{
			route::err(res, 400, "content_type must be movie or episode");
			return;
		}

		try
		{
			auto b              = json::parse(req.body);
			int64_t position_ms = b.value("position_ms", int64_t{0});
			int64_t duration_ms = b.value("duration_ms", int64_t{0});
			if (position_ms < 0) position_ms = 0;

			bool completed = b.value("completed", false) ||
				(duration_ms > 0 && position_ms >= static_cast<int64_t>(duration_ms * 0.95));
			if (completed && duration_ms > 0) position_ms = duration_ms;

			auto now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
			WatchProgressRepository(db_).upsert(user->user_id, content_type, content_id,
												position_ms, duration_ms, now_ms / 1000, completed);

			PlaybackHistoryRepository(db_).recordPing(
				user->user_id, content_type, content_id,
				lookupTitle(db_, content_type, content_id),
				b.value("device_type", ""), b.value("direct_stream", false),
				position_ms, duration_ms, now_ms, completed);

			route::ok(res, json{{"ok", true}, {"watched", completed}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PUT /api/watch-progress/:content_type/:id", e);
			route::err(res, 400, e.what());
		}
	});

	// ── GET /api/shows/:id/watch-state ────────────────────────────────────────
	// The most-recently-touched episode watch_progress row for this show,
	// completed or not — lets the player distinguish "resume mid-episode"
	// from "the last episode was finished, continue at the next one" (see
	// hades/src/player/resolvePlayTarget.ts). Unlike the Continue Watching
	// list above, this deliberately does not filter out completed rows.
	svr.Get(R"(/api/shows/(.+)/watch-state)", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}

		try
		{
			auto state = WatchProgressRepository(db_).getLatestForShow(user->user_id, req.matches[1].str());
			if (!state)
			{
				route::ok(res, "null");
				return;
			}

			route::ok(res, json{
						  {"content_id", state->content_id},
						  {"position_ms", state->position_ms},
						  {"duration_ms", state->duration_ms},
						  {"completed", state->completed},
						  {"updated_at", state->updated_at},
					  }.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/shows/:id/watch-state", e);
			route::err(res, 500, e.what());
		}
	});

	// ── GET /api/movies/:id/resolve-play-target ───────────────────────────────
	// Movie counterpart of the show endpoint below — a movie has no next-
	// episode branch to resolve, but it does still need a real watch-progress
	// lookup: several call sites (Android's shelf/hero "Play" actions,
	// DetailViewModel) used to hardcode position_ms=0 for every movie
	// regardless of actual progress, on the mistaken assumption that movies
	// "don't need" this endpoint. A finished movie (completed, i.e. a
	// rewatch-in-waiting) resolves to position_ms=0 same as a finished show's
	// finale-with-no-next-episode case, rather than resuming at the very end.
	svr.Get(R"(/api/movies/(.+)/resolve-play-target)", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}

		auto movie_id = req.matches[1].str();
		try
		{
			auto states         = WatchProgressRepository(db_).getStates(user->user_id, {{"movie", movie_id}});
			auto it             = states.find(watchStateKey("movie", movie_id));
			int64_t position_ms = (it != states.end() && !it->second.completed) ? it->second.position_ms : int64_t{0};

			route::ok(res, json{
						  {"kind", "movie"}, {"id", movie_id}, {"position_ms", position_ms},
					  }.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/movies/:id/resolve-play-target", e);
			route::err(res, 500, e.what());
		}
	});

	// ── GET /api/shows/:id/resolve-play-target ────────────────────────────────
	// Server-side version of hades/src/player/resolvePlayTarget.ts's show
	// branch — collapses what used to be 2-3 client-issued round-trips
	// (watch-state, then conditionally next-episode or the full episode list)
	// into one internal query chain, and means no client (Android, eventually
	// Roku) ever needs to re-implement this branch itself.
	svr.Get(R"(/api/shows/(.+)/resolve-play-target)", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}

		auto show_id = req.matches[1].str();
		try
		{
			ContentRepository content(db_);

			auto state = WatchProgressRepository(db_).getLatestForShow(user->user_id, show_id);
			if (state && !state->completed)
			{
				route::ok(res, json{
							  {"kind", "episode"}, {"id", state->content_id}, {"position_ms", state->position_ms},
						  }.dump());
				return;
			}

			if (state && state->completed)
			{
				auto next = content.getNextEpisode(state->content_id);
				if (next)
				{
					route::ok(res, json{
								  {"kind", "episode"}, {"id", next->episode_id}, {"position_ms", int64_t{0}},
							  }.dump());
					return;
				}
			}

			// No watch state at all, or the completed episode had no next one
			// — fall back to episode 1. listEpisodesForShow's default ordering
			// is season/episode ascending, so the first entry is exactly the
			// same fallback resolvePlayTarget.ts computes client-side.
			auto episodes = content.listEpisodesForShow(show_id);
			if (episodes.empty())
			{
				route::ok(res, "null");
				return;
			}

			route::ok(res, json{
						  {"kind", "episode"}, {"id", episodes.front().episode_id}, {"position_ms", int64_t{0}},
					  }.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/shows/:id/resolve-play-target", e);
			route::err(res, 500, e.what());
		}
	});

	// ── DELETE /api/watch-progress/:content_type/:id ──────────────────────────
	svr.Delete(R"(/api/watch-progress/([^/]+)/(.+))", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}

		auto content_type = req.matches[1].str();
		auto content_id   = req.matches[2].str();

		try
		{
			WatchProgressRepository(db_).remove(user->user_id, content_type, content_id);
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("DELETE /api/watch-progress/:content_type/:id", e);
			route::err(res, 400, e.what());
		}
	});
}