#include "ShowTrackPreferenceRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

ShowTrackPreferenceRepository::ShowTrackPreferenceRepository(Database& db) : db_(db) {}

std::optional<ShowTrackPreferenceRow> ShowTrackPreferenceRepository::get(const std::string& user_id, const std::string& show_id) {
	SQLite::Statement q(db_.get(), "SELECT audio_lang, subtitle_lang FROM show_track_preference WHERE user_id = ? AND show_id = ?");
	q.bind(1, user_id);
	q.bind(2, show_id);
	if (!q.executeStep()) return std::nullopt;

	ShowTrackPreferenceRow r;
	r.audio_lang    = q.getColumn(0).getString();
	r.subtitle_lang = q.getColumn(1).getString();
	return r;
}

void ShowTrackPreferenceRepository::set(const std::string& user_id, const std::string& show_id,
                                         const std::optional<std::string>& audio_lang,
                                         const std::optional<std::string>& subtitle_lang,
                                         int64_t updated_at) {
	if (!audio_lang && !subtitle_lang) return;

	auto existing = get(user_id, show_id);
	std::string audio    = audio_lang.value_or(existing ? existing->audio_lang : "");
	std::string subtitle = subtitle_lang.value_or(existing ? existing->subtitle_lang : "");

	SQLite::Statement q(db_.get(), R"SQL(
		INSERT INTO show_track_preference (user_id, show_id, audio_lang, subtitle_lang, updated_at)
		VALUES (?, ?, ?, ?, ?)
		ON CONFLICT(user_id, show_id) DO UPDATE SET
			audio_lang    = excluded.audio_lang,
			subtitle_lang = excluded.subtitle_lang,
			updated_at    = excluded.updated_at
	)SQL");
	q.bind(1, user_id);
	q.bind(2, show_id);
	q.bind(3, audio);
	q.bind(4, subtitle);
	q.bind(5, updated_at);
	q.exec();
}
