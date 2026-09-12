#include "MixedSort.h"
#include "FilterExpr.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <random>

bool mixedRowNaturalDesc(const std::string& sort)
{
	return sort != "title" && sort != "air_date" && sort != "random";
}

bool mixedRowLess(const MixedRow& a, const MixedRow& b, const std::string& sort, bool desc)
{
	if (sort == "random") return false; // see sortMixedRows
	if (sort == "year") return desc ? a.year > b.year : a.year < b.year;
	if (sort == "audience_rating") return desc ? a.audience_rating > b.audience_rating : a.audience_rating < b.audience_rating;
	if (sort == "recently_added") return desc ? a.added_at > b.added_at : a.added_at < b.added_at;
	if (sort == "duration") return desc ? a.duration_ms > b.duration_ms : a.duration_ms < b.duration_ms;
	if (sort == "recently_released_or_aired" || sort == "recently_aired" || sort == "recently_released")
	{
		if ((a.recent_key == 0) != (b.recent_key == 0)) return b.recent_key == 0;
		return desc ? a.recent_key > b.recent_key : a.recent_key < b.recent_key;
	}
	if (sort == "air_date")
	{
		if ((a.premiere_key == 0) != (b.premiere_key == 0)) return b.premiere_key == 0;
		return desc ? a.premiere_key > b.premiere_key : a.premiere_key < b.premiere_key;
	}
	return desc ? a.title > b.title : a.title < b.title; // default: title
}

void sortMixedRows(std::vector<MixedRow>& rows, const std::string& sort, bool desc)
{
	if (sort == "random")
	{
		std::shuffle(rows.begin(), rows.end(), std::mt19937{std::random_device{}()});
		return;
	}
	std::sort(rows.begin(), rows.end(), [&](const MixedRow& a, const MixedRow& b) { return mixedRowLess(a, b, sort, desc); });
}

std::string mixedDateToInt(const std::string& col)
{
	return "CAST(REPLACE(NULLIF(" + col + ",''), '-', '') AS INTEGER)";
}

std::vector<MixedRow> fetchMixedRows(SQLite::Database& db, const std::string& entity_type, const std::string& id_col,
									 const std::string& table, const std::string& alias,
									 const std::string& where_sql, const std::vector<std::string>& binds,
									 const std::string& duration_expr, const std::string& recent_expr,
									 const std::string& premiere_expr)
{
	SQLite::Statement q(db,
						"SELECT " + id_col + ", " + alias + ".title, " + alias + ".year, " + alias + ".audience_rating, "
						+ alias + ".added_at, " + duration_expr + ", " + recent_expr + ", " + premiere_expr + " "
						"FROM " + table + " " + alias + " WHERE (" + where_sql + ")");
	for (size_t i = 0; i < binds.size(); ++i) q.bind(static_cast<int>(i) + 1, binds[i]);
	std::vector<MixedRow> out;
	while (q.executeStep())
	{
		MixedRow r;
		r.entity_type = entity_type;
		r.id          = q.getColumn(0).getString();
		r.title       = q.getColumn(1).getString();
		if (!q.getColumn(2).isNull()) r.year = q.getColumn(2).getInt64();
		if (!q.getColumn(3).isNull()) r.audience_rating = q.getColumn(3).getDouble();
		r.added_at = q.getColumn(4).getInt64();
		if (!q.getColumn(5).isNull()) r.duration_ms = q.getColumn(5).getDouble();
		if (!q.getColumn(6).isNull()) r.recent_key = q.getColumn(6).getInt64();
		if (!q.getColumn(7).isNull()) r.premiere_key = q.getColumn(7).getInt64();
		out.push_back(std::move(r));
	}
	return out;
}

std::vector<MixedRow> fetchMixedEpisodeRows(SQLite::Database& db, const std::string& filter,
											const std::string& user_id,
											const std::string& extra_where,
											const std::vector<std::string>& extra_binds)
{
	auto compiled = compileFilterExpr(filter, FilterEntity::Show, "s");
	std::vector<std::string> watchBinds;
	std::string watchCond = episodeWatchCondition(extractWatchState(filter), user_id, "e.episode_id", watchBinds);
	std::string air_key   = mixedDateToInt("e.air_date");

	SQLite::Statement q(db,
						"SELECT e.episode_id, e.title, s.year, s.audience_rating, s.added_at, e.duration_ms, " + air_key + " "
						"FROM episode e JOIN show s ON s.show_id = e.show_id "
						"WHERE e.season > 0 AND (" + watchCond + ") AND (" + compiled.sql + ")" + extra_where);
	int idx = 1;
	for (auto& v : watchBinds) q.bind(idx++, v);
	for (auto& v : compiled.binds) q.bind(idx++, v);
	for (auto& v : extra_binds) q.bind(idx++, v);

	std::vector<MixedRow> out;
	while (q.executeStep())
	{
		MixedRow r;
		r.entity_type = "episode";
		r.id          = q.getColumn(0).getString();
		r.title       = q.getColumn(1).getString();
		if (!q.getColumn(2).isNull()) r.year = q.getColumn(2).getInt64();
		if (!q.getColumn(3).isNull()) r.audience_rating = q.getColumn(3).getDouble();
		r.added_at    = q.getColumn(4).getInt64();
		r.duration_ms = static_cast<double>(q.getColumn(5).getInt64());
		if (!q.getColumn(6).isNull()) r.recent_key = r.premiere_key = q.getColumn(6).getInt64();
		out.push_back(std::move(r));
	}
	return out;
}

std::vector<std::pair<std::string, std::string>> mixedEntityOrder(SQLite::Database& db, const std::string& filter_expr,
																  const std::string& sort, int limit,
																  const std::string& sort_dir,
																  const std::string& user_id)
{
	auto compiled_m = compileFilterExpr(filter_expr, FilterEntity::Movie, "m");

	std::string release_key = mixedDateToInt("m.release_date");
	auto rows               = fetchMixedRows(db, "movie", "m.movie_id", "movie", "m", compiled_m.sql, compiled_m.binds,
											 "m.duration_ms", release_key, release_key);

	auto episode_rows = fetchMixedEpisodeRows(db, filter_expr, user_id);
	rows.insert(rows.end(), std::make_move_iterator(episode_rows.begin()), std::make_move_iterator(episode_rows.end()));

	bool desc = sort_dir.empty() ? mixedRowNaturalDesc(sort) : (sort_dir == "desc");
	sortMixedRows(rows, sort, desc);
	if (limit > 0 && static_cast<int>(rows.size()) > limit) rows.resize(limit);

	std::vector<std::pair<std::string, std::string>> out;
	out.reserve(rows.size());
	for (auto& r : rows) out.push_back({r.entity_type, r.id});
	return out;
}