#include "ContentService.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../conf/ConfStore.h"
#include "../../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

ContentService::ContentService(const ServiceContext& ctx)
	: db_(ctx.db), conf_(ctx.conf) {}

void ContentService::proxyImage(const std::string& imgPath,
                                 const std::string& sourceId,
                                 httplib::Response& res) {
	if (imgPath.rfind("http", 0) == 0) {
		res.set_redirect(imgPath);
		return;
	}
	std::string base_url;
	SQLite::Statement q(db_.get(),
		"SELECT base_url FROM media_source WHERE source_id = ?");
	q.bind(1, sourceId);
	if (!q.executeStep() || (base_url = q.getColumn(0).getString()).empty()) {
		res.status = 404; return;
	}
	std::string token = conf_.token(sourceId);

	httplib::Client client(base_url);
	if (!token.empty())
		client.set_default_headers({{"X-Plex-Token", token}, {"Accept", "*/*"}});
	client.set_connection_timeout(10);
	client.set_read_timeout(15);

	auto img = client.Get(imgPath);
	if (!img || img->status != 200) { res.status = 502; return; }

	auto ct = img->get_header_value("Content-Type");
	if (ct.empty()) ct = "image/jpeg";
	res.set_content(img->body, ct);
}

void ContentService::registerRoutes(httplib::Server& svr) {

	// ── Libraries ────────────────────────────────────────────────────────────

	svr.Get("/api/libraries", [this](const Req&, Res& res) {
		SQLite::Statement q(db_.get(), R"(
			SELECT ml.library_id, ml.source_id, ml.display_name, ml.library_type,
			       ms.display_name AS source_name, ms.source_type
			FROM media_library ml
			JOIN media_source ms ON ms.source_id = ml.source_id
			ORDER BY ms.display_name, ml.display_name
		)");
		json result = json::array();
		while (q.executeStep()) {
			result.push_back({
				{"library_id",   q.getColumn(0).getString()},
				{"source_id",    q.getColumn(1).getString()},
				{"display_name", q.getColumn(2).getString()},
				{"library_type", q.getColumn(3).getString()},
				{"source_name",  q.getColumn(4).getString()},
				{"source_type",  q.getColumn(5).getString()},
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
			std::set<std::string> seen;
			json values = json::array();
			std::vector<std::string> lib = library_id.empty()
				? std::vector<std::string>{}
				: std::vector<std::string>{library_id};
			std::string show_lib  = library_id.empty() ? "" :
				" AND EXISTS (SELECT 1 FROM source_mapping sm"
				" WHERE sm.kairos_id = s.show_id AND sm.item_type = 'show'"
				" AND sm.library_id = ?)";
			std::string movie_lib = library_id.empty() ? "" :
				" AND EXISTS (SELECT 1 FROM source_mapping sm"
				" WHERE sm.kairos_id = m.movie_id AND sm.item_type = 'movie'"
				" AND sm.library_id = ?)";

			auto collect = [&](const std::string& sql, const std::vector<std::string>& binds) {
				SQLite::Statement q(db_.get(), sql);
				for (int i = 0; i < (int)binds.size(); ++i) q.bind(i + 1, binds[i]);
				while (q.executeStep()) {
					auto v = q.getColumn(0).getString();
					if (!v.empty() && seen.insert(v).second) values.push_back(v);
				}
			};

			if (field == "genre") {
				if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(s.genres) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
				if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(m.genres) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
			} else if (field == "studio") {
				if (type != "movie") collect("SELECT DISTINCT studio FROM show s WHERE studio != ''"         + show_lib  + " ORDER BY studio",         lib);
				if (type != "show")  collect("SELECT DISTINCT studio FROM movie m WHERE studio != ''"        + movie_lib + " ORDER BY studio",         lib);
			} else if (field == "director") {
				if (type != "show")  collect("SELECT DISTINCT director FROM movie m WHERE director != ''"    + movie_lib + " ORDER BY director",        lib);
			} else if (field == "content_rating") {
				if (type != "movie") collect("SELECT DISTINCT content_rating FROM show s WHERE content_rating != ''"  + show_lib  + " ORDER BY content_rating", lib);
				if (type != "show")  collect("SELECT DISTINCT content_rating FROM movie m WHERE content_rating != ''" + movie_lib + " ORDER BY content_rating", lib);
			} else if (field == "label") {
				if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(s.labels) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
				if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(m.labels) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
			} else if (field == "network") {
				if (type != "movie") collect("SELECT DISTINCT network FROM show s WHERE network != ''" + show_lib + " ORDER BY network", lib);
			} else if (field == "actor") {
				if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(s.actors) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
				if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(m.actors) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
			} else if (field == "country") {
				if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(s.countries) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
				if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(m.countries) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
			} else if (field == "collection") {
				if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(s.collections) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
				if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(m.collections) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
			}

			std::sort(values.begin(), values.end(), [](const json& a, const json& b) {
				return a.get<std::string>() < b.get<std::string>();
			});
			route::ok(res, json{{"values", values}}.dump());
		} catch (const std::exception& e) { route::err(res, 500, e.what()); }
	});

	// ── Shows ─────────────────────────────────────────────────────────────────

	svr.Get("/api/shows", [this](const Req& req, Res& res) {
		int         limit      = 50, offset = 0;
		std::string library_id, search_q, genre, year_p, rating;
		if (req.has_param("limit"))          limit      = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset"))         offset     = std::stoi(req.get_param_value("offset"));
		if (req.has_param("library_id"))     library_id = req.get_param_value("library_id");
		if (req.has_param("q"))              search_q   = req.get_param_value("q");
		if (req.has_param("genre"))          genre      = req.get_param_value("genre");
		if (req.has_param("year"))           year_p     = req.get_param_value("year");
		if (req.has_param("content_rating")) rating     = req.get_param_value("content_rating");
		std::string label_p, network_p, actor_p, country_p, collection_p, studio_p;
		if (req.has_param("label"))      label_p      = req.get_param_value("label");
		if (req.has_param("network"))    network_p    = req.get_param_value("network");
		if (req.has_param("actor"))      actor_p      = req.get_param_value("actor");
		if (req.has_param("country"))    country_p    = req.get_param_value("country");
		if (req.has_param("collection")) collection_p = req.get_param_value("collection");
		if (req.has_param("studio"))     studio_p     = req.get_param_value("studio");

		std::string extras;
		std::vector<std::string> extra_vals;
		if (!search_q.empty()) {
			extras += " AND (s.title LIKE '%'||?||'%' OR s.network LIKE '%'||?||'%' OR s.studio LIKE '%'||?||'%'"
			          " OR EXISTS (SELECT 1 FROM json_each(s.labels)  je WHERE je.value LIKE '%'||?||'%')"
			          " OR EXISTS (SELECT 1 FROM json_each(s.genres)  je WHERE je.value LIKE '%'||?||'%')"
			          " OR EXISTS (SELECT 1 FROM json_each(s.actors)  je WHERE je.value LIKE '%'||?||'%'))";
			for (int i = 0; i < 6; ++i) extra_vals.push_back(search_q);
		}
		if (!genre.empty())        route::appendJsonInClause("s", "genres",      genre,       extras, extra_vals);
		if (!year_p.empty())       { extras += " AND s.year = CAST(? AS INTEGER)"; extra_vals.push_back(year_p); }
		if (!rating.empty())       route::appendInClause("s.content_rating",      rating,      extras, extra_vals);
		if (!label_p.empty())      route::appendJsonInClause("s", "labels",      label_p,      extras, extra_vals);
		if (!network_p.empty())    { extras += " AND s.network LIKE '%' || ? || '%'"; extra_vals.push_back(network_p); }
		if (!actor_p.empty())      route::appendJsonInClause("s", "actors",      actor_p,      extras, extra_vals);
		if (!country_p.empty())    route::appendJsonInClause("s", "countries",   country_p,    extras, extra_vals);
		if (!collection_p.empty()) route::appendJsonInClause("s", "collections", collection_p, extras, extra_vals);
		if (!studio_p.empty())     { extras += " AND s.studio LIKE '%' || ? || '%'"; extra_vals.push_back(studio_p); }

		auto bindExtras = [&](SQLite::Statement& q, int& p) {
			for (const auto& v : extra_vals) q.bind(p++, v);
		};

		int  total = 0;
		json items = json::array();
		auto pushShow = [](json& arr, SQLite::Statement& q) {
			json entry = {{"show_id",        q.getColumn(0).getString()},
			              {"title",          q.getColumn(1).getString()},
			              {"content_rating", q.getColumn(2).getString()},
			              {"episode_count",  q.getColumn(3).getInt()}};
			if (!q.getColumn(4).isNull()) entry["year"] = q.getColumn(4).getInt();
			arr.push_back(std::move(entry));
		};

		if (library_id.empty()) {
			SQLite::Statement cnt(db_.get(), "SELECT COUNT(*) FROM show s WHERE 1=1" + extras);
			int p = 1; bindExtras(cnt, p);
			if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

			SQLite::Statement q(db_.get(), R"(
				SELECT s.show_id, s.title, s.content_rating,
				       COUNT(e.episode_id) AS episode_count, s.year
				FROM show s LEFT JOIN episode e ON e.show_id = s.show_id
				WHERE 1=1)" + extras + R"( GROUP BY s.show_id ORDER BY s.title LIMIT ? OFFSET ?)");
			p = 1; bindExtras(q, p);
			q.bind(p++, limit); q.bind(p++, offset);
			while (q.executeStep()) pushShow(items, q);
		} else {
			SQLite::Statement cnt(db_.get(), R"(
				SELECT COUNT(DISTINCT s.show_id) FROM show s
				JOIN source_mapping sm ON sm.kairos_id = s.show_id
				    AND sm.item_type = 'show' AND sm.library_id = ?
				WHERE 1=1)" + extras);
			int p = 1; cnt.bind(p++, library_id); bindExtras(cnt, p);
			if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

			SQLite::Statement q(db_.get(), R"(
				SELECT s.show_id, s.title, s.content_rating,
				       COUNT(e.episode_id) AS episode_count, s.year
				FROM show s
				JOIN source_mapping sm ON sm.kairos_id = s.show_id
				    AND sm.item_type = 'show' AND sm.library_id = ?
				LEFT JOIN episode e ON e.show_id = s.show_id
				WHERE 1=1)" + extras + R"( GROUP BY s.show_id ORDER BY s.title LIMIT ? OFFSET ?)");
			p = 1; q.bind(p++, library_id); bindExtras(q, p);
			q.bind(p++, limit); q.bind(p++, offset);
			while (q.executeStep()) pushShow(items, q);
		}
		route::ok(res, json{{"items", items}, {"total", total}}.dump());
	});

	svr.Get("/api/shows/:id/episodes", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string season_filter;
		if (req.has_param("season")) season_filter = req.get_param_value("season");

		bool has_season = !season_filter.empty();
		SQLite::Statement q(db_.get(),
			std::string("SELECT episode_id, season, episode, title, duration_ms, overview, air_date, thumb "
			            "FROM episode WHERE show_id = ?") +
			(has_season ? " AND season = ?" : "") + " ORDER BY season, episode");
		q.bind(1, id);
		if (has_season) q.bind(2, std::stoi(season_filter));
		json result = json::array();
		while (q.executeStep()) {
			result.push_back({
				{"episode_id",  q.getColumn(0).getString()},
				{"season",      q.getColumn(1).getInt()},
				{"episode",     q.getColumn(2).getInt()},
				{"title",       q.getColumn(3).getString()},
				{"duration_ms", q.getColumn(4).getInt64()},
				{"overview",    q.getColumn(5).getString()},
				{"air_date",    q.getColumn(6).getString()},
				{"thumb",       q.getColumn(7).getString()},
			});
		}
		route::ok(res, result.dump());
	});

	svr.Get("/api/shows/:id/seasons", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		SQLite::Statement q(db_.get(),
			"SELECT DISTINCT e.season, COALESCE(ss.season_name, '') "
			"FROM episode e "
			"LEFT JOIN show_season ss ON ss.show_id = e.show_id AND ss.season = e.season "
			"WHERE e.show_id = ? ORDER BY e.season");
		q.bind(1, id);
		json seasons = json::array();
		while (q.executeStep())
			seasons.push_back({{"number", q.getColumn(0).getInt()},
			                   {"name",   q.getColumn(1).getString()}});
		route::ok(res, json{{"seasons", seasons}}.dump());
	});

	svr.Get("/api/episodes", [this](const Req& req, Res& res) {
		int         limit   = 50, offset = 0, season_v = -1;
		std::string show_id, search_q;
		if (req.has_param("limit"))   limit    = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset"))  offset   = std::stoi(req.get_param_value("offset"));
		if (req.has_param("show_id")) show_id  = req.get_param_value("show_id");
		if (req.has_param("q"))       search_q = req.get_param_value("q");
		if (req.has_param("season"))  season_v = std::stoi(req.get_param_value("season"));

		std::string where = " WHERE 1=1";
		if (!show_id.empty())  where += " AND e.show_id = ?";
		if (season_v >= 0)     where += " AND e.season = ?";
		if (!search_q.empty()) where += " AND (e.title LIKE '%' || ? || '%' OR s.title LIKE '%' || ? || '%')";

		SQLite::Statement q(db_.get(), R"(
			SELECT e.episode_id, e.season, e.episode, e.title, e.duration_ms,
			       s.show_id, s.title AS show_title
			FROM episode e JOIN show s ON s.show_id = e.show_id
		)" + where + " ORDER BY s.title, e.season, e.episode LIMIT ? OFFSET ?");
		int p = 1;
		if (!show_id.empty())  q.bind(p++, show_id);
		if (season_v >= 0)     q.bind(p++, season_v);
		if (!search_q.empty()) { q.bind(p++, search_q); q.bind(p++, search_q); }
		q.bind(p++, limit); q.bind(p++, offset);

		json items = json::array();
		while (q.executeStep()) {
			items.push_back({
				{"episode_id",  q.getColumn(0).getString()},
				{"season",      q.getColumn(1).getInt()},
				{"episode",     q.getColumn(2).getInt()},
				{"title",       q.getColumn(3).getString()},
				{"duration_ms", q.getColumn(4).getInt64()},
				{"show_id",     q.getColumn(5).getString()},
				{"show_title",  q.getColumn(6).getString()},
			});
		}
		route::ok(res, json{{"items", items}}.dump());
	});

	// ── Movies ────────────────────────────────────────────────────────────────

	svr.Get("/api/movies", [this](const Req& req, Res& res) {
		int         limit      = 50, offset = 0;
		std::string library_id, search_q, genre, year_p, rating;
		if (req.has_param("limit"))          limit      = std::stoi(req.get_param_value("limit"));
		if (req.has_param("offset"))         offset     = std::stoi(req.get_param_value("offset"));
		if (req.has_param("library_id"))     library_id = req.get_param_value("library_id");
		if (req.has_param("q"))              search_q   = req.get_param_value("q");
		if (req.has_param("genre"))          genre      = req.get_param_value("genre");
		if (req.has_param("year"))           year_p     = req.get_param_value("year");
		if (req.has_param("content_rating")) rating     = req.get_param_value("content_rating");
		std::string label_p, actor_p, country_p, collection_p, studio_p;
		if (req.has_param("label"))      label_p      = req.get_param_value("label");
		if (req.has_param("actor"))      actor_p      = req.get_param_value("actor");
		if (req.has_param("country"))    country_p    = req.get_param_value("country");
		if (req.has_param("collection")) collection_p = req.get_param_value("collection");
		if (req.has_param("studio"))     studio_p     = req.get_param_value("studio");

		std::string extras;
		std::vector<std::string> extra_vals;
		if (!search_q.empty()) {
			extras += " AND (m.title LIKE '%'||?||'%' OR m.studio LIKE '%'||?||'%'"
			          " OR EXISTS (SELECT 1 FROM json_each(m.labels)  je WHERE je.value LIKE '%'||?||'%')"
			          " OR EXISTS (SELECT 1 FROM json_each(m.genres)  je WHERE je.value LIKE '%'||?||'%')"
			          " OR EXISTS (SELECT 1 FROM json_each(m.actors)  je WHERE je.value LIKE '%'||?||'%'))";
			for (int i = 0; i < 5; ++i) extra_vals.push_back(search_q);
		}
		if (!genre.empty())        route::appendJsonInClause("m", "genres",      genre,       extras, extra_vals);
		if (!year_p.empty())       { extras += " AND m.year = CAST(? AS INTEGER)"; extra_vals.push_back(year_p); }
		if (!rating.empty())       route::appendInClause("m.content_rating",      rating,      extras, extra_vals);
		if (!label_p.empty())      route::appendJsonInClause("m", "labels",      label_p,      extras, extra_vals);
		if (!actor_p.empty())      route::appendJsonInClause("m", "actors",      actor_p,      extras, extra_vals);
		if (!country_p.empty())    route::appendJsonInClause("m", "countries",   country_p,    extras, extra_vals);
		if (!collection_p.empty()) route::appendJsonInClause("m", "collections", collection_p, extras, extra_vals);
		if (!studio_p.empty())     { extras += " AND m.studio LIKE '%' || ? || '%'"; extra_vals.push_back(studio_p); }

		auto bindExtras = [&](SQLite::Statement& q, int& p) {
			for (const auto& v : extra_vals) q.bind(p++, v);
		};

		int  total = 0;
		json items = json::array();
		auto pushMovie = [](json& arr, SQLite::Statement& q) {
			auto entry = json{{"movie_id",       q.getColumn(0).getString()},
			                  {"title",          q.getColumn(1).getString()},
			                  {"content_rating", q.getColumn(2).getString()},
			                  {"duration_ms",    q.getColumn(3).getInt64()}};
			if (!q.getColumn(4).isNull()) entry["year"] = q.getColumn(4).getInt();
			arr.push_back(std::move(entry));
		};

		if (library_id.empty()) {
			SQLite::Statement cnt(db_.get(), "SELECT COUNT(*) FROM movie m WHERE 1=1" + extras);
			int p = 1; bindExtras(cnt, p);
			if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

			SQLite::Statement q(db_.get(),
				"SELECT m.movie_id, m.title, m.content_rating, m.duration_ms, m.year "
				"FROM movie m WHERE 1=1" + extras + " ORDER BY m.title LIMIT ? OFFSET ?");
			p = 1; bindExtras(q, p);
			q.bind(p++, limit); q.bind(p++, offset);
			while (q.executeStep()) pushMovie(items, q);
		} else {
			SQLite::Statement cnt(db_.get(), R"(
				SELECT COUNT(DISTINCT m.movie_id) FROM movie m
				JOIN source_mapping sm ON sm.kairos_id = m.movie_id
				    AND sm.item_type = 'movie' AND sm.library_id = ?
				WHERE 1=1)" + extras);
			int p = 1; cnt.bind(p++, library_id); bindExtras(cnt, p);
			if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

			SQLite::Statement q(db_.get(), R"(
				SELECT m.movie_id, m.title, m.content_rating, m.duration_ms, m.year
				FROM movie m
				JOIN source_mapping sm ON sm.kairos_id = m.movie_id
				    AND sm.item_type = 'movie' AND sm.library_id = ?
				WHERE 1=1)" + extras + " ORDER BY m.title LIMIT ? OFFSET ?");
			p = 1; q.bind(p++, library_id); bindExtras(q, p);
			q.bind(p++, limit); q.bind(p++, offset);
			while (q.executeStep()) pushMovie(items, q);
		}
		route::ok(res, json{{"items", items}, {"total", total}}.dump());
	});

	// ── Show detail ───────────────────────────────────────────────────────────

	svr.Get("/api/shows/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		SQLite::Statement q(db_.get(), R"(
			SELECT s.show_id, s.title, s.content_rating, s.overview, s.studio, s.status,
			       s.genres, s.thumb, s.art, s.imdb_id, s.tvdb_id, s.tmdb_id,
			       s.originally_available_at, s.year, s.audience_rating, s.locked,
			       COUNT(e.episode_id) AS episode_count,
			       s.labels, s.network, s.actors, s.countries, s.collections
			FROM show s
			LEFT JOIN episode e ON e.show_id = s.show_id
			WHERE s.show_id = ?
			GROUP BY s.show_id
		)");
		q.bind(1, id);
		if (!q.executeStep()) { route::err(res, 404, "show not found"); return; }

		std::string external_id, source_id, source_base_url;
		SQLite::Statement sm(db_.get(), R"(
			SELECT sm.external_id, sm.source_id, ms.base_url
			FROM source_mapping sm
			JOIN media_source ms ON ms.source_id = sm.source_id
			WHERE sm.item_type = 'show' AND sm.kairos_id = ?
			LIMIT 1
		)");
		sm.bind(1, id);
		if (sm.executeStep()) {
			external_id     = sm.getColumn(0).getString();
			source_id       = sm.getColumn(1).getString();
			source_base_url = sm.getColumn(2).getString();
		}

		json show;
		show["show_id"]                 = q.getColumn(0).getString();
		show["title"]                   = q.getColumn(1).getString();
		show["content_rating"]          = q.getColumn(2).getString();
		show["overview"]                = q.getColumn(3).getString();
		show["studio"]                  = q.getColumn(4).getString();
		show["status"]                  = q.getColumn(5).getString();
		try { show["genres"] = json::parse(q.getColumn(6).getString()); }
		catch (...) { show["genres"] = json::array(); }
		show["thumb"]                   = q.getColumn(7).getString();
		show["art"]                     = q.getColumn(8).getString();
		show["imdb_id"]                 = q.getColumn(9).getString();
		show["tvdb_id"]                 = q.getColumn(10).getString();
		show["tmdb_id"]                 = q.getColumn(11).getString();
		show["originally_available_at"] = q.getColumn(12).getString();
		if (!q.getColumn(13).isNull()) show["year"]            = q.getColumn(13).getInt();
		if (!q.getColumn(14).isNull()) show["audience_rating"] = q.getColumn(14).getDouble();
		show["locked"]        = q.getColumn(15).getInt() != 0;
		show["episode_count"] = q.getColumn(16).getInt();
		try { show["labels"]      = json::parse(q.getColumn(17).getString()); } catch (...) { show["labels"] = json::array(); }
		show["network"]       = q.getColumn(18).getString();
		try { show["actors"]      = json::parse(q.getColumn(19).getString()); } catch (...) { show["actors"] = json::array(); }
		try { show["countries"]   = json::parse(q.getColumn(20).getString()); } catch (...) { show["countries"] = json::array(); }
		try { show["collections"] = json::parse(q.getColumn(21).getString()); } catch (...) { show["collections"] = json::array(); }
		show["external_id"]       = external_id;
		show["source_id"]         = source_id;
		show["source_base_url"]   = source_base_url;

		{
			SQLite::Statement sq(db_.get(),
				"SELECT DISTINCT e.season, COALESCE(ss.season_name, '') "
				"FROM episode e "
				"LEFT JOIN show_season ss ON ss.show_id = e.show_id AND ss.season = e.season "
				"WHERE e.show_id = ? ORDER BY e.season");
			sq.bind(1, id);
			json seasons = json::array();
			while (sq.executeStep())
				seasons.push_back({{"number", sq.getColumn(0).getInt()},
				                   {"name",   sq.getColumn(1).getString()}});
			show["seasons"] = std::move(seasons);
		}

		route::ok(res, show.dump());
	});

	svr.Patch("/api/shows/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);

			std::vector<std::string> cols;
			std::vector<std::function<void(SQLite::Statement&, int)>> binders;
			auto addS = [&](const char* col, std::string val) {
				cols.push_back(col);
				binders.push_back([v = std::move(val)](SQLite::Statement& s, int p) { s.bind(p, v); });
			};
			auto addI = [&](const char* col, int val) {
				cols.push_back(col);
				binders.push_back([val](SQLite::Statement& s, int p) { s.bind(p, val); });
			};
			auto jsonStr = [](const json& j) { return j.is_array() ? j.dump() : j.get<std::string>(); };

			if (b.contains("title"))                   addS("title",                   b["title"].get<std::string>());
			if (b.contains("overview"))                addS("overview",                b["overview"].get<std::string>());
			if (b.contains("studio"))                  addS("studio",                  b["studio"].get<std::string>());
			if (b.contains("status"))                  addS("status",                  b["status"].get<std::string>());
			if (b.contains("content_rating"))          addS("content_rating",          b["content_rating"].get<std::string>());
			if (b.contains("originally_available_at")) addS("originally_available_at", b["originally_available_at"].get<std::string>());
			if (b.contains("imdb_id"))                 addS("imdb_id",                 b["imdb_id"].get<std::string>());
			if (b.contains("tvdb_id"))                 addS("tvdb_id",                 b["tvdb_id"].get<std::string>());
			if (b.contains("tmdb_id"))                 addS("tmdb_id",                 b["tmdb_id"].get<std::string>());
			if (b.contains("thumb"))                   addS("thumb",                   b["thumb"].get<std::string>());
			if (b.contains("art"))                     addS("art",                     b["art"].get<std::string>());
			if (b.contains("year"))                    addI("year",                    b["year"].get<int>());
			if (b.contains("genres"))                  addS("genres",                  jsonStr(b["genres"]));
			if (b.contains("labels"))                  addS("labels",                  jsonStr(b["labels"]));
			if (b.contains("network"))                 addS("network",                 b["network"].get<std::string>());
			if (b.contains("actors"))                  addS("actors",                  jsonStr(b["actors"]));
			if (b.contains("countries"))               addS("countries",               jsonStr(b["countries"]));
			if (b.contains("collections"))             addS("collections",             jsonStr(b["collections"]));

			std::string sql = "UPDATE show SET locked = 1";
			for (const auto& col : cols) sql += ", " + col + " = ?";
			sql += " WHERE show_id = ?";
			SQLite::Statement s(db_.get(), sql);
			for (int p = 0; p < (int)binders.size(); ++p) binders[p](s, p + 1);
			s.bind((int)binders.size() + 1, id);
			s.exec();

			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/shows/" + id, e); route::err(res, 400, e.what()); }
	});

	svr.Get("/api/shows/:id/thumb", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string thumb, source_id;
		SQLite::Statement q(db_.get(), R"(
			SELECT s.thumb, sm.source_id FROM show s
			LEFT JOIN source_mapping sm ON sm.item_type = 'show' AND sm.kairos_id = s.show_id
			WHERE s.show_id = ? LIMIT 1
		)");
		q.bind(1, id);
		if (!q.executeStep() || (thumb = q.getColumn(0).getString()).empty()) {
			res.status = 404; return;
		}
		source_id = q.getColumn(1).getString();
		proxyImage(thumb, source_id, res);
	});

	svr.Get("/api/shows/:id/art", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string art, source_id;
		SQLite::Statement q(db_.get(), R"(
			SELECT s.art, sm.source_id FROM show s
			LEFT JOIN source_mapping sm ON sm.item_type = 'show' AND sm.kairos_id = s.show_id
			WHERE s.show_id = ? LIMIT 1
		)");
		q.bind(1, id);
		if (!q.executeStep() || (art = q.getColumn(0).getString()).empty()) {
			res.status = 404; return;
		}
		source_id = q.getColumn(1).getString();
		proxyImage(art, source_id, res);
	});

	svr.Get("/api/episodes/:id/thumb", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string thumb, source_id;
		SQLite::Statement q(db_.get(), R"(
			SELECT e.thumb, sm.source_id FROM episode e
			LEFT JOIN source_mapping sm ON sm.item_type = 'episode' AND sm.kairos_id = e.episode_id
			WHERE e.episode_id = ? LIMIT 1
		)");
		q.bind(1, id);
		if (!q.executeStep() || (thumb = q.getColumn(0).getString()).empty()) {
			res.status = 404; return;
		}
		source_id = q.getColumn(1).getString();
		proxyImage(thumb, source_id, res);
	});

	// ── Movie detail ──────────────────────────────────────────────────────────

	svr.Get("/api/movies/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		SQLite::Statement q(db_.get(), R"(
			SELECT movie_id, title, content_rating, duration_ms, year,
			       overview, tagline, studio, director, genres, thumb, art,
			       imdb_id, tmdb_id, audience_rating, locked,
			       labels, actors, countries, collections
			FROM movie WHERE movie_id = ?
		)");
		q.bind(1, id);
		if (!q.executeStep()) { route::err(res, 404, "movie not found"); return; }

		std::string external_id, source_id, source_base_url;
		SQLite::Statement sm(db_.get(), R"(
			SELECT sm.external_id, sm.source_id, ms.base_url
			FROM source_mapping sm
			JOIN media_source ms ON ms.source_id = sm.source_id
			WHERE sm.item_type = 'movie' AND sm.kairos_id = ?
			LIMIT 1
		)");
		sm.bind(1, id);
		if (sm.executeStep()) {
			external_id     = sm.getColumn(0).getString();
			source_id       = sm.getColumn(1).getString();
			source_base_url = sm.getColumn(2).getString();
		}

		json movie;
		movie["movie_id"]        = q.getColumn(0).getString();
		movie["title"]           = q.getColumn(1).getString();
		movie["content_rating"]  = q.getColumn(2).getString();
		movie["duration_ms"]     = q.getColumn(3).getInt64();
		if (!q.getColumn(4).isNull()) movie["year"] = q.getColumn(4).getInt();
		movie["overview"]        = q.getColumn(5).getString();
		movie["tagline"]         = q.getColumn(6).getString();
		movie["studio"]          = q.getColumn(7).getString();
		movie["director"]        = q.getColumn(8).getString();
		try { movie["genres"] = json::parse(q.getColumn(9).getString()); }
		catch (...) { movie["genres"] = json::array(); }
		movie["thumb"]           = q.getColumn(10).getString();
		movie["art"]             = q.getColumn(11).getString();
		movie["imdb_id"]         = q.getColumn(12).getString();
		movie["tmdb_id"]         = q.getColumn(13).getString();
		if (!q.getColumn(14).isNull()) movie["audience_rating"] = q.getColumn(14).getDouble();
		movie["locked"]          = q.getColumn(15).getInt() != 0;
		try { movie["labels"]      = json::parse(q.getColumn(16).getString()); } catch (...) { movie["labels"] = json::array(); }
		try { movie["actors"]      = json::parse(q.getColumn(17).getString()); } catch (...) { movie["actors"] = json::array(); }
		try { movie["countries"]   = json::parse(q.getColumn(18).getString()); } catch (...) { movie["countries"] = json::array(); }
		try { movie["collections"] = json::parse(q.getColumn(19).getString()); } catch (...) { movie["collections"] = json::array(); }
		movie["external_id"]      = external_id;
		movie["source_id"]        = source_id;
		movie["source_base_url"]  = source_base_url;
		route::ok(res, movie.dump());
	});

	svr.Patch("/api/movies/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);

			std::vector<std::string> cols;
			std::vector<std::function<void(SQLite::Statement&, int)>> binders;
			auto addS = [&](const char* col, std::string val) {
				cols.push_back(col);
				binders.push_back([v = std::move(val)](SQLite::Statement& s, int p) { s.bind(p, v); });
			};
			auto addI = [&](const char* col, int val) {
				cols.push_back(col);
				binders.push_back([val](SQLite::Statement& s, int p) { s.bind(p, val); });
			};
			auto jsonStr = [](const json& j) { return j.is_array() ? j.dump() : j.get<std::string>(); };

			if (b.contains("title"))          addS("title",          b["title"].get<std::string>());
			if (b.contains("overview"))       addS("overview",       b["overview"].get<std::string>());
			if (b.contains("tagline"))        addS("tagline",        b["tagline"].get<std::string>());
			if (b.contains("studio"))         addS("studio",         b["studio"].get<std::string>());
			if (b.contains("director"))       addS("director",       b["director"].get<std::string>());
			if (b.contains("content_rating")) addS("content_rating", b["content_rating"].get<std::string>());
			if (b.contains("imdb_id"))        addS("imdb_id",        b["imdb_id"].get<std::string>());
			if (b.contains("tmdb_id"))        addS("tmdb_id",        b["tmdb_id"].get<std::string>());
			if (b.contains("thumb"))          addS("thumb",          b["thumb"].get<std::string>());
			if (b.contains("art"))            addS("art",            b["art"].get<std::string>());
			if (b.contains("year"))           addI("year",           b["year"].get<int>());
			if (b.contains("genres"))         addS("genres",         jsonStr(b["genres"]));
			if (b.contains("labels"))         addS("labels",         jsonStr(b["labels"]));
			if (b.contains("actors"))         addS("actors",         jsonStr(b["actors"]));
			if (b.contains("countries"))      addS("countries",      jsonStr(b["countries"]));
			if (b.contains("collections"))    addS("collections",    jsonStr(b["collections"]));

			std::string sql = "UPDATE movie SET locked = 1";
			for (const auto& col : cols) sql += ", " + col + " = ?";
			sql += " WHERE movie_id = ?";
			SQLite::Statement s(db_.get(), sql);
			for (int p = 0; p < (int)binders.size(); ++p) binders[p](s, p + 1);
			s.bind((int)binders.size() + 1, id);
			s.exec();

			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/movies/" + id, e); route::err(res, 400, e.what()); }
	});

	svr.Get("/api/movies/:id/thumb", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string thumb, source_id;
		SQLite::Statement q(db_.get(), R"(
			SELECT m.thumb, sm.source_id FROM movie m
			LEFT JOIN source_mapping sm ON sm.item_type = 'movie' AND sm.kairos_id = m.movie_id
			WHERE m.movie_id = ? LIMIT 1
		)");
		q.bind(1, id);
		if (!q.executeStep() || (thumb = q.getColumn(0).getString()).empty()) {
			res.status = 404; return;
		}
		source_id = q.getColumn(1).getString();
		proxyImage(thumb, source_id, res);
	});

	svr.Get("/api/movies/:id/art", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		std::string art, source_id;
		SQLite::Statement q(db_.get(), R"(
			SELECT m.art, sm.source_id FROM movie m
			LEFT JOIN source_mapping sm ON sm.item_type = 'movie' AND sm.kairos_id = m.movie_id
			WHERE m.movie_id = ? LIMIT 1
		)");
		q.bind(1, id);
		if (!q.executeStep() || (art = q.getColumn(0).getString()).empty()) {
			res.status = 404; return;
		}
		source_id = q.getColumn(1).getString();
		proxyImage(art, source_id, res);
	});
}
