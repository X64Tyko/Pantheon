#include "KairosService.h"
#include "../RouteHelpers.h"
#include "../../db/BlockRepository.h"
#include "../../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

KairosService::KairosService(const ServiceContext& ctx)
	: db_(ctx.db)
{}

namespace {

std::optional<std::pair<std::string, int>> tryMatchTitle(const std::string& title) {
	if (title.size() >= 4 && title.back() == ')') {
		auto op = title.rfind('(');
		if (op != std::string::npos && op > 0 && title[op-1] == ' ') {
			std::string num = title.substr(op+1, title.size()-op-2);
			bool all_dig = !num.empty() && std::all_of(num.begin(), num.end(), ::isdigit);
			if (all_dig) {
				int n = std::stoi(num);
				if (n >= 1 && n <= 20)
					return std::make_pair(title.substr(0, op-1), n);
			}
		}
	}
	static const std::vector<std::pair<std::string,int>> word_nums = {
		{"one",1},{"two",2},{"three",3},{"four",4},{"five",5},
		{"six",6},{"seven",7},{"eight",8},{"nine",9},{"ten",10}
	};
	static const std::vector<std::string> seps = {", part ", ": part ", " part "};
	std::string low = title;
	std::transform(low.begin(), low.end(), low.begin(), ::tolower);
	for (const auto& sep : seps) {
		auto pos = low.rfind(sep);
		if (pos == std::string::npos) continue;
		std::string after = low.substr(pos + sep.size());
		if (!after.empty() && std::all_of(after.begin(), after.end(), ::isdigit)) {
			int n = std::stoi(after);
			if (n >= 1 && n <= 20) return std::make_pair(title.substr(0, pos), n);
		}
		for (const auto& [word, num] : word_nums) {
			if (after == word) return std::make_pair(title.substr(0, pos), num);
		}
	}
	return std::nullopt;
}

struct EpRow { std::string ep_id; int season; int ep_num; std::string title; };
struct Part   { std::string ep_id; int part_num; int ep_idx; };

json buildGroupingCandidates(const std::vector<EpRow>& eps,
                             const std::unordered_set<std::string>& confirmed_ids) {
	std::map<std::string, std::vector<Part>> by_base;
	for (int i = 0; i < (int)eps.size(); ++i) {
		auto m = tryMatchTitle(eps[i].title);
		if (m) by_base[m->first].push_back({eps[i].ep_id, m->second, i});
	}

	json candidates = json::array();
	for (auto& [base, parts] : by_base) {
		if (parts.size() < 2) continue;
		std::sort(parts.begin(), parts.end(),
			[](const Part& a, const Part& b){ return a.part_num < b.part_num; });

		int  score = 0;
		bool starts_at_one     = (parts.front().part_num == 1);
		bool consecutive_parts = true;
		for (int i = 1; i < (int)parts.size(); ++i)
			if (parts[i].part_num != parts[i-1].part_num + 1) { consecutive_parts = false; break; }
		if (starts_at_one)     score += 25;
		if (consecutive_parts) score += 25;
		bool adjacent = true;
		for (int i = 1; i < (int)parts.size(); ++i)
			if (parts[i].ep_idx != parts[i-1].ep_idx + 1) { adjacent = false; break; }
		if (adjacent) score += 30;
		if ((int)parts.size() >= 3) score += 10;
		bool any_confirmed = false;
		for (auto& p : parts) if (confirmed_ids.count(p.ep_id)) { any_confirmed = true; break; }
		if (any_confirmed) score += 10;

		json group = {
			{"base_title",      base},
			{"confidence",      std::min(score, 100)},
			{"adjacent",        adjacent},
			{"already_grouped", any_confirmed},
			{"parts",           json::array()},
		};
		for (auto& p : parts)
			group["parts"].push_back({
				{"episode_id", p.ep_id},
				{"title",      eps[p.ep_idx].title},
				{"season",     eps[p.ep_idx].season},
				{"episode",    eps[p.ep_idx].ep_num},
				{"part_num",   p.part_num},
				{"confirmed",  confirmed_ids.count(p.ep_id) > 0},
			});
		candidates.push_back(std::move(group));
	}
	return candidates;
}

} // namespace

void KairosService::registerRoutes(httplib::Server& svr) {

	// ── Episode groups ────────────────────────────────────────────────────────

	svr.Get("/api/shows/:id/groups", [this](const Req& req, Res& res) {
		auto show_id = req.path_params.at("id");
		SQLite::Statement q(db_.get(),
			"SELECT group_id, name, group_type FROM episode_group WHERE show_id=? ORDER BY name");
		q.bind(1, show_id);
		json arr = json::array();
		while (q.executeStep()) {
			std::string gid = q.getColumn(0).getString();
			json g = {{"group_id", gid}, {"name", q.getColumn(1).getString()},
			          {"group_type", q.getColumn(2).getString()}, {"members", json::array()}};
			SQLite::Statement mq(db_.get(),
				"SELECT egm.id, egm.episode_id, egm.part_num, e.season, e.episode, e.title "
				"FROM episode_group_member egm JOIN episode e ON e.episode_id=egm.episode_id "
				"WHERE egm.group_id=? ORDER BY egm.part_num");
			mq.bind(1, gid);
			while (mq.executeStep())
				g["members"].push_back({
					{"id",         mq.getColumn(0).getInt()},
					{"episode_id", mq.getColumn(1).getString()},
					{"part_num",   mq.getColumn(2).getInt()},
					{"season",     mq.getColumn(3).getInt()},
					{"episode",    mq.getColumn(4).getInt()},
					{"title",      mq.getColumn(5).getString()},
				});
			arr.push_back(g);
		}
		route::ok(res, arr.dump());
	});

	svr.Post("/api/shows/:id/groups", [this](const Req& req, Res& res) {
		auto show_id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string name       = b.value("name",       "");
			std::string group_type = b.value("group_type", "multipart");
			if (name.empty()) { route::err(res, 400, "name required"); return; }
			std::string group_id = BlockRepository(db_).createEpisodeGroup(show_id, name, group_type);
			res.status = 201;
			route::ok(res, json{{"group_id", group_id}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/shows/:id/groups", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/shows/:id/groups/:gid", [this](const Req& req, Res& res) {
		auto gid = req.path_params.at("gid");
		try {
			BlockRepository(db_).removeEpisodeGroup(gid);
			route::ok(res, json{{"deleted", gid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/shows/:id/groups/" + gid, e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/shows/:id/groups/:gid/members", [this](const Req& req, Res& res) {
		auto gid = req.path_params.at("gid");
		try {
			auto b = json::parse(req.body);
			std::string episode_id = b.value("episode_id", "");
			int         part_num   = b.value("part_num",   1);
			if (episode_id.empty()) { route::err(res, 400, "episode_id required"); return; }
			auto [rowid, pn] = BlockRepository(db_).addGroupMember(gid, episode_id, part_num);
			res.status = 201;
			route::ok(res, json{{"id", rowid}, {"part_num", pn}}.dump());
		} catch (const SQLite::Exception& e) {
			route::logErr("POST /api/shows/:id/groups/:gid/members", e);
			route::err(res, 409, e.what());
		} catch (const std::exception& e) {
			route::logErr("POST /api/shows/:id/groups/:gid/members", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/shows/:id/groups/:gid/members/:mid", [this](const Req& req, Res& res) {
		auto mid_str = req.path_params.at("mid");
		try {
			int mid = std::stoi(mid_str);
			BlockRepository(db_).removeGroupMember(mid);
			route::ok(res, json{{"deleted", mid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/shows/:id/groups/:gid/members/" + mid_str, e);
			route::err(res, 500, e.what());
		}
	});

	// ── Grouping candidates ───────────────────────────────────────────────────

	svr.Get("/api/shows/:id/grouping-candidates", [this](const Req& req, Res& res) {
		try {
			auto show_id = req.path_params.at("id");

			std::vector<EpRow> eps;
			{
				SQLite::Statement q(db_.get(),
					"SELECT episode_id, season, episode, title FROM episode "
					"WHERE show_id=? ORDER BY season, episode");
				q.bind(1, show_id);
				while (q.executeStep())
					eps.push_back({q.getColumn(0).getString(), q.getColumn(1).getInt(),
					               q.getColumn(2).getInt(), q.getColumn(3).getString()});
			}

			std::unordered_set<std::string> confirmed_ids;
			{
				SQLite::Statement q(db_.get(),
					"SELECT egm.episode_id FROM episode_group_member egm "
					"JOIN episode_group eg ON eg.group_id = egm.group_id WHERE eg.show_id = ?");
				q.bind(1, show_id);
				while (q.executeStep()) confirmed_ids.insert(q.getColumn(0).getString());
			}

			json candidates = buildGroupingCandidates(eps, confirmed_ids);
			std::sort(candidates.begin(), candidates.end(), [](const json& a, const json& b) {
				int ca = a["confidence"].get<int>(), cb = b["confidence"].get<int>();
				if (ca != cb) return ca > cb;
				return a["base_title"].get<std::string>() < b["base_title"].get<std::string>();
			});

			route::ok(res, json{{"show_id", show_id}, {"candidates", candidates}}.dump());
		} catch (const std::exception& e) {
			route::err(res, 500, e.what());
		}
	});

	svr.Get("/api/grouping-candidates", [this](const Req&, Res& res) {
		try {
			struct ShowRow { std::string show_id; std::string title; };
			std::vector<ShowRow> shows;
			{
				SQLite::Statement q(db_.get(), "SELECT show_id, title FROM show ORDER BY title");
				while (q.executeStep())
					shows.push_back({q.getColumn(0).getString(), q.getColumn(1).getString()});
			}

			json result = json::array();
			for (const auto& show : shows) {
				std::vector<EpRow> eps;
				{
					SQLite::Statement q(db_.get(),
						"SELECT episode_id, season, episode, title FROM episode "
						"WHERE show_id=? ORDER BY season, episode");
					q.bind(1, show.show_id);
					while (q.executeStep())
						eps.push_back({q.getColumn(0).getString(), q.getColumn(1).getInt(),
						               q.getColumn(2).getInt(), q.getColumn(3).getString()});
				}
				if (eps.empty()) continue;

				std::unordered_set<std::string> confirmed_ids;
				{
					SQLite::Statement q(db_.get(),
						"SELECT egm.episode_id FROM episode_group_member egm "
						"JOIN episode_group eg ON eg.group_id = egm.group_id WHERE eg.show_id = ?");
					q.bind(1, show.show_id);
					while (q.executeStep()) confirmed_ids.insert(q.getColumn(0).getString());
				}

				json candidates = buildGroupingCandidates(eps, confirmed_ids);

				bool has_unconfirmed = false;
				for (const auto& c : candidates)
					if (!c["already_grouped"].get<bool>()) { has_unconfirmed = true; break; }
				if (!has_unconfirmed) continue;

				std::sort(candidates.begin(), candidates.end(), [](const json& a, const json& b) {
					int ca = a["confidence"].get<int>(), cb = b["confidence"].get<int>();
					if (ca != cb) return ca > cb;
					return a["base_title"].get<std::string>() < b["base_title"].get<std::string>();
				});

				result.push_back({
					{"show_id",    show.show_id},
					{"show_title", show.title},
					{"candidates", candidates},
				});
			}

			route::ok(res, result.dump());
		} catch (const std::exception& e) {
			route::err(res, 500, e.what());
		}
	});
}
