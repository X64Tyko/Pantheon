#include <gtest/gtest.h>
#include "scheduler/RuleEngine.h"
#include "model/Episode.h"
#include <vector>

// ---------------------------------------------------------------------------
// RuleEngine::advanceNatural — the watermark+ahead algorithm that replaced a
// raw episode-list index for natural-order (Sequential / Rerun / Timeslot)
// show advancement. A raw index desyncs the moment the episode list gets
// resorted or regrown (e.g. a library scan backfilling a previously-missing
// episode) — these tests pin the specific backfill scenario that motivated
// the rewrite, plus the surrounding edge cases (wraparound, single episode,
// removed/unknown watermark).
// ---------------------------------------------------------------------------

namespace {

Episode makeEp(const std::string& id, int season, int episode) {
    Episode e;
    e.episode_id = id;
    e.season     = season;
    e.episode    = episode;
    return e;
}

} // namespace

TEST(AdvanceNaturalTest, EmptyCatalogReturnsInvalidIndex) {
    auto adv = RuleEngine::advanceNatural("", {}, {});
    EXPECT_EQ(adv.index, -1);
}

TEST(AdvanceNaturalTest, FirstEverPickStartsAtIndexZero) {
    std::vector<Episode> eps{ makeEp("e1", 1, 1), makeEp("e2", 1, 2), makeEp("e3", 1, 3) };
    auto adv = RuleEngine::advanceNatural("", {}, eps);
    EXPECT_EQ(adv.index, 0);
    EXPECT_EQ(adv.new_watermark_id, "e1");
    EXPECT_TRUE(adv.new_ahead.empty());
}

TEST(AdvanceNaturalTest, ContiguousAdvanceMovesWatermarkForward) {
    std::vector<Episode> eps{ makeEp("e1", 1, 1), makeEp("e2", 1, 2), makeEp("e3", 1, 3) };
    auto adv = RuleEngine::advanceNatural("e1", {}, eps);
    EXPECT_EQ(adv.index, 1);
    EXPECT_EQ(adv.new_watermark_id, "e2");
    EXPECT_TRUE(adv.new_ahead.empty());
}

TEST(AdvanceNaturalTest, WrapFromLastToFirstIsNotTreatedAsOutOfOrder) {
    std::vector<Episode> eps{ makeEp("e1", 1, 1), makeEp("e2", 1, 2), makeEp("e3", 1, 3) };
    auto adv = RuleEngine::advanceNatural("e3", {}, eps);
    EXPECT_EQ(adv.index, 0);
    EXPECT_EQ(adv.new_watermark_id, "e1")
        << "starting a new rerun cycle must not be flagged as a gap";
    EXPECT_TRUE(adv.new_ahead.empty());
}

TEST(AdvanceNaturalTest, SingleEpisodeCatalogAlwaysReplaysItself) {
    std::vector<Episode> eps{ makeEp("e1", 1, 1) };
    auto first = RuleEngine::advanceNatural("", {}, eps);
    EXPECT_EQ(first.index, 0);
    auto second = RuleEngine::advanceNatural(first.new_watermark_id, first.new_ahead, eps);
    EXPECT_EQ(second.index, 0);
    EXPECT_TRUE(second.new_ahead.empty())
        << "a single-episode catalog must never accumulate ahead entries";
}

TEST(AdvanceNaturalTest, WatermarkNotFoundInCatalogRestartsFromBeginning) {
    // The watermark episode was removed from the catalog since it was set —
    // best-effort: restart from the top rather than getting stuck.
    std::vector<Episode> eps{ makeEp("e1", 1, 1), makeEp("e2", 1, 2) };
    auto adv = RuleEngine::advanceNatural("deleted-episode", {}, eps);
    EXPECT_EQ(adv.index, 0);
    EXPECT_EQ(adv.new_watermark_id, "e1");
}

// ---------------------------------------------------------------------------
// The exact backfill scenario: episode 1 airs, then episode 3 backfills into
// the library before episode 2 does (e.g. a scraper finds S1E3 before S1E2),
// then episode 2 finally backfills too. Expected: E1, E3, E2 — each exactly
// once, in the order they actually became available — and the watermark/ahead
// state fully self-heals (empty ahead) once the gap is filled, instead of
// needing a full wraparound cycle to reach the skipped episode.
// ---------------------------------------------------------------------------

TEST(AdvanceNaturalTest, BackfilledEpisodeOutOfOrder_ThenCatchesUp) {
    Episode e1 = makeEp("e1", 1, 1);
    Episode e2 = makeEp("e2", 1, 2);
    Episode e3 = makeEp("e3", 1, 3);

    // Step 1: catalog = {e1}. First-ever pick.
    auto step1 = RuleEngine::advanceNatural("", {}, {e1});
    ASSERT_EQ(step1.index, 0);
    EXPECT_EQ(step1.new_watermark_id, "e1");
    EXPECT_TRUE(step1.new_ahead.empty());

    // Step 2: e3 backfills before e2. catalog = {e1, e3}. Natural successor
    // of e1 (by season/episode numbering) is e2, which isn't in the catalog
    // yet — e3 plays instead, out of order, and gets remembered in `ahead`;
    // the watermark itself does NOT move past it.
    std::vector<Episode> catalog2{ e1, e3 };
    auto step2 = RuleEngine::advanceNatural(step1.new_watermark_id, step1.new_ahead, catalog2);
    ASSERT_EQ(step2.index, 1);
    EXPECT_EQ(catalog2[step2.index].episode_id, "e3");
    EXPECT_EQ(step2.new_watermark_id, "e1") << "watermark must not advance past a genuine gap";
    ASSERT_EQ(step2.new_ahead.size(), 1u);
    EXPECT_EQ(step2.new_ahead[0], "e3");

    // Step 3: e2 backfills. catalog = {e1, e2, e3}. The true next episode
    // after e1 (e2) is now available and not itself in `ahead` — it plays
    // next, and the watermark immediately absorbs the already-aired e3
    // sitting right behind it, since e3 is contiguous with e2. Ahead empties
    // out completely — fully caught up, nothing skipped, nothing replayed.
    std::vector<Episode> catalog3{ e1, e2, e3 };
    auto step3 = RuleEngine::advanceNatural(step2.new_watermark_id, step2.new_ahead, catalog3);
    ASSERT_EQ(step3.index, 1);
    EXPECT_EQ(catalog3[step3.index].episode_id, "e2");
    EXPECT_EQ(step3.new_watermark_id, "e3")
        << "having played e2, the watermark should absorb the already-aired e3 right behind it";
    EXPECT_TRUE(step3.new_ahead.empty()) << "fully caught up — nothing left pending";

    // Step 4: next cycle starts cleanly at e1, no anomalies remain.
    auto step4 = RuleEngine::advanceNatural(step3.new_watermark_id, step3.new_ahead, catalog3);
    EXPECT_EQ(catalog3[step4.index].episode_id, "e1");
    EXPECT_TRUE(step4.new_ahead.empty());
}

TEST(AdvanceNaturalTest, SeasonBoundaryIsNeverTreatedAsAGap) {
    // season*K+episode is not a valid linear ordinal across seasons — without
    // absolute_index there's no way to know a season is "done" from the
    // numbers alone, so S1E3 -> S2E1 must be treated as a natural, contiguous
    // transition, not a gap. Regression for a real bug: treating every season
    // boundary as a gap made a multi-season show's watermark get permanently
    // stuck at the last episode of season 1, endlessly re-flagging every
    // season-2+ episode as "out of order" and eventually replaying season 1.
    std::vector<Episode> eps{
        makeEp("s1e1", 1, 1), makeEp("s1e2", 1, 2), makeEp("s1e3", 1, 3),
        makeEp("s2e1", 2, 1), makeEp("s2e2", 2, 2), makeEp("s2e3", 2, 3),
    };

    std::string watermark;
    std::vector<std::string> ahead;
    std::vector<std::string> played_order;
    for (int i = 0; i < 6; ++i) {
        auto adv = RuleEngine::advanceNatural(watermark, ahead, eps);
        ASSERT_GE(adv.index, 0);
        played_order.push_back(eps[adv.index].episode_id);
        watermark = adv.new_watermark_id;
        ahead     = adv.new_ahead;
        EXPECT_TRUE(ahead.empty())
            << "no genuine gap exists in this catalog — ahead must stay empty at step " << i;
    }
    EXPECT_EQ(played_order, (std::vector<std::string>{
        "s1e1", "s1e2", "s1e3", "s2e1", "s2e2", "s2e3"
    })) << "must play straight through both seasons in order, with no stall or replay";
}

TEST(AdvanceNaturalTest, AbsoluteIndexOverridesSeasonEpisodeForGapDetection) {
    // When absolute_index is available (e.g. from TVDB), it's authoritative —
    // used here to detect a real gap that spans a season boundary.
    Episode s1e3 = makeEp("s1e3", 1, 3); s1e3.absolute_index = 3;
    Episode s2e1 = makeEp("s2e1", 2, 1); s2e1.absolute_index = 5; // absolute ep 4 is missing
    std::vector<Episode> eps{ s1e3, s2e1 };

    auto adv = RuleEngine::advanceNatural("s1e3", {}, eps);
    EXPECT_EQ(adv.index, 1) << "s2e1 is still the only available next episode";
    EXPECT_EQ(adv.new_watermark_id, "s1e3") << "watermark shouldn't advance past a real gap";
    ASSERT_EQ(adv.new_ahead.size(), 1u);
    EXPECT_EQ(adv.new_ahead[0], "s2e1");
}

TEST(AdvanceNaturalTest, MultipleGapsAllResolveIndependently) {
    // Episodes 2 and 4 are both missing when the show starts airing.
    Episode e1 = makeEp("e1", 1, 1);
    Episode e3 = makeEp("e3", 1, 3);
    Episode e5 = makeEp("e5", 1, 5);

    auto step1 = RuleEngine::advanceNatural("", {}, {e1});
    ASSERT_EQ(step1.new_watermark_id, "e1");

    std::vector<Episode> catalog{ e1, e3, e5 };
    auto step2 = RuleEngine::advanceNatural(step1.new_watermark_id, step1.new_ahead, catalog);
    EXPECT_EQ(catalog[step2.index].episode_id, "e3");
    ASSERT_EQ(step2.new_ahead.size(), 1u);
    EXPECT_EQ(step2.new_watermark_id, "e1");

    auto step3 = RuleEngine::advanceNatural(step2.new_watermark_id, step2.new_ahead, catalog);
    EXPECT_EQ(catalog[step3.index].episode_id, "e5");
    ASSERT_EQ(step3.new_ahead.size(), 2u) << "e3 and e5 are both still ahead of the watermark";
    EXPECT_EQ(step3.new_watermark_id, "e1");
}
