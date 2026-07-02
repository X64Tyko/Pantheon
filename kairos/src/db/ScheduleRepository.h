#pragma once
#include "Database.h"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

struct NowProgramRow {
    std::string item_type, item_id, block_id, file_path, title, show_title, show_id;
    int season = 0, episode = 0;
    int64_t duration_ms = 0, wall_clock_start = 0, wall_clock_end = 0;
    bool is_filler = false;
};

struct FillerFallbackRow {
    std::string item_type, item_id, file_path, title;
    int64_t duration_ms = 0;
};

struct OfflineFallbackRow {
    std::string vid_path, img_path, audio_id, audio_typ, logo_path;
};

struct NextProgramRow {
    std::string item_type, item_id, block_id, file_path, title, show_title, show_id;
    int season = 0, episode = 0;
    int64_t duration_ms = 0, wall_clock_start = 0;
};

struct EpgProgramRow {
    std::string item_type, item_id, block_id, status, title, show_title, show_id, file_path, overview;
    int season = 0, episode = 0;
    int64_t wall_clock_start = 0, wall_clock_end = 0, duration_ms = 0;
};

class ScheduleRepository {
public:
    explicit ScheduleRepository(Database& db);

    std::optional<NowProgramRow>      getNowProgram(const std::string& channel_id, int64_t at_sec);
    std::optional<FillerFallbackRow>  getChannelFillerFallback(const std::string& channel_id);
    std::optional<OfflineFallbackRow> getChannelOfflineConfig(const std::string& channel_id);
    std::optional<std::string>        getAudioFilePath(const std::string& item_type,
                                                        const std::string& item_id);
    std::optional<NextProgramRow>     getNextProgram(const std::string& channel_id, int64_t now_sec);
    std::vector<EpgProgramRow>        getEpgPrograms(const std::string& channel_id,
                                                      int64_t from_sec, int64_t to_sec);
    int                               clearAllScheduled();

    void recordPlayHistory(const std::string& item_type, const std::string& item_id,
                           const std::string& channel_id, const std::string& block_id);

    void recordScheduledPlayHistory(const std::string& item_type, const std::string& item_id,
                                    const std::string& channel_id, const std::string& block_id,
                                    std::time_t aired_at);

    void recordScheduledFillerHistory(const std::string& item_id, const std::string& channel_id,
                                      const std::string& block_id, std::time_t aired_at);

    // Run gen() with a temporary block graph injected for channel_id, rolled back afterwards.
    template<typename Fn>
    auto withPreviewBlocks(const std::string& channel_id,
                           const nlohmann::json& blocks, Fn gen) -> decltype(gen()) {
        db_.get().exec("SAVEPOINT preview_sp");
        try {
            insertPreviewBlocks(channel_id, blocks);
            auto result = gen();
            db_.get().exec("ROLLBACK TO SAVEPOINT preview_sp");
            db_.get().exec("RELEASE SAVEPOINT preview_sp");
            return result;
        } catch (...) {
            try { db_.get().exec("ROLLBACK TO SAVEPOINT preview_sp"); } catch (...) {}
            try { db_.get().exec("RELEASE SAVEPOINT preview_sp"); } catch (...) {}
            throw;
        }
    }

private:
    Database& db_;

    void insertPreviewBlocks(const std::string& channel_id, const nlohmann::json& blocks);
};
