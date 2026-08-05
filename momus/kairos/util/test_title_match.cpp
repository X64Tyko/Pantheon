// Tests for titlematch::detectFilePart/groupFileParts — the multi-part
// movie filename heuristic (CD1/CD2, Part 1/Part 2, Disc 1/Disc 2, pt.1/
// pt.2) used by LocalSource/JellyfinBaseSource to group sibling files of
// one movie. See GitHub #3.

#include <gtest/gtest.h>
#include "util/TitleMatch.h"

using namespace titlematch;

// ============================================================================
// detectFilePart
// ============================================================================

TEST(DetectFilePart, MatchesCdMarker)
{
	auto m = detectFilePart("Movie.Title.CD1");
	ASSERT_TRUE(m.has_value());
	EXPECT_EQ(m->second, 1);
}

TEST(DetectFilePart, MatchesCdWithSpace)
{
	auto m = detectFilePart("Movie Title CD 2");
	ASSERT_TRUE(m.has_value());
	EXPECT_EQ(m->second, 2);
}

TEST(DetectFilePart, MatchesPartWithDot)
{
	auto m = detectFilePart("Movie.Title.Part.2");
	ASSERT_TRUE(m.has_value());
	EXPECT_EQ(m->second, 2);
}

TEST(DetectFilePart, MatchesPt)
{
	auto m = detectFilePart("Movie Title pt.1");
	ASSERT_TRUE(m.has_value());
	EXPECT_EQ(m->second, 1);
}

TEST(DetectFilePart, MatchesDisc)
{
	auto m = detectFilePart("Movie Title Disc 2");
	ASSERT_TRUE(m.has_value());
	EXPECT_EQ(m->second, 2);
}

TEST(DetectFilePart, MatchesDisk)
{
	auto m = detectFilePart("Movie Title disk1");
	ASSERT_TRUE(m.has_value());
	EXPECT_EQ(m->second, 1);
}

TEST(DetectFilePart, NoMarkerReturnsNullopt)
{
	EXPECT_FALSE(detectFilePart("The Matrix 1999 1080p BluRay x264").has_value());
}

TEST(DetectFilePart, StripsMarkerFromBase)
{
	auto m = detectFilePart("Movie.Title.CD1");
	ASSERT_TRUE(m.has_value());
	EXPECT_EQ(m->first, "Movie.Title.");
}

// ============================================================================
// groupFileParts
// ============================================================================

TEST(GroupFileParts, GroupsConsistentPair)
{
	auto parts = groupFileParts({"Movie.Title.CD1", "Movie.Title.CD2"});
	ASSERT_EQ(parts.size(), 2u);
	EXPECT_EQ(parts[0], 1);
	EXPECT_EQ(parts[1], 2);
}

TEST(GroupFileParts, EmptyWhenFewerThanTwo)
{
	EXPECT_TRUE(groupFileParts({"Movie.Title.CD1"}).empty());
	EXPECT_TRUE(groupFileParts({}).empty());
}

TEST(GroupFileParts, EmptyWhenAnyFileHasNoMarker)
{
	EXPECT_TRUE(groupFileParts({"Movie.Title.CD1", "trailer"}).empty());
}

TEST(GroupFileParts, EmptyWhenBaseTitlesDiffer)
{
	EXPECT_TRUE(groupFileParts({"Movie One CD1", "Movie Two CD2"}).empty());
}

TEST(GroupFileParts, EmptyOnDuplicatePartNumbers)
{
	EXPECT_TRUE(groupFileParts({"Movie Title Part 1", "Movie Title Part 1"}).empty());
}

TEST(GroupFileParts, GroupsThreePartsOutOfOrderInput)
{
	auto parts = groupFileParts({"Movie Title CD3", "Movie Title CD1", "Movie Title CD2"});
	ASSERT_EQ(parts.size(), 3u);
	// Parallel to input order, not sorted — caller is responsible for sorting.
	EXPECT_EQ(parts[0], 3);
	EXPECT_EQ(parts[1], 1);
	EXPECT_EQ(parts[2], 2);
}