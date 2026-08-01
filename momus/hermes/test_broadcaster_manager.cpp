#include <gtest/gtest.h>
#include "../../hermes/src/broadcast/BroadcasterManager.h"
#include <thread>
#include <chrono>

TEST(BroadcasterManagerTest, GetOrCreateAndReuse)
{
	BroadcasterManager mgr("http://localhost:1234", 1);

	auto bc1 = mgr.getOrCreate("ch-1");
	auto bc2 = mgr.getOrCreate("ch-1");
	auto bc3 = mgr.getOrCreate("ch-2");

	EXPECT_EQ(bc1, bc2);
	EXPECT_NE(bc1, bc3);
}

// Regression note: this test used to assert `mgr.getOrCreate("ch-1") == bc`
// after a single reap() call with no wait for the broadcaster to actually
// die first — its own comments admitted "it might not be dead yet" and "we
// can't easily [control retry timing]". That assertion passes whether or
// not reap() does anything at all (getOrCreate() legitimately returns the
// same live object either way), so it never actually verified reaping
// worked. Fixed to poll isDead() with a generous bound (same pattern
// test_channel_broadcaster.cpp's own LingerAndStop/the death-ordering test
// use) before reaping, then assert identity actually changed.
TEST(BroadcasterManagerTest, ReapDeadBroadcasters)
{
	BroadcasterManager mgr("http://127.0.0.1:1", 0); // connection refused, not a DNS/timeout wait

	auto bc   = mgr.getOrCreate("ch-1");
	auto sink = bc->addClient();
	bc->removeClient(sink); // zero linger -> eligible for reap once dead

	int retries = 100;
	while (!bc->isDead() && retries-- > 0)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	ASSERT_TRUE(bc->isDead()) << "broadcaster never died after exhausting all retry attempts";

	mgr.reap();

	auto after = mgr.getOrCreate("ch-1");
	EXPECT_NE(after, bc) << "reap() should have dropped the dead broadcaster, so this getOrCreate() must create a fresh one";
}