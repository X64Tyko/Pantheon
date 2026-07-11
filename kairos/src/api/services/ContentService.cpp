#include "ContentService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../conf/ConfStore.h"
#include "../../db/ContentRepository.h"
#include "../../db/Database.h"
#include "../../db/DuplicateRepository.h"
#include "../../db/MetadataOverrideRepository.h"
#include "../../db/RestrictionRepository.h"
#include "../../db/SourceRepository.h"
#include "../../model/WritebackFields.h"
#include "../../scraper/RatingSeverity.h"
#include "../../scraper/ScraperManager.h"
#include "../../source/IMediaSource.h"
#include "../../source/MediaProbe.h"
#include "../../source/SyncManager.h"
#include "../../util/PathMatch.h"
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;
namespace fs = std::filesystem;

static std::string imgCacheKey(const std::string& sourceId, const std::string& imgPath) {
	uint64_t h = 14695981039346656037ULL;
	for (unsigned char c : sourceId) { h ^= c; h *= 1099511628211ULL; }
	h ^= ':'; h *= 1099511628211ULL;
	for (unsigned char c : imgPath)  { h ^= c; h *= 1099511628211ULL; }
	std::ostringstream oss;
	oss << std::hex << std::setfill('0') << std::setw(16) << h;
	return oss.str();
}

// Mirrors proxyImage()'s own hash derivation (the CDN-vs-source_id base
// choice) without the network-fetch side — used to invalidate a cached
// image on demand so a "refresh images" action doesn't have to wait out
// image_cache_ttl_hours. Empty return = a redirect-only URL (never cached).
static std::string imgCacheHashFor(const std::string& imgPath, const std::string& sourceId) {
	bool is_cdn = (imgPath.rfind("http", 0) == 0);
	if (!is_cdn) return imgCacheKey(sourceId, imgPath);
	auto scheme_end = imgPath.find("://");
	auto path_start = (scheme_end != std::string::npos) ? imgPath.find('/', scheme_end + 3) : std::string::npos;
	if (path_start == std::string::npos) return "";
	return imgCacheKey(imgPath.substr(0, path_start), imgPath);
}

static void clearImageCache(const std::string& imgPath, const std::string& sourceId) {
	if (imgPath.empty()) return;
	std::string hash = imgCacheHashFor(imgPath, sourceId);
	if (hash.empty()) return;
	fs::path cache_dir = "image-cache";
	std::error_code ec;
	fs::remove(cache_dir / hash, ec);
	fs::remove(cache_dir / (hash + ".ct"), ec);
}

ContentService::ContentService(const ServiceContext& ctx, ScraperManager& scraper)
	: db_(ctx.db), conf_(ctx.conf), sync_(ctx.sync), scraper_(scraper) {}

namespace {

// Per-item language probe results are cached in-memory since ffprobe is
// relatively expensive and a given file's language tracks never change.
struct ItemLangCache {
	std::mutex mtx;
	std::unordered_map<std::string, nlohmann::json> data;
};
ItemLangCache g_item_lang_cache;

// Builds the parental-controls context for the current request's user, for
// ContentRepository's search methods (see RestrictionContext) — the service
// layer owns "what does restricted mean for this user," the repository just
// applies a plain ceiling + override lookup.
RestrictionContext restrictionFor(const std::string& entity_type) {
	RestrictionContext ctx;
	if (!currentUser() || !currentUser()->restricted) return ctx;
	ctx.restricted = true;
	ctx.user_id = currentUser()->user_id;
	ctx.rating_ceiling = (entity_type == "show")
		? RatingSeverity::tvRatingSeverity(currentUser()->max_tv_rating)
		: RatingSeverity::movieRatingSeverity(currentUser()->max_movie_rating);
	return ctx;
}

nlohmann::json probeLanguagesCached(const std::string& cacheKey, const std::string& filePath, ConfStore& conf) {
	{
		std::lock_guard<std::mutex> lk(g_item_lang_cache.mtx);
		auto it = g_item_lang_cache.data.find(cacheKey);
		if (it != g_item_lang_cache.data.end()) return it->second;
	}

	nlohmann::json result = {{"audio", nlohmann::json::array()}, {"subtitle", nlohmann::json::array()}};
	if (!filePath.empty()) {
		auto langs = probeStreamLanguages(conf.applyPathMap(filePath));
		for (auto& l : langs.audio)    result["audio"].push_back(l);
		for (auto& l : langs.subtitle) result["subtitle"].push_back(l);
	}

	std::lock_guard<std::mutex> lk(g_item_lang_cache.mtx);
	g_item_lang_cache.data[cacheKey] = result;
	return result;
}

// Same in-memory-cache-in-front-of-ffprobe shape as probeLanguagesCached
// above — a file's codec/resolution/bit-depth never change either.
struct ItemVideoInfoCache {
	std::mutex mtx;
	std::unordered_map<std::string, nlohmann::json> data;
};
ItemVideoInfoCache g_item_videoinfo_cache;

nlohmann::json probeVideoInfoCached(const std::string& cacheKey, const std::string& filePath, ConfStore& conf) {
	{
		std::lock_guard<std::mutex> lk(g_item_videoinfo_cache.mtx);
		auto it = g_item_videoinfo_cache.data.find(cacheKey);
		if (it != g_item_videoinfo_cache.data.end()) return it->second;
	}

	nlohmann::json result = {{"codec", ""}, {"width", 0}, {"height", 0}, {"bit_depth", 8}};
	if (!filePath.empty()) {
		auto info = probeVideoInfo(conf.applyPathMap(filePath));
		result["codec"]     = info.codec;
		result["width"]     = info.width;
		result["height"]    = info.height;
		result["bit_depth"] = info.bit_depth;
	}

	std::lock_guard<std::mutex> lk(g_item_videoinfo_cache.mtx);
	g_item_videoinfo_cache.data[cacheKey] = result;
	return result;
}

} // namespace

void ContentService::proxyImage(const Req& req,
                                 const std::string& imgPath,
                                 const std::string& sourceId,
                                 Res& res) {
	// For absolute CDN URLs (AniDB, TMDB, TVDB, etc.) split into base + path
	// so we can proxy and cache server-side rather than redirecting. Hotlink
	// protection on cdn.anidb.net blocks direct browser fetches.
	std::string effective_base, fetch_path;
	bool is_cdn = (imgPath.rfind("http", 0) == 0);

	if (is_cdn) {
		auto scheme_end = imgPath.find("://");
		auto path_start = (scheme_end != std::string::npos)
		                  ? imgPath.find('/', scheme_end + 3)
		                  : std::string::npos;
		if (path_start == std::string::npos) {
			res.set_redirect(imgPath);
			return;
		}
		effective_base = imgPath.substr(0, path_start);
		fetch_path     = imgPath.substr(path_start);
	} else {
		ContentRepository repo(db_);
		effective_base = repo.getSourceBaseUrl(sourceId);
		if (effective_base.empty()) { res.status = 404; return; }
		fetch_path = imgPath;
	}

	std::string hash = imgCacheKey(is_cdn ? effective_base : sourceId, imgPath);
	std::string etag = "\"" + hash + "\"";

	if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == etag) {
		res.status = 304;
		res.set_header("Cache-Control", "public, max-age=86400");
		res.set_header("ETag", etag);
		return;
	}

	fs::path cache_dir  = "image-cache";
	try { fs::create_directories(cache_dir); } catch (...) {}
	fs::path cache_file = cache_dir / hash;
	fs::path ct_file    = cache_dir / (hash + ".ct");
	fs::path fail_file  = cache_dir / (hash + ".fail");

	struct stat st{};
	long long ttl_secs = (long long)conf_.getImageCacheTtlHours() * 3600;
	bool cache_hit = (stat(cache_file.c_str(), &st) == 0) &&
	                 fs::exists(ct_file) &&
	                 (time(nullptr) - st.st_mtime < ttl_secs);

	if (cache_hit) {
		std::string ct = "image/jpeg";
		{ std::ifstream f(ct_file); if (f) std::getline(f, ct); }
		std::ifstream f(cache_file, std::ios::binary);
		std::string body((std::istreambuf_iterator<char>(f)), {});
		res.set_header("Cache-Control", "public, max-age=86400");
		res.set_header("ETag", etag);
		res.set_content(body, ct);
		return;
	}

	// Negative cache: a fetch that failed recently is not retried immediately.
	// Without this, a poster URL that 404s (a fairly common state — missing
	// AniDB art, a stale scrape, etc.) got re-fetched from scratch on every
	// single render anywhere in the app, and for AniDB every one of those
	// re-fetches also re-paid the 2.1 s global rate-limit wait below — a
	// library with even a handful of broken posters was enough to serialize
	// tens of seconds of blocked backend worker threads per page view,
	// which is what was actually stalling the rest of the app (client-side
	// request cancellation can't help here: the browser dropping its side
	// of the connection doesn't stop this thread's blocking rate-limit wait
	// or the synchronous upstream fetch already in flight).
	constexpr long long kNegativeCacheTtlSecs = 3600;
	struct stat fst{};
	if (stat(fail_file.c_str(), &fst) == 0 && (time(nullptr) - fst.st_mtime < kNegativeCacheTtlSecs)) {
		res.set_header("Cache-Control", "public, max-age=1800");
		res.status = 404;
		return;
	}

	// cdn.anidb.net has its own hotlink/abuse protection, separate from the API rate limit.
	if (is_cdn && effective_base.find("anidb.net") != std::string::npos)
		scraper_.anidbRateLimitImage();

	std::string token = is_cdn ? "" : conf_.token(sourceId);
	httplib::Result img;
	auto markFailed = [&]() { try { std::ofstream f(fail_file); f << "1"; } catch (...) {} };
	try {
		httplib::Client client(effective_base);
		httplib::Headers headers{
			{"User-Agent", "kairos/1.0 (https://github.com/X64Tyko/Pantheon)"},
			{"Referer", "https://anidb.net/"}
		};
		if (!token.empty()) { headers.emplace("X-Plex-Token", token); headers.emplace("Accept", "*/*"); }
		client.set_default_headers(headers);
		client.set_connection_timeout(5);
		client.set_read_timeout(8);
		img = client.Get(fetch_path);
	} catch (const std::exception&) { markFailed(); res.status = 502; return; }
	if (!img || img->status != 200) { markFailed(); res.status = 502; return; }

	// A retry that now succeeds (upstream came back, art got fixed, etc.)
	// clears any stale negative-cache marker from an earlier failure.
	{ std::error_code ec; fs::remove(fail_file, ec); }

	auto ct = img->get_header_value("Content-Type");
	if (ct.empty()) ct = "image/jpeg";

	try {
		{ std::ofstream f(cache_file, std::ios::binary); f.write(img->body.data(), (std::streamsize)img->body.size()); }
		{ std::ofstream f(ct_file);                      f << ct; }
	} catch (...) {}

	res.set_header("Cache-Control", "public, max-age=86400");
	res.set_header("ETag", etag);
	res.set_content(img->body, ct);
}

void ContentService::registerRoutes(httplib::Server& svr) {

	// Parental controls — called by Hermes (via authedHephaestusProxy, using
	// the caller's own Authorization header) before starting a VOD/preview
	// session, so restriction is enforced at the one already-authenticated
	// playback-start boundary rather than only hiding blocked items from
	// browse/list views. type: "movie" | "episode" | "show".
	svr.Get("/api/content/:type/:id/access-check", [this](const Req& req, Res& res) {
		if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
		auto type = req.path_params.at("type");
		auto id   = req.path_params.at("id");
		auto lookup = ContentRepository(db_).resolveForRestriction(type, id);
		bool allowed = RestrictionRepository(db_).isAllowed(
			*currentUser(), lookup.entity_type, lookup.entity_id, lookup.content_rating);
		route::ok(res, json{{"allowed", allowed}}.dump());
	});

	// ── Public image proxy — used by <img> tags that can't send auth headers ──
	// Fetches and caches any external image URL. Exempt from auth in isPublicPath.
	svr.Get("/api/images/proxy", [this](const Req& req, Res& res) {
		if (!req.has_param("url")) { res.status = 400; return; }
		proxyImage(req, req.get_param_value("url"), "", res);
	});

	// Re-fetch and re-apply this item's full metadata (overview, genres,
	// images, etc.) from whichever scraper it's already matched to, then
	// clear the image cache so a same-URL-but-updated poster/backdrop
	// doesn't keep serving stale cached bytes for the rest of
	// image_cache_ttl_hours. Locked fields are still respected (same as any
	// other match-apply path). 404 if the item has no confirmed match to
	// refresh from — nothing to re-fetch.
	svr.Post("/api/shows/:id/refresh-metadata", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		if (!scraper_.refreshMetadata(id, "show")) { route::err(res, 404, "No confirmed match to refresh from"); return; }
		ContentRepository repo(db_);
		if (auto t = repo.getShowThumb(id)) clearImageCache(t->image_path, t->source_id);
		if (auto a = repo.getShowArt(id))   clearImageCache(a->image_path, a->source_id);
		route::ok(res, json{{"ok", true}}.dump());
	});
	svr.Post("/api/movies/:id/refresh-metadata", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		if (!scraper_.refreshMetadata(id, "movie")) { route::err(res, 404, "No confirmed match to refresh from"); return; }
		ContentRepository repo(db_);
		if (auto t = repo.getMovieThumb(id)) clearImageCache(t->image_path, t->source_id);
		if (auto a = repo.getMovieArt(id))   clearImageCache(a->image_path, a->source_id);
		route::ok(res, json{{"ok", true}}.dump());
	});

	// Per-item scrape exemption — deliberately separate from PATCH /api/shows/:id
	// (which always locks the record as a side effect of a metadata edit; this
	// flag is orthogonal to that). Turning it on also clears any pending
	// uncertain/unmatched state immediately (see ContentRepository::setShowSkipScraping).
	svr.Patch("/api/shows/:id/skip-scraping", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			ContentRepository(db_).setShowSkipScraping(id, b.at("skip_scraping").get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});
	svr.Patch("/api/movies/:id/skip-scraping", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			ContentRepository(db_).setMovieSkipScraping(id, b.at("skip_scraping").get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	// Per-show opt-in for the automatic specials scan (runs during normal
	// sync — see SyncManager::scanSpecialsForEligibleShows). Separate route
	// for the same reason as skip-scraping above.
	svr.Patch("/api/shows/:id/find-specials", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			ContentRepository(db_).setShowFindSpecials(id, b.at("find_specials").get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	// 'season' (default) buckets season-0 episodes into one Specials group;
	// 'aired' interleaves them between numbered seasons by air date — see
	// useMediaDetail.ts's grouping logic on the frontend.
	svr.Patch("/api/shows/:id/episode-display-order", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string order = b.at("episode_display_order").get<std::string>();
			if (order != "season" && order != "aired") {
				route::err(res, 400, "episode_display_order must be 'season' or 'aired'");
				return;
			}
			ContentRepository(db_).setShowEpisodeDisplayOrder(id, order);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	// ── Libraries ────────────────────────────────────────────────────────────

	svr.Get("/api/libraries", [this](const Req&, Res& res) {
		ContentRepository repo(db_);
		json result = json::array();
		for (const auto& r : repo.listLibraries()) {
			result.push_back({
				{"library_id",    r.library_id},
				{"source_id",     r.source_id},
				{"display_name",  r.display_name},
				{"library_type",  r.library_type},
				{"source_name",   r.source_name},
				{"source_type",   r.source_type},
				{"show_on_home",  r.show_on_home},
			});
		}
		route::ok(res, result.dump());
	});

	// Focused endpoint for the Home shelf cards' "hide from Home" shortcut —
	// only needs a library_id, unlike SourceService's nested
	// /api/sources/:id/libraries/:lid which also needs the source_id.
	svr.Patch("/api/libraries/:id/home-visibility", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			auto b = json::parse(req.body);
			if (!b.contains("show_on_home")) { route::err(res, 400, "show_on_home required"); return; }
			SourceRepository(db_).setLibraryShowOnHome(req.path_params.at("id"), b["show_on_home"].get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/libraries/:id/home-visibility", e); route::err(res, 500, e.what());
		}
	});

	// ── Metadata values for filter autocomplete ───────────────────────────────

	svr.Get("/api/metadata/values", [this](const Req& req, Res& res) {
		std::string field, type, library_id;
		if (req.has_param("field"))      field      = req.get_param_value("field");
		if (req.has_param("type"))       type       = req.get_param_value("type");
		if (req.has_param("library_id")) library_id = req.get_param_value("library_id");
		if (field.empty()) { route::err(res, 400, "field required"); return; }
		try {
			ContentRepository repo(db_);
			auto vals = repo.getMetadataValues(field, type, library_id);
			json values = json::array();
			for (const auto& v : vals) values.push_back(v);
			route::ok(res, json{{"values", values}}.dump());
		} catch (const std::exception& e) { route::err(res, 500, e.what()); }
	});

	// ── Shows ─────────────────────────────────────────────────────────────────

	svr.Get("/api/shows", [this](const Req& req, Res& res) {
		ShowSearchParams p;
		if (req.has_param("limit"))          p.limit         = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset"))         p.offset        = std::stoi(req.get_param_value("offset"));
		if (req.has_param("library_id"))     p.library_id    = req.get_param_value("library_id");
		if (req.has_param("q"))              p.q             = req.get_param_value("q");
		if (req.has_param("genre"))          p.genre         = req.get_param_value("genre");
		if (req.has_param("year"))           p.year          = req.get_param_value("year");
		if (req.has_param("content_rating")) p.content_rating= req.get_param_value("content_rating");
		if (req.has_param("label"))          p.label         = req.get_param_value("label");
		if (req.has_param("network"))        p.network       = req.get_param_value("network");
		if (req.has_param("actor"))          p.actor         = req.get_param_value("actor");
		if (req.has_param("country"))        p.country       = req.get_param_value("country");
		if (req.has_param("collection"))     p.collection    = req.get_param_value("collection");
		if (req.has_param("studio"))         p.studio        = req.get_param_value("studio");
		if (req.has_param("sort"))           p.sort          = req.get_param_value("sort");
		p.restriction = restrictionFor("show");

		ContentRepository repo(db_);
		auto result = repo.searchShows(p);
		json items = json::array();
		for (const auto& r : result.items) {
			json entry = {{"show_id",         r.show_id},
			              {"title",           r.title},
			              {"content_rating",  r.content_rating},
			              {"episode_count",   r.episode_count},
			              {"thumb",           r.thumb},
			              {"art",             r.art},
			              {"source_base_url", r.source_base_url},
			              {"library_id",      r.library_id},
			              {"match_status",    r.match_status.empty() ? "unscraped" : r.match_status}};
			if (r.year)            entry["year"]            = *r.year;
			if (r.audience_rating) entry["audience_rating"] = *r.audience_rating;
			if (r.match_score)     entry["match_score"]     = *r.match_score;
			// Only for this specific sort — Home's Recently Aired shelf needs
			// to know exactly which episode to jump straight to; every other
			// sort (and Library's own use of this same endpoint) doesn't need
			// per-item episode data, so this stays a small N+1 over an
			// already-paginated (16-24 item) page rather than a batch query
			// that would complicate every other caller of searchShows().
			if (p.sort == "recently_aired") {
				if (auto ep = repo.getLatestAiredEpisode(r.show_id)) {
					entry["latest_episode"] = {
						{"episode_id", ep->episode_id},
						{"season",     ep->season},
						{"episode",    ep->episode},
						{"air_date",   ep->air_date},
					};
				}
			}
			items.push_back(std::move(entry));
		}
		route::ok(res, json{{"items", items}, {"total", result.total}}.dump());
	});

	svr.Get("/api/shows/:id/episodes", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string season_filter;
		if (req.has_param("season")) season_filter = req.get_param_value("season");

		ContentRepository repo(db_);
		auto rows = repo.listEpisodesForShow(id, season_filter);
		json result = json::array();
		for (const auto& r : rows) {
			json entry{
				{"episode_id",  r.episode_id},
				{"season",      r.season},
				{"episode",     r.episode},
				{"title",       r.title},
				{"duration_ms", r.duration_ms},
				{"overview",    r.overview},
				{"air_date",    r.air_date},
				{"thumb",       r.thumb},
			};
			if (!r.file_path.empty()) entry["file_path"] = r.file_path;
			result.push_back(std::move(entry));
		}
		route::ok(res, result.dump());
	});

	// Minimal single-episode lookup — title/overview/show_title/season/episode
	// only, for admin-facing "what's this playing" views (the connected-
	// devices list) and Continue Watching's hero (Home/TV: HeroEpisodeOverride;
	// Roku: setHeroEpisode) that only have a bare episode_id to resolve into
	// something displayable. Not a full episode detail endpoint (no thumb/
	// file_path) — add fields here only when another caller actually needs them.
	svr.Get("/api/episodes/:id", [this](const Req& req, Res& res) {
		if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
		ContentRepository repo(db_);
		auto e = repo.getEpisode(req.path_params.at("id"));
		if (!e) { route::err(res, 404, "episode not found"); return; }
		route::ok(res, json{
			{"episode_id", e->episode_id},
			{"season",     e->season},
			{"episode",    e->episode},
			{"title",      e->title},
			{"overview",   e->overview},
			{"show_id",    e->show_id},
			{"show_title", e->show_title},
		}.dump());
	});

	// Next playable episode after :id, honoring the show's episode_display_order
	// (see ContentRepository::getNextEpisode) — used by the player's up-next/
	// auto-advance and by resolvePlayTarget once the last-watched episode is
	// completed. "null" (not 404) when :id is the last playable episode.
	// Deliberately NOT named ".../next" — Router.cpp's isPublicPath exempts
	// any path ending in "/next" from auth (for GET /api/channels/:id/next,
	// a live-channel schedule lookup Hephaestus/DVR clients hit with no
	// session) — this route would otherwise silently leak episode metadata
	// (including of restricted/parental-gated content) unauthenticated.
	svr.Get("/api/episodes/:id/next-episode", [this](const Req& req, Res& res) {
		ContentRepository repo(db_);
		auto next = repo.getNextEpisode(req.path_params.at("id"));
		if (!next) { route::ok(res, "null"); return; }

		route::ok(res, json{
			{"episode_id",  next->episode_id},
			{"season",      next->season},
			{"episode",     next->episode},
			{"title",       next->title},
			{"duration_ms", next->duration_ms},
			{"overview",    next->overview},
			{"air_date",    next->air_date},
			{"thumb",       next->thumb},
		}.dump());
	});

	svr.Get("/api/shows/:id/seasons", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		ContentRepository repo(db_);
		json seasons = json::array();
		for (const auto& r : repo.listSeasons(id))
			seasons.push_back({{"number", r.number}, {"name", r.name}});
		route::ok(res, json{{"seasons", seasons}}.dump());
	});

	// Audio/subtitle languages, probed from one representative episode file
	// and cached in-memory (ffprobe is too slow to run per list render).
	svr.Get("/api/shows/:id/languages", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string path;
		try {
			SQLite::Statement q(db_.get(),
				"SELECT file_path FROM episode WHERE show_id = ? AND file_path != '' "
				"ORDER BY season, episode LIMIT 1");
			q.bind(1, id);
			if (q.executeStep()) path = q.getColumn(0).getString();
		} catch (const std::exception& e) {
			route::logErr("GET /api/shows/" + id + "/languages", e);
		}
		route::ok(res, probeLanguagesCached("show:" + id, path, conf_).dump());
	});

	// Codec/resolution/bit-depth, probed from the same representative
	// episode file the languages endpoint uses, same in-memory cache shape.
	svr.Get("/api/shows/:id/videoinfo", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string path;
		try {
			SQLite::Statement q(db_.get(),
				"SELECT file_path FROM episode WHERE show_id = ? AND file_path != '' "
				"ORDER BY season, episode LIMIT 1");
			q.bind(1, id);
			if (q.executeStep()) path = q.getColumn(0).getString();
		} catch (const std::exception& e) {
			route::logErr("GET /api/shows/" + id + "/videoinfo", e);
		}
		route::ok(res, probeVideoInfoCached("show:" + id, path, conf_).dump());
	});

	svr.Get("/api/episodes", [this](const Req& req, Res& res) {
		int         limit   = 50, offset = 0, season_v = -1;
		std::string show_id, search_q;
		if (req.has_param("limit"))   limit    = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset"))  offset   = std::stoi(req.get_param_value("offset"));
		if (req.has_param("show_id")) show_id  = req.get_param_value("show_id");
		if (req.has_param("q"))       search_q = req.get_param_value("q");
		if (req.has_param("season"))  season_v = std::stoi(req.get_param_value("season"));

		ContentRepository repo(db_);
		auto rows = repo.searchEpisodes(show_id, search_q, season_v, limit, offset);
		json items = json::array();
		for (const auto& r : rows) {
			items.push_back({
				{"episode_id",  r.episode_id},
				{"season",      r.season},
				{"episode",     r.episode},
				{"title",       r.title},
				{"duration_ms", r.duration_ms},
				{"show_id",     r.show_id},
				{"show_title",  r.show_title},
			});
		}
		route::ok(res, json{{"items", items}}.dump());
	});

	// ── Movies ────────────────────────────────────────────────────────────────

	svr.Get("/api/movies", [this](const Req& req, Res& res) {
		MovieSearchParams p;
		if (req.has_param("limit"))          p.limit         = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset"))         p.offset        = std::stoi(req.get_param_value("offset"));
		if (req.has_param("library_id"))     p.library_id    = req.get_param_value("library_id");
		if (req.has_param("q"))              p.q             = req.get_param_value("q");
		if (req.has_param("genre"))          p.genre         = req.get_param_value("genre");
		if (req.has_param("year"))           p.year          = req.get_param_value("year");
		if (req.has_param("content_rating")) p.content_rating= req.get_param_value("content_rating");
		if (req.has_param("label"))          p.label         = req.get_param_value("label");
		if (req.has_param("actor"))          p.actor         = req.get_param_value("actor");
		if (req.has_param("country"))        p.country       = req.get_param_value("country");
		if (req.has_param("collection"))     p.collection    = req.get_param_value("collection");
		if (req.has_param("studio"))         p.studio        = req.get_param_value("studio");
		if (req.has_param("sort"))           p.sort          = req.get_param_value("sort");
		p.restriction = restrictionFor("movie");

		ContentRepository repo(db_);
		auto result = repo.searchMovies(p);
		json items = json::array();
		for (const auto& r : result.items) {
			json entry = {{"movie_id",        r.movie_id},
			              {"title",           r.title},
			              {"content_rating",  r.content_rating},
			              {"duration_ms",     r.duration_ms},
			              {"thumb",           r.thumb},
			              {"art",             r.art},
			              {"source_base_url", r.source_base_url},
			              {"library_id",      r.library_id},
			              {"match_status",    r.match_status.empty() ? "unscraped" : r.match_status}};
			if (r.year)            entry["year"]            = *r.year;
			if (!r.release_date.empty()) entry["release_date"] = r.release_date;
			if (r.audience_rating) entry["audience_rating"] = *r.audience_rating;
			if (r.match_score)     entry["match_score"]     = *r.match_score;
			items.push_back(std::move(entry));
		}
		route::ok(res, json{{"items", items}, {"total", result.total}}.dump());
	});

	// ── Show detail ───────────────────────────────────────────────────────────

	svr.Get("/api/shows/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		ContentRepository repo(db_);
		auto d = repo.getShowDetail(id);
		if (!d) { route::err(res, 404, "show not found"); return; }
		// Same 404 as a genuinely missing show — don't reveal existence of
		// blocked content via a distinct "forbidden" response.
		if (currentUser() && currentUser()->restricted
		    && !RestrictionRepository(db_).isAllowed(*currentUser(), "show", id, d->content_rating)) {
			route::err(res, 404, "show not found"); return;
		}

		auto parseArr = [](const std::string& s) -> json {
			try { return json::parse(s); } catch (...) { return json::array(); }
		};

		json show;
		show["show_id"]                 = d->show_id;
		show["title"]                   = d->title;
		show["content_rating"]          = d->content_rating;
		show["overview"]                = d->overview;
		show["studio"]                  = d->studio;
		show["status"]                  = d->status;
		show["genres"]                  = parseArr(d->genres);
		show["thumb"]                   = d->thumb;
		show["art"]                     = d->art;
		show["imdb_id"]                 = d->imdb_id;
		show["tvdb_id"]                 = d->tvdb_id;
		show["tmdb_id"]                 = d->tmdb_id;
		show["originally_available_at"] = d->originally_available_at;
		if (d->year)            show["year"]            = *d->year;
		if (d->audience_rating) show["audience_rating"] = *d->audience_rating;
		show["locked"]          = d->locked;
		show["skip_scraping"]   = d->skip_scraping;
		show["find_specials"]   = d->find_specials;
		show["episode_display_order"] = d->episode_display_order;
		show["episode_count"]   = d->episode_count;
		show["labels"]          = parseArr(d->labels);
		show["network"]         = d->network;
		show["actors"]          = parseArr(d->actors);
		show["countries"]       = parseArr(d->countries);
		show["collections"]     = parseArr(d->collections);
		show["external_id"]     = d->external_id;
		show["source_id"]       = d->source_id;
		show["source_base_url"] = d->source_base_url;
		show["match_status"]    = d->match_status;
		if (d->match_score) show["match_score"] = *d->match_score;
		show["match_confirmed"] = d->match_confirmed;
		if (!d->folder_path.empty()) show["folder_path"] = d->folder_path;

		// Full set of sources this show is mapped to (source_id/source_base_url above are just one of them).
		json sources = json::array();
		for (const auto& t : SourceRepository(db_).getWritebackTargets("show", id))
			sources.push_back({{"source_id", t.source_id}, {"source_type", t.source_type}, {"display_name", t.display_name}, {"external_id", t.external_id}});
		show["sources"] = std::move(sources);

		json seasons = json::array();
		for (const auto& s : d->seasons)
			seasons.push_back({{"number", s.number}, {"name", s.name}});
		show["seasons"] = std::move(seasons);

		route::ok(res, show.dump());
	});

	svr.Patch("/api/shows/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (sync_.isMediaLocked()) { route::err(res, 423, "sync in progress"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			auto jsonStr = [](const json& j) { return j.is_array() ? j.dump() : j.get<std::string>(); };

			std::vector<StrField> sf;
			std::vector<IntField> intf;
			if (b.contains("title"))                   sf.push_back({"title",                   b["title"].get<std::string>()});
			if (b.contains("overview"))                sf.push_back({"overview",                b["overview"].get<std::string>()});
			if (b.contains("studio"))                  sf.push_back({"studio",                  b["studio"].get<std::string>()});
			if (b.contains("status"))                  sf.push_back({"status",                  b["status"].get<std::string>()});
			if (b.contains("content_rating"))          sf.push_back({"content_rating",          b["content_rating"].get<std::string>()});
			if (b.contains("originally_available_at")) sf.push_back({"originally_available_at", b["originally_available_at"].get<std::string>()});
			if (b.contains("imdb_id"))                 sf.push_back({"imdb_id",                 b["imdb_id"].get<std::string>()});
			if (b.contains("tvdb_id"))                 sf.push_back({"tvdb_id",                 b["tvdb_id"].get<std::string>()});
			if (b.contains("tmdb_id"))                 sf.push_back({"tmdb_id",                 b["tmdb_id"].get<std::string>()});
			if (b.contains("thumb"))                   sf.push_back({"thumb",                   b["thumb"].get<std::string>()});
			if (b.contains("art"))                     sf.push_back({"art",                     b["art"].get<std::string>()});
			if (b.contains("genres"))                  sf.push_back({"genres",                  jsonStr(b["genres"])});
			if (b.contains("labels"))                  sf.push_back({"labels",                  jsonStr(b["labels"])});
			if (b.contains("network"))                 sf.push_back({"network",                 b["network"].get<std::string>()});
			if (b.contains("actors"))                  sf.push_back({"actors",                  jsonStr(b["actors"])});
			if (b.contains("countries"))               sf.push_back({"countries",               jsonStr(b["countries"])});
			if (b.contains("collections"))             sf.push_back({"collections",             jsonStr(b["collections"])});
			if (b.contains("year"))                    intf.push_back({"year", b["year"].get<int>()});

			ContentRepository(db_).updateShow(id, sf, intf);

			// A manual edit is an explicit, human-confirmed correction — record it as a
			// per-field override so it survives future syncs even if `locked` is later
			// cleared, without freezing the whole row the way `locked` alone would.
			MetadataOverrideRepository ovr(db_);
			for (const auto& f : sf)   ovr.set("show", id, f.col, f.val, "user");
			for (const auto& f : intf) ovr.set("show", id, f.col, std::to_string(f.val), "user");

			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/shows/" + id, e); route::err(res, 400, e.what()); }
	});

	svr.Post("/api/shows/:id/writeback", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			ContentRepository repo(db_);
			auto d = repo.getShowDetail(id);
			if (!d) { route::err(res, 404, "show not found"); return; }
			// Writeback is gated on a human-confirmed match, never an auto-accepted
			// one — see acceptCandidate() and the "Push to Sources" plan.
			if (!d->match_confirmed) { route::err(res, 403, "match not confirmed"); return; }

			WritebackFields fields;
			fields.title           = d->title;
			fields.overview        = d->overview;
			fields.genres          = d->genres;
			fields.content_rating  = d->content_rating;
			fields.studio          = d->studio;
			fields.network         = d->network;
			fields.actors          = d->actors;
			fields.countries       = d->countries;
			fields.collections     = d->collections;
			fields.release_date    = d->originally_available_at;

			auto targets = SourceRepository(db_).getWritebackTargets("show", id);
			json results = json::array();
			for (const auto& t : targets) {
				auto* src = sync_.findSource(t.source_id);
				bool ok = src && src->pushMetadata(t.external_id, t.external_lib_id, "show", fields);
				results.push_back({{"source_id", t.source_id}, {"source_type", t.source_type}, {"ok", ok}});
			}
			route::ok(res, json{{"results", results}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/shows/:id/writeback", e); route::err(res, 500, e.what()); }
	});

	// `:id` survives; body.duplicate_id is absorbed and gone (see mergeShowInto).
	svr.Post("/api/shows/:id/merge", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (sync_.isMediaLocked()) { route::err(res, 423, "sync in progress"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string dup_id = b.value("duplicate_id", "");
			if (dup_id.empty()) { route::err(res, 400, "duplicate_id required"); return; }
			bool confirmed = b.value("confirm", false);

			ContentRepository repo(db_);
			auto d_target = repo.getShowDetail(id);
			auto d_dup    = repo.getShowDetail(dup_id);
			if (!d_target) { route::err(res, 404, "show not found"); return; }
			if (!d_dup)    { route::err(res, 404, "duplicate show not found"); return; }

			// Compare via the same path-map + cheap-normalize logic
			// SyncManager's own dedup already uses — comparing raw, unmapped
			// paths (as before) could flag two folders as "different" that
			// sync-time dedup already treats as the same file, once
			// different sources' mount points are accounted for.
			if (!confirmed && !d_target->folder_path.empty() && !d_dup->folder_path.empty()
			        && pathutil::compareFolders(conf_.applyPathMap(d_target->folder_path),
			                                     conf_.applyPathMap(d_dup->folder_path)) != pathutil::FolderMatch::kExactCheap) {
				res.status = 409;
				route::ok(res, json{
					{"error", "folder_mismatch"},
					{"target_folder", d_target->folder_path},
					{"duplicate_folder", d_dup->folder_path},
				}.dump());
				return;
			}

			repo.mergeShowInto(id, dup_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/shows/:id/merge", e); route::err(res, 400, e.what()); }
	});

	svr.Get("/api/shows/:id/thumb", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		auto item = ContentRepository(db_).getShowThumb(id);
		if (!item) { res.status = 404; return; }
		proxyImage(req, item->image_path, item->source_id, res);
	});

	svr.Get("/api/shows/:id/art", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		auto item = ContentRepository(db_).getShowArt(id);
		if (!item) { res.status = 404; return; }
		proxyImage(req, item->image_path, item->source_id, res);
	});

	svr.Get("/api/episodes/:id/thumb", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		auto item = ContentRepository(db_).getEpisodeThumb(id);
		if (!item) { res.status = 404; return; }
		proxyImage(req, item->image_path, item->source_id, res);
	});

	// ── Movie detail ──────────────────────────────────────────────────────────

	svr.Get("/api/movies/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		ContentRepository repo(db_);
		auto d = repo.getMovieDetail(id);
		if (!d) { route::err(res, 404, "movie not found"); return; }
		// Same 404 as a genuinely missing movie — don't reveal existence of
		// blocked content via a distinct "forbidden" response.
		if (currentUser() && currentUser()->restricted
		    && !RestrictionRepository(db_).isAllowed(*currentUser(), "movie", id, d->content_rating)) {
			route::err(res, 404, "movie not found"); return;
		}

		auto parseArr = [](const std::string& s) -> json {
			try { return json::parse(s); } catch (...) { return json::array(); }
		};

		json movie;
		movie["movie_id"]        = d->movie_id;
		movie["title"]           = d->title;
		movie["content_rating"]  = d->content_rating;
		movie["duration_ms"]     = d->duration_ms;
		if (d->year)            movie["year"]            = *d->year;
		if (!d->release_date.empty()) movie["release_date"] = d->release_date;
		if (d->audience_rating) movie["audience_rating"] = *d->audience_rating;
		movie["overview"]        = d->overview;
		movie["tagline"]         = d->tagline;
		movie["studio"]          = d->studio;
		movie["director"]        = d->director;
		movie["genres"]          = parseArr(d->genres);
		movie["thumb"]           = d->thumb;
		movie["art"]             = d->art;
		movie["imdb_id"]         = d->imdb_id;
		movie["tmdb_id"]         = d->tmdb_id;
		movie["locked"]          = d->locked;
		movie["skip_scraping"]   = d->skip_scraping;
		movie["labels"]          = parseArr(d->labels);
		movie["actors"]          = parseArr(d->actors);
		movie["countries"]       = parseArr(d->countries);
		movie["collections"]     = parseArr(d->collections);
		movie["external_id"]     = d->external_id;
		movie["source_id"]       = d->source_id;
		movie["source_base_url"] = d->source_base_url;
		if (!d->file_path.empty()) movie["file_path"] = d->file_path;
		if (!d->folder_path.empty()) movie["folder_path"] = d->folder_path;
		movie["match_status"]    = d->match_status;
		if (d->match_score) movie["match_score"] = *d->match_score;
		movie["match_confirmed"] = d->match_confirmed;

		// Full set of sources this movie is mapped to.
		json sources = json::array();
		for (const auto& t : SourceRepository(db_).getWritebackTargets("movie", id))
			sources.push_back({{"source_id", t.source_id}, {"source_type", t.source_type}, {"display_name", t.display_name}, {"external_id", t.external_id}});
		movie["sources"] = std::move(sources);

		route::ok(res, movie.dump());
	});

	svr.Patch("/api/movies/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (sync_.isMediaLocked()) { route::err(res, 423, "sync in progress"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			auto jsonStr = [](const json& j) { return j.is_array() ? j.dump() : j.get<std::string>(); };

			std::vector<StrField> sf;
			std::vector<IntField> intf;
			if (b.contains("title"))          sf.push_back({"title",          b["title"].get<std::string>()});
			if (b.contains("overview"))       sf.push_back({"overview",       b["overview"].get<std::string>()});
			if (b.contains("tagline"))        sf.push_back({"tagline",        b["tagline"].get<std::string>()});
			if (b.contains("studio"))         sf.push_back({"studio",         b["studio"].get<std::string>()});
			if (b.contains("director"))       sf.push_back({"director",       b["director"].get<std::string>()});
			if (b.contains("content_rating")) sf.push_back({"content_rating", b["content_rating"].get<std::string>()});
			if (b.contains("imdb_id"))        sf.push_back({"imdb_id",        b["imdb_id"].get<std::string>()});
			if (b.contains("tmdb_id"))        sf.push_back({"tmdb_id",        b["tmdb_id"].get<std::string>()});
			if (b.contains("thumb"))          sf.push_back({"thumb",          b["thumb"].get<std::string>()});
			if (b.contains("art"))            sf.push_back({"art",            b["art"].get<std::string>()});
			if (b.contains("genres"))         sf.push_back({"genres",         jsonStr(b["genres"])});
			if (b.contains("labels"))         sf.push_back({"labels",         jsonStr(b["labels"])});
			if (b.contains("actors"))         sf.push_back({"actors",         jsonStr(b["actors"])});
			if (b.contains("countries"))      sf.push_back({"countries",      jsonStr(b["countries"])});
			if (b.contains("collections"))    sf.push_back({"collections",    jsonStr(b["collections"])});
			if (b.contains("year"))           intf.push_back({"year", b["year"].get<int>()});

			ContentRepository(db_).updateMovie(id, sf, intf);

			// A manual edit is an explicit, human-confirmed correction — record it as a
			// per-field override so it survives future syncs even if `locked` is later
			// cleared, without freezing the whole row the way `locked` alone would.
			MetadataOverrideRepository ovr(db_);
			for (const auto& f : sf)   ovr.set("movie", id, f.col, f.val, "user");
			for (const auto& f : intf) ovr.set("movie", id, f.col, std::to_string(f.val), "user");

			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/movies/" + id, e); route::err(res, 400, e.what()); }
	});

	svr.Post("/api/movies/:id/writeback", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			ContentRepository repo(db_);
			auto d = repo.getMovieDetail(id);
			if (!d) { route::err(res, 404, "movie not found"); return; }
			// Writeback is gated on a human-confirmed match, never an auto-accepted
			// one — see acceptCandidate() and the "Push to Sources" plan.
			if (!d->match_confirmed) { route::err(res, 403, "match not confirmed"); return; }

			WritebackFields fields;
			fields.title           = d->title;
			fields.overview        = d->overview;
			fields.genres          = d->genres;
			fields.content_rating  = d->content_rating;
			fields.studio          = d->studio;
			fields.director        = d->director;
			fields.tagline         = d->tagline;
			fields.actors          = d->actors;
			fields.countries       = d->countries;
			fields.collections     = d->collections;
			fields.release_date    = d->release_date;

			auto targets = SourceRepository(db_).getWritebackTargets("movie", id);
			json results = json::array();
			for (const auto& t : targets) {
				auto* src = sync_.findSource(t.source_id);
				bool ok = src && src->pushMetadata(t.external_id, t.external_lib_id, "movie", fields);
				results.push_back({{"source_id", t.source_id}, {"source_type", t.source_type}, {"ok", ok}});
			}
			route::ok(res, json{{"results", results}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/movies/:id/writeback", e); route::err(res, 500, e.what()); }
	});

	// Link a duplicate movie onto this one — see the analogous show route above.
	svr.Post("/api/movies/:id/merge", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (sync_.isMediaLocked()) { route::err(res, 423, "sync in progress"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string dup_id = b.value("duplicate_id", "");
			if (dup_id.empty()) { route::err(res, 400, "duplicate_id required"); return; }
			bool confirmed = b.value("confirm", false);

			ContentRepository repo(db_);
			auto d_target = repo.getMovieDetail(id);
			auto d_dup    = repo.getMovieDetail(dup_id);
			if (!d_target) { route::err(res, 404, "movie not found"); return; }
			if (!d_dup)    { route::err(res, 404, "duplicate movie not found"); return; }

			// See the analogous show route above for why this is path-mapped
			// + cheap-normalized rather than a raw string compare.
			if (!confirmed && !d_target->folder_path.empty() && !d_dup->folder_path.empty()
			        && pathutil::compareFolders(conf_.applyPathMap(d_target->folder_path),
			                                     conf_.applyPathMap(d_dup->folder_path)) != pathutil::FolderMatch::kExactCheap) {
				res.status = 409;
				route::ok(res, json{
					{"error", "folder_mismatch"},
					{"target_folder", d_target->folder_path},
					{"duplicate_folder", d_dup->folder_path},
				}.dump());
				return;
			}

			repo.mergeMovieInto(id, dup_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/movies/:id/merge", e); route::err(res, 400, e.what()); }
	});

	svr.Get("/api/movies/:id/thumb", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		auto item = ContentRepository(db_).getMovieThumb(id);
		if (!item) { res.status = 404; return; }
		proxyImage(req, item->image_path, item->source_id, res);
	});

	svr.Get("/api/movies/:id/art", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		auto item = ContentRepository(db_).getMovieArt(id);
		if (!item) { res.status = 404; return; }
		proxyImage(req, item->image_path, item->source_id, res);
	});

	svr.Get("/api/movies/:id/languages", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string path;
		try {
			SQLite::Statement q(db_.get(), "SELECT file_path FROM movie WHERE movie_id = ?");
			q.bind(1, id);
			if (q.executeStep()) path = q.getColumn(0).getString();
		} catch (const std::exception& e) {
			route::logErr("GET /api/movies/" + id + "/languages", e);
		}
		route::ok(res, probeLanguagesCached("movie:" + id, path, conf_).dump());
	});

	svr.Get("/api/movies/:id/videoinfo", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string path;
		try {
			SQLite::Statement q(db_.get(), "SELECT file_path FROM movie WHERE movie_id = ?");
			q.bind(1, id);
			if (q.executeStep()) path = q.getColumn(0).getString();
		} catch (const std::exception& e) {
			route::logErr("GET /api/movies/" + id + "/videoinfo", e);
		}
		route::ok(res, probeVideoInfoCached("movie:" + id, path, conf_).dump());
	});

	// GET /api/duplicates/queue — pending sync-time "possible duplicate" candidates
	// (see SyncManager's tiered dedup + duplicate_candidate table).
	svr.Get("/api/duplicates/queue", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		std::string item_type = req.has_param("item_type") ? req.get_param_value("item_type") : "";
		int limit = 48, offset = 0;
		if (req.has_param("limit"))  { try { limit  = std::stoi(req.get_param_value("limit"));  } catch (...) {} }
		if (req.has_param("offset")) { try { offset = std::stoi(req.get_param_value("offset")); } catch (...) {} }

		DuplicateRepository repo(db_);
		auto rows = repo.listPending(item_type, limit, offset);
		json items = json::array();
		for (const auto& r : rows) {
			items.push_back({
				{"candidate_id",     r.candidate_id},
				{"item_type",        r.item_type},
				{"kairos_id_a",      r.kairos_id_a},
				{"kairos_id_b",      r.kairos_id_b},
				{"title_a",          r.title_a},
				{"title_b",          r.title_b},
				{"year_a",           r.year_a},
				{"year_b",           r.year_b},
				{"thumb_a",          r.thumb_a},
				{"thumb_b",          r.thumb_b},
				{"trigger",          r.trigger},
				{"reason",           r.reason},
				{"title_similarity", r.title_similarity},
				{"folder_a",         r.folder_a},
				{"folder_b",         r.folder_b},
				{"created_at",       r.created_at},
			});
		}
		route::ok(res, json{{"items", items}, {"total", repo.countPending(item_type)}}.dump());
	});

	// POST /api/duplicates/:id/dismiss — mark a candidate pair "not a duplicate"
	// (remembered permanently — a later sync re-detecting the same pair is a no-op).
	svr.Post("/api/duplicates/:id/dismiss", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		if (DuplicateRepository(db_).dismiss(id)) route::ok(res, json{{"ok", true}}.dump());
		else route::err(res, 404, "candidate not found");
	});
}
