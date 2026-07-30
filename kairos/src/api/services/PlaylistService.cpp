#include "PlaylistService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "PlexSyncHelper.h"
#include "ListPushHelper.h"
#include "PlaylistPlayTargetHelper.h"
#include "../../db/Database.h"
#include "../../db/PlaylistRepository.h"
#include "../../db/PlaylistSerializer.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include "../../source/SyncManager.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

namespace
{
	// "MM-DD", loosely validated (no per-month day-count check — "02-30" passes,
	// same tolerance the rest of this codebase gives user-typed date fragments).
	bool isValidMonthDay(const std::string& s)
	{
		if (s.size() != 5 || s[2] != '-') return false;
		if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) ||
			!isdigit((unsigned char)s[3]) || !isdigit((unsigned char)s[4]))
			return false;
		int month = (s[0] - '0') * 10 + (s[1] - '0');
		int day   = (s[3] - '0') * 10 + (s[4] - '0');
		return month >= 1 && month <= 12 && day >= 1 && day <= 31;
	}
}

PlaylistService::PlaylistService(const ServiceContext& ctx)
	: db_(ctx.db)
	, sync_(ctx.sync)
{
}

void PlaylistService::registerRoutes(httplib::Server& svr)
{
	svr.Get("/api/playlists", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			json result = json::array();
			for (const auto& r : PlaylistRepository(db_).listAll())
			{
				json entry = {
					{"playlist_id", r.playlist_id},
					{"title", r.title},
					{"mode", r.mode},
					{"item_count", r.item_count},
					{"total_ms", r.total_ms},
					{"membership", r.membership},
					{"filter_expr", r.filter_expr},
					{"smart_type", r.smart_type},
					{"smart_sort", r.smart_sort},
					{"smart_sort_dir", r.smart_sort_dir},
					{"smart_expand_episodes", r.smart_expand_episodes},
					{"smart_limit", r.smart_limit},
					{
						"last_smart_refresh_at", r.last_smart_refresh_at
													 ? json(*r.last_smart_refresh_at)
													 : json(nullptr)
					},
					{"show_on_home", r.show_on_home},
					{"home_order", r.home_order},
					{"home_tile_limit", r.home_tile_limit},
					{"home_active_start", r.home_active_start},
					{"home_active_end", r.home_active_end},
					{"poster_source", r.poster_source},
				};
				if (r.plex_link)
				{
					entry["plex_link"] = {
						{"source_id", r.plex_link->source_id},
						{"external_id", r.plex_link->external_id},
						{"plex_type", r.plex_link->plex_type},
						{
							"last_synced_at", r.plex_link->last_synced_at
												  ? json(*r.plex_link->last_synced_at)
												  : json(nullptr)
						},
					};
				}
				result.push_back(entry);
			}
			route::ok(res, result.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/playlists", e);
			route::err(res, 500, e.what());
		}
	});

	// Home shelves — every smart playlist with show_on_home=1 whose active
	// window (if any) includes today (see PlaylistRepository::
	// listHomeShelves). Public/unauthenticated: structural Home-page
	// composition data, not per-user — same reasoning as /api/tv/manifest
	// and /api/config/public-settings (see Router.cpp's isPublicPath).
	svr.Get("/api/home-playlists", [this](const Req&, Res& res)
	{
		try
		{
			json result = json::array();
			for (const auto& r : PlaylistRepository(db_).listHomeShelves())
			{
				result.push_back({
					{"playlist_id", r.playlist_id},
					{"title", r.title},
					{"smart_type", r.smart_type},
					{"filter_expr", r.filter_expr},
					{"smart_sort", r.smart_sort},
					{"smart_sort_dir", r.smart_sort_dir},
					{"smart_expand_episodes", r.smart_expand_episodes},
					{"home_tile_limit", r.home_tile_limit},
				});
			}
			route::ok(res, result.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/home-playlists", e);
			route::err(res, 500, e.what());
		}
	});

	// Any authenticated user (not admin-only like the rest of this file) —
	// tile-rendering summary for the Library's new Playlists section, same
	// "playback affordance, not management" reasoning as resolve-play-target
	// below. Deliberately excludes membership/filter_expr/sync internals that
	// GET /api/playlists exposes for editing — this is browse-only.
	svr.Get("/api/playlists/browse", [this](const Req&, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		try
		{
			json result = json::array();
			for (const auto& r : PlaylistRepository(db_).listBrowse())
			{
				json preview = json::array();
				for (const auto& it : r.preview_items) preview.push_back({{"item_type", it.item_type}, {"item_id", it.item_id}});
				result.push_back({
					{"playlist_id", r.playlist_id},
					{"title", r.title},
					{"mode", r.mode},
					{"poster_source", r.poster_source},
					{"item_count", r.item_count},
					{"total_ms", r.total_ms},
					{"preview_items", preview},
				});
			}
			route::ok(res, result.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/playlists/browse", e);
			route::err(res, 500, e.what());
		}
	});

	// Must register before /:id routes
	svr.Post("/api/playlists/plex-sync-all", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		sync_.triggerPlexLinkSync();
		res.status = 202;
		route::ok(res, json{{"status", "accepted"}}.dump());
	});

	svr.Post("/api/playlists/source-sync-all", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		sync_.triggerPlexLinkSync();
		res.status = 202;
		route::ok(res, json{{"status", "accepted"}}.dump());
	});

	svr.Post("/api/playlists/refresh-smart-all", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		sync_.triggerSmartPlaylistRefresh();
		res.status = 202;
		route::ok(res, json{{"status", "accepted"}}.dump());
	});

	svr.Post("/api/playlists", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto b            = json::parse(req.body);
			std::string title = b.value("title", "");
			if (title.empty())
			{
				route::err(res, 400, "title required");
				return;
			}
			auto playlist_id = PlaylistRepository(db_).create(title);
			res.status       = 201;
			route::ok(res, json{{"playlist_id", playlist_id}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/playlists", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Get("/api/playlists/:id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto id = req.path_params.at("id");
			auto d  = PlaylistRepository(db_).getDetail(id);
			if (!d)
			{
				route::err(res, 404, "playlist not found");
				return;
			}
			json items = json::array();
			for (const auto& r : d->items)
			{
				json item = {
					{"id", r.id},
					{"position", r.position},
					{"item_type", r.item_type},
					{"item_id", r.item_id},
					{"title", r.title},
					{"duration_ms", r.duration_ms},
				};
				if (r.season) item["season"] = *r.season;
				if (r.episode) item["episode"] = *r.episode;
				items.push_back(item);
			}
			route::ok(res, json{
						  {"playlist_id", d->playlist_id},
						  {"title", d->title},
						  {"mode", d->mode},
						  {"items", items},
						  {"membership", d->membership},
						  {"filter_expr", d->filter_expr},
						  {"smart_type", d->smart_type},
						  {"smart_sort", d->smart_sort},
						  {"smart_sort_dir", d->smart_sort_dir},
						  {"smart_expand_episodes", d->smart_expand_episodes},
						  {"smart_limit", d->smart_limit},
						  {
							  "last_smart_refresh_at", d->last_smart_refresh_at
														   ? json(*d->last_smart_refresh_at)
														   : json(nullptr)
						  },
						  {"show_on_home", d->show_on_home},
						  {"home_order", d->home_order},
						  {"home_tile_limit", d->home_tile_limit},
						  {"home_active_start", d->home_active_start},
						  {"home_active_end", d->home_active_end},
						  {"poster_source", d->poster_source},
					  }.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/playlists/:id", e);
			route::err(res, 500, e.what());
		}
	});

	// Any authenticated user (not admin-only like GET /api/playlists/:id
	// above) — the Library page's Play/Restart/Shuffle actions (see
	// LibraryPage.tsx/resolvePlayTarget.ts) need the ordered item list to
	// build a client-side playback queue, but a viewer has no business
	// seeing membership/filter_expr/sync internals the admin detail route
	// exposes. Same "browse/playback, not management" split as
	// /api/playlists/browse and resolve-play-target below.
	svr.Get("/api/playlists/:id/items", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		try
		{
			auto id = req.path_params.at("id");
			auto d  = PlaylistRepository(db_).getDetail(id);
			if (!d)
			{
				route::err(res, 404, "playlist not found");
				return;
			}
			json items = json::array();
			for (const auto& r : d->items)
			{
				json item = {
					{"id", r.id},
					{"position", r.position},
					{"item_type", r.item_type},
					{"item_id", r.item_id},
					{"title", r.title},
					{"duration_ms", r.duration_ms},
				};
				if (r.season) item["season"] = *r.season;
				if (r.episode) item["episode"] = *r.episode;
				items.push_back(item);
			}
			route::ok(res, json{{"items", items}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/playlists/:id/items", e);
			route::err(res, 500, e.what());
		}
	});

	// Any authenticated user (not admin-only like the routes above) — this is
	// a playback affordance, not playlist management, same reasoning as
	// PlaybackService's /api/shows/:id/resolve-play-target. Algorithm lives in
	// PlaylistPlayTargetHelper.cpp, unit-tested directly there.
	svr.Get("/api/playlists/:id/resolve-play-target", [this](const Req& req, Res& res)
	{
		auto user = currentUser();
		if (!user)
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			resolvePlaylistPlayTarget(res, id, user->user_id, db_);
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/playlists/:id/resolve-play-target", e);
			route::err(res, 500, e.what());
		}
	});

	// Portable JSON export/import — same pattern as channel export/import, lets
	// a playlist be shared/moved between Pantheon instances (or people).
	// Must register the literal "import"/"import/preview" paths before any
	// bare POST /api/playlists/:id/... pattern that could otherwise swallow
	// them — there isn't one today, but keeping the same registration-order
	// discipline ChannelService follows.
	svr.Get("/api/playlists/:id/export", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id   = req.path_params.at("id");
		bool deep = req.has_param("deep") && req.get_param_value("deep") == "true";
		try
		{
			route::ok(res, PlaylistSerializer(db_).exportPlaylist(id, deep).dump(2));
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/playlists/:id/export", e);
			route::err(res, 404, e.what());
		}
	});

	svr.Post("/api/playlists/import/preview", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto body = json::parse(req.body);
			bool deep = body.value("depth", "shallow") == "deep";
			route::ok(res, PlaylistSerializer(db_).previewImport(body, deep).dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/playlists/import/preview", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/playlists/import", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto body = json::parse(req.body);
			if (!body.value("kairos_export", 0))
			{
				route::err(res, 400, "not a valid Kairos playlist export");
				return;
			}
			bool deep                      = body.value("depth", "shallow") == "deep";
			auto [playlist_id, unresolved] = PlaylistSerializer(db_).importPlaylist(body, deep);
			res.status                     = 201;
			route::ok(res, json{{"playlist_id", playlist_id}, {"unresolved", unresolved}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/playlists/import", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/playlists/:id/plex-sync", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			auto b           = json::parse(req.body);
			std::string src  = b.value("source_id", "");
			std::string ext  = b.value("external_id", "");
			std::string kind = b.value("plex_type", "");
			if (src.empty() || ext.empty() || (kind != "playlist" && kind != "collection"))
			{
				route::err(res, 400, "source_id, external_id, plex_type required");
				return;
			}
			syncPlexListItems(res, "playlist", id, src, ext, kind, db_, sync_);
		}
		catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Post("/api/playlists/:id/source-sync", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			auto b           = json::parse(req.body);
			std::string src  = b.value("source_id", "");
			std::string ext  = b.value("external_id", "");
			std::string kind = b.value("list_kind", "");
			if (src.empty() || ext.empty() || (kind != "playlist" && kind != "collection"))
			{
				route::err(res, 400, "source_id, external_id, list_kind required");
				return;
			}
			syncSourceListItems(res, "playlist", id, src, ext, kind, db_, sync_);
		}
		catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	// Recomputes a smart playlist's items from its stored filter_expr — the
	// per-playlist analog of plex-sync/source-sync, just against a locally
	// stored filter instead of a remote list. See SyncManager::
	// refreshSmartPlaylists for the bulk/background-thread equivalent that
	// runs automatically at the end of every full sync cycle.
	svr.Post("/api/playlists/:id/refresh-smart", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			int synced = PlaylistRepository(db_).refreshSmart(id, currentUser()->user_id);
			route::ok(res, json{{"synced", synced}}.dump());
		}
		catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	// Pushes this playlist's items TO a remote Plex/Jellyfin/Emby playlist or
	// collection — the write direction, symmetric with plex-sync/source-sync
	// above (see ListPushHelper.cpp). Creates a new remote list the first
	// time, reconciles (add/remove diff) on subsequent pushes to the same
	// linked target.
	svr.Post("/api/playlists/:id/push", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			auto b                      = json::parse(req.body);
			std::string source_id       = b.value("source_id", "");
			std::string kind            = b.value("kind", "");
			std::string title           = b.value("title", "");
			std::string external_lib_id = b.value("external_lib_id", "");
			if (source_id.empty() || (kind != "playlist" && kind != "collection"))
			{
				route::err(res, 400, "source_id and kind ('playlist'|'collection') required");
				return;
			}
			pushListToSource(res, id, source_id, kind, title, external_lib_id, db_, sync_);
		}
		catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/playlists/:id/plex-link", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			PlaylistRepository(db_).unlinkPlex(id);
			res.status = 204;
			res.set_content("", "application/json");
		}
		catch (const std::exception& e)
		{
			route::logErr("DELETE /api/playlists/:id/plex-link", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Delete("/api/playlists/:id/source-link", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			PlaylistRepository(db_).unlinkPlex(id);
			res.status = 204;
			res.set_content("", "application/json");
		}
		catch (const std::exception& e)
		{
			route::logErr("DELETE /api/playlists/:id/source-link", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/playlists/:id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			auto b = json::parse(req.body);
			PlaylistRepository repo(db_);
			if (b.contains("title")) repo.updateField(id, "title", b["title"].get<std::string>());
			if (b.contains("mode"))
			{
				std::string mode = b["mode"].get<std::string>();
				if (mode != "sequential" && mode != "show_collection" && mode != "shuffle")
				{
					route::err(res, 400, "mode must be 'sequential', 'show_collection', or 'shuffle'");
					return;
				}
				repo.updateField(id, "mode", mode);
			}
			// Smart-membership fields (Database.cpp migration 87) — setting
			// membership='smart' just marks the definition; the frontend
			// calls POST .../refresh-smart separately to actually populate
			// playlist_item from it (same "define, then sync" two-step the
			// Plex/Jellyfin/Emby link flow already uses).
			if (b.contains("membership"))
			{
				std::string membership = b["membership"].get<std::string>();
				if (membership != "static" && membership != "smart")
				{
					route::err(res, 400, "membership must be 'static' or 'smart'");
					return;
				}
				repo.updateField(id, "membership", membership);
			}
			if (b.contains("filter_expr")) repo.updateField(id, "filter_expr", b["filter_expr"].get<std::string>());
			if (b.contains("smart_type"))
			{
				std::string smart_type = b["smart_type"].get<std::string>();
				if (smart_type != "show" && smart_type != "movie" && smart_type != "mixed")
				{
					route::err(res, 400, "smart_type must be 'show', 'movie', or 'mixed'");
					return;
				}
				repo.updateField(id, "smart_type", smart_type);
			}
			if (b.contains("smart_sort")) repo.updateField(id, "smart_sort", b["smart_sort"].get<std::string>());
			if (b.contains("smart_sort_dir"))
			{
				std::string dir = b["smart_sort_dir"].get<std::string>();
				if (dir != "" && dir != "asc" && dir != "desc")
				{
					route::err(res, 400, "smart_sort_dir must be '', 'asc', or 'desc'");
					return;
				}
				repo.updateField(id, "smart_sort_dir", dir);
			}
			if (b.contains("smart_expand_episodes")) repo.updateField(id, "smart_expand_episodes", b["smart_expand_episodes"].get<bool>() ? "1" : "0");
			if (b.contains("smart_limit")) repo.updateField(id, "smart_limit", std::to_string(b["smart_limit"].get<int>()));

			// Home-shelf fields (Database.cpp migration 88) — a shelf is
			// just a smart playlist with show_on_home=1 (see
			// PlaylistRepository::listHomeShelves, which only ever reads
			// smart playlists — static ones have no filter to hand
			// getShows/getMovies the way the Home page's shelves need).
			if (b.contains("show_on_home"))
			{
				bool show_on_home = b["show_on_home"].get<bool>();
				if (show_on_home)
				{
					std::string membership = b.contains("membership")
												 ? b["membership"].get<std::string>()
												 : [&]
												 {
													 SQLite::Statement q(db_.get(), "SELECT membership FROM playlist WHERE playlist_id = ?");
													 q.bind(1, id);
													 return q.executeStep() ? q.getColumn(0).getString() : std::string();
												 }();
					if (membership != "smart")
					{
						route::err(res, 400, "only a smart playlist (membership='smart') can be shown on Home");
						return;
					}
				}
				repo.updateField(id, "show_on_home", show_on_home ? "1" : "0");
			}
			if (b.contains("home_order")) repo.updateField(id, "home_order", std::to_string(b["home_order"].get<int>()));
			if (b.contains("home_tile_limit"))
			{
				int limit = b["home_tile_limit"].get<int>();
				if (limit < 1)
				{
					route::err(res, 400, "home_tile_limit must be at least 1");
					return;
				}
				repo.updateField(id, "home_tile_limit", std::to_string(limit));
			}
			if (b.contains("home_active_start") || b.contains("home_active_end"))
			{
				std::string start = b.value("home_active_start", "");
				std::string end   = b.value("home_active_end", "");
				// Both empty (no window = always shown) or both a valid MM-DD —
				// one set without the other is an ambiguous half-window.
				if (!(start.empty() && end.empty()) && !(isValidMonthDay(start) && isValidMonthDay(end)))
				{
					route::err(res, 400, "home_active_start/home_active_end must both be 'MM-DD' or both empty");
					return;
				}
				repo.updateField(id, "home_active_start", start);
				repo.updateField(id, "home_active_end", end);
			}
			if (b.contains("poster_source")) repo.updateField(id, "poster_source", b["poster_source"].get<std::string>());
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PATCH /api/playlists/:id", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/playlists/:id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		try
		{
			PlaylistRepository(db_).remove(id);
			route::ok(res, json{{"deleted", id}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("DELETE /api/playlists/:id", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/playlists/:id/items", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto playlist_id = req.path_params.at("id");
		try
		{
			auto b                = json::parse(req.body);
			std::string item_type = b.value("item_type", "episode");
			std::string item_id   = b.value("item_id", "");
			if (item_id.empty())
			{
				route::err(res, 400, "item_id required");
				return;
			}
			auto [id, position] = PlaylistRepository(db_).addItem(playlist_id, item_type, item_id);
			res.status          = 201;
			route::ok(res, json{{"id", id}, {"position", position}}.dump());
		}
		catch (const SQLite::Exception& e)
		{
			route::logErr("POST /api/playlists/:id/items", e);
			route::err(res, 409, e.what());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/playlists/:id/items", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/playlists/:id/items/bulk", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto playlist_id = req.path_params.at("id");
		try
		{
			auto b = json::parse(req.body);
			std::vector<std::pair<std::string, std::string>> items;
			for (const auto& item : b.at("items")) items.emplace_back(item.value("item_type", "episode"), item.value("item_id", ""));
			int added = PlaylistRepository(db_).addItems(playlist_id, items);
			route::ok(res, json{{"added", added}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/playlists/:id/items/bulk", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/playlists/:id/items/:iid", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto playlist_id = req.path_params.at("id");
		auto iid         = std::stoi(req.path_params.at("iid"));
		try
		{
			PlaylistRepository(db_).removeItem(iid, playlist_id);
			route::ok(res, json{{"deleted", iid}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("DELETE /api/playlists/:id/items/:iid", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/playlists/:id/items/:iid", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto playlist_id = req.path_params.at("id");
		auto iid         = std::stoi(req.path_params.at("iid"));
		try
		{
			auto b = json::parse(req.body);
			if (b.contains("position")) PlaylistRepository(db_).moveItem(iid, playlist_id, b["position"].get<int>());
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PATCH /api/playlists/:id/items/:iid", e);
			route::err(res, 400, e.what());
		}
	});
}