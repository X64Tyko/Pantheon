#include "PlaylistPlayTargetHelper.h"
#include "../RouteHelpers.h"
#include "../../db/PlaylistRepository.h"
#include "../../db/WatchProgressRepository.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void resolvePlaylistPlayTarget(
		httplib::Response&  res,
		const std::string&  playlist_id,
		const std::string&  user_id,
		Database&           db) {
	auto d = PlaylistRepository(db).getDetail(playlist_id);
	if (!d || d->items.empty()) { route::ok(res, "null"); return; }

	std::vector<std::pair<std::string, std::string>> keys;
	for (const auto& it : d->items) keys.emplace_back(it.item_type, it.item_id);
	auto states = WatchProgressRepository(db).getStates(user_id, keys);

	for (const auto& it : d->items) {
		auto found = states.find(watchStateKey(it.item_type, it.item_id));
		if (found == states.end()) {
			route::ok(res, json{{"kind", it.item_type}, {"id", it.item_id}, {"position_ms", int64_t{0}}}.dump());
			return;
		}
		if (!found->second.completed) {
			route::ok(res, json{{"kind", it.item_type}, {"id", it.item_id}, {"position_ms", found->second.position_ms}}.dump());
			return;
		}
	}

	const auto& first = d->items.front();
	route::ok(res, json{{"kind", first.item_type}, {"id", first.item_id}, {"position_ms", int64_t{0}}}.dump());
}
