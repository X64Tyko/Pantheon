#include "ContentService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../conf/ConfStore.h"
#include "../../db/ContentRepository.h"
#include "../../db/Database.h"
#include "../../source/MediaProbe.h"
#include "../../source/SyncManager.h"
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

ContentService::ContentService(const ServiceContext& ctx)
	: db_(ctx.db), conf_(ctx.conf), sync_(ctx.sync) {}

namespace {

// Per-item language probe results are cached in-memory since ffprobe is
// relatively expensive and a given file's language tracks never change.
struct ItemLangCache {
	std::mutex mtx;
	std::unordered_map<std::string, nlohmann::json> data;
};
ItemLangCache g_item_lang_cache;

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

	std::string token = is_cdn ? "" : conf_.token(sourceId);
	httplib::Result img;
	try {
		httplib::Client client(effective_base);
		if (!token.empty())
			client.set_default_headers({{"X-Plex-Token", token}, {"Accept", "*/*"}});
		client.set_connection_timeout(10);
		client.set_read_timeout(15);
		img = client.Get(fetch_path);
	} catch (const std::exception&) { res.status = 502; return; }
	if (!img || img->status != 200) { res.status = 502; return; }

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

	// ── Public image proxy — used by <img> tags that can't send auth headers ──
	// Fetches and caches any external image URL. Exempt from auth in isPublicPath.
	svr.Get("/api/images/proxy", [this](const Req& req, Res& res) {
		if (!req.has_param("url")) { res.status = 400; return; }
		proxyImage(req, req.get_param_value("url"), "", res);
	});

	// ── Libraries ────────────────────────────────────────────────────────────

	svr.Get("/api/libraries", [this](const Req&, Res& res) {
		ContentRepository repo(db_);
		json result = json::array();
		for (const auto& r : repo.listLibraries()) {
			result.push_back({
				{"library_id",   r.library_id},
				{"source_id",    r.source_id},
				{"display_name", r.display_name},
				{"library_type", r.library_type},
				{"source_name",  r.source_name},
				{"source_type",  r.source_type},
			});
		}
		route::ok(res, result.dump());
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
			              {"match_status",    r.match_status.empty() ? "unscraped" : r.match_status}};
			if (r.year)            entry["year"]            = *r.year;
			if (r.audience_rating) entry["audience_rating"] = *r.audience_rating;
			if (r.match_score)     entry["match_score"]     = *r.match_score;
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
			result.push_back({
				{"episode_id",  r.episode_id},
				{"season",      r.season},
				{"episode",     r.episode},
				{"title",       r.title},
				{"duration_ms", r.duration_ms},
				{"overview",    r.overview},
				{"air_date",    r.air_date},
				{"thumb",       r.thumb},
			});
		}
		route::ok(res, result.dump());
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
			              {"match_status",    r.match_status.empty() ? "unscraped" : r.match_status}};
			if (r.year)            entry["year"]            = *r.year;
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
		show["episode_count"]   = d->episode_count;
		show["labels"]          = parseArr(d->labels);
		show["network"]         = d->network;
		show["actors"]          = parseArr(d->actors);
		show["countries"]       = parseArr(d->countries);
		show["collections"]     = parseArr(d->collections);
		show["external_id"]     = d->external_id;
		show["source_id"]       = d->source_id;
		show["source_base_url"] = d->source_base_url;

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
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/shows/" + id, e); route::err(res, 400, e.what()); }
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

		auto parseArr = [](const std::string& s) -> json {
			try { return json::parse(s); } catch (...) { return json::array(); }
		};

		json movie;
		movie["movie_id"]        = d->movie_id;
		movie["title"]           = d->title;
		movie["content_rating"]  = d->content_rating;
		movie["duration_ms"]     = d->duration_ms;
		if (d->year)            movie["year"]            = *d->year;
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
		movie["labels"]          = parseArr(d->labels);
		movie["actors"]          = parseArr(d->actors);
		movie["countries"]       = parseArr(d->countries);
		movie["collections"]     = parseArr(d->collections);
		movie["external_id"]     = d->external_id;
		movie["source_id"]       = d->source_id;
		movie["source_base_url"] = d->source_base_url;
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
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/movies/" + id, e); route::err(res, 400, e.what()); }
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
}
