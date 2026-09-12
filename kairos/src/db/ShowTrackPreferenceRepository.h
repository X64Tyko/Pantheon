#pragma once
#include <cstdint>
#include <optional>
#include <string>

class Database;

// Plex-style sticky per-(user, show) audio/subtitle language — see migration
// v92. Empty string means "no preference set" for that field.
struct ShowTrackPreferenceRow {
	std::string audio_lang;
	std::string subtitle_lang;
};

class ShowTrackPreferenceRepository {
public:
	explicit ShowTrackPreferenceRepository(Database& db);

	std::optional<ShowTrackPreferenceRow> get(const std::string& user_id, const std::string& show_id);

	// Only fields with a value are changed; the other side of the pair is
	// left as-is (nullopt on both is a no-op). Upserts.
	void set(const std::string& user_id, const std::string& show_id,
	         const std::optional<std::string>& audio_lang,
	         const std::optional<std::string>& subtitle_lang,
	         int64_t updated_at);

private:
	Database& db_;
};
