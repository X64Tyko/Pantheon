#include <gtest/gtest.h>
#include "conf/ConfStore.h"
#include <filesystem>
#include <cstdio>

namespace {

class ConfStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path = (std::filesystem::temp_directory_path()
            / ("momus_kairos_conf_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed())
               + "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".conf")).string();
        std::remove(path.c_str());
    }
    void TearDown() override { std::remove(path.c_str()); }

    std::string path;
};

} // namespace

// Reported bug: the Settings page's "Sync Worker Threads" value reverted to
// whatever KAIROS_SYNC_THREADS the compose file sets (4) on every restart —
// setSyncThreadsOverride only ever updated SyncManager's in-memory atomic,
// never ConfStore's own persisted file. Simulates a restart by constructing
// a fresh ConfStore against the same path.
TEST_F(ConfStoreTest, SyncThreadsOverridePersistsAcrossRestart) {
    {
        ConfStore conf(path);
        EXPECT_EQ(conf.getSyncThreadsOverride(), 0); // unset by default
        conf.setSyncThreadsOverride(16);
        EXPECT_EQ(conf.getSyncThreadsOverride(), 16);
    }
    {
        ConfStore reloaded(path); // fresh instance == restart
        EXPECT_EQ(reloaded.getSyncThreadsOverride(), 16);
    }
}

TEST_F(ConfStoreTest, SyncThreadsOverrideOfZeroClearsIt) {
    ConfStore conf(path);
    conf.setSyncThreadsOverride(8);
    conf.setSyncThreadsOverride(0);
    EXPECT_EQ(conf.getSyncThreadsOverride(), 0);

    ConfStore reloaded(path);
    EXPECT_EQ(reloaded.getSyncThreadsOverride(), 0); // never written since it's the default
}

// setBufferSize had the same bug class: it updated the in-memory value (and
// the global g_buffer_size) but never called saveLocked(), so it silently
// reverted on restart too.
TEST_F(ConfStoreTest, BufferSizePersistsAcrossRestart) {
    {
        ConfStore conf(path);
        conf.setBufferSize(2048); // KB
        EXPECT_EQ(conf.getBufferSize(), 2048);
    }
    {
        ConfStore reloaded(path);
        EXPECT_EQ(reloaded.getBufferSize(), 2048);
    }
}
