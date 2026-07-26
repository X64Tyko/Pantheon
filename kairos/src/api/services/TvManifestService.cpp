#include "TvManifestService.h"
#include "../RouteHelpers.h"
#include "../../db/Database.h"
#include "../../db/PlaylistRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

TvManifestService::TvManifestService(const ServiceContext& ctx)
	: db_(ctx.db)
{
}

namespace
{
	// Design tokens generated from hades/src/index.css by
	// hades/scripts/generate-tv-tokens.mjs — "styling comes from the manifest,
	// not a per-client hardcoded guess" (see that script's own header comment).
	// KAIROS_TV_TOKENS_PATH lets dev.sh/dev.ps1 point this at the repo-relative
	// kairos/assets/tv-tokens.json for local dev, same env-var-over-CLI-flag
	// convention as KAIROS_API_THREADS elsewhere in this codebase (no Router/
	// ServiceContext/TvManifestService constructor signature changes needed —
	// several momus test fixtures construct these directly). The Docker image
	// bakes a copy at the default path (kairos/Dockerfile), so production never
	// needs the env var set at all. Missing/unreadable/malformed file → nullopt,
	// not an error — the manifest is still fully usable without a theme, same
	// "degrade gracefully" reasoning as this route's other optional fields.
	std::optional<json> loadThemeTokens()
	{
		const char* env_path   = std::getenv("KAIROS_TV_TOKENS_PATH");
		const std::string path = env_path ? env_path : "/usr/local/share/kairos/assets/tv-tokens.json";

		std::ifstream f(path);
		if (!f) return std::nullopt;
		std::ostringstream buf;
		buf << f.rdbuf();
		try
		{
			return json::parse(buf.str());
		}
		catch (const json::exception&)
		{
			return std::nullopt;
		}
	}

	// hero's data sources are two merged endpoints (recently-added shows +
	// movies, filtered to items with backdrop art) — not reproducible as a
	// single {endpoint, params} pair the way every other shelf is, so this shape
	// is fixed here rather than read from tv_shelf.params_json (which is left
	// '{}' for the hero row in the v81 seed data).
	json heroRowJson(int order)
	{
		json params = {{"limit", 16}, {"sort", "recently_added"}, {"home", true}, {"hide_empty", true}};
		return {
			{"id", "hero"}, {"order", order}, {"type", "hero"},
			{
				"dataSources", {
					{"shows", {{"endpoint", "/api/shows"}, {"params", params}}},
					{"movies", {{"endpoint", "/api/movies"}, {"params", params}}},
				}
			},
			{"requiresArt", true},
			{"actions", json::array({"play-resolved", "open-detail"})},
		};
	}
} // namespace

void TvManifestService::registerRoutes(httplib::Server& svr)
{
	svr.Get("/api/tv/manifest", [this](const Req&, Res& res)
	{
		try
		{
			json home_rows = json::array();
			int max_order  = 0;
			{
				SQLite::Statement q(db_.get(),
									"SELECT id, row_order, row_type, title, endpoint, params_json, item_action, end_tile, empty_behavior "
									"FROM tv_shelf WHERE enabled = 1 ORDER BY row_order");
				while (q.executeStep())
				{
					auto id       = q.getColumn(0).getString();
					int order     = q.getColumn(1).getInt();
					auto row_type = q.getColumn(2).getString();
					max_order     = std::max(max_order, order);

					if (row_type == "hero")
					{
						home_rows.push_back(heroRowJson(order));
						continue;
					}
					if (row_type == "guide")
					{
						home_rows.push_back({{"id", id}, {"order", order}, {"type", "guide"}});
						continue;
					}

					auto title          = q.getColumn(3).getString();
					auto endpoint       = q.getColumn(4).getString();
					auto params         = json::parse(q.getColumn(5).getString());
					auto item_action    = q.getColumn(6).getString();
					auto end_tile       = q.getColumn(7).getString();
					auto empty_behavior = q.getColumn(8).getString();

					json row = {
						{"id", id}, {"order", order}, {"type", "shelf"}, {"title", title},
						{"dataSource", {{"endpoint", endpoint}, {"params", params}}},
						{"emptyBehavior", empty_behavior},
					};
					// item_action/end_tile are '' when unset (see v81 seed data)
					// — omitted rather than emitted empty, so the client's own
					// "open-detail is the default" fallback actually applies.
					if (!item_action.empty()) row["itemAction"] = item_action;
					if (!end_tile.empty()) row["endTile"] = end_tile;
					home_rows.push_back(row);
				}
			}

			// Playlist-backed home shelves (a smart playlist's "show on home"
			// toggle, see PlaylistRepository::listHomeShelves) — previously
			// only the regular web Home page (hades/src/pages/HomePage.tsx's
			// customShelves, via GET /api/home-playlists) ever rendered
			// these; /tv and every native client silently never saw them
			// since this manifest only ever read the static tv_shelf table.
			// Reuses the exact same dataSource shape (endpoint + params)
			// every tv_shelf row above already emits, so no wire-contract
			// change is needed on any client — they already know how to
			// fetch/render a "shelf" row pointed at /api/shows or
			// /api/movies. Ordered after every tv_shelf row, preserving
			// listHomeShelves' own home_order/title ordering among themselves.
			for (const auto& shelf : PlaylistRepository(db_).listHomeShelves())
			{
				json params = {
					{"limit", shelf.home_tile_limit}, {"sort", shelf.smart_sort},
					{"filter", shelf.filter_expr}, {"home", true}, {"hide_empty", true},
				};
				home_rows.push_back({
					{"id", "playlist-" + shelf.playlist_id}, {"order", ++max_order},
					{"type", "shelf"}, {"title", shelf.title},
					{
						"dataSource", {
							{"endpoint", shelf.smart_type == "movie" ? "/api/movies" : "/api/shows"},
							{"params", params},
						}
					},
					{"itemAction", "open-detail"}, {"endTile", "navigate-library"},
					{"emptyBehavior", "hide"},
				});
			}

			auto zonesForScreen = [this](const std::string& screen)
			{
				json zones = json::array();
				SQLite::Statement q(db_.get(),
									"SELECT zone_id, zone_order, config_json FROM tv_zone "
									"WHERE screen = ? AND enabled = 1 ORDER BY zone_order");
				q.bind(1, screen);
				while (q.executeStep())
				{
					json zone   = {{"id", q.getColumn(0).getString()}, {"order", q.getColumn(1).getInt()}};
					auto config = json::parse(q.getColumn(2).getString());
					zone.update(config); // merges dataSource/filterFields/itemAction/showOnly — whatever this zone declares
					zones.push_back(zone);
				}
				return zones;
			};

			json manifest = {
				{"version", 1},
				{"home", {{"rows", home_rows}}},
				{"library", {{"zones", zonesForScreen("library")}}},
				{"detail", {{"zones", zonesForScreen("detail")}}},
				{"guide", {{"zones", zonesForScreen("guide")}}},
			};
			if (auto theme = loadThemeTokens()) manifest["theme"] = *theme;

			route::ok(res, manifest.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/tv/manifest", e);
			route::err(res, 500, e.what());
		}
	});
}