#pragma once
#include "model/Chapter.h"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Algorithmic pieces for chapter-structure detection. No DB or threading
// here — see detect/ChapterDetectionManager.h for orchestration.

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

// ── Cross-episode audio fingerprinting (intro/credits) ─────────────────────
//
// Same technique third-party "intro skipper" tools use: chromaprint's fpcalc
// emits one ~32-bit fingerprint integer per ~123.5ms of audio (empirically
// measured against the chromaprint build this was developed against — see
// kFingerprintMsPerItem's comment if detected timestamps drift once this
// ships against a different chromaprint version). A show's intro/credits
// music plays back essentially bit-identical across episodes, so aligning
// two episodes' fingerprints and looking for a long run of near-identical
// (low Hamming distance) items finds it — random unrelated audio content
// averages ~16/32 differing bits per item, nowhere near a sustained run
// below the match threshold.
//
// Runs `fpcalc -raw -length 0` (shelled out, same convention as ffmpeg/
// ffprobe elsewhere in this file) over the whole file's audio. Empty on
// fpcalc failure/missing binary.
std::vector<uint32_t> computeAudioFingerprint(const std::string& file_path);

// One episode's precomputed fingerprint, keyed for detectRecurringSegments.
struct EpisodeFingerprint {
    std::string episode_id;
    int64_t     duration_ms = 0;
    std::vector<uint32_t> fingerprint;
};

// Finds the best-aligned matching run between two fingerprints (the same
// audio content at some possibly-different offset in each — e.g. a shared
// intro or credits sequence) and returns each side's [start_ms,end_ms) span
// in its own file's time coordinates. nullopt if no run at least
// kMinMatchItems long clears the per-item Hamming-distance threshold at any
// alignment offset (i.e., these two episodes don't appear to share a
// recurring segment at all).
std::optional<std::pair<Chapter, Chapter>>
findMatchedSegment(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b);

// Cross-episode detection entry point (orchestrated per-show by
// ChapterDetectionManager, which resolves file paths and computes each
// episode's fingerprint in its existing worker pool before calling this).
// Aligns every episode against a reference episode (the first with a
// non-empty fingerprint), keeps only matches corroborated by at least half
// of the other episodes compared (guards against one coincidental match),
// and classifies each kept match by its position in the episode: within the
// first third of runtime -> "intro", within the last third -> "credits".
// Returns episode_id -> detected Chapters (source="detected", never locked —
// same review-before-trust convention as detectAdBreaks; "unclassified"
// file-embedded chapters go through the same convention already). Episodes
// with no individually-confirmed match contribute no chapters, even if the
// group's intro/credits overall got confirmed — this stays conservative
// rather than blanket-copying the reference episode's timing onto them.
std::unordered_map<std::string, std::vector<Chapter>>
detectRecurringSegments(const std::vector<EpisodeFingerprint>& episodes);

// Boundary chapters derivable once one episode's own intro/credits anchors
// (from detectRecurringSegments, nullopt if not found for it) and its own
// sceneChangeTimeline() cuts are known:
//   - "pre_roll"      for [0, intro.start_ms)        if that gap exceeds a
//     few seconds AND has scene-cut activity in it (confirms it's real
//     content — a network ID card, cold open, etc — not just silence).
//   - "post_credits"  for (credits.end_ms, duration_ms], same reasoning.
//   - "recap"/"next_time" — a dense cluster of scene cuts (montage-like,
//     i.e. unusually short gaps between cuts) found within the pre_roll or
//     post_credits window respectively. Lower-confidence than the
//     fingerprint-matched intro/credits themselves (per-episode-unique
//     content can't be cross-episode-matched the same way) — same
//     source="detected", never-auto-trusted treatment applies.
// Pass this episode's own cuts (sceneChangeTimeline(this_episode_file_path)),
// not the reference episode's.
std::vector<Chapter> deriveBoundaryChapters(std::optional<Chapter> intro,
                                             std::optional<Chapter> credits,
                                             const std::vector<int64_t>& cuts,
                                             int64_t duration_ms);
