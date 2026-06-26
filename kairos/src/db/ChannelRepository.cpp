#include "ChannelRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <iomanip>
#include <random>
#include <sstream>

namespace {

std::string generateId() {
    thread_local std::mt19937_64 rng(std::random_device{}());
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << rng();
    return ss.str();
}

} // namespace

ChannelRepository::ChannelRepository(Database& db) : db_(db) {}

Channel ChannelRepository::rowToChannel(SQLite::Statement& q) {
	Channel c;
	c.channel_id               = q.getColumn(0).getString();
	c.name                     = q.getColumn(1).getString();
	c.number                   = q.getColumn(2).getInt();
	c.timezone                 = q.getColumn(3).getString();
	c.default_filler_selection = q.getColumn(4).getString();
	c.seed                     = q.getColumn(5).getInt();
	c.advance_mode             = q.getColumn(6).getString();
	c.offline_video_path       = q.getColumn(7).getString();
	c.offline_image_path       = q.getColumn(8).getString();
	c.offline_audio_id         = q.getColumn(9).getString();
	c.offline_audio_type       = q.getColumn(10).getString();
	c.offline_audio_title      = q.getColumn(11).getString();
	c.logo_path                = q.getColumn(12).getString();
	if (!q.getColumn(13).isNull()) c.anchor_hashes = q.getColumn(13).getString();
	return c;
}

std::vector<Channel> ChannelRepository::listChannels() {
	SQLite::Statement q(db_.get(),
		"SELECT channel_id, name, number, timezone, default_filler_selection, seed, advance_mode, "
		"       offline_video_path, offline_image_path, offline_audio_id, offline_audio_type, "
		"       offline_audio_title, logo_path, anchor_hashes "
		"FROM channel ORDER BY number");
	std::vector<Channel> result;
	while (q.executeStep()) result.push_back(rowToChannel(q));
	return result;
}

std::optional<Channel> ChannelRepository::findById(const std::string& channel_id) {
	SQLite::Statement q(db_.get(),
		"SELECT channel_id, name, number, timezone, default_filler_selection, seed, advance_mode, "
		"       offline_video_path, offline_image_path, offline_audio_id, offline_audio_type, "
		"       offline_audio_title, logo_path, anchor_hashes "
		"FROM channel WHERE channel_id = ?");
	q.bind(1, channel_id);
	if (!q.executeStep()) return std::nullopt;
	return rowToChannel(q);
}

std::string ChannelRepository::create(const std::string& name, int number,
                                       const std::string& timezone,
                                       const std::string& advance_mode) {
	std::string channel_id = generateId();
	SQLite::Statement s(db_.get(),
		"INSERT INTO channel (channel_id, name, number, timezone, advance_mode) VALUES (?,?,?,?,?)");
	s.bind(1, channel_id); s.bind(2, name);
	s.bind(3, number);     s.bind(4, timezone); s.bind(5, advance_mode);
	s.exec();
	return channel_id;
}

void ChannelRepository::updateField(const std::string& channel_id,
                                     const std::string& col,
                                     const std::string& value) {
	SQLite::Statement s(db_.get(),
		std::string("UPDATE channel SET ") + col + " = ? WHERE channel_id = ?");
	s.bind(1, value); s.bind(2, channel_id); s.exec();
}

void ChannelRepository::updateField(const std::string& channel_id,
                                     const std::string& col,
                                     int value) {
	SQLite::Statement s(db_.get(),
		std::string("UPDATE channel SET ") + col + " = ? WHERE channel_id = ?");
	s.bind(1, value); s.bind(2, channel_id); s.exec();
}

void ChannelRepository::remove(const std::string& channel_id) {
	SQLite::Statement s(db_.get(), "DELETE FROM channel WHERE channel_id = ?");
	s.bind(1, channel_id); s.exec();
}

nlohmann::json ChannelRepository::listFillerEntries(const std::string& channel_id) {
	SQLite::Statement q(db_.get(), R"(
		SELECT cfe.id, cfe.content_type, cfe.content_id,
		       COALESCE(fl.title, pl.title, sh.title, mv.title, cfe.content_id),
		       cfe.advancement, cfe.weight, cfe.position, cfe.season_filter
		FROM channel_filler_entry cfe
		LEFT JOIN filler_list fl ON cfe.content_type='filler_list' AND fl.filler_list_id=cfe.content_id
		LEFT JOIN playlist    pl ON cfe.content_type='playlist'    AND pl.playlist_id=cfe.content_id
		LEFT JOIN show        sh ON cfe.content_type='show'        AND sh.show_id=cfe.content_id
		LEFT JOIN movie       mv ON cfe.content_type='movie'       AND mv.movie_id=cfe.content_id
		WHERE cfe.channel_id = ? ORDER BY cfe.position
	)");
	q.bind(1, channel_id);
	nlohmann::json arr = nlohmann::json::array();
	while (q.executeStep()) {
		nlohmann::json fe = {
			{"id",           q.getColumn(0).getInt()},
			{"content_type", q.getColumn(1).getString()},
			{"content_id",   q.getColumn(2).getString()},
			{"title",        q.getColumn(3).getString()},
			{"advancement",  q.getColumn(4).getString()},
			{"weight",       q.getColumn(5).getInt()},
			{"position",     q.getColumn(6).getInt()},
		};
		if (!q.getColumn(7).isNull()) fe["season_filter"] = q.getColumn(7).getInt();
		arr.push_back(fe);
	}
	return arr;
}

ChannelRepository::FillerEntryResult ChannelRepository::addFillerEntry(
	const std::string& channel_id,
	const std::string& content_type,
	const std::string& content_id,
	const std::string& advancement,
	int weight,
	std::optional<int> season_filter)
{
	int position = 0;
	{
		SQLite::Statement pq(db_.get(),
			"SELECT COALESCE(MAX(position), -1) + 1 FROM channel_filler_entry WHERE channel_id = ?");
		pq.bind(1, channel_id);
		if (pq.executeStep()) position = pq.getColumn(0).getInt();
	}
	std::string title;
	{
		const char* tsql =
			content_type == "playlist"   ? "SELECT title FROM playlist    WHERE playlist_id=?"   :
			content_type == "show"       ? "SELECT title FROM show        WHERE show_id=?"       :
			content_type == "movie"      ? "SELECT title FROM movie       WHERE movie_id=?"      :
			/* filler_list */               "SELECT title FROM filler_list WHERE filler_list_id=?";
		SQLite::Statement tq(db_.get(), tsql);
		tq.bind(1, content_id);
		if (tq.executeStep()) title = tq.getColumn(0).getString();
	}
	SQLite::Statement s(db_.get(), R"(
		INSERT INTO channel_filler_entry
		    (channel_id, content_type, content_id, advancement, weight, position, season_filter)
		VALUES (?,?,?,?,?,?,?)
	)");
	s.bind(1, channel_id); s.bind(2, content_type); s.bind(3, content_id);
	s.bind(4, advancement); s.bind(5, weight); s.bind(6, position);
	if (season_filter.has_value()) s.bind(7, season_filter.value()); else s.bind(7);
	s.exec();
	return {db_.get().getLastInsertRowid(), position, title};
}

void ChannelRepository::updateFillerEntryField(int id, const std::string& col, const std::string& val) {
	SQLite::Statement s(db_.get(),
		"UPDATE channel_filler_entry SET " + col + " = ? WHERE id = ?");
	s.bind(1, val); s.bind(2, id); s.exec();
}

void ChannelRepository::updateFillerEntryField(int id, const std::string& col, int val) {
	SQLite::Statement s(db_.get(),
		"UPDATE channel_filler_entry SET " + col + " = ? WHERE id = ?");
	s.bind(1, val); s.bind(2, id); s.exec();
}

void ChannelRepository::removeFillerEntry(int id) {
	SQLite::Statement s(db_.get(), "DELETE FROM channel_filler_entry WHERE id = ?");
	s.bind(1, id); s.exec();
}
