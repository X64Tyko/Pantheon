#include <gtest/gtest.h>
#include "detect/ChapterDetector.h"
#include <vector>

// pickAdBreakPoints() is pure — no ffmpeg, no DB — so these exercise it
// directly against synthetic scene-cut timelines.

namespace {
constexpr int64_t kMin  = 60'000;
constexpr int64_t kGuard = 3 * kMin;
constexpr int64_t kWindow = 90'000;
}

TEST(PickAdBreakPoints, EmptyCutsProducesNoPoints) {
    auto points = pickAdBreakPoints({}, 30 * kMin, 10 * kMin, kGuard, kGuard, kWindow);
    EXPECT_TRUE(points.empty());
}

TEST(PickAdBreakPoints, SnapsToNearestCutWithinWindow) {
    // Target at 10min; real cut sits 20s later, well inside the 90s window.
    std::vector<int64_t> cuts = {10 * kMin + 20'000};
    auto points = pickAdBreakPoints(cuts, 30 * kMin, 10 * kMin, kGuard, kGuard, kWindow);
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0], 10 * kMin + 20'000);
}

TEST(PickAdBreakPoints, SkipsIntervalWithNoCutInWindow) {
    // Only cut available is 5 minutes away from the 10min target — outside the 90s window.
    std::vector<int64_t> cuts = {15 * kMin};
    auto points = pickAdBreakPoints(cuts, 30 * kMin, 10 * kMin, kGuard, kGuard, kWindow);
    EXPECT_TRUE(points.empty()) << "should leave a gap rather than force a distant cut";
}

TEST(PickAdBreakPoints, IgnoresCutsInsideStartGuard) {
    // First target sits exactly at the guard boundary; the only nearby cut is
    // just inside the guard zone (well within the search window) and must
    // still never be chosen.
    std::vector<int64_t> cuts = {kGuard - 1000};
    auto points = pickAdBreakPoints(cuts, 30 * kMin, kGuard, kGuard, kGuard, kWindow);
    EXPECT_TRUE(points.empty());
}

TEST(PickAdBreakPoints, IgnoresCutsInsideEndGuard) {
    const int64_t duration         = 30 * kMin;
    const int64_t usable_end       = duration - kGuard;
    const int64_t target_interval  = usable_end - kMin; // single target, 1 min inside usable_end
    const int64_t cut              = usable_end + 500;  // just inside the end-guard band, but close to the target
    auto points = pickAdBreakPoints({cut}, duration, target_interval, kGuard, kGuard, kWindow);
    EXPECT_TRUE(points.empty());
}

TEST(PickAdBreakPoints, DedupesAdjacentIntervalsSnappingToSameCut) {
    // A single cut sits almost exactly between two target intervals (10min, 20min) —
    // both could snap to it if the window is wide enough; result must not duplicate.
    std::vector<int64_t> cuts = {15 * kMin};
    auto points = pickAdBreakPoints(cuts, 40 * kMin, 10 * kMin, kGuard, kGuard, 6 * kMin);
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0], 15 * kMin);
}

TEST(PickAdBreakPoints, MultipleEvenlySpacedCutsProduceOrderedPoints) {
    const int64_t duration = 40 * kMin;
    std::vector<int64_t> cuts = {10 * kMin, 20 * kMin, 30 * kMin};
    auto points = pickAdBreakPoints(cuts, duration, 10 * kMin, kGuard, kGuard, kWindow);
    ASSERT_EQ(points.size(), 3u);
    EXPECT_EQ(points[0], 10 * kMin);
    EXPECT_EQ(points[1], 20 * kMin);
    EXPECT_EQ(points[2], 30 * kMin);
}

TEST(PickAdBreakPoints, NonPositiveIntervalReturnsEmpty) {
    std::vector<int64_t> cuts = {10 * kMin};
    EXPECT_TRUE(pickAdBreakPoints(cuts, 30 * kMin, 0, kGuard, kGuard, kWindow).empty());
    EXPECT_TRUE(pickAdBreakPoints(cuts, 30 * kMin, -1, kGuard, kGuard, kWindow).empty());
}

TEST(PickAdBreakPoints, DurationTooShortForGuardsReturnsEmpty) {
    std::vector<int64_t> cuts = {2 * kMin};
    auto points = pickAdBreakPoints(cuts, kGuard, 10 * kMin, kGuard, kGuard, kWindow);
    EXPECT_TRUE(points.empty());
}

// ── findMatchedSegment / detectRecurringSegments / deriveBoundaryChapters ───
//
// All three are pure (no ffmpeg/fpcalc/DB) — exercised directly against
// synthetic fingerprints and scene-cut timelines, same spirit as the
// pickAdBreakPoints tests above.
//
// kApproxMsPerItem mirrors ChapterDetector.cpp's private kFingerprintMsPerItem
// (empirically measured from fpcalc's output rate — see that constant's
// comment) closely enough for tolerant assertions below; it isn't expected to
// be exact, and tests use a generous tolerance for exactly that reason.
namespace {
constexpr double kApproxMsPerItem = 123.5;
// Mirrors ChapterDetector.cpp's private kBoundaryMinGapMs/kMontageMaxGapMs/kMontageMinCuts.
constexpr int64_t kApproxBoundaryMinGapMs = 5000;
constexpr int64_t kApproxMontageMaxGapMs  = 2500;

// Deterministic "unrelated audio" filler — different seedOffsets produce
// fingerprints with high (~random) Hamming distance from one another at any
// given index, same as genuinely different content would. A plain linear
// hash (e.g. i*C) isn't good enough here: b[i]-a[i] stays constant across i
// for two different seedOffsets, and bit-avalanche under XOR is poor enough
// that whole stretches can accidentally land under the match threshold. This
// is splitmix32's finalizer, which avalanches properly.
uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
uint32_t fillerHash(int i, int seedOffset) {
    return mix32(static_cast<uint32_t>(i) * 0x9E3779B1u + static_cast<uint32_t>(seedOffset) * 0x85EBCA6Bu);
}

// A totalLen-item fingerprint of filler, with a distinctive, exactly
// reproducible "signature" (standing in for a shared intro/credits cue)
// written over [sigStart, sigStart+sigLen).
std::vector<uint32_t> buildTrack(int totalLen, int sigStart, int sigLen, int seedOffset) {
    std::vector<uint32_t> v(static_cast<size_t>(totalLen));
    for (int i = 0; i < totalLen; ++i) v[static_cast<size_t>(i)] = fillerHash(i, seedOffset);
    for (int k = 0; k < sigLen; ++k)
        v[static_cast<size_t>(sigStart + k)] = 0xABCD0000u + static_cast<uint32_t>(k);
    return v;
}
} // namespace

TEST(FindMatchedSegment, FindsIdenticalSignatureAtSameOffset) {
    auto a = buildTrack(500, 50, 100, 0);
    auto b = buildTrack(500, 50, 100, 10'000); // different filler, same signature+offset
    auto result = findMatchedSegment(a, b);
    ASSERT_TRUE(result.has_value());
    const auto& [segA, segB] = *result;
    EXPECT_NEAR(static_cast<double>(segA.start_ms), 50 * kApproxMsPerItem, 5 * kApproxMsPerItem);
    EXPECT_NEAR(static_cast<double>(segA.end_ms),   150 * kApproxMsPerItem, 5 * kApproxMsPerItem);
    EXPECT_NEAR(static_cast<double>(segB.start_ms), 50 * kApproxMsPerItem, 5 * kApproxMsPerItem);
    EXPECT_NEAR(static_cast<double>(segB.end_ms),   150 * kApproxMsPerItem, 5 * kApproxMsPerItem);
}

TEST(FindMatchedSegment, FindsSignatureAtDifferentOffsetsPerTrack) {
    // Same shared signature, but track b has it 30 items later than track a
    // (e.g. b's file has some extra lead-in a's doesn't).
    auto a = buildTrack(500, 50, 100, 0);
    auto b = buildTrack(500, 80, 100, 20'000);
    auto result = findMatchedSegment(a, b);
    ASSERT_TRUE(result.has_value());
    const auto& [segA, segB] = *result;
    EXPECT_NEAR(static_cast<double>(segA.start_ms), 50 * kApproxMsPerItem, 5 * kApproxMsPerItem);
    EXPECT_NEAR(static_cast<double>(segB.start_ms), 80 * kApproxMsPerItem, 5 * kApproxMsPerItem);
}

TEST(FindMatchedSegment, NoSharedSignatureReturnsNullopt) {
    // a has a real signature; b is pure filler (sigLen=0) — nothing in
    // common between them anywhere.
    auto a = buildTrack(500, 50, 100, 0);
    auto b = buildTrack(500, 50, 0, 99'999);
    EXPECT_FALSE(findMatchedSegment(a, b).has_value());
}

TEST(FindMatchedSegment, TooShortToBeGenuineReturnsNullopt) {
    // A shared run of only 10 items is far below any plausible "this is a
    // real recurring intro/credits cue" length.
    auto a = buildTrack(200, 50, 10, 0);
    auto b = buildTrack(200, 50, 10, 5'000);
    EXPECT_FALSE(findMatchedSegment(a, b).has_value());
}

TEST(DetectRecurringSegments, ClassifiesSharedEarlyAndLateSegmentsAcrossEpisodes) {
    // 1000 items * ~123.5ms/item ≈ 123.5s runtime — first/last thirds split
    // around ~41.2s (idx ~333) / ~82.3s (idx ~666). Intro signature sits at
    // idx [20,100) (well inside the first third); credits at idx [900,980)
    // (well inside the last third).
    const int64_t durationMs = static_cast<int64_t>(1000 * kApproxMsPerItem);
    std::vector<EpisodeFingerprint> eps;
    for (int e = 0; e < 4; ++e) {
        auto fp = buildTrack(1000, 20, 80, e * 100'000); // distinct filler per episode
        for (int k = 0; k < 80; ++k) fp[static_cast<size_t>(900 + k)] = 0xC0FFEE00u + static_cast<uint32_t>(k);
        eps.push_back({"ep" + std::to_string(e), durationMs, std::move(fp)});
    }

    auto result = detectRecurringSegments(eps);
    ASSERT_EQ(result.size(), 4u) << "every episode (incl. the reference) should get chapters";
    for (const auto& ep : eps) {
        auto it = result.find(ep.episode_id);
        ASSERT_NE(it, result.end()) << ep.episode_id;
        bool hasIntro = false, hasCredits = false;
        for (const auto& c : it->second) {
            if (c.chapter_type == "intro")   { hasIntro = true;   EXPECT_NEAR(static_cast<double>(c.start_ms), 20 * kApproxMsPerItem, 8 * kApproxMsPerItem); }
            if (c.chapter_type == "credits") { hasCredits = true; EXPECT_NEAR(static_cast<double>(c.start_ms), 900 * kApproxMsPerItem, 8 * kApproxMsPerItem); }
            EXPECT_EQ(c.source, "detected");
        }
        EXPECT_TRUE(hasIntro)   << ep.episode_id;
        EXPECT_TRUE(hasCredits) << ep.episode_id;
    }
}

TEST(DetectRecurringSegments, SingleOtherEpisodeIsEnoughCorroboration) {
    const int64_t durationMs = static_cast<int64_t>(1000 * kApproxMsPerItem);
    auto refFp   = buildTrack(1000, 20, 80, 0);
    auto otherFp = buildTrack(1000, 20, 80, 500'000);
    std::vector<EpisodeFingerprint> eps = {
        {"ref",   durationMs, refFp},
        {"other", durationMs, otherFp},
    };
    auto result = detectRecurringSegments(eps);
    EXPECT_EQ(result.size(), 2u);
}

TEST(DetectRecurringSegments, FewerThanTwoEpisodesReturnsEmpty) {
    std::vector<EpisodeFingerprint> eps = {{"only", 100'000, buildTrack(500, 20, 80, 0)}};
    EXPECT_TRUE(detectRecurringSegments(eps).empty());
}

TEST(DetectRecurringSegments, NoSharedContentAcrossEpisodesReturnsEmpty) {
    const int64_t durationMs = static_cast<int64_t>(1000 * kApproxMsPerItem);
    std::vector<EpisodeFingerprint> eps = {
        {"a", durationMs, buildTrack(1000, 20, 0, 0)},   // sigLen=0 -> pure filler, nothing to share
        {"b", durationMs, buildTrack(1000, 20, 0, 777)},
    };
    EXPECT_TRUE(detectRecurringSegments(eps).empty());
}

TEST(DeriveBoundaryChapters, NoAnchorsProducesNothing) {
    EXPECT_TRUE(deriveBoundaryChapters(std::nullopt, std::nullopt, {1000, 2000}, 100'000).empty());
}

TEST(DeriveBoundaryChapters, PreRollRequiresSceneActivityBeforeIntro) {
    Chapter intro; intro.chapter_type = "intro"; intro.start_ms = 20'000; intro.end_ms = 40'000;

    auto withCuts = deriveBoundaryChapters(intro, std::nullopt, {5'000, 12'000}, 100'000);
    ASSERT_EQ(withCuts.size(), 1u);
    EXPECT_EQ(withCuts[0].chapter_type, "pre_roll");
    EXPECT_EQ(withCuts[0].start_ms, 0);
    EXPECT_EQ(withCuts[0].end_ms, 20'000);

    auto withoutCuts = deriveBoundaryChapters(intro, std::nullopt, {}, 100'000);
    EXPECT_TRUE(withoutCuts.empty()) << "no scene activity before intro — probably just silence, not real content";
}

TEST(DeriveBoundaryChapters, SkipsPreRollWhenIntroStartsTooEarly) {
    Chapter intro; intro.chapter_type = "intro"; intro.start_ms = kApproxBoundaryMinGapMs - 1; intro.end_ms = 20'000;
    auto result = deriveBoundaryChapters(intro, std::nullopt, {1'000}, 100'000);
    EXPECT_TRUE(result.empty());
}

TEST(DeriveBoundaryChapters, PostCreditsRequiresRealRuntimeAndSceneActivityAfterCredits) {
    Chapter credits; credits.chapter_type = "credits"; credits.start_ms = 80'000; credits.end_ms = 90'000;
    auto result = deriveBoundaryChapters(std::nullopt, credits, {95'000}, 100'000);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].chapter_type, "post_credits");
    EXPECT_EQ(result[0].start_ms, 90'000);
    EXPECT_EQ(result[0].end_ms, 100'000);

    // Credits run right up to (within the min-gap of) the file's end — no
    // post_credits worth detecting regardless of any cuts reported after it.
    Chapter creditsAtEnd; creditsAtEnd.chapter_type = "credits";
    creditsAtEnd.start_ms = 80'000; creditsAtEnd.end_ms = 100'000 - kApproxBoundaryMinGapMs + 1;
    EXPECT_TRUE(deriveBoundaryChapters(std::nullopt, creditsAtEnd, {99'000}, 100'000).empty());
}

TEST(DeriveBoundaryChapters, FindsRecapMontageBeforeIntro) {
    Chapter intro; intro.chapter_type = "intro"; intro.start_ms = 30'000; intro.end_ms = 50'000;
    // A dense cluster of cuts (montage-like) plus one lone unrelated cut far earlier.
    std::vector<int64_t> cuts = {1'000, 10'000, 11'000, 12'500, 14'000};
    auto result = deriveBoundaryChapters(intro, std::nullopt, cuts, 100'000);
    bool foundRecap = false;
    for (const auto& c : result) {
        if (c.chapter_type == "recap") {
            foundRecap = true;
            EXPECT_EQ(c.start_ms, 10'000);
            EXPECT_EQ(c.end_ms, 14'000);
        }
    }
    EXPECT_TRUE(foundRecap);
}

TEST(DeriveBoundaryChapters, FindsNextTimeMontageAfterCredits) {
    Chapter credits; credits.chapter_type = "credits"; credits.start_ms = 80'000; credits.end_ms = 90'000;
    std::vector<int64_t> cuts = {91'000, 92'200, 93'400, 94'600};
    auto result = deriveBoundaryChapters(std::nullopt, credits, cuts, 100'000);
    bool foundNextTime = false;
    for (const auto& c : result) {
        if (c.chapter_type == "next_time") {
            foundNextTime = true;
            EXPECT_EQ(c.start_ms, 91'000);
            EXPECT_EQ(c.end_ms, 94'600);
        }
    }
    EXPECT_TRUE(foundNextTime);
}
