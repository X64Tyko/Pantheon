#include "ChapterDetectionManager.h"
#include "ChapterDetector.h"
#include "conf/ConfStore.h"
#include "db/ChapterRepository.h"
#include "db/ContentRepository.h"
#include "db/SourceRepository.h"
#include "source/SyncManager.h"
#include "thread/TaskRegistry.h"
#include <atomic>
#include <iostream>
#include <malloc.h> // malloc_trim — glibc extension, not declared by <cstdlib>
#include <thread>
#include <vector>

ChapterDetectionManager::ChapterDetectionManager(Database& db, ConfStore& conf, SyncManager& sync)
    : db_(db), conf_(conf), sync_(sync) {}

bool ChapterDetectionManager::triggerShowDetect(const std::string& show_id) {
    bool expected = false;
    if (!detecting_.compare_exchange_strong(expected, true)) return false;
    TaskRegistry::global().spawn([this, show_id]() {
        runShowDetect(show_id);
        detecting_.store(false);
    });
    return true;
}

bool ChapterDetectionManager::triggerMovieDetect(const std::string& movie_id) {
    bool expected = false;
    if (!detecting_.compare_exchange_strong(expected, true)) return false;
    TaskRegistry::global().spawn([this, movie_id]() {
        runMovieDetect(movie_id);
        detecting_.store(false);
    });
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

    // Workers only run ffmpeg/fpcalc and fill their own index — no DB access
    // from worker threads, same split SyncManager::syncChaptersFromFiles uses.
    // Scene cuts are needed both for ad_break (existing) and, later in this
    // function, to confirm pre_roll/recap/post_credits/next_time boundaries
    // once intro/credits anchors are known — computed once here either way.
    struct Result {
        std::vector<Chapter>  ad_breaks;
        std::vector<int64_t>  cuts;
        std::vector<uint32_t> fingerprint;
    };
    std::vector<Result> results(items.size());
    {
        std::atomic<size_t> next{0};
        const int worker_count = std::min<int>(sync_.getThreadCount(), static_cast<int>(items.size()));
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(worker_count));
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t i = next.fetch_add(1); i < items.size(); i = next.fetch_add(1)) {
                    results[i].cuts        = sceneChangeTimeline(items[i].mapped_path);
                    results[i].ad_breaks   = detectAdBreaks(items[i].mapped_path, items[i].duration_ms, "episode");
                    results[i].fingerprint = computeAudioFingerprint(items[i].mapped_path);
                }
            });
        }
        for (auto& t : workers) t.join();
    }

    // Cross-episode intro/credits alignment needs every episode's fingerprint
    // together — sequential, after the parallel per-episode work above.
    std::vector<EpisodeFingerprint> fps;
    fps.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i)
        fps.push_back({items[i].episode_id, items[i].duration_ms, results[i].fingerprint});
    auto recurring = detectRecurringSegments(fps);

    ChapterRepository repo(db_);
    int written = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        std::vector<Chapter> chapters = results[i].ad_breaks;

        std::optional<Chapter> intro, credits;
        if (auto it = recurring.find(items[i].episode_id); it != recurring.end()) {
            for (const auto& c : it->second) {
                if (c.chapter_type == "intro")   intro   = c;
                if (c.chapter_type == "credits") credits = c;
            }
            chapters.insert(chapters.end(), it->second.begin(), it->second.end());
        }
        auto boundary = deriveBoundaryChapters(intro, credits, results[i].cuts, items[i].duration_ms);
        chapters.insert(chapters.end(), boundary.begin(), boundary.end());

        if (!chapters.empty()) {
            repo.syncChapters("episode", items[i].episode_id, "detected", std::move(chapters));
            ++written;
        }
    }
    std::cout << "[detect] show " << show_id << ": detection wrote " << written
               << "/" << items.size() << " episode(s)\n";

	// Same reasoning as SyncManager::syncAll's own malloc_trim(0) call: the
	// worker pool above briefly allocates large per-episode scratch vectors
	// (scene cuts, fingerprints) that glibc's malloc won't hand back to the
	// OS on its own once freed.
	malloc_trim(0);
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
