#include "ChapterDetector.h"
#include "log/DebugLog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {

// Same private-copy convention as MediaProbe.cpp/DownloadManager.cpp — this
// codebase duplicates shellQuote per shell-out module rather than sharing it.
std::string shellQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else           r += c;
    }
    return r + "'";
}

constexpr int kSceneDetectTimeoutSecs = 900; // full-file video decode, no audio — generous ceiling
constexpr double kSceneThreshold      = 0.3; // ffmpeg select='gt(scene,N)' — standard scene-cut default

constexpr int64_t kTvTargetIntervalMs    = 11 * 60 * 1000; // ~10-12 min act breaks
constexpr int64_t kMovieTargetIntervalMs = 22 * 60 * 1000; // ~20-25 min
constexpr int64_t kAdBreakGuardMs        = 3  * 60 * 1000; // don't break in the first/last 3 min
constexpr int64_t kSearchWindowMs        = 90 * 1000;      // how far from target to look for a real cut

constexpr int kFpcalcTimeoutSecs = 300;

// Empirically measured against this dev environment's chromaprint 1.6.0 (see
// ChapterDetector.h's comment on computeAudioFingerprint) — 8.1 fingerprint
// items/sec, i.e. ~123.5ms of audio per raw fingerprint uint32. Deliberately
// not adding fpcalc's ~2.7s startup-buffering intercept here: this constant
// is only ever used for a relative index->ms conversion feeding a
// human-reviewable "detected" chapter, so a fixed few-hundred-ms bias is an
// acceptable tradeoff against presenting false precision from a value that
// will itself drift slightly across chromaprint versions/builds.
constexpr double kFingerprintMsPerItem = 123.5;

// Per-item Hamming distance (out of 32 bits) below which two fingerprint
// items are considered "the same audio" — loose enough to tolerate minor
// encode differences between episodes' files, tight enough that unrelated
// content (which averages ~16/32 differing bits) essentially never sustains
// a run past kMinMatchItems by chance.
constexpr int kHammingMatchThreshold = 10;

// ~8 seconds at kFingerprintMsPerItem — shortest run treated as a genuine
// recurring segment rather than a coincidental short match.
constexpr int kMinMatchItems = 65;

// A recurring segment needs corroboration from at least this fraction of the
// other episodes it was compared against, so one fluke alignment (e.g. two
// otherwise-unrelated episodes that happen to share a stock sound effect)
// can't get treated as "the intro."
constexpr double kMinCorroborationFraction = 0.5;

int hammingDistance(uint32_t a, uint32_t b) { return __builtin_popcount(a ^ b); }

// Shortest gap worth calling pre_roll/post_credits at all.
constexpr int64_t kBoundaryMinGapMs = 5 * 1000;
// Scene cuts spaced no further apart than this count as one "montage" run —
// recap/next_time previews cut rapidly between clips, unlike normal scenes.
constexpr int64_t kMontageMaxGapMs = 2500;
constexpr int      kMontageMinCuts  = 4;

// Longest run of cuts (assumed ascending, as sceneChangeTimeline returns
// them) with no gap wider than maxGapMs between consecutive members;
// nullopt if the longest such run is shorter than minCuts.
std::optional<std::pair<int64_t, int64_t>> findDenseCutCluster(
        const std::vector<int64_t>& cuts, int64_t maxGapMs, int minCuts) {
    if (static_cast<int>(cuts.size()) < minCuts) return std::nullopt;

    size_t bestStart = 0, bestLen = 1, curStart = 0, curLen = 1;
    for (size_t i = 1; i < cuts.size(); ++i) {
        if (cuts[i] - cuts[i - 1] <= maxGapMs) {
            ++curLen;
        } else {
            if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
            curStart = i;
            curLen = 1;
        }
    }
    if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }

    if (static_cast<int>(bestLen) < minCuts) return std::nullopt;
    return std::make_pair(cuts[bestStart], cuts[bestStart + bestLen - 1]);
}

} // namespace

std::vector<int64_t> sceneChangeTimeline(const std::string& file_path) {
    const std::string cmd =
        "timeout -k 5 " + std::to_string(kSceneDetectTimeoutSecs) + " ffmpeg -i "
        + shellQuote(file_path)
        + " -vf \"scale=iw/4:ih/4,select='gt(scene," + std::to_string(kSceneThreshold)
        + ")',showinfo\" -f null - 2>&1";
    DLOG << "[detect] scene timeline cmd: " << cmd << '\n';
    const auto t0 = std::chrono::steady_clock::now();

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        DLOG << "[detect] scene timeline popen failed: " << file_path << '\n';
        return {};
    }
    std::vector<int64_t> cuts;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) {
        const std::string line(buf);
        auto pos = line.find("pts_time:");
        if (pos == std::string::npos) continue;
        pos += std::string("pts_time:").size();
        try {
            const double secs = std::stod(line.substr(pos));
            cuts.push_back(static_cast<int64_t>(secs * 1000.0));
        } catch (...) {}
    }
    pclose(pipe);

    std::sort(cuts.begin(), cuts.end());
    DLOG << "[detect] scene timeline done in " << elapsedMs(t0, std::chrono::steady_clock::now())
         << "ms → " << cuts.size() << " cut(s): " << file_path << '\n';
    return cuts;
}

std::vector<int64_t> pickAdBreakPoints(const std::vector<int64_t>& cuts_ms,
                                       int64_t duration_ms,
                                       int64_t target_interval_ms,
                                       int64_t guard_start_ms,
                                       int64_t guard_end_ms,
                                       int64_t search_window_ms) {
    std::vector<int64_t> points;
    if (target_interval_ms <= 0 || duration_ms <= guard_start_ms + guard_end_ms)
        return points;

    // Targets are spaced from the start of the content, not from the guard
    // boundary — the guard only excludes candidate cuts near the edges, it
    // doesn't shift where breaks would otherwise fall.
    const int64_t usable_end = duration_ms - guard_end_ms;
    for (int64_t target = target_interval_ms; target < usable_end; target += target_interval_ms) {
        int64_t best = -1, best_dist = search_window_ms + 1;
        for (int64_t cut : cuts_ms) {
            if (cut < guard_start_ms || cut > usable_end) continue;
            const int64_t dist = std::llabs(cut - target);
            if (dist <= search_window_ms && dist < best_dist) { best = cut; best_dist = dist; }
        }
        if (best >= 0) points.push_back(best);
    }

    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

std::vector<Chapter> detectAdBreaks(const std::string& file_path,
                                     int64_t duration_ms,
                                     const std::string& media_type) {
    const int64_t target_interval_ms = (media_type == "movie") ? kMovieTargetIntervalMs : kTvTargetIntervalMs;
    const auto cuts   = sceneChangeTimeline(file_path);
    const auto points = pickAdBreakPoints(cuts, duration_ms, target_interval_ms,
                                           kAdBreakGuardMs, kAdBreakGuardMs, kSearchWindowMs);

    std::vector<Chapter> result;
    int pos = 0;
    for (int64_t ms : points) {
        Chapter c;
        c.chapter_type = "ad_break";
        c.source       = "detected";
        c.position     = pos++;
        c.start_ms     = ms;
        c.end_ms       = ms; // zero-length point marker — the scheduler will decide the cut duration later
        result.push_back(std::move(c));
    }
    return result;
}

std::vector<uint32_t> computeAudioFingerprint(const std::string& file_path) {
    const std::string cmd =
        "timeout -k 5 " + std::to_string(kFpcalcTimeoutSecs) + " fpcalc -raw -length 0 "
        + shellQuote(file_path) + " 2>/dev/null";
    DLOG << "[detect] fingerprint cmd: " << cmd << '\n';
    const auto t0 = std::chrono::steady_clock::now();

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        DLOG << "[detect] fingerprint popen failed: " << file_path << '\n';
        return {};
    }
    std::vector<uint32_t> fp;
    char buf[65536];
    while (fgets(buf, sizeof(buf), pipe)) {
        const std::string line(buf);
        const std::string prefix = "FINGERPRINT=";
        auto pos = line.find(prefix);
        if (pos == std::string::npos) continue;
        pos += prefix.size();
        size_t start = pos;
        while (start <= line.size()) {
            auto comma = line.find(',', start);
            const std::string tok = line.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            try {
                fp.push_back(static_cast<uint32_t>(std::stoll(tok)));
            } catch (...) {}
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    pclose(pipe);

    DLOG << "[detect] fingerprint done in " << elapsedMs(t0, std::chrono::steady_clock::now())
         << "ms → " << fp.size() << " item(s): " << file_path << '\n';
    return fp;
}

std::optional<std::pair<Chapter, Chapter>>
findMatchedSegment(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() < static_cast<size_t>(kMinMatchItems) || b.size() < static_cast<size_t>(kMinMatchItems))
        return std::nullopt;

    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());

    // offset: b[i + offset] is compared against a[i]. Scored over every
    // alignment with at least kMinMatchItems of overlap.
    int bestOffset = 0, bestScore = -1;
    for (int offset = -(m - kMinMatchItems); offset <= n - kMinMatchItems; ++offset) {
        const int lo = std::max(0, -offset);
        const int hi = std::min(n, m - offset);
        if (hi - lo < kMinMatchItems) continue;
        int score = 0;
        for (int i = lo; i < hi; ++i)
            if (hammingDistance(a[static_cast<size_t>(i)], b[static_cast<size_t>(i + offset)]) <= kHammingMatchThreshold)
                ++score;
        if (score > bestScore) { bestScore = score; bestOffset = offset; }
    }
    if (bestScore < kMinMatchItems) return std::nullopt;

    // The alignment above just picks the best offset overall; the actual
    // segment boundary is the longest contiguous matching run within it.
    // Isolated near-misses (encode noise) don't break the run — only a
    // sustained stretch of mismatches does.
    constexpr int kMaxConsecutiveMisses = 3;
    const int lo = std::max(0, -bestOffset);
    const int hi = std::min(n, m - bestOffset);
    int runStart = lo, bestRunStart = -1, bestRunLen = 0, curLen = 0, missStreak = 0;
    for (int i = lo; i < hi; ++i) {
        const bool match = hammingDistance(a[static_cast<size_t>(i)], b[static_cast<size_t>(i + bestOffset)]) <= kHammingMatchThreshold;
        if (match) {
            if (curLen == 0) runStart = i;
            ++curLen;
            missStreak = 0;
        } else if (curLen > 0 && missStreak < kMaxConsecutiveMisses) {
            ++curLen;
            ++missStreak;
        } else {
            curLen = 0;
            missStreak = 0;
        }
        if (curLen > bestRunLen) { bestRunLen = curLen; bestRunStart = runStart; }
    }
    if (bestRunLen < kMinMatchItems || bestRunStart < 0) return std::nullopt;

    Chapter ca, cb;
    ca.start_ms = static_cast<int64_t>(bestRunStart * kFingerprintMsPerItem);
    ca.end_ms   = static_cast<int64_t>((bestRunStart + bestRunLen) * kFingerprintMsPerItem);
    cb.start_ms = static_cast<int64_t>((bestRunStart + bestOffset) * kFingerprintMsPerItem);
    cb.end_ms   = static_cast<int64_t>((bestRunStart + bestRunLen + bestOffset) * kFingerprintMsPerItem);
    return std::make_pair(ca, cb);
}

std::unordered_map<std::string, std::vector<Chapter>>
detectRecurringSegments(const std::vector<EpisodeFingerprint>& episodes) {
    std::unordered_map<std::string, std::vector<Chapter>> result;
    if (episodes.size() < 2) return result;

    const EpisodeFingerprint* ref = nullptr;
    for (const auto& ep : episodes) {
        if (ep.fingerprint.size() >= static_cast<size_t>(kMinMatchItems)) { ref = &ep; break; }
    }
    if (!ref) return result;

    struct PerEpisode { std::string episode_id; std::optional<Chapter> early, late; };
    std::vector<PerEpisode> perEp;
    std::vector<Chapter> refEarlySpans, refLateSpans; // ref's own side of each corroborating match
    int comparedCount = 0;

    const int64_t refThirdA = ref->duration_ms / 3;
    const int64_t refThirdB = ref->duration_ms * 2 / 3;

    for (const auto& ep : episodes) {
        if (&ep == ref) continue;
        if (ep.fingerprint.size() < static_cast<size_t>(kMinMatchItems)) continue;
        ++comparedCount;

        // Searched as two independent halves, not one whole-track match: a
        // real episode can share BOTH an intro near its start and a credits
        // sequence near its end, but findMatchedSegment only ever reports
        // its single longest run — searching the whole track at once would
        // silently drop whichever of the two is shorter.
        const size_t refMid = ref->fingerprint.size() / 2;
        const size_t epMid  = ep.fingerprint.size() / 2;
        const std::vector<uint32_t> refEarly(ref->fingerprint.begin(), ref->fingerprint.begin() + static_cast<long>(refMid));
        const std::vector<uint32_t> epEarly(ep.fingerprint.begin(), ep.fingerprint.begin() + static_cast<long>(epMid));
        const std::vector<uint32_t> refLate(ref->fingerprint.begin() + static_cast<long>(refMid), ref->fingerprint.end());
        const std::vector<uint32_t> epLate(ep.fingerprint.begin() + static_cast<long>(epMid), ep.fingerprint.end());
        const int64_t refMidMs = static_cast<int64_t>(refMid * kFingerprintMsPerItem);
        const int64_t epMidMs  = static_cast<int64_t>(epMid  * kFingerprintMsPerItem);

        PerEpisode pe{ep.episode_id, std::nullopt, std::nullopt};

        if (auto early = findMatchedSegment(refEarly, epEarly)) {
            auto& [refSeg, epSeg] = *early;
            if (refSeg.start_ms < refThirdA) { // still gate on "genuinely early," not just "in the first half"
                refEarlySpans.push_back(refSeg);
                pe.early = epSeg;
            }
        }
        if (auto late = findMatchedSegment(refLate, epLate)) {
            auto& [refSeg, epSeg] = *late;
            Chapter refShifted = refSeg, epShifted = epSeg;
            refShifted.start_ms += refMidMs; refShifted.end_ms += refMidMs;
            epShifted.start_ms  += epMidMs;  epShifted.end_ms  += epMidMs;
            if (refShifted.start_ms >= refThirdB) { // gate on "genuinely late," not just "in the second half"
                refLateSpans.push_back(refShifted);
                pe.late = epShifted;
            }
        }
        // A match found in neither half's genuinely-early/late zone isn't
        // intro or credits (some other coincidentally-shared audio, e.g. a
        // recurring sound sting) — not classified, not written anywhere.
        if (pe.early || pe.late) perEp.push_back(std::move(pe));
    }
    if (comparedCount == 0) return result;

    const int minCorroboration = std::max(1, static_cast<int>(std::ceil(comparedCount * kMinCorroborationFraction)));
    const bool introConfirmed   = static_cast<int>(refEarlySpans.size()) >= minCorroboration;
    const bool creditsConfirmed = static_cast<int>(refLateSpans.size())  >= minCorroboration;
    if (!introConfirmed && !creditsConfirmed) return result;

    // Robust-to-outliers "typical" span from a set of corroborating matches.
    auto medianSpan = [](std::vector<Chapter> spans) -> std::optional<Chapter> {
        if (spans.empty()) return std::nullopt;
        std::sort(spans.begin(), spans.end(), [](const Chapter& x, const Chapter& y) { return x.start_ms < y.start_ms; });
        return spans[spans.size() / 2];
    };

    auto pushChapters = [](std::vector<Chapter>& chs, std::optional<Chapter> intro, std::optional<Chapter> credits) {
        int pos = 0;
        if (intro)   { intro->chapter_type   = "intro";   intro->source   = "detected"; intro->position   = pos++; chs.push_back(*intro); }
        if (credits) { credits->chapter_type = "credits"; credits->source = "detected"; credits->position = pos++; chs.push_back(*credits); }
    };

    if (introConfirmed || creditsConfirmed) {
        std::vector<Chapter> refChapters;
        pushChapters(refChapters, introConfirmed ? medianSpan(refEarlySpans) : std::nullopt,
                                   creditsConfirmed ? medianSpan(refLateSpans) : std::nullopt);
        if (!refChapters.empty()) result[ref->episode_id] = std::move(refChapters);
    }

    for (auto& pe : perEp) {
        std::vector<Chapter> chs;
        pushChapters(chs, introConfirmed ? pe.early : std::nullopt, creditsConfirmed ? pe.late : std::nullopt);
        if (!chs.empty()) result[pe.episode_id] = std::move(chs);
    }
    return result;
}

std::vector<Chapter> deriveBoundaryChapters(std::optional<Chapter> intro,
                                             std::optional<Chapter> credits,
                                             const std::vector<int64_t>& cuts,
                                             int64_t duration_ms) {
    std::vector<Chapter> extra;
    int pos = 0;

    auto cutsInRange = [&](int64_t lo, int64_t hi) {
        std::vector<int64_t> in;
        for (int64_t c : cuts) if (c >= lo && c < hi) in.push_back(c);
        return in;
    };

    if (intro && intro->start_ms >= kBoundaryMinGapMs) {
        auto before = cutsInRange(0, intro->start_ms);
        if (!before.empty()) {
            Chapter c; c.chapter_type = "pre_roll"; c.source = "detected"; c.position = pos++;
            c.start_ms = 0; c.end_ms = intro->start_ms;
            extra.push_back(c);
        }
        if (auto cluster = findDenseCutCluster(before, kMontageMaxGapMs, kMontageMinCuts)) {
            Chapter c; c.chapter_type = "recap"; c.source = "detected"; c.position = pos++;
            c.start_ms = cluster->first; c.end_ms = cluster->second;
            extra.push_back(c);
        }
    }

    if (credits && credits->end_ms <= duration_ms - kBoundaryMinGapMs) {
        auto after = cutsInRange(credits->end_ms, duration_ms);
        if (!after.empty()) {
            Chapter c; c.chapter_type = "post_credits"; c.source = "detected"; c.position = pos++;
            c.start_ms = credits->end_ms; c.end_ms = duration_ms;
            extra.push_back(c);
        }
        if (auto cluster = findDenseCutCluster(after, kMontageMaxGapMs, kMontageMinCuts)) {
            Chapter c; c.chapter_type = "next_time"; c.source = "detected"; c.position = pos++;
            c.start_ms = cluster->first; c.end_ms = cluster->second;
            extra.push_back(c);
        }
    }
    return extra;
}
