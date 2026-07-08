add_executable(momus_hephaestus
    test_log_buffer.cpp
    test_media_probe.cpp
    test_encoder_args.cpp
    test_channel_session.cpp
    test_hw_probe.cpp
)

target_link_libraries(momus_hephaestus
    PRIVATE
    hephaestus_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(momus_hephaestus)
