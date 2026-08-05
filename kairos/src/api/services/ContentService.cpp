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
#include "../../db/SubtitleTrackRepository.h"
#include "../../model/WritebackFields.h"
#include "../../scraper/RatingSeverity.h"
#include "../../scraper/ScraperManager.h"
#include "../../source/IMediaSource.h"
#include "../../source/MediaProbe.h"
#include "../../source/SyncManager.h"
#include "../../util/PathMatch.h"
#include "thread/TaskRegistry.h"
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;
namespace fs = std::filesystem;

static std::string imgCacheKey(const std::string& sourceId, const std::string& imgPath)
{
	uint64_t h = 14695981039346656037ULL;
	for (unsigned char c : sourceId)
	{
		h ^= c;
		h *= 1099511628211ULL;
	}
	h ^= ':';
	h *= 1099511628211ULL;
	for (unsigned char c : imgPath)
	{
		h ^= c;
		h *= 1099511628211ULL;
	}
	std::ostringstream oss;
	oss << std::hex << std::setfill('0') << std::setw(16) << h;
	return oss.str();
}

// Mirrors proxyImage()'s own hash derivation (the CDN-vs-source_id base
// choice) without the network-fetch side — used to invalidate a cached
// image on demand so a "refresh images" action doesn't have to wait out
// image_cache_ttl_hours. Empty return = a redirect-only URL (never cached).
static std::string imgCacheHashFor(const std::string& imgPath, const std::string& sourceId)
{
	bool is_cdn = (imgPath.rfind("http", 0) == 0);
	if (!is_cdn) return imgCacheKey(sourceId, imgPath);
	auto scheme_end = imgPath.find("://");
	auto path_start = (scheme_end != std::string::npos) ? imgPath.find('/', scheme_end + 3) : std::string::npos;
	if (path_start == std::string::npos) return "";
	return imgCacheKey(imgPath.substr(0, path_start), imgPath);
}

static void clearImageCache(const std::string& imgPath, const std::string& sourceId)
{
	if (imgPath.empty()) return;
	std::string hash = imgCacheHashFor(imgPath, sourceId);
	if (hash.empty()) return;
	fs::path cache_dir = "image-cache";
	std::error_code ec;
	fs::remove(cache_dir / hash, ec);
	fs::remove(cache_dir / (hash + ".ct"), ec);
}

ContentService::ContentService(const ServiceContext& ctx, ScraperManager& scraper)
	: db_(ctx.db)
	, conf_(ctx.conf)
	, sync_(ctx.sync)
	, scraper_(scraper)
{
}

namespace
{
	// Case-insensitive prefix check — used only for the "local:" sentinel guard
	// below, so a bypass attempt like "LOCAL:/etc/passwd" can't slip past a
	// literal-case check on one side while still matching case-insensitively
	// (or not) on the other.
	bool startsWithCI(const std::string& v, const std::string& prefix)
	{
		if (v.size() < prefix.size()) return false;
		for (size_t i = 0; i < prefix.size(); ++i) if (std::tolower(static_cast<unsigned char>(v[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
		return true;
	}

	// "local:" is proxyImage()'s internal sentinel for serving a file straight
	// off disk — produced by sync-time code only (ScraperManager::
	// persistAnidbThumbLocally, and LocalSource's own poster.jpg/fanart.jpg/NFO
	// sidecar detection, see SidecarMetadata.h), never by user input. thumb/art
	// are otherwise free-text admin-edit fields (PATCH /api/shows/:id, PATCH
	// /api/movies/:id); without this guard an admin could type
	// "local:/etc/passwd" into one and turn the resulting GET .../thumb route —
	// which only requires a valid session, not admin — into an arbitrary local
	// file read for any logged-in user.
	bool isLocalSentinel(const std::string& v) { return startsWithCI(v, "local:"); }

	// SSRF guard for the *unauthenticated* /api/images/proxy route only — never
	// applied inside proxyImage() itself, since its other callers (DB-driven
	// /thumb, /art) legitimately target a self-hosted Plex/Jellyfin server that's
	// often on a private LAN address, and imgPath there is never user input. The
	// public route takes an arbitrary URL straight from an anonymous request's
	// querystring, so without this it's an open proxy an outside attacker can
	// point at internal Docker services (kairos:8080, hephaestus:8082) or a cloud
	// metadata endpoint (169.254.169.254) and read the response back.
	bool isPrivateOrReservedAddress(const sockaddr* sa)
	{
		if (sa->sa_family == AF_INET)
		{
			uint32_t ip  = ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr);
			auto inRange = [ip](uint32_t base, int bits)
			{
				uint32_t mask = bits == 0 ? 0u : (~0u << (32 - bits));
				return (ip & mask) == (base & mask);
			};
			return inRange(0x00000000, 8) || // 0.0.0.0/8
				inRange(0x7F000000, 8) ||    // 127.0.0.0/8 loopback
				inRange(0x0A000000, 8) ||    // 10.0.0.0/8
				inRange(0xAC100000, 12) ||   // 172.16.0.0/12 (covers Docker's default bridge too)
				inRange(0xC0A80000, 16) ||   // 192.168.0.0/16
				inRange(0xA9FE0000, 16) ||   // 169.254.0.0/16 link-local + cloud metadata
				inRange(0x64400000, 10);     // 100.64.0.0/10 CGNAT
		}
		if (sa->sa_family == AF_INET6)
		{
			const auto* a6 = &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
			if (IN6_IS_ADDR_LOOPBACK(a6) || IN6_IS_ADDR_LINKLOCAL(a6) || IN6_IS_ADDR_SITELOCAL(a6) ||
				IN6_IS_ADDR_V4MAPPED(a6))
				return true;
			if ((a6->s6_addr[0] & 0xFE) == 0xFC) return true; // fc00::/7 unique local
			return false;
		}
		return true; // unknown address family — fail closed
	}

	std::string extractHost(const std::string& url)
	{
		auto scheme_end  = url.find("://");
		std::string rest = scheme_end != std::string::npos ? url.substr(scheme_end + 3) : url;
		auto slash       = rest.find('/');
		if (slash != std::string::npos) rest = rest.substr(0, slash);
		auto at = rest.find('@'); // strip userinfo@ if present
		if (at != std::string::npos) rest = rest.substr(at + 1);
		if (!rest.empty() && rest.front() == '[')
		{
			// IPv6 literal, e.g. [::1]:443
			auto close = rest.find(']');
			if (close != std::string::npos) return rest.substr(1, close - 1);
		}
		auto colon = rest.rfind(':');
		if (colon != std::string::npos) rest = rest.substr(0, colon);
		return rest;
	}

	// Resolves host and rejects if ANY resolved address is private/loopback/
	// link-local/reserved — this blocks both literal private-IP URLs and a
	// public-looking DNS name that resolves to an internal address (DNS
	// rebinding), which a simple string denylist on the hostname wouldn't catch.
	bool isSafeExternalHost(const std::string& host)
	{
		if (host.empty() || startsWithCI(host, "localhost")) return false;
		addrinfo hints{};
		hints.ai_family  = AF_UNSPEC;
		addrinfo* result = nullptr;
		if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) return false;
		bool safe = true;
		for (auto* p = result; p; p = p->ai_next)
		{
			if (isPrivateOrReservedAddress(p->ai_addr))
			{
				safe = false;
				break;
			}
		}
		freeaddrinfo(result);
		return safe;
	}

	// Builds the parental-controls context for the current request's user, for
	// ContentRepository's search methods (see RestrictionContext) — the service
	// layer owns "what does restricted mean for this user," the repository just
	// applies a plain ceiling + override lookup.
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

	// External sidecar subtitle files (subtitle_track table) are a separate
	// source from the embedded_subtitle_languages column and have to be merged
	// in on top — same "union, don't denormalize" reasoning as
	// ContentRepository::getMetadataValues's subtitle_language filter facet.
	void addExternalSubtitleLanguages(nlohmann::json& result, const std::vector<SubtitleTrack>& tracks)
	{
		std::set<std::string> seen(result["subtitle"].begin(), result["subtitle"].end());
		for (const auto& t : tracks)
		{
			if (!t.language.empty() && seen.insert(t.language).second) result["subtitle"].push_back(t.language);
		}
	}

	// Shows have no single subtitle_track owner the way a movie does — sidecar
	// files are attached per-episode, so this aggregates across every episode of
	// the show rather than a single file.
	void addExternalSubtitleLanguagesForShow(nlohmann::json& result, const std::string& show_id, Database& db)
	{
		std::set<std::string> seen(result["subtitle"].begin(), result["subtitle"].end());
		try
		{
			SQLite::Statement q(db.get(),
								"SELECT DISTINCT st.language FROM subtitle_track st "
								"JOIN episode e ON e.episode_id = st.media_id "
								"WHERE st.media_type = 'episode' AND e.show_id = ? AND st.language != '' AND st.valid = 1");
			q.bind(1, show_id);
			while (q.executeStep())
			{
				auto lang = q.getColumn(0).getString();
				if (seen.insert(lang).second) result["subtitle"].push_back(lang);
			}
		}
		catch (const std::exception& e)
		{
			route::logErr("addExternalSubtitleLanguagesForShow", e);
		}
	}

	// audio_languages/embedded_subtitle_languages are persisted at sync time
	// (SyncManager::syncMediaProbeFromFiles) — no need to re-run ffprobe
	// on-demand per detail-page view the way this endpoint used to (that
	// predates the v86 migration; see Database.cpp's schema-history comment for
	// it). Plain DB reads, same json_each pattern as ContentRepository::
	// getMetadataValues's audio_language/subtitle_language filter facets, just
	// scoped to one id instead of the whole library.
	nlohmann::json movieLanguagesFromDb(Database& db, const std::string& id)
	{
		nlohmann::json result = {{"audio", nlohmann::json::array()}, {"subtitle", nlohmann::json::array()}};
		try
		{
			SQLite::Statement q(db.get(),
								"SELECT DISTINCT je.value FROM movie m, json_each(NULLIF(m.audio_languages,'')) je "
								"WHERE m.movie_id = ? AND je.value != ''");
			q.bind(1, id);
			while (q.executeStep()) result["audio"].push_back(q.getColumn(0).getString());
		}
		catch (const std::exception& e) { route::logErr("movieLanguagesFromDb (audio)", e); }
		try
		{
			SQLite::Statement q(db.get(),
								"SELECT DISTINCT je.value FROM movie m, json_each(NULLIF(m.embedded_subtitle_languages,'')) je "
								"WHERE m.movie_id = ? AND je.value != ''");
			q.bind(1, id);
			while (q.executeStep()) result["subtitle"].push_back(q.getColumn(0).getString());
		}
		catch (const std::exception& e) { route::logErr("movieLanguagesFromDb (subtitle)", e); }
		addExternalSubtitleLanguages(result, SubtitleTrackRepository(db).get("movie", id));
		return result;
	}

	// Same as movieLanguagesFromDb but aggregated (DISTINCT) across every
	// episode of the show, same reasoning addExternalSubtitleLanguagesForShow
	// already documents for the external-subtitle half.
	nlohmann::json showLanguagesFromDb(Database& db, const std::string& id)
	{
		nlohmann::json result = {{"audio", nlohmann::json::array()}, {"subtitle", nlohmann::json::array()}};
		try
		{
			SQLite::Statement q(db.get(),
								"SELECT DISTINCT je.value FROM episode e, json_each(NULLIF(e.audio_languages,'')) je "
								"WHERE e.show_id = ? AND je.value != ''");
			q.bind(1, id);
			while (q.executeStep()) result["audio"].push_back(q.getColumn(0).getString());
		}
		catch (const std::exception& e) { route::logErr("showLanguagesFromDb (audio)", e); }
		try
		{
			SQLite::Statement q(db.get(),
								"SELECT DISTINCT je.value FROM episode e, json_each(NULLIF(e.embedded_subtitle_languages,'')) je "
								"WHERE e.show_id = ? AND je.value != ''");
			q.bind(1, id);
			while (q.executeStep()) result["subtitle"].push_back(q.getColumn(0).getString());
		}
		catch (const std::exception& e) { route::logErr("showLanguagesFromDb (subtitle)", e); }
		addExternalSubtitleLanguagesForShow(result, id, db);
		return result;
	}

	// Same in-memory-cache-in-front-of-ffprobe shape as probeLanguagesCached
	// above — a file's codec/resolution/bit-depth never change either.
	struct ItemVideoInfoCache
	{
		std::mutex mtx;
		std::unordered_map<std::string, nlohmann::json> data;
	};

	ItemVideoInfoCache g_item_videoinfo_cache;

	nlohmann::json probeVideoInfoCached(const std::string& cacheKey, const std::string& filePath, ConfStore& conf)
	{
		{
			std::lock_guard<std::mutex> lk(g_item_videoinfo_cache.mtx);
			auto it = g_item_videoinfo_cache.data.find(cacheKey);
			if (it != g_item_videoinfo_cache.data.end()) return it->second;
		}

		nlohmann::json result = {{"codec", ""}, {"width", 0}, {"height", 0}, {"bit_depth", 8}};
		if (!filePath.empty())
		{
			auto info           = probeVideoInfo(conf.applyPathMap(filePath));
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
								Res& res)
{
	// Permanently downloaded art (currently just AniDB posters, opt-in via
	// anidb_download_posters — see ScraperManager::persistAnidbThumbLocally)
	// lives on disk next to the media instead of behind a remote CDN, so it's
	// served directly with no network fetch, no rate limit, and no TTL-driven
	// re-fetch — the file itself is the cache.
	if (isLocalSentinel(imgPath))
	{
		fs::path local_path = imgPath.substr(6);
		std::error_code ec;
		if (!fs::exists(local_path, ec))
		{
			res.status = 404;
			return;
		}
		std::string local_etag = "\"" + imgCacheKey("local", imgPath) + "\"";
		if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == local_etag)
		{
			res.status = 304;
			res.set_header("Cache-Control", "public, max-age=86400");
			res.set_header("ETag", local_etag);
			return;
		}
		std::ifstream f(local_path, std::ios::binary);
		if (!f)
		{
			res.status = 404;
			return;
		}
		std::string body((std::istreambuf_iterator<char>(f)), {});
		res.set_header("Cache-Control", "public, max-age=86400");
		res.set_header("ETag", local_etag);
		res.set_content(body, "image/jpeg");
		return;
	}

	// For absolute CDN URLs (AniDB, TMDB, TVDB, etc.) split into base + path
	// so we can proxy and cache server-side rather than redirecting. Hotlink
	// protection on cdn.anidb.net blocks direct browser fetches.
	std::string effective_base, fetch_path;
	bool is_cdn = (imgPath.rfind("http", 0) == 0);

	if (is_cdn)
	{
		auto scheme_end = imgPath.find("://");
		auto path_start = (scheme_end != std::string::npos)
							  ? imgPath.find('/', scheme_end + 3)
							  : std::string::npos;
		if (path_start == std::string::npos)
		{
			res.set_redirect(imgPath);
			return;
		}
		effective_base = imgPath.substr(0, path_start);
		fetch_path     = imgPath.substr(path_start);
	}
	else
	{
		ContentRepository repo(db_);
		effective_base = repo.getSourceBaseUrl(sourceId);
		if (effective_base.empty())
		{
			res.status = 404;
			return;
		}
		fetch_path = imgPath;
	}

	std::string hash = imgCacheKey(is_cdn ? effective_base : sourceId, imgPath);
	std::string etag = "\"" + hash + "\"";

	if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == etag)
	{
		res.status = 304;
		res.set_header("Cache-Control", "public, max-age=86400");
		res.set_header("ETag", etag);
		return;
	}

	fs::path cache_dir = "image-cache";
	try { fs::create_directories(cache_dir); }
	catch (...)
	{
	}
	fs::path cache_file = cache_dir / hash;
	fs::path ct_file    = cache_dir / (hash + ".ct");
	fs::path fail_file  = cache_dir / (hash + ".fail");

	struct stat st{};
	long long ttl_secs = (long long)conf_.getImageCacheTtlHours() * 3600;
	bool cache_hit     = (stat(cache_file.c_str(), &st) == 0) &&
		fs::exists(ct_file) &&
		(time(nullptr) - st.st_mtime < ttl_secs);

	if (cache_hit)
	{
		std::string ct = "image/jpeg";
		{
			std::ifstream f(ct_file);
			if (f) std::getline(f, ct);
		}
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
	if (stat(fail_file.c_str(), &fst) == 0 && (time(nullptr) - fst.st_mtime < kNegativeCacheTtlSecs))
	{
		res.set_header("Cache-Control", "public, max-age=1800");
		res.status = 404;
		return;
	}

	// cdn.anidb.net has its own hotlink/abuse protection, separate from the API rate limit.
	if (is_cdn && effective_base.find("anidb.net") != std::string::npos) scraper_.anidbRateLimitImage();

	std::string token = is_cdn ? "" : conf_.token(sourceId);
	httplib::Result img;
	auto markFailed = [&]()
	{
		try
		{
			std::ofstream f(fail_file);
			f << "1";
		}
		catch (...)
		{
		}
	};
	try
	{
		httplib::Client client(effective_base);
		httplib::Headers headers{
			{"User-Agent", "kairos/1.0 (https://github.com/X64Tyko/Pantheon)"},
			{"Referer", "https://anidb.net/"}
		};
		if (!token.empty())
		{
			headers.emplace("X-Plex-Token", token);
			headers.emplace("Accept", "*/*");
		}
		client.set_default_headers(headers);
		client.set_connection_timeout(5);
		client.set_read_timeout(8);
		img = client.Get(fetch_path);
	}
	catch (const std::exception&)
	{
		markFailed();
		res.status = 502;
		return;
	}
	if (!img || img->status != 200)
	{
		markFailed();
		res.status = 502;
		return;
	}

	// A retry that now succeeds (upstream came back, art got fixed, etc.)
	// clears any stale negative-cache marker from an earlier failure.
	{
		std::error_code ec;
		fs::remove(fail_file, ec);
	}

	auto ct = img->get_header_value("Content-Type");
	if (ct.empty()) ct = "image/jpeg";

	try
	{
		{
			std::ofstream f(cache_file, std::ios::binary);
			f.write(img->body.data(), (std::streamsize)img->body.size());
		}
		{
			std::ofstream f(ct_file);
			f << ct;
		}
	}
	catch (...)
	{
	}

	res.set_header("Cache-Control", "public, max-age=86400");
	res.set_header("ETag", etag);
	res.set_content(img->body, ct);
}

std::optional<WritebackImage> ContentService::fetchImageBytes(const std::string& imgPath,
															  const std::string& sourceId)
{
	if (imgPath.empty()) return std::nullopt;

	if (isLocalSentinel(imgPath))
	{
		fs::path local_path = imgPath.substr(6);
		std::ifstream f(local_path, std::ios::binary);
		if (!f) return std::nullopt;
		std::string body((std::istreambuf_iterator<char>(f)), {});
		return WritebackImage{std::move(body), "image/jpeg"};
	}

	std::string effective_base, fetch_path;
	bool is_cdn = (imgPath.rfind("http", 0) == 0);
	if (is_cdn)
	{
		auto scheme_end = imgPath.find("://");
		auto path_start = (scheme_end != std::string::npos) ? imgPath.find('/', scheme_end + 3) : std::string::npos;
		if (path_start == std::string::npos) return std::nullopt;
		effective_base = imgPath.substr(0, path_start);
		fetch_path     = imgPath.substr(path_start);
	}
	else
	{
		ContentRepository repo(db_);
		effective_base = repo.getSourceBaseUrl(sourceId);
		if (effective_base.empty()) return std::nullopt;
		fetch_path = imgPath;
	}

	std::string token = is_cdn ? "" : conf_.token(sourceId);
	try
	{
		httplib::Client client(effective_base);
		httplib::Headers headers{{"User-Agent", "kairos/1.0 (https://github.com/X64Tyko/Pantheon)"}};
		if (!token.empty())
		{
			headers.emplace("X-Plex-Token", token);
			headers.emplace("Accept", "*/*");
		}
		client.set_default_headers(headers);
		client.set_connection_timeout(5);
		client.set_read_timeout(8);
		auto img = client.Get(fetch_path);
		if (!img || img->status != 200) return std::nullopt;
		std::string ct = img->get_header_value("Content-Type");
		if (ct.empty()) ct = "image/jpeg";
		return WritebackImage{std::move(img->body), std::move(ct)};
	}
	catch (const std::exception&) { return std::nullopt; }
}

namespace
{
	// Applies a target's per-field writeback opt-outs to a copy of `fields` —
	// shared by writebackShow/writebackMovie's target loops. Takes fields by
	// value deliberately: each target in a multi-target loop may have different
	// settings, so the base object built once per item can't be mutated in
	// place.
	WritebackFields shapeForTarget(WritebackFields fields, const SourceRepository::WritebackTarget& t)
	{
		if (!t.writeback_update_art)
		{
			fields.thumb.reset();
			fields.art.reset();
		}
		if (!t.writeback_update_external_ids)
		{
			fields.imdb_id.clear();
			fields.tvdb_id.clear();
			fields.tmdb_id.clear();
			// match_confirmed only round-trips meaningfully alongside the provider
			// ids it's confirming — a target that's opted out of pushing ids has
			// nothing for a future rescan to trust, so there's nothing to mark.
			fields.match_confirmed = false;
		}
		if (!t.writeback_update_collections) fields.collections.clear();
		return fields;
	}
}

json ContentService::writebackShow(const ShowDetail& d, const std::string& source_id_filter, bool auto_only)
{
	WritebackFields fields;
	fields.title           = d.title;
	fields.original_title  = d.original_title;
	fields.overview        = d.overview;
	fields.genres          = d.genres;
	fields.content_rating  = d.content_rating;
	fields.studio          = d.studio;
	fields.network         = d.network;
	fields.actors          = d.actors;
	fields.countries       = d.countries;
	fields.collections     = d.collections;
	fields.labels          = d.labels;
	fields.audience_rating = d.audience_rating;
	fields.imdb_id         = d.imdb_id;
	fields.tvdb_id         = d.tvdb_id;
	fields.tmdb_id         = d.tmdb_id;
	fields.release_date    = d.originally_available_at;
	fields.thumb           = fetchImageBytes(d.thumb, d.source_id);
	fields.art             = fetchImageBytes(d.art, d.source_id);
	fields.match_confirmed = d.match_confirmed;
	fields.locked          = d.locked;

	auto targets = SourceRepository(db_).getWritebackTargets("show", d.show_id);
	json results = json::array();
	for (const auto& t : targets)
	{
		if (!source_id_filter.empty() && t.source_id != source_id_filter) continue;
		if (auto_only && !t.auto_writeback) continue;
		auto* src = sync_.findSource(t.source_id);
		bool ok   = src && src->pushMetadata(t.external_id, t.external_lib_id, "show", shapeForTarget(fields, t));
		results.push_back({{"source_id", t.source_id}, {"source_type", t.source_type}, {"ok", ok}});
	}
	return results;
}

json ContentService::writebackMovie(const MovieDetail& d, const std::string& source_id_filter, bool auto_only)
{
	WritebackFields fields;
	fields.title           = d.title;
	fields.original_title  = d.original_title;
	fields.overview        = d.overview;
	fields.genres          = d.genres;
	fields.content_rating  = d.content_rating;
	fields.studio          = d.studio;
	fields.director        = d.director;
	fields.writer          = d.writer;
	fields.tagline         = d.tagline;
	fields.actors          = d.actors;
	fields.countries       = d.countries;
	fields.collections     = d.collections;
	fields.labels          = d.labels;
	fields.audience_rating = d.audience_rating;
	fields.imdb_id         = d.imdb_id;
	fields.tmdb_id         = d.tmdb_id;
	fields.release_date    = d.release_date;
	fields.thumb           = fetchImageBytes(d.thumb, d.source_id);
	fields.art             = fetchImageBytes(d.art, d.source_id);
	fields.match_confirmed = d.match_confirmed;
	fields.locked          = d.locked;

	auto targets = SourceRepository(db_).getWritebackTargets("movie", d.movie_id);
	json results = json::array();
	for (const auto& t : targets)
	{
		if (!source_id_filter.empty() && t.source_id != source_id_filter) continue;
		if (auto_only && !t.auto_writeback) continue;
		auto* src = sync_.findSource(t.source_id);
		bool ok   = src && src->pushMetadata(t.external_id, t.external_lib_id, "movie", shapeForTarget(fields, t));
		results.push_back({{"source_id", t.source_id}, {"source_type", t.source_type}, {"ok", ok}});
	}
	return results;
}

void ContentService::autoWritebackIfEnabled(const std::string& item_type, const std::string& kairos_id)
{
	ContentRepository repo(db_);
	if (item_type == "show")
	{
		auto d = repo.getShowDetail(kairos_id);
		if (d && d->match_confirmed) writebackShow(*d, "", /*auto_only=*/true);
	}
	else if (item_type == "movie")
	{
		auto d = repo.getMovieDetail(kairos_id);
		if (d && d->match_confirmed) writebackMovie(*d, "", /*auto_only=*/true);
	}
}

bool ContentService::triggerWritebackAll(const std::string& library_id, const std::string& source_id)
{
	bool expected = false;
	if (!writeback_running_.compare_exchange_strong(expected, true)) return false;

	TaskRegistry::global().spawn([this, library_id, source_id]()
	{
		try
		{
			runWritebackAll(library_id, source_id);
		}
		catch (const std::exception& e)
		{
			std::cerr << "[writeback] error: " << e.what() << std::endl;
		}
		writeback_running_.store(false);
	});
	return true;
}

void ContentService::runWritebackAll(const std::string& library_id, const std::string& source_id)
{
	ContentRepository repo(db_);
	const auto show_ids  = repo.getWritebackEligibleShowIds(library_id, source_id);
	const auto movie_ids = repo.getWritebackEligibleMovieIds(library_id, source_id);
	std::cout << "[writeback] starting: " << show_ids.size() << " show(s), "
		<< movie_ids.size() << " movie(s)"
		<< (library_id.empty() ? "" : " (library=" + library_id + ")")
		<< (source_id.empty() ? "" : " (source=" + source_id + ")") << std::endl;

	int ok_count = 0, fail_count = 0;
	for (const auto& id : show_ids)
	{
		auto d = repo.getShowDetail(id);
		if (!d) continue; // deleted mid-run — skip rather than fail the whole pass
		for (const auto& r : writebackShow(*d, source_id)) (r.value("ok", false) ? ok_count : fail_count)++;
	}
	for (const auto& id : movie_ids)
	{
		auto d = repo.getMovieDetail(id);
		if (!d) continue;
		for (const auto& r : writebackMovie(*d, source_id)) (r.value("ok", false) ? ok_count : fail_count)++;
	}
	std::cout << "[writeback] done: " << ok_count << " target(s) ok, "
		<< fail_count << " failed" << std::endl;
}

void ContentService::registerRoutes(httplib::Server& svr)
{
	// Parental controls — called by Hermes (via authedHephaestusProxy, using
	// the caller's own Authorization header) before starting a VOD/preview
	// session, so restriction is enforced at the one already-authenticated
	// playback-start boundary rather than only hiding blocked items from
	// browse/list views. type: "movie" | "episode" | "show".
	svr.Get(R"(/api/content/([^/]+)/(.+)/access-check)", [this](const Req& req, Res& res)
	{
		if (!currentUser())
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		std::string type = req.matches[1];
		std::string id   = req.matches[2];
		auto lookup      = ContentRepository(db_).resolveForRestriction(type, id);
		bool allowed     = RestrictionRepository(db_).isAllowed(
			*currentUser(), lookup.entity_type, lookup.entity_id, lookup.content_rating);
		route::ok(res, json{{"allowed", allowed}}.dump());
	});

	// ── Public image proxy — used by <img> tags that can't send auth headers ──
	// Fetches and caches any external image URL. Exempt from auth in isPublicPath.
	svr.Get("/api/images/proxy", [this](const Req& req, Res& res)
	{
		if (!req.has_param("url"))
		{
			res.status = 400;
			return;
		}
		std::string url = req.get_param_value("url");
		// "local:" is proxyImage()'s signal to read an arbitrary server-side
		// path (see persistAnidbThumbLocally) — safe from the DB-driven
		// /thumb and /art routes below, whose image_path is never user
		// input, but this endpoint takes `url` straight from the querystring
		// with no auth, so allowing it through here would be an arbitrary
		// local file read.
		if (isLocalSentinel(url))
		{
			res.status = 400;
			return;
		}
		if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
		{
			res.status = 400;
			return;
		}
		if (!isSafeExternalHost(extractHost(url)))
		{
			res.status = 400;
			return;
		}
		proxyImage(req, url, "", res);
	});

	// Re-fetch and re-apply this item's full metadata (overview, genres,
	// images, etc.) from whichever scraper it's already matched to, then
	// clear the image cache so a same-URL-but-updated poster/backdrop
	// doesn't keep serving stale cached bytes for the rest of
	// image_cache_ttl_hours. Locked fields are still respected (same as any
	// other match-apply path). 404 if the item has no confirmed match to
	// refresh from — nothing to re-fetch.
	svr.Post(R"(/api/shows/(.+)/refresh-metadata)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.matches[1].str();
		if (!scraper_.refreshMetadata(id, "show"))
		{
			route::err(res, 404, "No confirmed match to refresh from");
			return;
		}
		ContentRepository repo(db_);
		if (auto t = repo.getShowThumb(id)) clearImageCache(t->image_path, t->source_id);
		if (auto a = repo.getShowArt(id)) clearImageCache(a->image_path, a->source_id);
		route::ok(res, json{{"ok", true}}.dump());
	});
	svr.Post(R"(/api/movies/(.+)/refresh-metadata)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.matches[1].str();
		if (!scraper_.refreshMetadata(id, "movie"))
		{
			route::err(res, 404, "No confirmed match to refresh from");
			return;
		}
		ContentRepository repo(db_);
		if (auto t = repo.getMovieThumb(id)) clearImageCache(t->image_path, t->source_id);
		if (auto a = repo.getMovieArt(id)) clearImageCache(a->image_path, a->source_id);
		route::ok(res, json{{"ok", true}}.dump());
	});

	// Per-item scrape exemption — deliberately separate from PATCH /api/shows/:id
	// (which always locks the record as a side effect of a metadata edit; this
	// flag is orthogonal to that). Turning it on also clears any pending
	// uncertain/unmatched state immediately (see ContentRepository::setShowSkipScraping).
	svr.Patch(R"(/api/shows/(.+)/skip-scraping)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b = json::parse(req.body);
			ContentRepository(db_).setShowSkipScraping(id, b.at("skip_scraping").get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});
	svr.Patch(R"(/api/movies/(.+)/skip-scraping)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b = json::parse(req.body);
			ContentRepository(db_).setMovieSkipScraping(id, b.at("skip_scraping").get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	// Per-show opt-in for the automatic specials scan (runs during normal
	// sync — see SyncManager::scanSpecialsForEligibleShows). Separate route
	// for the same reason as skip-scraping above.
	svr.Patch(R"(/api/shows/(.+)/find-specials)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b = json::parse(req.body);
			ContentRepository(db_).setShowFindSpecials(id, b.at("find_specials").get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	// 'season' (default) buckets season-0 episodes into one Specials group;
	// 'aired' interleaves them between numbered seasons by air date — see
	// useMediaDetail.ts's grouping logic on the frontend.
	svr.Patch(R"(/api/shows/(.+)/episode-display-order)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b            = json::parse(req.body);
			std::string order = b.at("episode_display_order").get<std::string>();
			if (order != "season" && order != "aired")
			{
				route::err(res, 400, "episode_display_order must be 'season' or 'aired'");
				return;
			}
			ContentRepository(db_).setShowEpisodeDisplayOrder(id, order);
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	// ── Libraries ────────────────────────────────────────────────────────────

	svr.Get("/api/libraries", [this](const Req&, Res& res)
	{
		ContentRepository repo(db_);
		json result = json::array();
		for (const auto& r : repo.listLibraries())
		{
			result.push_back({
				{"library_id", r.library_id},
				{"source_id", r.source_id},
				{"display_name", r.display_name},
				{"library_type", r.library_type},
				{"source_name", r.source_name},
				{"source_type", r.source_type},
				{"show_on_home", r.show_on_home},
			});
		}
		route::ok(res, result.dump());
	});

	// Focused endpoint for the Home shelf cards' "hide from Home" shortcut —
	// only needs a library_id, unlike SourceService's nested
	// /api/sources/:id/libraries/:lid which also needs the source_id.
	svr.Patch("/api/libraries/:id/home-visibility", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto b = json::parse(req.body);
			if (!b.contains("show_on_home"))
			{
				route::err(res, 400, "show_on_home required");
				return;
			}
			SourceRepository(db_).setLibraryShowOnHome(req.path_params.at("id"), b["show_on_home"].get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const json::exception& e)
		{
			route::err(res, 400, e.what());
		}
		catch (const std::exception& e)
		{
			route::logErr("PATCH /api/libraries/:id/home-visibility", e);
			route::err(res, 500, e.what());
		}
	});

	// ── Metadata values for filter autocomplete ───────────────────────────────

	svr.Get("/api/metadata/values", [this](const Req& req, Res& res)
	{
		std::string field, type, library_id;
		if (req.has_param("field")) field = req.get_param_value("field");
		if (req.has_param("type")) type = req.get_param_value("type");
		if (req.has_param("library_id")) library_id = req.get_param_value("library_id");
		if (field.empty())
		{
			route::err(res, 400, "field required");
			return;
		}
		try
		{
			ContentRepository repo(db_);
			auto vals   = repo.getMetadataValues(field, type, library_id);
			json values = json::array();
			for (const auto& v : vals) values.push_back(v);
			route::ok(res, json{{"values", values}}.dump());
		}
		catch (const std::exception& e) { route::err(res, 500, e.what()); }
	});

	// ── Shows ─────────────────────────────────────────────────────────────────

	svr.Get("/api/shows", [this](const Req& req, Res& res)
	{
		ShowSearchParams p;
		if (req.has_param("limit")) p.limit = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset")) p.offset = std::stoi(req.get_param_value("offset"));
		if (req.has_param("library_ids")) p.library_ids = route::splitComma(req.get_param_value("library_ids"));
		else if (req.has_param("library_id")) p.library_ids = {req.get_param_value("library_id")};
		if (req.has_param("filter")) p.filter = req.get_param_value("filter");
		if (req.has_param("sort")) p.sort = req.get_param_value("sort");
		if (req.has_param("sort_dir")) p.sort_dir = req.get_param_value("sort_dir");
		if (req.has_param("seed")) p.random_seed = std::stoll(req.get_param_value("seed"));
		if (req.has_param("playlist_id")) p.playlist_id = req.get_param_value("playlist_id");
		if (req.has_param("home")) p.home_only = req.get_param_value("home") == "1";
		if (req.has_param("hide_empty")) p.hide_empty = req.get_param_value("hide_empty") == "1";
		p.restriction = restrictionFor("show");
		auto user     = currentUser();
		p.user_id     = user ? user->user_id : "";

		ContentRepository repo(db_);
		auto result = repo.searchShows(p);
		json items  = json::array();
		for (const auto& r : result.items)
		{
			json entry = {
				{"show_id", r.show_id},
				{"title", r.title},
				{"content_rating", r.content_rating},
				{"episode_count", r.episode_count},
				{"watched_episode_count", r.watched_episode_count},
				{"thumb", r.thumb},
				{"art", r.art},
				{"source_base_url", r.source_base_url},
				{"library_id", r.library_id},
				{"match_status", r.match_status.empty() ? "unscraped" : r.match_status}
			};
			if (r.year) entry["year"] = *r.year;
			if (r.audience_rating) entry["audience_rating"] = *r.audience_rating;
			if (r.match_score) entry["match_score"] = *r.match_score;
			// Only for this specific sort — Home's Recently Aired shelf needs
			// to know exactly which episode to jump straight to; every other
			// sort (and Library's own use of this same endpoint) doesn't need
			// per-item episode data, so this stays a small N+1 over an
			// already-paginated (16-24 item) page rather than a batch query
			// that would complicate every other caller of searchShows().
			if (p.sort == "recently_aired")
			{
				if (auto ep = repo.getLatestAiredEpisode(r.show_id))
				{
					entry["latest_episode"] = {
						{"episode_id", ep->episode_id},
						{"season", ep->season},
						{"episode", ep->episode},
						{"air_date", ep->air_date},
					};
				}
			}
			items.push_back(std::move(entry));
		}
		route::ok(res, json{{"items", items}, {"total", result.total}}.dump());
	});

	svr.Get(R"(/api/shows/(.+)/episodes)", [this](const Req& req, Res& res)
	{
		auto id = req.matches[1].str();
		std::string season_filter;
		if (req.has_param("season")) season_filter = req.get_param_value("season");
		auto user = currentUser();

		ContentRepository repo(db_);
		auto rows   = repo.listEpisodesForShow(id, season_filter, user ? user->user_id : "");
		json result = json::array();
		for (const auto& r : rows)
		{
			json entry{
				{"episode_id", r.episode_id},
				{"season", r.season},
				{"episode", r.episode},
				{"title", r.title},
				{"duration_ms", r.duration_ms},
				{"overview", r.overview},
				{"air_date", r.air_date},
				{"thumb", r.thumb},
				{"watched", r.watched},
				{"view_count", r.view_count},
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
	// Next playable episode after :id, honoring the show's episode_display_order
	// (see ContentRepository::getNextEpisode) — used by the player's up-next/
	// auto-advance and by resolvePlayTarget once the last-watched episode is
	// completed. "null" (not 404) when :id is the last playable episode.
	// Deliberately NOT named ".../next" — Router.cpp's isPublicPath exempts
	// any path ending in "/next" from auth (for GET /api/channels/:id/next,
	// a live-channel schedule lookup Hephaestus/DVR clients hit with no
	// session) — this route would otherwise silently leak episode metadata
	// (including of restricted/parental-gated content) unauthenticated.
	svr.Get(R"(/api/episodes/(.+)/next-episode)", [this](const Req& req, Res& res)
	{
		ContentRepository repo(db_);
		auto next = repo.getNextEpisode(req.matches[1].str());
		if (!next)
		{
			route::ok(res, "null");
			return;
		}

		route::ok(res, json{
					  {"episode_id", next->episode_id},
					  {"season", next->season},
					  {"episode", next->episode},
					  {"title", next->title},
					  {"duration_ms", next->duration_ms},
					  {"overview", next->overview},
					  {"air_date", next->air_date},
					  {"thumb", next->thumb},
				  }.dump());
	});

	svr.Get(R"(/api/shows/(.+)/seasons)", [this](const Req& req, Res& res)
	{
		auto id = req.matches[1].str();
		ContentRepository repo(db_);
		json seasons = json::array();
		for (const auto& r : repo.listSeasons(id)) seasons.push_back({{"number", r.number}, {"name", r.name}});
		route::ok(res, json{{"seasons", seasons}}.dump());
	});

	// Audio/subtitle languages — see movieLanguagesFromDb/showLanguagesFromDb.
	svr.Get(R"(/api/shows/(.+)/languages)", [this](const Req& req, Res& res)
	{
		route::ok(res, showLanguagesFromDb(db_, req.matches[1].str()).dump());
	});

	// Codec/resolution/bit-depth, probed from the same representative
	// episode file the languages endpoint uses, same in-memory cache shape.
	svr.Get(R"(/api/shows/(.+)/videoinfo)", [this](const Req& req, Res& res)
	{
		auto id = req.matches[1].str();
		std::string path;
		try
		{
			SQLite::Statement q(db_.get(),
								"SELECT file_path FROM episode WHERE show_id = ? AND file_path != '' "
								"ORDER BY season, episode LIMIT 1");
			q.bind(1, id);
			if (q.executeStep()) path = q.getColumn(0).getString();
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/shows/" + id + "/videoinfo", e);
		}
		route::ok(res, probeVideoInfoCached("show:" + id, path, conf_).dump());
	});

	svr.Get("/api/episodes", [this](const Req& req, Res& res)
	{
		EpisodeSearchParams p;
		if (req.has_param("limit")) p.limit = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset")) p.offset = std::stoi(req.get_param_value("offset"));
		if (req.has_param("show_id")) p.show_id = req.get_param_value("show_id");
		if (req.has_param("q")) p.q = req.get_param_value("q");
		if (req.has_param("season")) p.season = std::stoi(req.get_param_value("season"));
		if (req.has_param("sort")) p.sort = req.get_param_value("sort");
		if (req.has_param("sort_dir")) p.sort_dir = req.get_param_value("sort_dir");
		if (req.has_param("playlist_id")) p.playlist_id = req.get_param_value("playlist_id");

		ContentRepository repo(db_);
		auto result = repo.searchEpisodes(p);
		json items  = json::array();
		for (const auto& r : result.items)
		{
			items.push_back({
				{"episode_id", r.episode_id},
				{"season", r.season},
				{"episode", r.episode},
				{"title", r.title},
				{"duration_ms", r.duration_ms},
				{"show_id", r.show_id},
				{"show_title", r.show_title},
			});
		}
		route::ok(res, json{{"items", items}, {"total", result.total}}.dump());
	});

	// ── Movies ────────────────────────────────────────────────────────────────

	svr.Get("/api/movies", [this](const Req& req, Res& res)
	{
		MovieSearchParams p;
		if (req.has_param("limit")) p.limit = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset")) p.offset = std::stoi(req.get_param_value("offset"));
		if (req.has_param("library_ids")) p.library_ids = route::splitComma(req.get_param_value("library_ids"));
		else if (req.has_param("library_id")) p.library_ids = {req.get_param_value("library_id")};
		if (req.has_param("filter")) p.filter = req.get_param_value("filter");
		if (req.has_param("sort")) p.sort = req.get_param_value("sort");
		if (req.has_param("sort_dir")) p.sort_dir = req.get_param_value("sort_dir");
		if (req.has_param("seed")) p.random_seed = std::stoll(req.get_param_value("seed"));
		if (req.has_param("playlist_id")) p.playlist_id = req.get_param_value("playlist_id");
		if (req.has_param("home")) p.home_only = req.get_param_value("home") == "1";
		if (req.has_param("hide_empty")) p.hide_empty = req.get_param_value("hide_empty") == "1";
		p.restriction = restrictionFor("movie");
		auto user     = currentUser();
		p.user_id     = user ? user->user_id : "";

		ContentRepository repo(db_);
		auto result = repo.searchMovies(p);
		json items  = json::array();
		for (const auto& r : result.items)
		{
			json entry = {
				{"movie_id", r.movie_id},
				{"title", r.title},
				{"content_rating", r.content_rating},
				{"duration_ms", r.duration_ms},
				{"thumb", r.thumb},
				{"art", r.art},
				{"source_base_url", r.source_base_url},
				{"library_id", r.library_id},
				{"watched", r.watched},
				{"view_count", r.view_count},
				{"match_status", r.match_status.empty() ? "unscraped" : r.match_status}
			};
			if (r.year) entry["year"] = *r.year;
			if (!r.release_date.empty()) entry["release_date"] = r.release_date;
			if (r.audience_rating) entry["audience_rating"] = *r.audience_rating;
			if (r.match_score) entry["match_score"] = *r.match_score;
			items.push_back(std::move(entry));
		}
		route::ok(res, json{{"items", items}, {"total", result.total}}.dump());
	});

	// Truly interleaved show+movie browsing (Library's "All" content type, a
	// 'mixed' smart playlist's Home shelf) — index returns every match,
	// already sorted; hydrate fills in tile data for whatever page of that
	// index a caller wants to render. See ContentRepository::searchMixedIndex.
	svr.Get("/api/mixed-media/index", [this](const Req& req, Res& res)
	{
		MixedSearchParams p;
		if (req.has_param("library_ids")) p.library_ids = route::splitComma(req.get_param_value("library_ids"));
		else if (req.has_param("library_id")) p.library_ids = {req.get_param_value("library_id")};
		if (req.has_param("filter")) p.filter = req.get_param_value("filter");
		if (req.has_param("sort")) p.sort = req.get_param_value("sort");
		if (req.has_param("sort_dir")) p.sort_dir = req.get_param_value("sort_dir");
		if (req.has_param("home")) p.home_only = req.get_param_value("home") == "1";
		if (req.has_param("hide_empty")) p.hide_empty = req.get_param_value("hide_empty") == "1";
		if (req.has_param("expand_episodes")) p.expand_episodes = req.get_param_value("expand_episodes") == "1";
		if (req.has_param("include_movies")) p.include_movies = req.get_param_value("include_movies") == "1";
		p.restriction_show  = restrictionFor("show");
		p.restriction_movie = restrictionFor("movie");
		auto user           = currentUser();
		p.user_id           = user ? user->user_id : "";

		ContentRepository repo(db_);
		json items = json::array();
		for (const auto& r : repo.searchMixedIndex(p)) items.push_back({{"content_type", r.content_type}, {"id", r.id}, {"title", r.title}});
		route::ok(res, json{{"items", items}}.dump());
	});

	svr.Get("/api/mixed-media/hydrate", [this](const Req& req, Res& res)
	{
		// "movie:123,show:456,..."
		std::vector<std::pair<std::string, std::string>> ids;
		for (const auto& tok : route::splitComma(req.get_param_value("ids")))
		{
			auto colon = tok.find(':');
			if (colon == std::string::npos) continue;
			ids.push_back({tok.substr(0, colon), tok.substr(colon + 1)});
		}

		ContentRepository repo(db_);
		auto user  = currentUser();
		json items = json::array();
		for (const auto& r : repo.hydrateMixed(ids, restrictionFor("show"), restrictionFor("movie"), user ? user->user_id : ""))
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
			items.push_back(std::move(entry));
		}
		route::ok(res, json{{"items", items}}.dump());
	});

	// ── Show detail ───────────────────────────────────────────────────────────

	svr.Patch(R"(/api/shows/(.+))", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		if (sync_.isMediaLocked())
		{
			route::err(res, 423, "sync in progress");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b       = json::parse(req.body);
			auto jsonStr = [](const json& j) { return j.is_array() ? j.dump() : j.get<std::string>(); };

			std::vector<StrField> sf;
			std::vector<IntField> intf;
			if (b.contains("title")) sf.push_back({"title", b["title"].get<std::string>()});
			if (b.contains("original_title")) sf.push_back({"original_title", b["original_title"].get<std::string>()});
			if (b.contains("overview")) sf.push_back({"overview", b["overview"].get<std::string>()});
			if (b.contains("studio")) sf.push_back({"studio", b["studio"].get<std::string>()});
			if (b.contains("status")) sf.push_back({"status", b["status"].get<std::string>()});
			if (b.contains("content_rating")) sf.push_back({"content_rating", b["content_rating"].get<std::string>()});
			if (b.contains("originally_available_at")) sf.push_back({"originally_available_at", b["originally_available_at"].get<std::string>()});
			if (b.contains("imdb_id")) sf.push_back({"imdb_id", b["imdb_id"].get<std::string>()});
			if (b.contains("tvdb_id")) sf.push_back({"tvdb_id", b["tvdb_id"].get<std::string>()});
			if (b.contains("tmdb_id")) sf.push_back({"tmdb_id", b["tmdb_id"].get<std::string>()});
			if (b.contains("thumb"))
			{
				std::string v = b["thumb"].get<std::string>();
				if (isLocalSentinel(v))
				{
					route::err(res, 400, "invalid thumb value");
					return;
				}
				sf.push_back({"thumb", v});
			}
			if (b.contains("art"))
			{
				std::string v = b["art"].get<std::string>();
				if (isLocalSentinel(v))
				{
					route::err(res, 400, "invalid art value");
					return;
				}
				sf.push_back({"art", v});
			}
			if (b.contains("genres")) sf.push_back({"genres", jsonStr(b["genres"])});
			if (b.contains("tags")) sf.push_back({"tags", jsonStr(b["tags"])});
			if (b.contains("labels")) sf.push_back({"labels", jsonStr(b["labels"])});
			if (b.contains("network")) sf.push_back({"network", b["network"].get<std::string>()});
			if (b.contains("actors")) sf.push_back({"actors", jsonStr(b["actors"])});
			if (b.contains("countries")) sf.push_back({"countries", jsonStr(b["countries"])});
			if (b.contains("collections")) sf.push_back({"collections", jsonStr(b["collections"])});
			if (b.contains("year")) intf.push_back({"year", b["year"].get<int>()});

			ContentRepository(db_).updateShow(id, sf, intf);

			// A manual edit is an explicit, human-confirmed correction — record it as a
			// per-field override so it survives future syncs even if `locked` is later
			// cleared, without freezing the whole row the way `locked` alone would.
			MetadataOverrideRepository ovr(db_);
			for (const auto& f : sf) ovr.set("show", id, f.col, f.val, "user");
			for (const auto& f : intf) ovr.set("show", id, f.col, std::to_string(f.val), "user");

			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PATCH /api/shows/" + id, e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post(R"(/api/shows/(.+)/writeback)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			ContentRepository repo(db_);
			auto d = repo.getShowDetail(id);
			if (!d)
			{
				route::err(res, 404, "show not found");
				return;
			}
			// Writeback is gated on a human-confirmed match, never an auto-accepted
			// one — see acceptCandidate() and the "Push to Sources" plan.
			if (!d->match_confirmed)
			{
				route::err(res, 403, "match not confirmed");
				return;
			}

			route::ok(res, json{{"results", writebackShow(*d, "")}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/shows/:id/writeback", e);
			route::err(res, 500, e.what());
		}
	});

	// `:id` survives; body.duplicate_id is absorbed and gone (see mergeShowInto).
	svr.Post(R"(/api/shows/(.+)/merge)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		if (sync_.isMediaLocked())
		{
			route::err(res, 423, "sync in progress");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b             = json::parse(req.body);
			std::string dup_id = b.value("duplicate_id", "");
			if (dup_id.empty())
			{
				route::err(res, 400, "duplicate_id required");
				return;
			}
			bool confirmed = b.value("confirm", false);

			ContentRepository repo(db_);
			auto d_target = repo.getShowDetail(id);
			auto d_dup    = repo.getShowDetail(dup_id);
			if (!d_target)
			{
				route::err(res, 404, "show not found");
				return;
			}
			if (!d_dup)
			{
				route::err(res, 404, "duplicate show not found");
				return;
			}

			// Compare via the same path-map + cheap-normalize logic
			// SyncManager's own dedup already uses — comparing raw, unmapped
			// paths (as before) could flag two folders as "different" that
			// sync-time dedup already treats as the same file, once
			// different sources' mount points are accounted for.
			if (!confirmed && !d_target->folder_path.empty() && !d_dup->folder_path.empty()
				&& pathutil::compareFolders(conf_.applyPathMap(d_target->folder_path),
											conf_.applyPathMap(d_dup->folder_path)) != pathutil::FolderMatch::kExactCheap)
			{
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
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/shows/:id/merge", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Get(R"(/api/shows/(.+)/thumb)", [this](const Req& req, Res& res)
	{
		auto id   = req.matches[1].str();
		auto item = ContentRepository(db_).getShowThumb(id);
		if (!item)
		{
			res.status = 404;
			return;
		}
		proxyImage(req, item->image_path, item->source_id, res);
	});

	svr.Get(R"(/api/shows/(.+)/art)", [this](const Req& req, Res& res)
	{
		auto id   = req.matches[1].str();
		auto item = ContentRepository(db_).getShowArt(id);
		if (!item)
		{
			res.status = 404;
			return;
		}
		proxyImage(req, item->image_path, item->source_id, res);
	});

	// Registered after /thumb and /art (not where GET /api/shows/:id would
	// normally sit): local kairos_ids contain '/', so this (.+) capture is
	// greedy with no boundary against the routes above — it must come after
	// them or it would swallow ".../thumb" and ".../art" requests too (see
	// ScraperService.cpp's manual-match/metadata routes for the same pattern).
	svr.Get(R"(/api/shows/(.+))", [this](const Req& req, Res& res)
	{
		std::string id = req.matches[1];
		ContentRepository repo(db_);
		auto user = currentUser();
		auto d    = repo.getShowDetail(id, user ? user->user_id : "");
		if (!d)
		{
			route::err(res, 404, "show not found");
			return;
		}
		// Same 404 as a genuinely missing show — don't reveal existence of
		// blocked content via a distinct "forbidden" response.
		if (currentUser() && currentUser()->restricted
			&& !RestrictionRepository(db_).isAllowed(*currentUser(), "show", id, d->content_rating))
		{
			route::err(res, 404, "show not found");
			return;
		}

		auto parseArr = [](const std::string& s) -> json
		{
			try { return json::parse(s); }
			catch (...) { return json::array(); }
		};

		json show;
		show["show_id"]                 = d->show_id;
		show["title"]                   = d->title;
		show["original_title"]          = d->original_title;
		show["content_rating"]          = d->content_rating;
		show["overview"]                = d->overview;
		show["studio"]                  = d->studio;
		show["status"]                  = d->status;
		show["genres"]                  = parseArr(d->genres);
		show["tags"]                    = parseArr(d->tags);
		show["thumb"]                   = d->thumb;
		show["art"]                     = d->art;
		show["imdb_id"]                 = d->imdb_id;
		show["tvdb_id"]                 = d->tvdb_id;
		show["tmdb_id"]                 = d->tmdb_id;
		show["originally_available_at"] = d->originally_available_at;
		if (d->year) show["year"] = *d->year;
		if (d->audience_rating) show["audience_rating"] = *d->audience_rating;
		show["locked"]                = d->locked;
		show["skip_scraping"]         = d->skip_scraping;
		show["find_specials"]         = d->find_specials;
		show["episode_display_order"] = d->episode_display_order;
		show["episode_count"]         = d->episode_count;
		show["watched_episode_count"] = d->watched_episode_count;
		show["labels"]                = parseArr(d->labels);
		show["network"]               = d->network;
		show["actors"]                = parseArr(d->actors);
		show["countries"]             = parseArr(d->countries);
		show["collections"]           = parseArr(d->collections);
		show["external_id"]           = d->external_id;
		show["source_id"]             = d->source_id;
		show["source_base_url"]       = d->source_base_url;
		show["match_status"]          = d->match_status;
		if (d->match_score) show["match_score"] = *d->match_score;
		show["match_confirmed"] = d->match_confirmed;
		if (!d->folder_path.empty()) show["folder_path"] = d->folder_path;

		// Full set of sources this show is mapped to (source_id/source_base_url above are just one of them).
		json sources = json::array();
		for (const auto& t : SourceRepository(db_).getWritebackTargets("show", id)) sources.push_back({{"source_id", t.source_id}, {"source_type", t.source_type}, {"display_name", t.display_name}, {"external_id", t.external_id}});
		show["sources"] = std::move(sources);

		json seasons = json::array();
		for (const auto& s : d->seasons) seasons.push_back({{"number", s.number}, {"name", s.name}});
		show["seasons"] = std::move(seasons);

		route::ok(res, show.dump());
	});

	svr.Get(R"(/api/episodes/(.+)/thumb)", [this](const Req& req, Res& res)
	{
		auto id   = req.matches[1].str();
		auto item = ContentRepository(db_).getEpisodeThumb(id);
		if (!item)
		{
			res.status = 404;
			return;
		}
		proxyImage(req, item->image_path, item->source_id, res);
	});

	// Registered after /next-episode and /thumb for the same reason as the
	// shows detail route above — a (.+) id capture is greedy, so the plain
	// route must be registered last within this resource's GET routes.
	svr.Get(R"(/api/episodes/(.+))", [this](const Req& req, Res& res)
	{
		if (!currentUser())
		{
			route::err(res, 401, "Unauthorized");
			return;
		}
		ContentRepository repo(db_);
		auto e = repo.getEpisode(req.matches[1].str());
		if (!e)
		{
			route::err(res, 404, "episode not found");
			return;
		}
		route::ok(res, json{
					  {"episode_id", e->episode_id},
					  {"season", e->season},
					  {"episode", e->episode},
					  {"title", e->title},
					  {"overview", e->overview},
					  {"show_id", e->show_id},
					  {"show_title", e->show_title},
				  }.dump());
	});

	// ── Movie detail ──────────────────────────────────────────────────────────

	svr.Patch(R"(/api/movies/(.+))", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		if (sync_.isMediaLocked())
		{
			route::err(res, 423, "sync in progress");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b       = json::parse(req.body);
			auto jsonStr = [](const json& j) { return j.is_array() ? j.dump() : j.get<std::string>(); };

			std::vector<StrField> sf;
			std::vector<IntField> intf;
			if (b.contains("title")) sf.push_back({"title", b["title"].get<std::string>()});
			if (b.contains("original_title")) sf.push_back({"original_title", b["original_title"].get<std::string>()});
			if (b.contains("overview")) sf.push_back({"overview", b["overview"].get<std::string>()});
			if (b.contains("tagline")) sf.push_back({"tagline", b["tagline"].get<std::string>()});
			if (b.contains("studio")) sf.push_back({"studio", b["studio"].get<std::string>()});
			if (b.contains("director")) sf.push_back({"director", b["director"].get<std::string>()});
			if (b.contains("writer")) sf.push_back({"writer", b["writer"].get<std::string>()});
			if (b.contains("content_rating")) sf.push_back({"content_rating", b["content_rating"].get<std::string>()});
			if (b.contains("imdb_id")) sf.push_back({"imdb_id", b["imdb_id"].get<std::string>()});
			if (b.contains("tmdb_id")) sf.push_back({"tmdb_id", b["tmdb_id"].get<std::string>()});
			if (b.contains("thumb"))
			{
				std::string v = b["thumb"].get<std::string>();
				if (isLocalSentinel(v))
				{
					route::err(res, 400, "invalid thumb value");
					return;
				}
				sf.push_back({"thumb", v});
			}
			if (b.contains("art"))
			{
				std::string v = b["art"].get<std::string>();
				if (isLocalSentinel(v))
				{
					route::err(res, 400, "invalid art value");
					return;
				}
				sf.push_back({"art", v});
			}
			if (b.contains("genres")) sf.push_back({"genres", jsonStr(b["genres"])});
			if (b.contains("tags")) sf.push_back({"tags", jsonStr(b["tags"])});
			if (b.contains("labels")) sf.push_back({"labels", jsonStr(b["labels"])});
			if (b.contains("actors")) sf.push_back({"actors", jsonStr(b["actors"])});
			if (b.contains("countries")) sf.push_back({"countries", jsonStr(b["countries"])});
			if (b.contains("collections")) sf.push_back({"collections", jsonStr(b["collections"])});
			if (b.contains("year")) intf.push_back({"year", b["year"].get<int>()});

			ContentRepository(db_).updateMovie(id, sf, intf);

			// A manual edit is an explicit, human-confirmed correction — record it as a
			// per-field override so it survives future syncs even if `locked` is later
			// cleared, without freezing the whole row the way `locked` alone would.
			MetadataOverrideRepository ovr(db_);
			for (const auto& f : sf) ovr.set("movie", id, f.col, f.val, "user");
			for (const auto& f : intf) ovr.set("movie", id, f.col, std::to_string(f.val), "user");

			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("PATCH /api/movies/" + id, e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post(R"(/api/movies/(.+)/writeback)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			ContentRepository repo(db_);
			auto d = repo.getMovieDetail(id);
			if (!d)
			{
				route::err(res, 404, "movie not found");
				return;
			}
			// Writeback is gated on a human-confirmed match, never an auto-accepted
			// one — see acceptCandidate() and the "Push to Sources" plan.
			if (!d->match_confirmed)
			{
				route::err(res, 403, "match not confirmed");
				return;
			}

			route::ok(res, json{{"results", writebackMovie(*d, "")}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/movies/:id/writeback", e);
			route::err(res, 500, e.what());
		}
	});

	// Fires "Writeback All" as a detached background pass, same
	// trigger-then-202-immediately shape as POST /api/sync/all — a full
	// library push is exactly the kind of thing that can run long, and
	// there's already a convention (Activity page log stream) for surfacing
	// that kind of progress without the caller blocking on it. Body is
	// optional; library_id/source_id absent or "" = unfiltered (every
	// match-confirmed show and movie, pushed to every one of its targets).
	svr.Post("/api/writeback/all", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		std::string library_id, source_id;
		try
		{
			auto b     = req.body.empty() ? json::object() : json::parse(req.body);
			library_id = b.value("library_id", "");
			source_id  = b.value("source_id", "");
		}
		catch (const json::exception&)
		{
			/* malformed body — fall through unfiltered, same as an empty body */
		}

		if (!triggerWritebackAll(library_id, source_id))
		{
			route::err(res, 409, "writeback already running");
			return;
		}

		res.status = 202;
		route::ok(res, json{{"status", "started"}}.dump());
	});

	// Polled by Hades (same shape/cadence as GET /api/sync/status) so the
	// "Writeback in progress" indicator survives a page navigation/refresh
	// instead of only reflecting the triggering tab's own local state.
	svr.Get("/api/writeback/status", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		route::ok(res, json{{"running", writeback_running_.load()}}.dump());
	});

	// Link a duplicate movie onto this one — see the analogous show route above.
	svr.Post(R"(/api/movies/(.+)/merge)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		if (sync_.isMediaLocked())
		{
			route::err(res, 423, "sync in progress");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b             = json::parse(req.body);
			std::string dup_id = b.value("duplicate_id", "");
			if (dup_id.empty())
			{
				route::err(res, 400, "duplicate_id required");
				return;
			}
			bool confirmed = b.value("confirm", false);

			ContentRepository repo(db_);
			auto d_target = repo.getMovieDetail(id);
			auto d_dup    = repo.getMovieDetail(dup_id);
			if (!d_target)
			{
				route::err(res, 404, "movie not found");
				return;
			}
			if (!d_dup)
			{
				route::err(res, 404, "duplicate movie not found");
				return;
			}

			// See the analogous show route above for why this is path-mapped
			// + cheap-normalized rather than a raw string compare.
			if (!confirmed && !d_target->folder_path.empty() && !d_dup->folder_path.empty()
				&& pathutil::compareFolders(conf_.applyPathMap(d_target->folder_path),
											conf_.applyPathMap(d_dup->folder_path)) != pathutil::FolderMatch::kExactCheap)
			{
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
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/movies/:id/merge", e);
			route::err(res, 400, e.what());
		}
	});

	// Multi-part movies (GitHub #3): auto-detect (at sync time) + manual
	// link/unlink. Registered ahead of the generic GET /api/movies/(.+)
	// detail route below so "grouping-candidates" isn't captured as an :id.
	svr.Get(R"(/api/movies/grouping-candidates)", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			ContentRepository repo(db_);
			json out = json::array();
			for (const auto& cand : repo.getMovieGroupingCandidates())
			{
				json parts = json::array();
				for (const auto& p : cand.parts)
				{
					json pj{{"movie_id", p.movie_id}, {"title", p.title}, {"file_path", p.file_path}, {"part_num", p.part_num}};
					if (p.year.has_value()) pj["year"] = *p.year;
					parts.push_back(std::move(pj));
				}
				out.push_back(json{{"base_title", cand.base_title}, {"confidence", cand.confidence}, {"parts", parts}});
			}
			route::ok(res, json{{"candidates", out}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/movies/grouping-candidates", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post(R"(/api/movies/(.+)/parts/link)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		if (sync_.isMediaLocked())
		{
			route::err(res, 423, "sync in progress");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b = json::parse(req.body);
			std::vector<std::string> other_ids;
			for (const auto& v : b.value("movie_ids", json::array())) other_ids.push_back(v.get<std::string>());
			if (other_ids.empty())
			{
				route::err(res, 400, "movie_ids required");
				return;
			}

			ContentRepository repo(db_);
			repo.linkMovieParts(id, other_ids);
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/movies/:id/parts/link", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post(R"(/api/movies/(.+)/parts/unlink)", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		if (sync_.isMediaLocked())
		{
			route::err(res, 423, "sync in progress");
			return;
		}
		auto id = req.matches[1].str();
		try
		{
			auto b = json::parse(req.body);
			if (!b.contains("part_num"))
			{
				route::err(res, 400, "part_num required");
				return;
			}
			int part_num = b.at("part_num").get<int>();

			ContentRepository repo(db_);
			repo.unlinkMoviePart(id, part_num);
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/movies/:id/parts/unlink", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Get(R"(/api/movies/(.+)/thumb)", [this](const Req& req, Res& res)
	{
		auto id   = req.matches[1].str();
		auto item = ContentRepository(db_).getMovieThumb(id);
		if (!item)
		{
			res.status = 404;
			return;
		}
		proxyImage(req, item->image_path, item->source_id, res);
	});

	svr.Get(R"(/api/movies/(.+)/art)", [this](const Req& req, Res& res)
	{
		auto id   = req.matches[1].str();
		auto item = ContentRepository(db_).getMovieArt(id);
		if (!item)
		{
			res.status = 404;
			return;
		}
		proxyImage(req, item->image_path, item->source_id, res);
	});

	svr.Get(R"(/api/movies/(.+)/languages)", [this](const Req& req, Res& res)
	{
		route::ok(res, movieLanguagesFromDb(db_, req.matches[1].str()).dump());
	});

	svr.Get(R"(/api/movies/(.+)/videoinfo)", [this](const Req& req, Res& res)
	{
		auto id = req.matches[1].str();
		std::string path;
		try
		{
			SQLite::Statement q(db_.get(), "SELECT file_path FROM movie WHERE movie_id = ?");
			q.bind(1, id);
			if (q.executeStep()) path = q.getColumn(0).getString();
		}
		catch (const std::exception& e)
		{
			route::logErr("GET /api/movies/" + id + "/videoinfo", e);
		}
		route::ok(res, probeVideoInfoCached("movie:" + id, path, conf_).dump());
	});

	// Registered after /thumb, /art, /languages and /videoinfo for the same
	// reason as the shows detail route above.
	svr.Get(R"(/api/movies/(.+))", [this](const Req& req, Res& res)
	{
		std::string id = req.matches[1];
		auto user      = currentUser();
		ContentRepository repo(db_);
		auto d = repo.getMovieDetail(id, user ? user->user_id : "");
		if (!d)
		{
			route::err(res, 404, "movie not found");
			return;
		}
		// Same 404 as a genuinely missing movie — don't reveal existence of
		// blocked content via a distinct "forbidden" response.
		if (user && user->restricted
			&& !RestrictionRepository(db_).isAllowed(*user, "movie", id, d->content_rating))
		{
			route::err(res, 404, "movie not found");
			return;
		}

		auto parseArr = [](const std::string& s) -> json
		{
			try { return json::parse(s); }
			catch (...) { return json::array(); }
		};

		json movie;
		movie["movie_id"]       = d->movie_id;
		movie["title"]          = d->title;
		movie["original_title"] = d->original_title;
		movie["content_rating"] = d->content_rating;
		movie["duration_ms"]    = d->duration_ms;
		if (d->year) movie["year"] = *d->year;
		if (!d->release_date.empty()) movie["release_date"] = d->release_date;
		if (d->audience_rating) movie["audience_rating"] = *d->audience_rating;
		movie["overview"]        = d->overview;
		movie["tagline"]         = d->tagline;
		movie["studio"]          = d->studio;
		movie["director"]        = d->director;
		movie["writer"]          = d->writer;
		movie["genres"]          = parseArr(d->genres);
		movie["tags"]            = parseArr(d->tags);
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
		movie["match_status"] = d->match_status;
		if (d->match_score) movie["match_score"] = *d->match_score;
		movie["match_confirmed"] = d->match_confirmed;
		movie["watched"]         = d->watched;
		movie["view_count"]      = d->view_count;
		movie["is_multi_part"]   = d->is_multi_part;
		if (d->is_multi_part)
		{
			json parts = json::array();
			for (const auto& p : d->parts) parts.push_back({{"part_num", p.part_num}, {"file_path", p.file_path}, {"duration_ms", p.duration_ms}});
			movie["parts"] = std::move(parts);
		}

		// Full set of sources this movie is mapped to.
		json sources = json::array();
		for (const auto& t : SourceRepository(db_).getWritebackTargets("movie", id)) sources.push_back({{"source_id", t.source_id}, {"source_type", t.source_type}, {"display_name", t.display_name}, {"external_id", t.external_id}});
		movie["sources"] = std::move(sources);

		route::ok(res, movie.dump());
	});

	// GET /api/duplicates/queue — pending sync-time "possible duplicate" candidates
	// (see SyncManager's tiered dedup + duplicate_candidate table).
	svr.Get("/api/duplicates/queue", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		std::string item_type = req.has_param("item_type") ? req.get_param_value("item_type") : "";
		int limit             = 48, offset = 0;
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

		DuplicateRepository repo(db_);
		auto rows  = repo.listPending(item_type, limit, offset);
		json items = json::array();
		for (const auto& r : rows)
		{
			items.push_back({
				{"candidate_id", r.candidate_id},
				{"item_type", r.item_type},
				{"kairos_id_a", r.kairos_id_a},
				{"kairos_id_b", r.kairos_id_b},
				{"title_a", r.title_a},
				{"title_b", r.title_b},
				{"year_a", r.year_a},
				{"year_b", r.year_b},
				{"thumb_a", r.thumb_a},
				{"thumb_b", r.thumb_b},
				{"trigger", r.trigger},
				{"reason", r.reason},
				{"title_similarity", r.title_similarity},
				{"folder_a", r.folder_a},
				{"folder_b", r.folder_b},
				{"created_at", r.created_at},
			});
		}
		route::ok(res, json{{"items", items}, {"total", repo.countPending(item_type)}}.dump());
	});

	// POST /api/duplicates/:id/dismiss — mark a candidate pair "not a duplicate"
	// (remembered permanently — a later sync re-detecting the same pair is a no-op).
	svr.Post("/api/duplicates/:id/dismiss", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto id = req.path_params.at("id");
		if (DuplicateRepository(db_).dismiss(id)) route::ok(res, json{{"ok", true}}.dump());
		else route::err(res, 404, "candidate not found");
	});
}