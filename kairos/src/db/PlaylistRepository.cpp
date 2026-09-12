#include "PlaylistRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include "FilterExpr.h"
#include "MixedSort.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace
{
	// Mirrors ContentRepository::searchShows/searchMovies's sort-column mapping
	// (simplified — no sort_dir/random_seed, a smart playlist just needs a
	// sensible deterministic order), extended with entity-specific expressions
	// for the sorts that have no identically-named column on both show and
	// movie. `isShow` picks which side of that split applies; `desc` records
	// direction so a "mixed" playlist (see refreshSmart) can UNION ALL a show
	// query and a movie query and still ORDER BY one consistent column.
	struct SmartSortField
	{
		std::string expr;
		bool desc;
	};

	SmartSortField smartSortField(const std::string& sort, const std::string& alias, bool isShow)
	{
		if (sort == "recently_added") return {alias + ".added_at", true};
		if (sort == "year") return {"COALESCE(" + alias + ".year, 0)", true};
		if (sort == "audience_rating") return {"COALESCE(" + alias + ".audience_rating, 0)", true};
		// Shows have no single duration column (runtime varies per episode) —
		// same AVG-over-episodes scalar subquery ContentRepository::searchShows
		// uses for its own "duration" sort.
		if (sort == "duration")
			return isShow
					   ? SmartSortField{"COALESCE((SELECT AVG(e_d.duration_ms) FROM episode e_d WHERE e_d.show_id = " + alias + ".show_id), 0)", true}
					   : SmartSortField{alias + ".duration_ms", true};
		// Most-recently-aired/released first — the same combined concept the
		// Library page's sort picker already offers as one option (see
		// LibraryFilters.tsx's "Recently Aired/Released"), since shows and
		// movies don't share a column for it. Unset dates sort last regardless
		// of direction (COALESCE to the direction-appropriate sentinel).
		if (sort == "recently_released_or_aired")
			return isShow
					   ? SmartSortField{
						   "COALESCE((SELECT MAX(e2.air_date) FROM episode e2 WHERE e2.show_id = " + alias + ".show_id "
						   "AND e2.air_date != '' AND e2.air_date <= date('now')), '0000-00-00')",
						   true
					   }
					   : SmartSortField{"COALESCE(NULLIF(" + alias + ".release_date, ''), '0000-00-00')", true};
		// Chronological (oldest-first) release/premiere order — e.g. a
		// watch-through playlist ordered the way things actually came out,
		// which is also the main reason a "mixed" show+movie playlist is
		// useful at all (a franchise's movies and spin-off shows interleaved
		// by release date, not grouped by type).
		if (sort == "air_date")
			return isShow
					   ? SmartSortField{"COALESCE(NULLIF(" + alias + ".originally_available_at, ''), '9999-99-99')", false}
					   : SmartSortField{"COALESCE(NULLIF(" + alias + ".release_date, ''), '9999-99-99')", false};
		if (sort == "random") return {"RANDOM()", false};
		return {alias + ".title", false}; // default: title ASC
	}

	// sort_dir: "" (default) = the field's own natural direction (f.desc);
	// "asc"/"desc" explicitly overrides it — same convention as
	// ShowSearchParams::sort_dir. Random's a no-op regardless (no direction).
	std::string smartOrderBy(const std::string& sort, const std::string& alias, bool isShow, const std::string& sort_dir = "")
	{
		auto f    = smartSortField(sort, alias, isShow);
		bool desc = sort_dir.empty() ? f.desc : (sort_dir == "desc");
		return " ORDER BY " + f.expr + (desc ? " DESC" : " ASC");
	}

	// Both empty = always shown. MM-DD strings compare correctly lexicographically
	// as long as both are always zero-padded (enforced by whoever writes them —
	// see PlaylistService.cpp's PATCH validation), including the wraparound case
	// (e.g. start="11-15" end="01-05" spanning the year boundary).
	bool inActiveWindow(const std::string& start, const std::string& end)
	{
		if (start.empty() || end.empty()) return true;
		std::time_t now = std::time(nullptr);
		std::tm tmnow{};
		localtime_r(&now, &tmnow);
		char buf[6];
		std::snprintf(buf, sizeof(buf), "%02d-%02d", tmnow.tm_mon + 1, tmnow.tm_mday);
		std::string today(buf);
		if (start <= end) return today >= start && today <= end;
		return today >= start || today <= end;
	}
} // namespace

PlaylistRepository::PlaylistRepository(Database& db)
	: db_(db)
{
}

std::string PlaylistRepository::create(const std::string& title, const std::string& mode)
{
	std::string playlist_id = db::generateId();
	SQLite::Statement s(db_.get(),
						"INSERT INTO playlist (playlist_id, title, source, mode) VALUES (?,?,'custom',?)");
	s.bind(1, playlist_id);
	s.bind(2, title);
	s.bind(3, mode);
	s.exec();
	return playlist_id;
}

void PlaylistRepository::updateField(const std::string& playlist_id,
									 const std::string& col,
									 const std::string& val)
{
	SQLite::Statement s(db_.get(),
						"UPDATE playlist SET " + col + " = ? WHERE playlist_id = ?");
	s.bind(1, val);
	s.bind(2, playlist_id);
	s.exec();
}

void PlaylistRepository::remove(const std::string& playlist_id)
{
	// plex_list_link has no FK back to playlist (only to media_source), so
	// nothing at the DB level stops this row from outliving the playlist it
	// points at — leaving it behind means the next syncPlexLinks() pass tries
	// to INSERT INTO playlist_item for a playlist_id that no longer exists
	// and dies on the FK constraint. Deleting it here, not just in
	// unlinkPlex(), makes "this playlist is gone" actually true regardless of
	// which route the caller went through.
	SQLite::Transaction tx(db_.get());
	SQLite::Statement link(db_.get(), "DELETE FROM plex_list_link WHERE list_type = 'playlist' AND list_id = ?");
	link.bind(1, playlist_id);
	link.exec();
	SQLite::Statement s(db_.get(), "DELETE FROM playlist WHERE playlist_id = ?");
	s.bind(1, playlist_id);
	s.exec();
	tx.commit();
}

void PlaylistRepository::unlinkPlex(const std::string& playlist_id)
{
	SQLite::Statement s(db_.get(),
						"DELETE FROM plex_list_link WHERE list_type = 'playlist' AND list_id = ?");
	s.bind(1, playlist_id);
	s.exec();
}

std::pair<int64_t, int> PlaylistRepository::addItem(const std::string& playlist_id,
													const std::string& item_type,
													const std::string& item_id)
{
	int position = 0;
	{
		SQLite::Statement pq(db_.get(),
							 "SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_item WHERE playlist_id = ?");
		pq.bind(1, playlist_id);
		if (pq.executeStep()) position = pq.getColumn(0).getInt();
	}
	SQLite::Statement s(db_.get(),
						"INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
	s.bind(1, playlist_id);
	s.bind(2, position);
	s.bind(3, item_type);
	s.bind(4, item_id);
	s.exec();
	return {db_.get().getLastInsertRowid(), position};
}

int PlaylistRepository::addItems(const std::string& playlist_id,
								 const std::vector<std::pair<std::string, std::string>>& items)
{
	SQLite::Statement pq(db_.get(),
						 "SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_item WHERE playlist_id = ?");
	pq.bind(1, playlist_id);
	int position = pq.executeStep() ? pq.getColumn(0).getInt() : 0;
	int added    = 0;
	SQLite::Transaction tx(db_.get());
	for (const auto& [item_type, item_id] : items)
	{
		if (item_id.empty()) continue;
		try
		{
			SQLite::Statement s(db_.get(),
								"INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
			s.bind(1, playlist_id);
			s.bind(2, position);
			s.bind(3, item_type);
			s.bind(4, item_id);
			s.exec();
			++position;
			++added;
		}
		catch (const SQLite::Exception&)
		{
			/* skip duplicates */
		}
	}
	tx.commit();
	return added;
}

void PlaylistRepository::removeItem(int item_id, const std::string& playlist_id)
{
	int pos = -1;
	{
		SQLite::Statement q(db_.get(),
							"SELECT position FROM playlist_item WHERE id = ? AND playlist_id = ?");
		q.bind(1, item_id);
		q.bind(2, playlist_id);
		if (!q.executeStep()) return;
		pos = q.getColumn(0).getInt();
	}
	SQLite::Statement del(db_.get(), "DELETE FROM playlist_item WHERE id = ?");
	del.bind(1, item_id);
	del.exec();
	SQLite::Statement ren(db_.get(),
						  "UPDATE playlist_item SET position = position - 1 WHERE playlist_id = ? AND position > ?");
	ren.bind(1, playlist_id);
	ren.bind(2, pos);
	ren.exec();
}

void PlaylistRepository::moveItem(int item_id, const std::string& playlist_id, int new_position)
{
	SQLite::Statement s(db_.get(),
						"UPDATE playlist_item SET position = ? WHERE id = ? AND playlist_id = ?");
	s.bind(1, new_position);
	s.bind(2, item_id);
	s.bind(3, playlist_id);
	s.exec();
}

std::vector<PlaylistRow> PlaylistRepository::listAll()
{
	SQLite::Statement q(db_.get(), R"(
        SELECT p.playlist_id, p.title, p.mode,
               COUNT(pi.id) AS item_count,
               COALESCE(SUM(CASE pi.item_type
                   WHEN 'episode' THEN e.duration_ms
                   WHEN 'movie'   THEN m.duration_ms ELSE 0 END), 0) AS total_ms,
               pll.source_id, pll.external_id, pll.plex_type, pll.last_synced_at,
               p.membership, p.filter_expr, p.smart_type, p.smart_sort, p.smart_limit, p.last_smart_refresh_at,
               p.show_on_home, p.home_order, p.home_tile_limit, p.home_active_start, p.home_active_end,
               p.poster_source, p.smart_sort_dir, p.smart_expand_episodes
        FROM playlist p
        LEFT JOIN playlist_item pi ON pi.playlist_id = p.playlist_id
        LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
        LEFT JOIN plex_list_link pll ON pll.list_type = 'playlist' AND pll.list_id = p.playlist_id
        GROUP BY p.playlist_id ORDER BY p.title
    )");
	std::vector<PlaylistRow> rows;
	while (q.executeStep())
	{
		PlaylistRow r;
		r.playlist_id = q.getColumn(0).getString();
		r.title       = q.getColumn(1).getString();
		r.mode        = q.getColumn(2).getString();
		r.item_count  = q.getColumn(3).getInt();
		r.total_ms    = q.getColumn(4).getInt64();
		if (!q.getColumn(5).isNull())
		{
			PlexLinkRow pl;
			pl.source_id   = q.getColumn(5).getString();
			pl.external_id = q.getColumn(6).getString();
			pl.plex_type   = q.getColumn(7).getString();
			if (!q.getColumn(8).isNull()) pl.last_synced_at = q.getColumn(8).getInt64();
			r.plex_link = std::move(pl);
		}
		r.membership  = q.getColumn(9).getString();
		r.filter_expr = q.getColumn(10).getString();
		r.smart_type  = q.getColumn(11).getString();
		r.smart_sort  = q.getColumn(12).getString();
		r.smart_limit = q.getColumn(13).getInt();
		if (!q.getColumn(14).isNull()) r.last_smart_refresh_at = q.getColumn(14).getInt64();
		r.show_on_home          = q.getColumn(15).getInt() != 0;
		r.home_order            = q.getColumn(16).getInt();
		r.home_tile_limit       = q.getColumn(17).getInt();
		r.home_active_start     = q.getColumn(18).getString();
		r.home_active_end       = q.getColumn(19).getString();
		r.poster_source         = q.getColumn(20).getString();
		r.smart_sort_dir        = q.getColumn(21).getString();
		r.smart_expand_episodes = q.getColumn(22).getInt() != 0;
		rows.push_back(std::move(r));
	}
	return rows;
}

std::optional<PlaylistDetail> PlaylistRepository::getDetail(const std::string& playlist_id)
{
	SQLite::Statement ph(db_.get(),
						 "SELECT playlist_id, title, mode, membership, filter_expr, smart_type, smart_sort, smart_limit, last_smart_refresh_at, "
						 "show_on_home, home_order, home_tile_limit, home_active_start, home_active_end, poster_source, "
						 "smart_sort_dir, smart_expand_episodes "
						 "FROM playlist WHERE playlist_id = ?");
	ph.bind(1, playlist_id);
	if (!ph.executeStep()) return std::nullopt;

	PlaylistDetail d;
	d.playlist_id = ph.getColumn(0).getString();
	d.title       = ph.getColumn(1).getString();
	d.mode        = ph.getColumn(2).getString();
	d.membership  = ph.getColumn(3).getString();
	d.filter_expr = ph.getColumn(4).getString();
	d.smart_type  = ph.getColumn(5).getString();
	d.smart_sort  = ph.getColumn(6).getString();
	d.smart_limit = ph.getColumn(7).getInt();
	if (!ph.getColumn(8).isNull()) d.last_smart_refresh_at = ph.getColumn(8).getInt64();
	d.show_on_home          = ph.getColumn(9).getInt() != 0;
	d.home_order            = ph.getColumn(10).getInt();
	d.home_tile_limit       = ph.getColumn(11).getInt();
	d.home_active_start     = ph.getColumn(12).getString();
	d.home_active_end       = ph.getColumn(13).getString();
	d.poster_source         = ph.getColumn(14).getString();
	d.smart_sort_dir        = ph.getColumn(15).getString();
	d.smart_expand_episodes = ph.getColumn(16).getInt() != 0;

	SQLite::Statement q(db_.get(), R"(
        SELECT pi.id, pi.position, pi.item_type, pi.item_id,
               CASE pi.item_type
                   WHEN 'episode' THEN s.title || ' S' || PRINTF('%02d',e.season) ||
                                       'E' || PRINTF('%02d',e.episode) || ' — ' || e.title
                   WHEN 'movie'   THEN m.title ELSE ''
               END AS title,
               CASE pi.item_type
                   WHEN 'episode' THEN e.duration_ms
                   WHEN 'movie'   THEN m.duration_ms ELSE 0
               END AS duration_ms,
               e.season, e.episode
        FROM playlist_item pi
        LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        LEFT JOIN show    s ON e.show_id = s.show_id
        LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
        WHERE pi.playlist_id = ? ORDER BY pi.position
    )");
	q.bind(1, playlist_id);
	while (q.executeStep())
	{
		PlaylistItemRow r;
		r.id          = q.getColumn(0).getInt64();
		r.position    = q.getColumn(1).getInt();
		r.item_type   = q.getColumn(2).getString();
		r.item_id     = q.getColumn(3).getString();
		r.title       = q.getColumn(4).getString();
		r.duration_ms = q.getColumn(5).getInt64();
		if (!q.getColumn(6).isNull())
		{
			r.season  = q.getColumn(6).getInt();
			r.episode = q.getColumn(7).getInt();
		}
		d.items.push_back(std::move(r));
	}
	return d;
}

std::vector<std::pair<std::string, std::string>> PlaylistRepository::expandBlockItems(const std::string& block_id)
{
	std::vector<std::pair<std::string, std::string>> items;
	SQLite::Statement cq(db_.get(), R"(
        SELECT content_type, content_id, season_filter, episode_order, include_specials
        FROM block_content WHERE block_id = ? ORDER BY position
    )");
	cq.bind(1, block_id);
	while (cq.executeStep())
	{
		std::string ct  = cq.getColumn(0).getString();
		std::string cid = cq.getColumn(1).getString();
		if (ct == "show")
		{
			bool has_season         = !cq.getColumn(2).isNull();
			int season_filter       = has_season ? cq.getColumn(2).getInt() : -1;
			bool incl_specials      = cq.getColumn(4).getInt() != 0;
			std::string ep_order    = cq.getColumn(3).getString();
			std::string order_col   = (ep_order == "air_date") ? "air_date" : "season, episode";
			std::string where_extra = has_season
										  ? " AND season = ?"
										  : !incl_specials
										  ? " AND season > 0"
										  : "";
			SQLite::Statement eq(db_.get(),
								 "SELECT episode_id FROM episode WHERE show_id = ?" +
								 where_extra + " ORDER BY " + order_col);
			eq.bind(1, cid);
			if (has_season) eq.bind(2, season_filter);
			while (eq.executeStep()) items.push_back({"episode", eq.getColumn(0).getString()});
		}
		else if (ct == "movie")
		{
			items.push_back({"movie", cid});
		}
	}
	return items;
}

std::pair<std::string, int> PlaylistRepository::createFromBlock(const std::string& block_id,
																const std::string& title)
{
	auto items = expandBlockItems(block_id);

	std::string playlist_id = db::generateId();
	SQLite::Transaction txn(db_.get());
	SQLite::Statement ps(db_.get(),
						 "INSERT INTO playlist (playlist_id, title, source, mode) VALUES (?,?,'custom','sequential')");
	ps.bind(1, playlist_id);
	ps.bind(2, title);
	ps.exec();

	int pos = 0;
	for (const auto& item : items)
	{
		SQLite::Statement ins(db_.get(),
							  "INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
		ins.bind(1, playlist_id);
		ins.bind(2, pos++);
		ins.bind(3, item.first);
		ins.bind(4, item.second);
		ins.exec();
	}
	txn.commit();
	return {std::move(playlist_id), static_cast<int>(items.size())};
}

void PlaylistRepository::replaceListItems(
	const std::string& list_type,
	const std::string& list_id,
	const std::vector<std::pair<std::string, std::string>>& items)
{
	const std::string fk_col   = (list_type == "playlist") ? "playlist_id" : "filler_list_id";
	const std::string item_tbl = (list_type == "playlist") ? "playlist_item" : "filler_list_item";

	SQLite::Transaction txn(db_.get());
	{
		SQLite::Statement del(db_.get(), "DELETE FROM " + item_tbl + " WHERE " + fk_col + " = ?");
		del.bind(1, list_id);
		del.exec();
	}
	int pos = 0;
	for (const auto& [itype, iid] : items)
	{
		SQLite::Statement ins(db_.get(),
							  "INSERT OR IGNORE INTO " + item_tbl +
							  " (" + fk_col + ", position, item_type, item_id) VALUES (?,?,?,?)");
		ins.bind(1, list_id);
		ins.bind(2, pos++);
		ins.bind(3, itype);
		ins.bind(4, iid);
		ins.exec();
	}
	txn.commit();
}

void PlaylistRepository::upsertPlexLink(const std::string& list_type,
										const std::string& list_id,
										const std::string& source_id,
										const std::string& external_id,
										const std::string& plex_type)
{
	int64_t now = static_cast<int64_t>(std::time(nullptr));
	SQLite::Statement s(db_.get(), R"(
        INSERT INTO plex_list_link (list_type, list_id, source_id, external_id, plex_type, last_synced_at)
        VALUES (?,?,?,?,?,?)
        ON CONFLICT(list_type, list_id) DO UPDATE SET
            source_id      = excluded.source_id,
            external_id    = excluded.external_id,
            plex_type      = excluded.plex_type,
            last_synced_at = excluded.last_synced_at
    )");
	s.bind(1, list_type);
	s.bind(2, list_id);
	s.bind(3, source_id);
	s.bind(4, external_id);
	s.bind(5, plex_type);
	s.bind(6, now);
	s.exec();
}

std::optional<PlexLinkRow> PlaylistRepository::getLink(const std::string& list_type, const std::string& list_id)
{
	SQLite::Statement q(db_.get(),
						"SELECT source_id, external_id, plex_type, last_synced_at FROM plex_list_link "
						"WHERE list_type = ? AND list_id = ?");
	q.bind(1, list_type);
	q.bind(2, list_id);
	if (!q.executeStep()) return std::nullopt;
	PlexLinkRow r;
	r.source_id   = q.getColumn(0).getString();
	r.external_id = q.getColumn(1).getString();
	r.plex_type   = q.getColumn(2).getString();
	if (!q.getColumn(3).isNull()) r.last_synced_at = q.getColumn(3).getInt64();
	return r;
}

int PlaylistRepository::refreshSmart(const std::string& playlist_id, const std::string& user_id)
{
	SmartPlaylistFields f;
	{
		SQLite::Statement q(db_.get(),
							"SELECT membership, filter_expr, smart_type, smart_sort, smart_limit, smart_sort_dir, smart_expand_episodes "
							"FROM playlist WHERE playlist_id = ?");
		q.bind(1, playlist_id);
		if (!q.executeStep()) throw std::runtime_error("playlist not found");
		f.membership            = q.getColumn(0).getString();
		f.filter_expr           = q.getColumn(1).getString();
		f.smart_type            = q.getColumn(2).getString();
		f.smart_sort            = q.getColumn(3).getString();
		f.smart_limit           = q.getColumn(4).getInt();
		f.smart_sort_dir        = q.getColumn(5).getString();
		f.smart_expand_episodes = q.getColumn(6).getInt() != 0;
	}
	if (f.membership != "smart") throw std::runtime_error("playlist is not a smart playlist");

	const std::string limit_clause = f.smart_limit > 0 ? " LIMIT " + std::to_string(f.smart_limit) : "";
	std::vector<std::pair<std::string, std::string>> items;

	if (f.smart_type == "mixed")
	{
		// One filter_expr, compiled separately per entity — a field with no
		// meaning on one side (e.g. director:X on a show) already degrades
		// to "1=0" there (see FilterExpr.cpp), so it naturally excludes
		// that side rather than needing special-casing here. mixedEntityOrder
		// already returns movie/episode pairs (not shows), so this is a
		// direct copy — no per-show expansion loop needed.
		items = mixedEntityOrder(db_.get(), f.filter_expr, f.smart_sort, f.smart_limit, f.smart_sort_dir, user_id);
	}
	else if (f.smart_type == "movie")
	{
		auto compiled = compileFilterExpr(f.filter_expr, FilterEntity::Movie, "m");
		SQLite::Statement q(db_.get(),
							"SELECT m.movie_id FROM movie m WHERE (" + compiled.sql + ")" +
							smartOrderBy(f.smart_sort, "m", false, f.smart_sort_dir) + limit_clause);
		for (size_t i = 0; i < compiled.binds.size(); ++i) q.bind(static_cast<int>(i) + 1, compiled.binds[i]);
		while (q.executeStep()) items.push_back({"movie", q.getColumn(0).getString()});
	}
	else if (f.smart_expand_episodes)
	{
		// Explicit per-playlist toggle (Database.cpp migration 102) —
		// flatten to a per-episode list under `smart_sort`, any mode, rather
		// than the grouped-by-show branch below. The original motivation
		// was date sorts specifically (ordering the matching shows and then
		// dumping each one's ENTIRE run underneath it buries this week's
		// episode of show B under show A's five-year-old back catalog just
		// because show A's latest episode happens to be a few days newer),
		// but any sort mode can flatten now if that's what's wanted.
		// smart_limit becomes an episode cap here, not a show cap.
		auto episodes = fetchMixedEpisodeRows(db_.get(), f.filter_expr, user_id);
		bool desc     = f.smart_sort_dir.empty() ? mixedRowNaturalDesc(f.smart_sort) : (f.smart_sort_dir == "desc");
		sortMixedRows(episodes, f.smart_sort, desc);
		if (f.smart_limit > 0 && static_cast<int>(episodes.size()) > f.smart_limit) episodes.resize(f.smart_limit);
		for (auto& ep : episodes) items.push_back({"episode", ep.id});
	}
	else
	{
		// Show-typed smart playlists expand to every matching show's
		// episodes (season/episode order, specials excluded — same default
		// as expandBlockItems's no-season-filter case) rather than the shows
		// themselves: playlist_item has no "show" item_type, only
		// episode/movie (see Database.cpp's playlist_item CHECK constraint).
		auto compiled = compileFilterExpr(f.filter_expr, FilterEntity::Show, "s");
		SQLite::Statement q(db_.get(),
							"SELECT s.show_id FROM show s WHERE (" + compiled.sql + ")" +
							smartOrderBy(f.smart_sort, "s", true, f.smart_sort_dir) + limit_clause);
		for (size_t i = 0; i < compiled.binds.size(); ++i) q.bind(static_cast<int>(i) + 1, compiled.binds[i]);
		std::vector<std::string> show_ids;
		while (q.executeStep()) show_ids.push_back(q.getColumn(0).getString());

		auto watchState = extractWatchState(f.filter_expr);
		for (const auto& show_id : show_ids)
		{
			std::vector<std::string> epBinds;
			std::string watchCond = episodeWatchCondition(watchState, user_id, "episode_id", epBinds);
			SQLite::Statement eq(db_.get(),
								 "SELECT episode_id FROM episode WHERE show_id = ? AND season > 0 AND (" + watchCond + ") ORDER BY season, episode");
			eq.bind(1, show_id);
			for (size_t i = 0; i < epBinds.size(); ++i) eq.bind(static_cast<int>(i) + 2, epBinds[i]);
			while (eq.executeStep()) items.push_back({"episode", eq.getColumn(0).getString()});
		}
	}

	replaceListItems("playlist", playlist_id, items);

	SQLite::Statement ts(db_.get(), "UPDATE playlist SET last_smart_refresh_at = ? WHERE playlist_id = ?");
	ts.bind(1, static_cast<int64_t>(std::time(nullptr)));
	ts.bind(2, playlist_id);
	ts.exec();

	return static_cast<int>(items.size());
}

std::vector<PlaylistRepository::BrowseEntry> PlaylistRepository::listBrowse()
{
	SQLite::Statement q(db_.get(), R"(
        SELECT p.playlist_id, p.title, p.mode, p.poster_source,
               COUNT(pi.id) AS item_count,
               COALESCE(SUM(CASE pi.item_type
                   WHEN 'episode' THEN e.duration_ms
                   WHEN 'movie'   THEN m.duration_ms ELSE 0 END), 0) AS total_ms
        FROM playlist p
        LEFT JOIN playlist_item pi ON pi.playlist_id = p.playlist_id
        LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
        GROUP BY p.playlist_id ORDER BY p.title
    )");
	std::vector<BrowseEntry> rows;
	while (q.executeStep())
	{
		BrowseEntry r;
		r.playlist_id   = q.getColumn(0).getString();
		r.title         = q.getColumn(1).getString();
		r.mode          = q.getColumn(2).getString();
		r.poster_source = q.getColumn(3).getString();
		r.item_count    = q.getColumn(4).getInt();
		r.total_ms      = q.getColumn(5).getInt64();
		rows.push_back(std::move(r));
	}

	// Preview items (first 4 by position) — only needed for the client-side
	// collage fallback when poster_source is empty, so skip the lookup
	// otherwise.
	for (auto& r : rows)
	{
		if (!r.poster_source.empty()) continue;
		SQLite::Statement pq(db_.get(),
							 "SELECT item_type, item_id FROM playlist_item WHERE playlist_id = ? ORDER BY position LIMIT 4");
		pq.bind(1, r.playlist_id);
		while (pq.executeStep()) r.preview_items.push_back({pq.getColumn(0).getString(), pq.getColumn(1).getString()});
	}
	return rows;
}

std::vector<PlaylistRepository::HomeShelfRow> PlaylistRepository::listHomeShelves()
{
	SQLite::Statement q(db_.get(), R"(
        SELECT playlist_id, title, smart_type, filter_expr, smart_sort, home_tile_limit,
               home_active_start, home_active_end, smart_sort_dir, smart_expand_episodes
        FROM playlist
        WHERE membership = 'smart' AND show_on_home = 1
        ORDER BY home_order, title
    )");
	std::vector<HomeShelfRow> rows;
	while (q.executeStep())
	{
		std::string start = q.getColumn(6).getString();
		std::string end   = q.getColumn(7).getString();
		if (!inActiveWindow(start, end)) continue;
		HomeShelfRow r;
		r.playlist_id           = q.getColumn(0).getString();
		r.title                 = q.getColumn(1).getString();
		r.smart_type            = q.getColumn(2).getString();
		r.filter_expr           = q.getColumn(3).getString();
		r.smart_sort            = q.getColumn(4).getString();
		r.home_tile_limit       = q.getColumn(5).getInt();
		r.smart_sort_dir        = q.getColumn(8).getString();
		r.smart_expand_episodes = q.getColumn(9).getInt() != 0;
		rows.push_back(std::move(r));
	}
	return rows;
}