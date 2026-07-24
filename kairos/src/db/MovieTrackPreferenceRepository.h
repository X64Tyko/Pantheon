#pragma once
#include <cstdint>
#include <optional>
#include <string>

class Database;

// Movie counterpart of ShowTrackPreferenceRepository — see migration v94.
// Same shape, same "empty string means no preference set" convention;
// movies don't have the "carries into episode 2" reasoning shows do, but a
// sticky per-movie preference across rewatches (and being directly settable
// from the detail page before ever pressing play) is just as useful.
struct MovieTrackPreferenceRow {
	std::string audio_lang;
	std::string subtitle_lang;
};

class MovieTrackPreferenceRepository {
public:
	explicit MovieTrackPreferenceRepository(Database& db);

	std::optional<MovieTrackPreferenceRow> get(const std::string& user_id, const std::string& movie_id);

	// Only fields with a value are changed; the other side of the pair is
	// left as-is (nullopt on both is a no-op). Upserts.
	void set(const std::string& user_id, const std::string& movie_id,
	         const std::optional<std::string>& audio_lang,
	         const std::optional<std::string>& subtitle_lang,
	         int64_t updated_at);

private:
	Database& db_;
};
