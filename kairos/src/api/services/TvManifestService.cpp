#include "TvManifestService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../db/ContentRepository.h"
#include "../../db/Database.h"
#include "../../db/PlaylistRepository.h"
#include "../../scraper/RatingSeverity.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
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

	// Same "service layer owns what 'restricted' means, repository just
	// applies a ceiling + override lookup" split as ContentService.cpp's own
	// restrictionFor — duplicated rather than shared since it's three lines
	// and pulling it into a header for one more caller isn't worth the
	// indirection.
	RestrictionContext restrictionFor(const std::string& entity_type)
	{
		RestrictionContext ctx;
		if (!currentUser() || !currentUser()->restricted) return ctx;
		ctx.restricted     = true;
		ctx.user_id        = currentUser()->user_id;
		ctx.rating_ceiling = (entity_type == "show")
								 ? RatingSeverity::tvRatingSeverity(currentUser()->max_tv_rating)
								 : RatingSeverity::movieRatingSeverity(currentUser()->max_movie_rating);
		return ctx;
	}

	// Same shape GET /api/mixed-media/hydrate already serializes a
	// MixedTileRow into, plus the new optional latest_episode.
	json tileJson(const MixedTileRow& r)
	{
		json entry = {
			{"content_type", r.content_type},
			{"id", r.id},
			{"title", r.title},
			{"thumb", r.thumb},
			{"art", r.art},
			{"library_id", r.library_id},
			{"watched", r.watched},
			{"view_count", r.view_count},
		};
		if (r.year) entry["year"] = *r.year;
		if (r.audience_rating) entry["audience_rating"] = *r.audience_rating;
		if (r.content_type == "show") entry["episode_count"] = r.episode_count;
		if (r.content_type == "episode")
		{
			entry["duration_ms"] = r.duration_ms;
			entry["season"]      = r.season;
			entry["episode"]     = r.episode;
			entry["show_id"]     = r.show_id;
			entry["show_title"]  = r.show_title;
		}
		if (r.latest_episode)
		{
			entry["latest_episode"] = {
				{"episode_id", r.latest_episode->episode_id},
				{"season", r.latest_episode->season},
				{"episode", r.latest_episode->episode},
				{"air_date", r.latest_episode->air_date},
			};
		}
		return entry;
	}

	// hero's "recently-added shows+movies, art preferred" merge is resolved
	// server-side by GET /api/tv/shelf-items' "hero" content_type branch (see
	// registerRoutes below) — the client just forwards this filter, same as
	// every other row, instead of fetching two sources and merging itself.
	json heroRowJson(int order)
	{
		json filter = {
			{"content_type", "hero"}, {"sort", "recently_added"}, {"limit", 16},
			{"home", true}, {"hide_empty", true},
		};
		return {
			{"id", "hero"}, {"order", order}, {"type", "hero"},
			{"filter", filter},
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
									"SELECT id, row_order, row_type, title, content_type, params_json, item_action, end_tile, empty_behavior "
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
					if (row_type == "watch-together")
					{
						// No filter/dataSource — a Watch Together session's shape
						// (host/member_count) doesn't fit the uniform ShelfTile GET
						// /api/tv/shelf-items already serves every other row through,
						// so the client fetches its own data (GET
						// /api/watch-together/active) whenever this row is present,
						// same "presence is the whole signal" pattern as 'guide' above.
						home_rows.push_back({{"id", id}, {"order", order}, {"type", "watch-together"}});
						continue;
					}

					auto title          = q.getColumn(3).getString();
					auto content_type   = q.getColumn(4).getString();
					auto filter         = json::parse(q.getColumn(5).getString());
					auto item_action    = q.getColumn(6).getString();
					auto end_tile       = q.getColumn(7).getString();
					auto empty_behavior = q.getColumn(8).getString();

					// content_type folds into the same opaque object as every other
					// filter key -- the client never special-cases it, just forwards
					// the whole thing to GET /api/tv/shelf-items (empty for
					// continue-watching, whose id the client already matches on to
					// skip the generic filter mechanism entirely).
					filter["content_type"] = content_type;

					json row = {
						{"id", id}, {"order", order}, {"type", "shelf"}, {"title", title},
						{"filter", filter},
						{"emptyBehavior", empty_behavior},
					};
					// item_action/end_tile are '' when unset (see v81 seed data)
					// -- omitted rather than emitted empty, so the client's own
					// "open-detail is the default" fallback actually applies.
					if (!item_action.empty()) row["itemAction"] = item_action;
					if (!end_tile.empty()) row["endTile"] = end_tile;
					home_rows.push_back(row);
				}
			}

			// Playlist-backed home shelves (a smart playlist's "show on home"
			// toggle, see PlaylistRepository::listHomeShelves) -- previously only
			// the regular web Home page (hades/src/pages/HomePage.tsx's
			// customShelves, via GET /api/home-playlists) ever rendered these;
			// /tv and every native client silently never saw them since this
			// manifest only ever read the static tv_shelf table. Ordered after
			// every tv_shelf row, preserving listHomeShelves' own home_order/
			// title ordering among themselves.
			//
			// content_type mirrors HomePage.tsx's own dispatch (see
			// mixedToShelfEntry/showToMixed/movieToMixed there): a "mixed" smart
			// playlist AND a "show"-typed one with smart_expand_episodes both
			// resolve through the mixed index/hydrate path server-side -- GET
			// /api/tv/shelf-items' "mixed" branch -- distinguished only by
			// include_movies (movies excluded for the show+expand_episodes case,
			// since that's a pure per-episode feed of one show's own episodes).
			// This is the actual fix for the bug that prompted this refactor: the
			// old dataSource-endpoint-selection ternary here had no third branch
			// for "mixed" at all and silently fell back to /api/shows, dropping
			// every movie from a mixed home shelf.
			for (const auto& shelf : PlaylistRepository(db_).listHomeShelves())
			{
				const bool is_mixed = shelf.smart_type == "mixed" ||
					(shelf.smart_type == "show" && shelf.smart_expand_episodes);

				json filter = {
					{"content_type", is_mixed ? "mixed" : shelf.smart_type},
					{"limit", shelf.home_tile_limit}, {"sort", shelf.smart_sort},
					{"sort_dir", shelf.smart_sort_dir},
					{"filter", shelf.filter_expr}, {"home", true}, {"hide_empty", true},
				};
				if (is_mixed) filter["include_movies"] = (shelf.smart_type == "mixed");

				home_rows.push_back({
					{"id", "playlist-" + shelf.playlist_id}, {"order", ++max_order},
					{"type", "shelf"}, {"title", shelf.title},
					{"filter", filter},
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

	// Resolves one Home row's `filter` (see above) into render-ready tiles —
	// the one generic call every manifest consumer (Hades /tv, native
	// clients) makes for every shelf and the hero row, regardless of what
	// content_type says. Unlike GET /api/tv/manifest this is NOT public (see
	// Router.cpp's isPublicPath) — it needs currentUser() for restriction/
	// watch-state scoping, same as /api/shows and /api/movies already do.
	svr.Get("/api/tv/shelf-items", [this](const Req& req, Res& res)
	{
		if (!currentUser())
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		try
		{
			auto paramBool = [&](const char* name, bool def)
			{
				if (!req.has_param(name)) return def;
				auto v = req.get_param_value(name);
				return v == "1" || v == "true";
			};
			auto paramInt = [&](const char* name, int def)
			{
				if (!req.has_param(name)) return def;
				try { return std::stoi(req.get_param_value(name)); }
				catch (...) { return def; }
			};
			auto paramStr = [&](const char* name)
			{
				return req.has_param(name) ? req.get_param_value(name) : std::string();
			};

			const std::string content_type = paramStr("content_type");
			const std::string sort         = paramStr("sort");
			const std::string sort_dir     = paramStr("sort_dir");
			const std::string filter_expr  = paramStr("filter");
			// Every caller of this endpoint is a bounded Home shelf/hero row —
			// home_only/hide_empty default to true here (unlike /api/shows'/
			// /api/movies' own defaults) since nothing else calls this route.
			const int limit       = paramInt("limit", 16);
			const bool home_only  = paramBool("home", true);
			const bool hide_empty = paramBool("hide_empty", true);

			ContentRepository repo(db_);
			const std::string user_id = currentUser()->user_id;
			json items                = json::array();

			if (content_type == "movie")
			{
				MovieSearchParams p;
				p.limit       = limit;
				p.sort        = sort;
				p.sort_dir    = sort_dir;
				p.filter      = filter_expr;
				p.home_only   = home_only;
				p.hide_empty  = hide_empty;
				p.restriction = restrictionFor("movie");
				p.user_id     = user_id;
				for (const auto& r : repo.searchMovies(p).items) items.push_back(tileJson(ContentRepository::toTile(r)));
			}
			else if (content_type == "show")
			{
				ShowSearchParams p;
				p.limit       = limit;
				p.sort        = sort;
				p.sort_dir    = sort_dir;
				p.filter      = filter_expr;
				p.home_only   = home_only;
				p.hide_empty  = hide_empty;
				p.restriction = restrictionFor("show");
				p.user_id     = user_id;
				for (const auto& r : repo.searchShows(p).items)
				{
					auto tile = ContentRepository::toTile(r);
					// Only for this sort — Home's Recently Aired shelf needs to
					// know exactly which episode to jump to; every other sort
					// doesn't, so this stays a small N+1 over an already-bounded
					// (~16-24 item) page, same posture as GET /api/shows' own
					// identical conditional lookup.
					if (sort == "recently_aired")
					{
						if (auto ep = repo.getLatestAiredEpisode(r.show_id))
						{
							tile.latest_episode = MixedTileRow::LatestEpisodeRef{
								ep->episode_id, ep->air_date, ep->season, ep->episode
							};
						}
					}
					items.push_back(tileJson(tile));
				}
			}
			else if (content_type == "mixed")
			{
				MixedSearchParams p;
				p.filter     = filter_expr;
				p.sort       = sort;
				p.sort_dir   = sort_dir;
				p.home_only  = home_only;
				p.hide_empty = hide_empty;
				// A mixed home shelf is always episode-granularity for shows
				// (see MixedSort.h — a "mixed" playlist's membership is never
				// whole shows); include_movies distinguishes a real "mixed"
				// smart playlist from a show+smart_expand_episodes one, which
				// wants episodes only (see TvManifestService's own row-
				// building code above).
				p.expand_episodes   = true;
				p.include_movies    = paramBool("include_movies", true);
				p.restriction_show  = restrictionFor("show");
				p.restriction_movie = restrictionFor("movie");
				p.user_id           = user_id;

				auto index = repo.searchMixedIndex(p);
				if (static_cast<int>(index.size()) > limit) index.resize(limit);
				std::vector<std::pair<std::string, std::string>> ids;
				ids.reserve(index.size());
				for (auto& e : index) ids.emplace_back(e.content_type, e.id);
				for (const auto& r : repo.hydrateMixed(ids, p.restriction_show, p.restriction_movie, user_id)) items.push_back(tileJson(r));
			}
			else if (content_type == "hero")
			{
				// Whole shows + whole movies, art preferred with a shows-only
				// fallback if nothing has art — the same two-fetch-then-merge
				// TvHome.tsx/HomeViewModel used to do client-side (twice),
				// moved here so it only exists once.
				ShowSearchParams sp;
				sp.limit       = limit;
				sp.sort        = sort;
				sp.home_only   = home_only;
				sp.hide_empty  = hide_empty;
				sp.restriction = restrictionFor("show");
				sp.user_id     = user_id;
				MovieSearchParams mp;
				mp.limit       = limit;
				mp.sort        = sort;
				mp.home_only   = home_only;
				mp.hide_empty  = hide_empty;
				mp.restriction = restrictionFor("movie");
				mp.user_id     = user_id;

				std::vector<json> shows_all, shows_with_art, movies_with_art;
				for (const auto& r : repo.searchShows(sp).items)
				{
					json j = tileJson(ContentRepository::toTile(r));
					shows_all.push_back(j);
					if (!r.art.empty()) shows_with_art.push_back(std::move(j));
				}
				for (const auto& r : repo.searchMovies(mp).items)
				{
					if (!r.art.empty()) movies_with_art.push_back(tileJson(ContentRepository::toTile(r)));
				}

				std::vector<json> merged = std::move(shows_with_art);
				merged.insert(merged.end(),
							  std::make_move_iterator(movies_with_art.begin()),
							  std::make_move_iterator(movies_with_art.end()));
				if (merged.empty()) merged = std::move(shows_all);
				for (auto& j : merged) items.push_back(std::move(j));
			}
			// Any other content_type (empty, or a row like continue-watching
			// that no client ever actually calls this endpoint for) resolves
			// to an empty item list rather than an error.

			route::ok(res, json{{"items", items}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/tv/shelf-items", e);
			route::err(res, 500, e.what());
		}
	});
}