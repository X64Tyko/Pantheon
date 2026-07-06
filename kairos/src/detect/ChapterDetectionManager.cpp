#include "ChapterDetectionManager.h"
#include "ChapterDetector.h"
#include "conf/ConfStore.h"
#include "db/ChapterRepository.h"
#include "db/ContentRepository.h"
#include "db/SourceRepository.h"
#include "source/SyncManager.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

ChapterDetectionManager::ChapterDetectionManager(Database& db, ConfStore& conf, SyncManager& sync)
    : db_(db), conf_(conf), sync_(sync) {}

bool ChapterDetectionManager::triggerShowDetect(const std::string& show_id) {
    bool expected = false;
    if (!detecting_.compare_exchange_strong(expected, true)) return false;
    std::thread([this, show_id]() {
        runShowDetect(show_id);
        detecting_.store(false);
    }).detach();
    return true;
}

bool ChapterDetectionManager::triggerMovieDetect(const std::string& movie_id) {
    bool expected = false;
    if (!detecting_.compare_exchange_strong(expected, true)) return false;
    std::thread([this, movie_id]() {
        runMovieDetect(movie_id);
        detecting_.store(false);
    }).detach();
    return true;
}

void ChapterDetectionManager::runShowDetect(const std::string& show_id) {
    auto episodes = ContentRepository(db_).listEpisodesForShow(show_id, "");
    if (episodes.empty()) return;

    // Resolve real (mapped) file paths up front — EpisodeRow::file_path is
    // display-only (see ContentRepository.h), the real path lives behind
    // source_mapping, same as the existing per-episode chapters/sync route.
    SourceRepository src_repo(db_);
    struct Item { std::string episode_id; std::string mapped_path; int64_t duration_ms; };
    std::vector<Item> items;
    for (const auto& ep : episodes) {
        auto resolved = src_repo.resolveItemSource("episode", ep.episode_id);
        if (!resolved || resolved->file_path.empty()) continue;
        items.push_back({ep.episode_id, conf_.applyPathMap(resolved->file_path), ep.duration_ms});
    }
    if (items.empty()) return;

    // Workers only run ffmpeg and fill their own index — no DB access from
    // worker threads, same split SyncManager::syncChaptersFromFiles uses.
    struct Result { std::string episode_id; std::vector<Chapter> chapters; };
    std::vector<Result> results(items.size());
    {
        std::atomic<size_t> next{0};
        const int worker_count = std::min<int>(sync_.getThreadCount(), static_cast<int>(items.size()));
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(worker_count));
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t i = next.fetch_add(1); i < items.size(); i = next.fetch_add(1)) {
                    auto chapters = detectAdBreaks(items[i].mapped_path, items[i].duration_ms, "episode");
                    if (!chapters.empty())
                        results[i] = {items[i].episode_id, std::move(chapters)};
                }
            });
        }
        for (auto& t : workers) t.join();
    }

    ChapterRepository repo(db_);
    int written = 0;
    for (auto& res : results) {
        if (!res.chapters.empty()) {
            repo.syncChapters("episode", res.episode_id, "detected", std::move(res.chapters));
            ++written;
        }
    }
    std::cout << "[detect] show " << show_id << ": ad_break detection wrote " << written
               << "/" << items.size() << " episode(s)\n";
}

void ChapterDetectionManager::runMovieDetect(const std::string& movie_id) {
    auto detail = ContentRepository(db_).getMovieDetail(movie_id);
    if (!detail) return;

    auto resolved = SourceRepository(db_).resolveItemSource("movie", movie_id);
    if (!resolved || resolved->file_path.empty()) return;
    const std::string mapped = conf_.applyPathMap(resolved->file_path);

    auto chapters = detectAdBreaks(mapped, detail->duration_ms, "movie");
    if (chapters.empty()) {
        std::cout << "[detect] movie " << movie_id << ": no ad_break points found\n";
        return;
    }
    const size_t count = chapters.size();
    ChapterRepository(db_).syncChapters("movie", movie_id, "detected", std::move(chapters));
    std::cout << "[detect] movie " << movie_id << ": ad_break detection wrote " << count << " point(s)\n";
}
