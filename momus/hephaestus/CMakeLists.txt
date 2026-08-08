add_executable(momus_hephaestus
    test_log_buffer.cpp
    test_media_probe.cpp
    test_encoder_args.cpp
    test_channel_session.cpp
        test_channel_viewer_registry.cpp
    test_hw_probe.cpp
        test_channel_playlist_splicer.cpp
        test_preview_session.cpp
        test_preview_session_manager.cpp
        test_vod_encode_stream.cpp
        test_vod_session_playback.cpp
)

target_link_libraries(momus_hephaestus
    PRIVATE
    hephaestus_core
    GTest::gtest_main
)

# Absolute path to the committed media fixture used by
# test_vod_session_playback.cpp — avoids depending on ctest's CWD.
target_compile_definitions(momus_hephaestus PRIVATE
        MOMUS_HEPHAESTUS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(GoogleTest)
gtest_discover_tests(momus_hephaestus)
