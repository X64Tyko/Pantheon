#include "ScraperService.h"
#include "scraper/ScraperManager.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

static void ok(Res& res, const json& body)
{
	res.set_content(body.dump(), "application/json");
}

static void err(Res& res, int status, const std::string& msg)
{
	res.status = status;
	res.set_content(json{{"error", msg}}.dump(), "application/json");
}

ScraperService::ScraperService(ScraperManager& scraper)
	: scraper_(scraper)
{
}

void ScraperService::registerRoutes(httplib::Server& svr)
{
	// GET /api/scrapers/config
	svr.Get("/api/scrapers/config", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		auto s = scraper_.getSettings();
		json out;
		out["match_threshold"]                      = s.match_threshold;
		out["dedup_fuzzy_title_threshold"]          = s.dedup_fuzzy_title_threshold;
		out["dedup_folder_corroboration_threshold"] = s.dedup_folder_corroboration_threshold;
		out["anidb_download_posters"]               = s.anidb_download_posters;
		out["configs"]                              = json::array();
		for (const auto& c : s.configs)
		{
			json cj;
			cj["source"] = c.source;
			// Never return the real key — admin-gated already, but this page
			// was still the one place in the app that broke the "GET returns
			// has_token, not the token" convention every other credential
			// endpoint follows (see ConfigService.cpp's /api/config/sources
			// and /api/config/credentials/:source_id). "" here is a write-
			// only sentinel: PATCH below only overwrites the stored key when
			// this comes back non-empty, so leaving the field untouched in
			// Hades's Settings page (which resends the whole form on every
			// save, not just changed fields) can't wipe an already-set key.
			cj["api_key"]         = "";
			cj["has_api_key"]     = !c.api_key.empty();
			cj["language"]        = c.language;
			cj["enabled"]         = c.enabled;
			cj["language_weight"] = c.language_weight;
			if (c.source == "tvdb")
			{
				cj["pin"]     = "";
				cj["has_pin"] = !c.pin.empty();
			}
			if (c.source == "anidb") cj["note"] = "api_key is your registered AniDB client name";
			if (c.source == "tvmaze") cj["note"] = "Free public API — no API key required";
			if (c.source == "trakt") cj["note"] = "api_key is your Trakt app's Client ID (register at trakt.tv/oauth/applications)";
			if (c.source == "anilist") cj["note"] = "Free public API — no API key required. Anime/manga only — add \"anilist\" to a library's scraper priority order (Sources) to enable it there; no separate checkbox.";
			if (c.source == "wikidata") cj["note"] = "Free public API — no API key required. Broadest coverage of any source here (Wikidata + Wikipedia), but no per-episode data and thinner fields than dedicated media databases — best as a low-priority fallback for obscure/regional titles.";
			out["configs"].push_back(cj);
		}
		ok(res, out);
	});

	// PATCH /api/scrapers/config
	svr.Patch("/api/scrapers/config", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto body         = json::parse(req.body);
			ScraperSettings s = scraper_.getSettings();

			if (body.contains("match_threshold") && body["match_threshold"].is_number()) s.match_threshold = body["match_threshold"].get<double>();
			if (body.contains("dedup_fuzzy_title_threshold") && body["dedup_fuzzy_title_threshold"].is_number()) s.dedup_fuzzy_title_threshold = body["dedup_fuzzy_title_threshold"].get<double>();
			if (body.contains("dedup_folder_corroboration_threshold") && body["dedup_folder_corroboration_threshold"].is_number()) s.dedup_folder_corroboration_threshold = body["dedup_folder_corroboration_threshold"].get<double>();
			if (body.contains("anidb_download_posters") && body["anidb_download_posters"].is_boolean()) s.anidb_download_posters = body["anidb_download_posters"].get<bool>();

			if (body.contains("configs") && body["configs"].is_array())
			{
				for (const auto& cj : body["configs"])
				{
					std::string src = cj.value("source", "");
					for (auto& c : s.configs)
					{
						if (c.source != src) continue;
						// Empty means "leave unchanged," not "clear the key"
						// — GET never returns the real key/pin anymore (see
						// the GET handler's own comment above), and Hades's
						// Settings page resends this whole form on every
						// save, so a field the admin never touched arrives
						// here as "" rather than the (now never-sent) stored
						// value.
						if (cj.contains("api_key") && !cj["api_key"].get<std::string>().empty()) c.api_key = cj["api_key"].get<std::string>();
						if (cj.contains("language")) c.language = cj["language"].get<std::string>();
						if (cj.contains("enabled")) c.enabled = cj["enabled"].get<bool>();
						if (cj.contains("language_weight")) c.language_weight = cj["language_weight"].get<double>();
						if (src == "tvdb" && cj.contains("pin") && !cj["pin"].get<std::string>().empty()) c.pin = cj["pin"].get<std::string>();
					}
				}
			}
			scraper_.updateSettings(s);
			ok(res, json{{"ok", true}});
		}
		catch (const std::exception& e)
		{
			err(res, 400, e.what());
		}
	});

	// POST /api/scrapers/match
	svr.Post("/api/scrapers/match", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string target_id, item_type;
		try
		{
			if (!req.body.empty())
			{
				auto body = json::parse(req.body);
				target_id = body.value("target_id", "");
				item_type = body.value("item_type", "");
			}
		}
		catch (...)
		{
		}

		if (scraper_.isMatching())
		{
			res.status = 202;
			ok(res, json{{"status", "already_running"}});
			return;
		}
		scraper_.triggerMatch(target_id, item_type);
		res.status = 202;
		ok(res, json{{"status", "started"}});
	});

	// GET /api/scrapers/match/status
	svr.Get("/api/scrapers/match/status", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		ok(res, json{{"running", scraper_.isMatching()}});
	});

	// POST /api/scrapers/refresh-all — re-pull full metadata for every
	// already-matched show/movie from its linked source(s), in the
	// background. Distinct from /match, which only looks for NEW matches on
	// unscraped/uncertain/unmatched items.
	svr.Post("/api/scrapers/refresh-all", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		if (scraper_.isRefreshingAll())
		{
			res.status = 202;
			ok(res, json{{"status", "already_running"}});
			return;
		}
		scraper_.triggerRefreshAll();
		res.status = 202;
		ok(res, json{{"status", "started"}});
	});

	// GET /api/scrapers/refresh-all/status — total/processed/refreshed/failed
	// alongside running, so the client can render a real progress bar
	// instead of a static "running" flag (see ScraperManager::
	// RefreshAllProgress's own comment).
	svr.Get("/api/scrapers/refresh-all/status", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		auto p = scraper_.refreshAllProgress();
		ok(res, json{
			   {"running", p.running},
			   {"total", p.total},
			   {"processed", p.processed},
			   {"refreshed", p.refreshed},
			   {"failed", p.failed},
		   });
	});

	// GET /api/scrapers/stats
	svr.Get("/api/scrapers/stats", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		auto s = scraper_.stats();
		ok(res, json{
			   {"total", s.total},
			   {"matched", s.matched},
			   {"uncertain", s.uncertain},
			   {"unmatched", s.unmatched},
			   {"unscraped", s.unscraped},
			   {"skipped", s.skipped},
		   });
	});

	// GET /api/scrapers/queue
	svr.Get("/api/scrapers/queue", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string status_filter = "all";
		int limit                 = 48, offset = 0;
		if (req.has_param("status")) status_filter = req.get_param_value("status");
		if (req.has_param("limit"))
		{
			try { limit = std::stoi(req.get_param_value("limit")); }
			catch (...)
			{
			}
		}
		if (req.has_param("offset"))
		{
			try { offset = std::stoi(req.get_param_value("offset")); }
			catch (...)
			{
			}
		}

		auto items = scraper_.getQueue(status_filter, limit, offset);
		int total  = scraper_.queueTotal(status_filter);

		json arr = json::array();
		for (const auto& qi : items)
		{
			json qj;
			qj["kairos_id"] = qi.kairos_id;
			qj["item_type"] = qi.item_type;
			qj["title"]     = qi.title;
			if (qi.year > 0) qj["year"] = qi.year;
			qj["thumb"]           = qi.thumb;
			qj["source_id"]       = qi.source_id;
			qj["source_base_url"] = qi.source_base_url;
			qj["match_status"]    = qi.match_status;
			qj["match_score"]     = qi.match_score;
			if (!qi.folder_path.empty()) qj["folder_path"] = qi.folder_path;
			qj["candidates"] = json::array();
			for (const auto& c : qi.candidates)
			{
				json cj;
				cj["candidate_id"] = c.candidate_id;
				cj["source"]       = c.source;
				cj["external_id"]  = c.external_id;
				cj["title"]        = c.title;
				if (c.year > 0) cj["year"] = c.year;
				cj["score"]      = c.score;
				cj["accepted"]   = (c.accepted == -1) ? json(nullptr) : json(c.accepted == 1);
				cj["poster_url"] = c.poster_url;
				cj["overview"]   = c.overview;
				qj["candidates"].push_back(cj);
			}
			arr.push_back(qj);
		}
		ok(res, json{{"items", arr}, {"total", total}});
	});

	// POST /api/scrapers/queue/:kairos_id/manual-match
	// kairos_id uses (.+) rather than ([^/]+): local-source ids are full filesystem
	// paths and contain '/', which httplib's decode_url() restores from %2F before
	// routing runs, so a non-greedy segment match 404s on any local-only item.
	svr.Post(R"(/api/scrapers/queue/(.+)/manual-match)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string kairos_id = req.matches[1];
		try
		{
			auto body               = json::parse(req.body);
			std::string item_type   = body.value("item_type", "");
			std::string source      = body.value("source", "");
			std::string external_id = body.value("external_id", "");
			std::string title       = body.value("title", "");
			int year                = (body.contains("year") && body["year"].is_number()) ? body["year"].get<int>() : 0;
			std::string poster_url  = body.value("poster_url", "");
			std::string overview    = body.value("overview", "");
			if (item_type.empty() || source.empty() || external_id.empty())
			{
				std::cerr << "[scraper] manual match 400: item_type=" << item_type
					<< ", source=" << source << ", external_id=" << external_id << "\n";
				err(res, 400, "item_type, source, and external_id are required");
				return;
			}
			auto result = scraper_.manualMatch(kairos_id, item_type, source, external_id, title, year, poster_url, overview);
			if (!result.found)
			{
				err(res, 404, "item not found");
				return;
			}
			json resp{{"ok", true}};
			if (!result.merged_into_kairos_id.empty())
			{
				resp["merged_into"] = {
					{"kairos_id", result.merged_into_kairos_id},
					{"item_type", result.item_type},
					{"title", result.merged_into_title},
				};
				resp["folder_mismatch"] = result.folder_mismatch;
			}
			ok(res, resp);
		}
		catch (const std::exception& e)
		{
			err(res, 400, e.what());
		}
	});

	// POST /api/scrapers/queue/:id/accept
	svr.Post(R"(/api/scrapers/queue/([^/]+)/accept)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string cid = req.matches[1];
		try
		{
			auto result = scraper_.acceptCandidate(cid);
			if (!result.found)
			{
				err(res, 404, "candidate not found");
				return;
			}
			json body{{"ok", true}};
			if (!result.merged_into_kairos_id.empty())
			{
				body["merged_into"] = {
					{"kairos_id", result.merged_into_kairos_id},
					{"item_type", result.item_type},
					{"title", result.merged_into_title},
				};
				body["folder_mismatch"] = result.folder_mismatch;
			}
			ok(res, body);
		}
		catch (const std::exception& e)
		{
			std::cerr << "[scraper] accept error: " << e.what() << "\n";
			err(res, 500, e.what());
		}
	});

	// POST /api/scrapers/queue/:id/reject
	svr.Post(R"(/api/scrapers/queue/([^/]+)/reject)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string cid = req.matches[1];
		if (scraper_.rejectCandidate(cid)) ok(res, json{{"ok", true}});
		else err(res, 404, "candidate not found");
	});

	// POST /api/scrapers/queue/:kairos_id/confirm
	// Confirms the item's *current* match (already auto-accepted, or picked
	// earlier via manual-match) as human-reviewed, without a re-search +
	// re-pick round trip — see ScraperManager::confirmMatch(). Uses (.+) for
	// the same reason as manual-match above: local kairos_ids contain '/'.
	svr.Post(R"(/api/scrapers/queue/(.+)/confirm)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string kairos_id = req.matches[1];
		try
		{
			auto body             = json::parse(req.body.empty() ? "{}" : req.body);
			std::string item_type = body.value("item_type", "");
			if (item_type.empty())
			{
				err(res, 400, "item_type is required");
				return;
			}
			if (scraper_.confirmMatch(item_type, kairos_id)) ok(res, json{{"ok", true}});
			else err(res, 404, "item not currently matched");
		}
		catch (const std::exception& e)
		{
			err(res, 400, e.what());
		}
	});

	// POST /api/scrapers/confirm-all
	// Bulk-confirms every currently-matched-but-unconfirmed show/movie — see
	// ScraperManager::confirmAllMatches(). Pure DB flip (no network calls),
	// so unlike /refresh-all this runs synchronously — no background-job/
	// status-polling pattern needed.
	svr.Post("/api/scrapers/confirm-all", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		int n = scraper_.confirmAllMatches();
		ok(res, json{{"ok", true}, {"confirmed", n}});
	});

	// POST /api/scrapers/queue/:kairos_id/unconfirm
	// Reverses a confirm — see ScraperManager::unconfirmMatch(). Uses (.+) for
	// the same reason as confirm/manual-match above: local kairos_ids contain '/'.
	svr.Post(R"(/api/scrapers/queue/(.+)/unconfirm)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string kairos_id = req.matches[1];
		try
		{
			auto body             = json::parse(req.body.empty() ? "{}" : req.body);
			std::string item_type = body.value("item_type", "");
			if (item_type.empty())
			{
				err(res, 400, "item_type is required");
				return;
			}
			if (scraper_.unconfirmMatch(item_type, kairos_id)) ok(res, json{{"ok", true}});
			else err(res, 404, "item has no confirmed match");
		}
		catch (const std::exception& e)
		{
			err(res, 400, e.what());
		}
	});

	// POST /api/scrapers/unconfirm-all
	// Bulk-clears match_confirmed on every currently-confirmed show/movie —
	// see ScraperManager::unconfirmAllMatches(). The undo button for an
	// accidental "Confirm All Matches". Pure DB flip, synchronous like confirm-all.
	svr.Post("/api/scrapers/unconfirm-all", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		int n = scraper_.unconfirmAllMatches();
		ok(res, json{{"ok", true}, {"unconfirmed", n}});
	});

	// GET /api/scrapers/anidb/poster/:aid — public, no auth (loaded by <img> tags)
	svr.Get(R"(/api/scrapers/anidb/poster/([^/]+))", [this](const Req& req, Res& res)
	{
		std::string aid = req.matches[1];
		std::string cdn = scraper_.anidbPosterUrl(aid);
		if (cdn.empty())
		{
			res.status = 404;
			return;
		}
		res.set_redirect("/api/images/proxy?url=" + route::urlEncode(cdn));
	});

	// GET /api/scrapers/search?q=...&type=show|movie
	svr.Get("/api/scrapers/search", [this](const Req& req, Res& res)
	{
		std::string q    = req.has_param("q") ? req.get_param_value("q") : "";
		std::string type = req.has_param("type") ? req.get_param_value("type") : "";
		if (q.empty())
		{
			err(res, 400, "q is required");
			return;
		}

		auto results = scraper_.search(q, type);
		json arr     = json::array();
		for (const auto& r : results)
		{
			json rj;
			rj["source"]      = r.source;
			rj["external_id"] = r.external_id;
			rj["title"]       = r.title;
			if (r.year > 0) rj["year"] = r.year;
			rj["overview"] = r.overview;
			// AniDB search results have no poster in the title dump — use the lazy
			// per-AID endpoint so the browser fetches each one independently.
			if (r.source == "anidb" && !r.external_id.empty())
			{
				rj["poster_url"] = "/api/scrapers/anidb/poster/" + r.external_id;
			}
			else
			{
				rj["poster_url"] = r.poster_url.empty()
									   ? ""
									   : "/api/images/proxy?url=" + route::urlEncode(r.poster_url);
			}
			rj["content_type"] = r.content_type;
			rj["in_library"]   = r.in_library;
			if (!r.library_id.empty()) rj["library_id"] = r.library_id;
			if (!r.request_status.empty()) rj["request_status"] = r.request_status;
			arr.push_back(rj);
		}
		ok(res, json{{"items", arr}});
	});

	// GET /api/scrapers/metadata/:item_type/:kairos_id (see manual-match above re: (.+))
	svr.Get(R"(/api/scrapers/metadata/(show|movie)/(.+))", [this](const Req& req, Res& res)
	{
		if (!currentUser())
		{
			err(res, 401, "Unauthorized");
			return;
		}
		std::string type = req.matches[1];
		std::string kid  = req.matches[2];

		auto ids  = scraper_.getExternalIds(kid, type);
		auto alts = scraper_.getAlternateTitles(kid, type);

		json j_ids = json::array();
		for (const auto& id : ids)
		{
			j_ids.push_back({{"source", id.source}, {"external_id", id.external_id}, {"priority", id.priority}});
		}

		ok(res, json{{"external_ids", j_ids}, {"alternate_titles", alts}});
	});

	// POST /api/scrapers/metadata/:item_type/:kairos_id/refresh
	// Registered before the plain metadata POST below: both use a (.+) id capture
	// (local kairos_ids contain '/'), and since regex_match has no boundary between
	// them, the plain route would otherwise greedily swallow ".../refresh" too —
	// whichever pattern is checked first by httplib wins, so the more specific one
	// (this one) has to come first.
	svr.Post(R"(/api/scrapers/metadata/(show|movie)/(.+)/refresh)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string type = req.matches[1];
		std::string kid  = req.matches[2];

		if (scraper_.refreshMetadata(kid, type)) ok(res, json{{"ok", true}});
		else err(res, 404, "no metadata sources found for this item");
	});

	// POST /api/scrapers/metadata/:item_type/:kairos_id
	svr.Post(R"(/api/scrapers/metadata/(show|movie)/(.+))", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			err(res, 403, "Forbidden");
			return;
		}
		std::string type = req.matches[1];
		std::string kid  = req.matches[2];

		try
		{
			auto body = json::parse(req.body);
			if (body.contains("external_ids"))
			{
				std::vector<ScraperManager::ExternalId> ids;
				for (const auto& item : body["external_ids"])
				{
					ids.push_back({item.value("source", ""), item.value("external_id", ""), item.value("priority", 0)});
				}
				scraper_.setExternalIds(kid, type, ids);
			}
			if (body.contains("alternate_titles"))
			{
				scraper_.setAlternateTitles(kid, type, body["alternate_titles"].get<std::vector<std::string>>());
			}
			ok(res, json{{"ok", true}});
		}
		catch (const std::exception& e)
		{
			err(res, 400, e.what());
		}
	});
}