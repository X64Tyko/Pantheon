#include <gtest/gtest.h>
#include "../../hermes/src/kairos/KairosClient.h"
#include <httplib.h>
#include <thread>

TEST(KairosClientTest, GetChannelsParse) {
    httplib::Server svr;
    svr.Get("/api/channels", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"([
            {"channel_id": "ch1", "name": "Channel 1", "number": 101},
            {"channel_id": "ch2", "name": "Channel 2", "number": 102}
        ])", "application/json");
    });

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&]() { svr.listen_after_bind(); });
    
    KairosClient client("http://127.0.0.1:" + std::to_string(port));
    auto channels = client.getChannels();
    
    ASSERT_EQ(channels.size(), 2);
    EXPECT_EQ(channels[0].channel_id, "ch1");
    EXPECT_EQ(channels[0].name, "Channel 1");
    EXPECT_EQ(channels[0].number, 101);
    EXPECT_EQ(channels[1].channel_id, "ch2");

    svr.stop();
    t.join();
}

TEST(KairosClientTest, HandleEmptyResponse) {
    httplib::Server svr;
    svr.Get("/api/channels", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&]() { svr.listen_after_bind(); });
    
    KairosClient client("http://127.0.0.1:" + std::to_string(port));
    auto channels = client.getChannels();
    
    EXPECT_TRUE(channels.empty());

    svr.stop();
    t.join();
}

TEST(KairosClientTest, HandleError) {
    httplib::Server svr;
    svr.Get("/api/channels", [](const httplib::Request&, httplib::Response& res) {
        res.status = 500;
    });

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&]() { svr.listen_after_bind(); });
    
    KairosClient client("http://127.0.0.1:" + std::to_string(port));
    auto channels = client.getChannels();
    
    EXPECT_TRUE(channels.empty());

    svr.stop();
    t.join();
}
