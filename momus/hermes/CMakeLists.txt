add_executable(momus_hermes
    test_log_buffer.cpp
    test_channel_broadcaster.cpp
    test_broadcaster_manager.cpp
    test_kairos_client.cpp
    test_device_session_manager.cpp
)

target_link_libraries(momus_hermes
    PRIVATE
    hermes_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(momus_hermes)
