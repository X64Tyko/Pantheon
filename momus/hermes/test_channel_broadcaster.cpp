#include <gtest/gtest.h>
#include "../../hermes/src/broadcast/ChannelBroadcaster.h"
#include <httplib.h>
#include <thread>
#include <chrono>
#include <atomic>

// A simple mock Hephaestus server using httplib
class MockHephaestus {
public:
    MockHephaestus() {
        svr.Get("/stream/channels/:id", [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.path_params.at("id");
            res.set_content_provider(
                "video/mp2t",
                [this](size_t offset, httplib::DataSink &sink) {
                    if (stop_server) return false;
                    
                    std::string chunk = "data-chunk";
                    sink.write(chunk.data(), chunk.size());
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    return true;
                }
            );
        });
    }

    void start() {
        int port = svr.bind_to_any_port("127.0.0.1");
        url = "http://127.0.0.1:" + std::to_string(port);
        t = std::thread([this]() { svr.listen_after_bind(); });
    }

    void stop() {
        stop_server = true;
        svr.stop();
        if (t.joinable()) t.join();
    }

    std::string url;
    httplib::Server svr;
    std::thread t;
    std::atomic<bool> stop_server{false};
};

TEST(ChannelBroadcasterTest, FanOutData) {
    MockHephaestus mock;
    mock.start();

    ChannelBroadcaster bc("test-ch", mock.url, 1);
    auto sink1 = bc.addClient();
    auto sink2 = bc.addClient();

    // Wait for some data to arrive
    auto wait_for_data = [](std::shared_ptr<ChannelBroadcaster::Sink> sink) {
        std::unique_lock lock(sink->mtx);
        sink->cv.wait_for(lock, std::chrono::seconds(2), [&] { return !sink->queue.empty(); });
        return !sink->queue.empty();
    };

    EXPECT_TRUE(wait_for_data(sink1));
    EXPECT_TRUE(wait_for_data(sink2));

    auto chunk1 = sink1->queue.front();
    auto chunk2 = sink2->queue.front();
    
    std::string s1(chunk1.begin(), chunk1.end());
    std::string s2(chunk2.begin(), chunk2.end());
    
    EXPECT_EQ(s1, "data-chunk");
    EXPECT_EQ(s2, "data-chunk");

    bc.removeClient(sink1);
    bc.removeClient(sink2);
    mock.stop();
}

TEST(ChannelBroadcasterTest, LingerAndStop) {
    MockHephaestus mock;
    mock.start();

    // 1 second linger
    ChannelBroadcaster bc("test-ch", mock.url, 1);
    auto sink = bc.addClient();
    
    // Wait for start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(bc.isDead());

    bc.removeClient(sink);
    
    // Should still be alive immediately after removal (lingering)
    EXPECT_FALSE(bc.isDead());

    // Wait for linger to expire (> 1s)
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    
    // Broadcaster should eventually exit
    int retries = 10;
    while (!bc.isDead() && retries-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(bc.isDead());

    mock.stop();
}

TEST(ChannelBroadcasterTest, CancelLingerOnNewClient) {
    MockHephaestus mock;
    mock.start();

    ChannelBroadcaster bc("test-ch", mock.url, 1);
    auto sink1 = bc.addClient();
    bc.removeClient(sink1);
    
    // Now lingering. Add new client before it expires.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto sink2 = bc.addClient();
    
    // Wait more than linger time from first removal
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // Should still be alive because sink2 cancelled the linger
    EXPECT_FALSE(bc.isDead());

    bc.removeClient(sink2);
    mock.stop();
}
