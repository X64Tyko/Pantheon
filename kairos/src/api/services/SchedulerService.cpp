#include "SchedulerService.h"
#include "../RouteHelpers.h"
#include "../ScheduleCache.h"
#include "../ServiceContext.h"
#include "../../conf/ConfStore.h"
#include "../../db/Database.h"
#include "../../db/ChannelRepository.h"
#include "../../db/ContentRepository.h"
#include "../../db/ScheduleRepository.h"
#include "../../db/SourceRepository.h"
#include "../../scheduler/EPGDivergenceChecker.h"
#include "../../scheduler/EPGMaterializer.h"
#include "../../scheduler/RuleEngine.h"
#include <nlohmann/json.hpp>
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>
#include <filesystem>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

SchedulerService::SchedulerService(const ServiceContext& ctx)
	: db_(ctx.db)
	, conf_(ctx.conf)
	, engine_(ctx.engine)
	, materializer_(ctx.materializer)
	, schedule_cache_(ctx.schedule_cache)
	, divergence_checker_(ctx.divergence_checker)
{
}

namespace
{
	// How far out an "offline" gap-filler response's wall_clock_end_ms reaches —
	// Hephaestus's ChannelSession bounds the offline slate's encode to roughly
	// this long (see ChannelSession::spawnOffline) so it naturally exits and
	// re-polls /now afterward, instead of looping the slate forever with no way
	// to ever notice the gap has ended and real programming has resumed.
	constexpr int64_t kOfflineRecheckMs = 20'000;

	// Merges cached direct-stream keyframe data (movie/episode.keyframes_ms —
	// see Database.cpp's v98 migration and SyncManager::syncMediaProbeFromFiles)
	// into a /now response, the same data VOD sessions already get from
	// /api/playback/:content_type/:id. Lets ChannelSession (Hephaestus) snap a
	// direct-stream item's start offset to a real keyframe instead of handing
	// ffmpeg a blind offset on every transition — see ChannelSession.cpp's
	// snapToKeyframe(). No-op for filler/offline or any item_type other than
	// movie/episode; leaves j untouched (Hephaestus treats absent fields the
	// same as an empty/stale cache) if the row has never been keyframe-probed.
	void attachKeyframes(Database& db, json& j, const std::string& item_type, const std::string& item_id)
	{
		if (item_type != "movie" && item_type != "episode") return;
		const char* sql = item_type == "movie"
							  ? "SELECT keyframes_ms, keyframes_size, keyframes_mtime FROM movie WHERE movie_id = ?"
							  : "SELECT keyframes_ms, keyframes_size, keyframes_mtime FROM episode WHERE episode_id = ?";
		SQLite::Statement q(db.get(), sql);
		q.bind(1, item_id);
		if (!q.executeStep()) return;
		try
		{
			json keyframes_ms = json::parse(q.getColumn(0).getString());
			if (!keyframes_ms.is_array() || keyframes_ms.empty()) return;
			j["keyframes_ms"]    = std::move(keyframes_ms);
			j["keyframes_size"]  = q.getColumn(1).getInt64();
			j["keyframes_mtime"] = q.getColumn(2).getInt64();
		}
		catch (...)
		{
		}
	}

	// Neither /now nor /next ever confirmed the file behind a resolved item
	// actually exists on Kairos's own filesystem — only that file_path was
	// non-empty — so a source whose media isn't reachable here (a bad/absent
	// path_map, an unmounted share) got served with total confidence,
	// guaranteeing a downstream ffmpeg spawn failure in Hephaestus with no
	// diagnostic short of its own stderr. mapped is the already path-mapped
	// value (conf_.applyPathMap's output); empty is left alone (a pre-
	// existing, differently-handled degenerate case Hephaestus's own
	// spawnFfmpeg already checks for) — this only catches "non-empty but not
	// actually there."
	bool fileReachable(const std::string& mapped)
	{
		return mapped.empty() || std::filesystem::exists(mapped);
	}
} // namespace

void SchedulerService::registerRoutes(httplib::Server& svr)
{
	svr.Get("/playlist.m3u", [this](const Req& req, Res& res)
	{
		std::string host = req.get_header_value("Host");
		if (host.empty()) host = "localhost:8080";
		res.set_content(materializer_.generateM3U("http://" + host), "application/x-mpegURL");
	});

	// ── Media language catalog ─────────────────────────────────────────────────
	// Real per-item persisted data (episode/movie.audio_languages, unioned with
	// subtitle_track for subtitles) — see ContentRepository::getMetadataValues.
	// Used to be a random-sample-of-40-files live ffprobe with a 1hr cache;
	// now a plain DB read, so no caching needed here either.
	svr.Get("/api/media/languages", [this](const Req&, Res& res)
	{
		try
		{
			ContentRepository repo(db_);
			json result = {{"audio", json::array()}, {"subtitle", json::array()}};
			for (const auto& l : repo.getMetadataValues("audio_language", "", "")) result["audio"].push_back(l);
			for (const auto& l : repo.getMetadataValues("subtitle_language", "", "")) result["subtitle"].push_back(l);
			route::ok(res, result.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/media/languages", e);
			route::err(res, 500, e.what());
		}
	});

	auto xmltvHandler = [this](const Req& req, Res& res)
	{
		try
		{
			int hours = 24;
			if (req.has_param("hours"))
			{
				try { hours = std::stoi(req.get_param_value("hours")); }
				catch (...)
				{
				}
			}
			hours            = std::max(1, std::min(hours, 72));
			std::string host = req.get_header_value("Host");
			if (host.empty()) host = "localhost:8080";
			res.set_content(materializer_.generateXMLTV(hours, "http://" + host), "application/xml");
		}
		catch (const std::exception& e)
		{
			route::logErr("GET xmltv", e);
			route::err(res, 500, e.what());
		}
	};
	svr.Get("/epg.xml", xmltvHandler);
	svr.Get("/api/epg.xml", xmltvHandler);
	svr.Get("/api/xmltv.xml", xmltvHandler);
	svr.Get("/api/channels.xml", xmltvHandler);

	// ── What's playing now ────────────────────────────────────────────────────
	svr.Get(R"(/api/channels/([^/]+)/now)", [this](const Req& req, Res& res)
	{
		try
		{
			std::string channel_id = req.matches[1];
			auto t                 = std::time(nullptr);
			if (req.has_param("at"))
			{
				try
				{
					int64_t at_ms = std::stoll(req.get_param_value("at"));
					t             = static_cast<std::time_t>(at_ms / 1000);
				}
				catch (...)
				{
				}
			}

			materializer_.ensureScheduled(channel_id, t - 3600, 4);

			ScheduleRepository sched(db_);
			if (auto row = sched.getNowProgram(channel_id, t))
			{
				std::string mapped = conf_.applyPathMap(row->file_path);
				if (fileReachable(mapped))
				{
					json j = {
						{"item_type", row->item_type},
						{"item_id", row->item_id},
						{"file_path", mapped},
						{"duration_ms", row->duration_ms},
						{"title", row->title},
						{"block_id", row->block_id},
						{"wall_clock_start_ms", row->wall_clock_start * 1000LL},
						{"wall_clock_end_ms", row->wall_clock_end * 1000LL},
						{"is_filler", row->is_filler},
					};
					if (!row->show_title.empty())
					{
						j["show_title"]  = row->show_title;
						j["show_id"]     = row->show_id;
						j["season"]      = row->season;
						j["episode_num"] = row->episode;
					}
					if (auto sm = SourceRepository(db_).getSourceMapping(row->item_id))
					{
						j["source_id"]   = sm->source_id;
						j["external_id"] = sm->external_id;
					}
					attachKeyframes(db_, j, row->item_type, row->item_id);
					route::ok(res, j.dump());
					return;
				}
				std::cerr << "[now] scheduled file missing on disk for channel " << channel_id
					<< " (" << row->item_type << " " << row->item_id << "): " << mapped
					<< " — falling through to the next tier\n";
			}

			auto block_opt = engine_.resolveBlock(channel_id, t);
			std::optional<ScheduledItem> item_opt;
			if (block_opt) item_opt = engine_.nextItem(channel_id, *block_opt, std::time(nullptr));

			if (block_opt && item_opt)
			{
				const auto& item   = *item_opt;
				std::string mapped = conf_.applyPathMap(item.file_path);
				if (fileReachable(mapped))
				{
					json j = {
						{"item_type", item.item_type},
						{"item_id", item.item_id},
						{"file_path", mapped},
						{"duration_ms", item.duration_ms},
						{"title", item.title},
						{"block_id", item.block_id},
						{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
						{"wall_clock_end_ms", static_cast<int64_t>(t) * 1000 + item.duration_ms},
						{"is_filler", item.is_filler},
					};
					if (!item.show_title.empty())
					{
						j["show_title"]  = item.show_title;
						j["show_id"]     = item.show_id;
						j["season"]      = item.season;
						j["episode_num"] = item.episode_num;
					}
					if (auto sm = SourceRepository(db_).getSourceMapping(item.item_id))
					{
						j["source_id"]   = sm->source_id;
						j["external_id"] = sm->external_id;
					}
					attachKeyframes(db_, j, item.item_type, item.item_id);
					route::ok(res, j.dump());
					return;
				}
				std::cerr << "[now] scheduled file missing on disk for channel " << channel_id
					<< " (" << item.item_type << " " << item.item_id << "): " << mapped
					<< " — falling through to the next tier\n";
			}

			if (auto filler = sched.getChannelFillerFallback(channel_id))
			{
				std::string mapped = conf_.applyPathMap(filler->file_path);
				if (fileReachable(mapped))
				{
					int64_t dur = filler->duration_ms;
					json j      = {
						{"item_type", filler->item_type},
						{"item_id", filler->item_id},
						{"file_path", mapped},
						{"title", filler->title},
						{"duration_ms", dur},
						{"block_id", ""},
						{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
						{"wall_clock_end_ms", static_cast<int64_t>(t) * 1000 + dur},
						{"is_filler", true},
					};
					attachKeyframes(db_, j, filler->item_type, filler->item_id);
					route::ok(res, j.dump());
					return;
				}
				std::cerr << "[now] filler fallback file missing on disk for channel " << channel_id
					<< " (" << filler->item_type << " " << filler->item_id << "): " << mapped
					<< " — falling through to offline\n";
			}

			if (auto offline = sched.getChannelOfflineConfig(channel_id))
			{
				if (!offline->vid_path.empty())
				{
					route::ok(res, json{
								  {"item_type", "offline"},
								  {"file_path", conf_.applyPathMap(offline->vid_path)},
								  {"duration_ms", 0},
								  {"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
								  {"wall_clock_end_ms", static_cast<int64_t>(t) * 1000 + kOfflineRecheckMs},
							  }.dump());
					return;
				}

				// Fall back to the channel's logo when no offline image is configured.
				// If neither exists, still respond 200 (no image field) rather than
				// 404ing — Hephaestus supplies its own generic default in that case.
				const std::string& image = !offline->img_path.empty() ? offline->img_path : offline->logo_path;
				json j                   = {
					{"item_type", "offline"},
					{"duration_ms", 0},
					{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
					{"wall_clock_end_ms", static_cast<int64_t>(t) * 1000 + kOfflineRecheckMs},
				};
				if (!image.empty())
				{
					j["offline_image_path"] = conf_.applyPathMap(image);
					if (!offline->audio_id.empty() && !offline->audio_typ.empty())
					{
						if (auto ap = sched.getAudioFilePath(offline->audio_typ, offline->audio_id)) j["offline_audio_path"] = conf_.applyPathMap(*ap);
					}
				}
				route::ok(res, j.dump());
				return;
			}

			route::err(res, 404, "channel not found");
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/channels/now", e);
			route::err(res, 500, e.what());
		}
	});

	// ── What's playing next ───────────────────────────────────────────────────
	svr.Get(R"(/api/channels/([^/]+)/next)", [this](const Req& req, Res& res)
	{
		try
		{
			std::string channel_id = req.matches[1];
			auto t                 = std::time(nullptr);

			materializer_.ensureScheduled(channel_id, t, 4);

			ScheduleRepository sched(db_);
			auto row = sched.getNextProgram(channel_id, t);
			if (!row)
			{
				route::err(res, 404, "no next item available");
				return;
			}

			// No fallback tier to fall through to here (unlike /now) — this
			// endpoint only ever backs ChannelSession::prefetchLoop()'s
			// best-effort cache-warming, which already treats a 404 the same
			// as "nothing to prefetch." Better that than handing back a
			// file_path that doesn't actually resolve on disk.
			std::string mapped = conf_.applyPathMap(row->file_path);
			if (!fileReachable(mapped))
			{
				std::cerr << "[next] scheduled file missing on disk for channel " << channel_id
					<< " (" << row->item_type << " " << row->item_id << "): " << mapped << "\n";
				route::err(res, 404, "no next item available");
				return;
			}

			json j = {
				{"item_type", row->item_type},
				{"item_id", row->item_id},
				{"file_path", mapped},
				{"duration_ms", row->duration_ms},
				{"title", row->title},
				{"block_id", row->block_id},
				{"wall_clock_start_ms", row->wall_clock_start * 1000LL},
			};
			if (!row->show_title.empty())
			{
				j["show_title"]  = row->show_title;
				j["show_id"]     = row->show_id;
				j["season"]      = row->season;
				j["episode_num"] = row->episode;
			}
			if (auto sm = SourceRepository(db_).getSourceMapping(row->item_id))
			{
				j["source_id"]   = sm->source_id;
				j["external_id"] = sm->external_id;
			}
			route::ok(res, j.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/channels/next", e);
			route::err(res, 500, e.what());
		}
	});

	// ── Report playback completion ────────────────────────────────────────────
	svr.Post(R"(/api/channels/([^/]+)/played)", [this](const Req& req, Res& res)
	{
		try
		{
			std::string channel_id = req.matches[1];
			auto b                 = json::parse(req.body);
			std::string item_type  = b.value("item_type", "episode");
			std::string item_id    = b.value("item_id", "");
			std::string block_id   = b.value("block_id", "");
			int64_t duration_ms    = b.value("duration_actual_ms", int64_t(0));

			if (item_id.empty())
			{
				route::err(res, 400, "item_id required");
				return;
			}
			engine_.markPlayed(channel_id, block_id, item_type, item_id, duration_ms);
			materializer_.notifyPlayed(channel_id, item_id);
			if (ChannelRepository(db_).getAdvanceMode(channel_id) == "on_play") schedule_cache_.clear(channel_id);
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/channels/played", e);
			route::err(res, 400, e.what());
		}
	});

	// ── EPG preview ───────────────────────────────────────────────────────────
	svr.Post(R"(/api/channels/([^/]+)/epg/preview)", [this](const Req& req, Res& res)
	{
		try
		{
			std::string channel_id = req.matches[1];

			json body = json::object();
			if (!req.body.empty())
			{
				try { body = json::parse(req.body); }
				catch (...)
				{
				}
			}

			int hours = 336;
			if (body.contains("hours"))
			{
				try { hours = body["hours"].get<int>(); }
				catch (...)
				{
				}
			}
			hours = std::max(1, std::min(hours, 672));

			int req_seed  = -1;
			bool has_seed = body.contains("seed") && !body["seed"].is_null();
			if (has_seed)
			{
				try { req_seed = body["seed"].get<int>(); }
				catch (...)
				{
				}
			}

			bool has_blocks = body.contains("blocks") && body["blocks"].is_array()
				&& !body["blocks"].empty();

			auto now = std::time(nullptr);

			// Preview always starts from the current week's Monday 00:00 (channel-tz
			// aware, matching RuleEngine::project()'s own week-walk), not "now" — a
			// "now"-anchored start truncated every "today" view down to whatever's
			// left of the day and made the whole preview a rolling `hours` window
			// from the moment you happened to load it, rather than a stable
			// Monday-through-`hours` schedule. week_anchor doubles as the projection
			// start and the preview cache key below.
			std::time_t week_anchor = engine_.weekMondayForChannel(channel_id, now);
			auto horizon            = static_cast<int64_t>(week_anchor) + hours * 3600LL;

			if (!has_blocks && !has_seed)
			{
				std::string cached;
				if (schedule_cache_.getPreview(channel_id, req_seed, week_anchor, cached))
				{
					route::ok(res, cached);
					return;
				}
			}

			GenerateResult gr;
			if (has_blocks)
			{
				ScheduleRepository sched(db_);
				gr = sched.withPreviewBlocks(channel_id, body["blocks"],
											 [&]() { return materializer_.generate(channel_id, week_anchor, hours, req_seed); });
			}
			else
			{
				gr = materializer_.generate(channel_id, week_anchor, hours, req_seed);
			}

			json arr = json::array();

			// Emit filler items as item_type="filler" so the frontend's mergeFiller()
			// can combine consecutive filler clips into a single visual block.
			// Non-filler items keep their original times; no extension is applied here —
			// extension is only done for the committed schedule (live EPG / XMLTV).
			for (const auto& item : gr.items)
			{
				std::time_t ws = item.wall_clock_start_ms / 1000;
				std::time_t we = item.wall_clock_end_ms / 1000;
				if (we <= week_anchor) continue;
				if (ws >= horizon) break;

				json j = {
					{"item_type", item.is_filler ? "filler" : item.item_type},
					{"item_id", item.item_id},
					{"block_id", item.block_id},
					{"wall_clock_start_ms", item.wall_clock_start_ms},
					{"wall_clock_end_ms", item.wall_clock_end_ms},
					{"status", "scheduled"},
					{"title", item.title},
					{"duration_ms", item.duration_ms},
				};
				if (!item.show_title.empty())
				{
					j["show_title"]  = item.show_title;
					j["show_id"]     = item.show_id;
					j["season"]      = item.season;
					j["episode_num"] = item.episode_num;
				}
				arr.push_back(j);
			}

			// Anchors are always the RNG + cursor-state snapshot per Monday.
			json anchors_j = json::object();
			if (auto ah = ChannelRepository(db_).getAnchorHashes(channel_id))
			{
				try { anchors_j = json::parse(*ah); }
				catch (...)
				{
				}
			}
			for (auto& [ts, snap_str] : gr.anchors)
			{
				try { anchors_j[std::to_string(ts)] = json::parse(snap_str); }
				catch (...)
				{
				}
			}

			json divs_j = json::array();
			for (const auto& d : gr.divergences)
			{
				divs_j.push_back({
					{"wall_clock_start", d.wall_clock_start},
					{"wall_clock_end", d.wall_clock_end},
					{"block_id", d.block_id},
					{"prev_item_type", d.prev_item_type},
					{"prev_item_id", d.prev_item_id},
					{"new_item_type", d.new_item_type},
					{"new_item_id", d.new_item_id},
				});
			}

			std::string resp_body = json{
				{"programs", arr},
				{"anchors", anchors_j},
				{"divergences", divs_j}
			}.dump();
			if (!has_blocks && !has_seed) schedule_cache_.setPreview(channel_id, req_seed, week_anchor, resp_body);
			route::ok(res, resp_body);
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/channels/epg/preview", e);
			route::err(res, 500, e.what());
		}
	});

	// ── Clear EPG cache ───────────────────────────────────────────────────────
	// ?hard=true also wipes CursorState + the RNG/cursor anchor snapshot, not just
	// scheduled_program — use when a structural change (or anything else) makes
	// the accumulated cursor state itself suspect, not just the materialized rows.
	//
	// ?live=true additionally drops ScheduleCache's usual carve-out for whatever's
	// currently on-air (see ScheduleCache::clear()'s own comment) — an explicit,
	// user-confirmed "apply my edits to the live stream now" action, not something
	// any route fires as a side effect of an ordinary edit. Hades only sends this
	// after the user has agreed to interrupt live playback; every other mutation
	// route still calls schedule_cache_.clear()/hardReset() with the safe default.
	svr.Post(R"(/api/channels/([^/]+)/epg/clear)", [this](const Req& req, Res& res)
	{
		try
		{
			bool hard = req.has_param("hard") && req.get_param_value("hard") == "true";
			bool live = req.has_param("live") && req.get_param_value("live") == "true";
			if (hard) schedule_cache_.hardReset(req.matches[1], !live);
			else schedule_cache_.clear(req.matches[1], !live);
			route::ok(res, json{{"ok", true}, {"hard", hard}, {"live", live}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/channels/epg/clear", e);
			route::err(res, 500, e.what());
		}
	});

	// ── EPG divergence check ─────────────────────────────────────────────────
	// Starts a background check (cheap anchor comparison, falling through to a
	// full projection + item diff only when needed) and returns immediately with
	// a job id; poll it via the jobs list below.
	svr.Post(R"(/api/channels/([^/]+)/epg/divergence-check)", [this](const Req& req, Res& res)
	{
		try
		{
			auto job_id = divergence_checker_.startCheck(req.matches[1]);
			route::ok(res, json{{"job_id", job_id}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/channels/epg/divergence-check", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Get("/api/channels/epg/divergence-check/jobs", [this](const Req&, Res& res)
	{
		try
		{
			json arr = json::array();
			for (const auto& j : divergence_checker_.getJobs())
			{
				arr.push_back({
					{"id", j.id},
					{"channel_id", j.channel_id},
					{"status", j.status},
					{"stage", j.stage},
					{"anchor_checked", j.anchor_checked},
					{"anchor_diverged", j.anchor_diverged},
					{"deep_ran", j.deep_ran},
					{"divergence_count", j.divergence_count},
					{"started_at", j.started_at},
					{"finished_at", j.finished_at},
					{"error", j.error},
				});
			}
			route::ok(res, arr.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/channels/epg/divergence-check/jobs", e);
			route::err(res, 500, e.what());
		}
	});

	// ── EPG projection ────────────────────────────────────────────────────────
	svr.Get(R"(/api/channels/([^/]+)/epg)", [this](const Req& req, Res& res)
	{
		try
		{
			std::string channel_id = req.matches[1];
			int hours              = 24;
			if (req.has_param("hours"))
			{
				try { hours = std::stoi(req.get_param_value("hours")); }
				catch (...)
				{
				}
			}
			hours = std::max(1, std::min(hours, 72));

			auto now = std::time(nullptr);
			materializer_.ensureScheduled(channel_id, now, hours);

			// Optional `from` param (Unix seconds) lets callers query from an
			// earlier point (e.g. today's local midnight) to show a full-day strip.
			int64_t from_sec = static_cast<int64_t>(now);
			if (req.has_param("from"))
			{
				try { from_sec = std::stoll(req.get_param_value("from")); }
				catch (...)
				{
				}
			}
			auto horizon = static_cast<int64_t>(now + hours * 3600LL);
			auto rows    = ScheduleRepository(db_).getEpgPrograms(channel_id, from_sec, horizon);

			json arr = json::array();
			for (const auto& r : rows)
			{
				json j = {
					{"item_type", r.item_type},
					{"item_id", r.item_id},
					{"block_id", r.block_id},
					{"wall_clock_start_ms", r.wall_clock_start * 1000},
					{"wall_clock_end_ms", r.wall_clock_end * 1000},
					{"status", r.status},
					{"title", r.title},
					{"file_path", r.file_path},
					{"duration_ms", r.duration_ms},
					{"overview", r.overview},
				};
				if (!r.show_title.empty())
				{
					j["show_title"]  = r.show_title;
					j["show_id"]     = r.show_id;
					j["season"]      = r.season;
					j["episode_num"] = r.episode;
				}
				arr.push_back(j);
			}
			route::ok(res, arr.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/channels/epg", e);
			route::err(res, 500, e.what());
		}
	});
}