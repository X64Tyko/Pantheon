// Tests for ScheduleCache::clear()/hardReset()'s preserve_current behavior —
// the default (true) keeps whatever's on-air right now out of the delete so a
// live edit doesn't yank the currently-streaming item (see the header's own
// comment); preserve_current=false is the explicit "apply to the live stream
// now" escape hatch (SchedulerService.cpp's POST /epg/clear?live=true) that
// drops that carve-out instead, on the reasoning that a regenerated schedule
// has no awareness of a preserved row and could otherwise overlap it.

#include <gtest/gtest.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <ctime>
#include <string>

#include "api/ScheduleCache.h"
#include "db/Database.h"

class ScheduleCacheTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ScheduleCache cache{db};

	void SetUp() override
	{
		db.get().exec("INSERT INTO channel (channel_id, name, number) VALUES ('c1','Test',1)");
	}

	// wall_clock_start/end relative to now (seconds), so a negative start / positive
	// end straddles "now" (currently on-air); both positive is a future row.
	void insertRow(const std::string& item_id, int64_t start_offset, int64_t end_offset)
	{
		auto now = static_cast<int64_t>(std::time(nullptr));
		SQLite::Statement s(db.get(), R"(
            INSERT INTO scheduled_program
                (channel_id, item_type, item_id, wall_clock_start, wall_clock_end, cursor_json, created_at)
            VALUES ('c1','movie',?,?,?,'{}',?)
        )");
		s.bind(1, item_id);
		s.bind(2, now + start_offset);
		s.bind(3, now + end_offset);
		s.bind(4, now);
		s.exec();
	}

	int countRows()
	{
		SQLite::Statement q(db.get(), "SELECT COUNT(*) FROM scheduled_program WHERE channel_id='c1'");
		q.executeStep();
		return q.getColumn(0).getInt();
	}

	bool rowExists(const std::string& item_id)
	{
		SQLite::Statement q(db.get(), "SELECT 1 FROM scheduled_program WHERE channel_id='c1' AND item_id=?");
		q.bind(1, item_id);
		return q.executeStep();
	}
};

TEST_F(ScheduleCacheTest, Clear_DefaultPreservesCurrentlyAiringRow)
{
	insertRow("onair", -300, 300);   // straddles now
	insertRow("future", 3600, 7200); // not started yet

	cache.clear("c1");

	EXPECT_TRUE(rowExists("onair"));
	EXPECT_FALSE(rowExists("future"));
	EXPECT_EQ(countRows(), 1);
}

TEST_F(ScheduleCacheTest, Clear_LiveModeWipesCurrentlyAiringRowToo)
{
	insertRow("onair", -300, 300);
	insertRow("future", 3600, 7200);

	cache.clear("c1", /*preserve_current=*/false);

	EXPECT_FALSE(rowExists("onair"));
	EXPECT_FALSE(rowExists("future"));
	EXPECT_EQ(countRows(), 0);
}

TEST_F(ScheduleCacheTest, Clear_AlreadyAiredRowIsAlwaysWiped)
{
	insertRow("past", -7200, -3600);

	cache.clear("c1");

	EXPECT_FALSE(rowExists("past"));
}

TEST_F(ScheduleCacheTest, HardReset_DefaultPreservesCurrentlyAiringRowAndWipesAnchor)
{
	insertRow("onair", -300, 300);
	db.get().exec("UPDATE channel SET anchor_hashes = '{\"1\":{}}' WHERE channel_id='c1'");

	cache.hardReset("c1");

	EXPECT_TRUE(rowExists("onair"));

	SQLite::Statement q(db.get(), "SELECT anchor_hashes FROM channel WHERE channel_id='c1'");
	q.executeStep();
	EXPECT_TRUE(q.isColumnNull(0));
}

TEST_F(ScheduleCacheTest, HardReset_LiveModeWipesCurrentlyAiringRowAndAnchor)
{
	insertRow("onair", -300, 300);
	db.get().exec("UPDATE channel SET anchor_hashes = '{\"1\":{}}' WHERE channel_id='c1'");

	cache.hardReset("c1", /*preserve_current=*/false);

	EXPECT_FALSE(rowExists("onair"));

	SQLite::Statement q(db.get(), "SELECT anchor_hashes FROM channel WHERE channel_id='c1'");
	q.executeStep();
	EXPECT_TRUE(q.isColumnNull(0));
}