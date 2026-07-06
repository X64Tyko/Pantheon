#pragma once
#include "model/Chapter.h"
#include <cstdint>
#include <string>
#include <vector>

// Algorithmic pieces for chapter-structure detection (ad_break points for now;
// intro/credits/pre_roll/post_credits/recap land in later phases). No DB or
// threading here — see detect/ChapterDetectionManager.h for orchestration.

// Runs ffmpeg's scene-change detector over the whole file and returns cut
// timestamps (ms) above the detection threshold, in ascending order. Decodes
// video only (scaled down first for speed), no audio. Empty on ffmpeg
// failure or a file with no detected cuts.
std::vector<int64_t> sceneChangeTimeline(const std::string& file_path);

// Pure — picks one point per target_interval_ms, snapped to the nearest cut
// in cuts_ms within search_window_ms of that target. Skips an interval
// (leaves a gap) rather than placing a break at an arbitrary non-cut point.
// Never places a point inside [0, guard_start_ms) or (duration_ms -
// guard_end_ms, duration_ms]. Deduplicates cuts shared by adjacent intervals.
std::vector<int64_t> pickAdBreakPoints(const std::vector<int64_t>& cuts_ms,
                                       int64_t duration_ms,
                                       int64_t target_interval_ms,
                                       int64_t guard_start_ms,
                                       int64_t guard_end_ms,
                                       int64_t search_window_ms);

// Runs sceneChangeTimeline() + pickAdBreakPoints() with the movie/episode
// target spacing, and returns zero-length "ad_break" Chapter points
// (start_ms == end_ms), source="detected", ready for ChapterRepository::syncChapters.
std::vector<Chapter> detectAdBreaks(const std::string& file_path,
                                     int64_t duration_ms,
                                     const std::string& media_type);
