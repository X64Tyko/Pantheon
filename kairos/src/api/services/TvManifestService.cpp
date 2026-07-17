#include "TvManifestService.h"
#include "../RouteHelpers.h"
#include "../../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

TvManifestService::TvManifestService(const ServiceContext& ctx) : db_(ctx.db) {}

namespace {

// hero's data sources are two merged endpoints (recently-added shows +
// movies, filtered to items with backdrop art) — not reproducible as a
// single {endpoint, params} pair the way every other shelf is, so this shape
// is fixed here rather than read from tv_shelf.params_json (which is left
// '{}' for the hero row in the v81 seed data).
json heroRowJson(int order) {
	json params = {{"limit", 16}, {"sort", "recently_added"}, {"home", true}, {"hide_empty", true}};
	return {
		{"id", "hero"}, {"order", order}, {"type", "hero"},
		{"dataSources", {
			{"shows",  {{"endpoint", "/api/shows"},  {"params", params}}},
			{"movies", {{"endpoint", "/api/movies"}, {"params", params}}},
		}},
		{"requiresArt", true},
		{"actions", json::array({"play-resolved", "open-detail"})},
	};
}

} // namespace

void TvManifestService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/tv/manifest", [this](const Req&, Res& res) {
		try {
			json home_rows = json::array();
			{
				SQLite::Statement q(db_.get(),
					"SELECT id, row_order, row_type, title, endpoint, params_json, item_action, end_tile, empty_behavior "
					"FROM tv_shelf WHERE enabled = 1 ORDER BY row_order");
				while (q.executeStep()) {
					auto id        = q.getColumn(0).getString();
					int  order     = q.getColumn(1).getInt();
					auto row_type  = q.getColumn(2).getString();

					if (row_type == "hero")  { home_rows.push_back(heroRowJson(order)); continue; }
					if (row_type == "guide") { home_rows.push_back({{"id", id}, {"order", order}, {"type", "guide"}}); continue; }

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
					if (!end_tile.empty())    row["endTile"]    = end_tile;
					home_rows.push_back(row);
				}
			}

			auto zonesForScreen = [this](const std::string& screen) {
				json zones = json::array();
				SQLite::Statement q(db_.get(),
					"SELECT zone_id, zone_order, config_json FROM tv_zone "
					"WHERE screen = ? AND enabled = 1 ORDER BY zone_order");
				q.bind(1, screen);
				while (q.executeStep()) {
					json zone = {{"id", q.getColumn(0).getString()}, {"order", q.getColumn(1).getInt()}};
					auto config = json::parse(q.getColumn(2).getString());
					zone.update(config); // merges dataSource/filterFields/itemAction/showOnly — whatever this zone declares
					zones.push_back(zone);
				}
				return zones;
			};

			route::ok(res, json{
				{"version", 1},
				{"home",    {{"rows", home_rows}}},
				{"library", {{"zones", zonesForScreen("library")}}},
				{"detail",  {{"zones", zonesForScreen("detail")}}},
				{"guide",   {{"zones", zonesForScreen("guide")}}},
			}.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/tv/manifest", e); route::err(res, 500, e.what());
		}
	});
}
