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
