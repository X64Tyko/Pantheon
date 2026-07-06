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
