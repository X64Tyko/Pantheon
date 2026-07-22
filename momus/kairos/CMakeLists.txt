include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
# Prevent GTest from overriding CRT settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(momus_kairos
    api/test_content_service_security.cpp
    api/test_list_push_helper.cpp
    api/test_sync_source_list_items.cpp
    api/test_playlist_play_target.cpp
    api/test_auth_service_routes.cpp
    api/test_restriction_access_check.cpp
    api/test_playback_and_channel_routes.cpp
    api/test_writeback_all.cpp
    api/test_content_service_auto_writeback.cpp
    auth/test_auth_store.cpp
    conf/test_conf_store.cpp
    db/test_migrations.cpp
    db/test_schema_constraints.cpp
    db/test_chapter_repository.cpp
    db/test_block_repository.cpp
    db/test_source_repository.cpp
    db/test_content_repository_writeback.cpp
    db/test_filter_expr.cpp
    db/test_playlist_repository.cpp
    detect/test_chapter_detector.cpp
    scheduler/test_cursor_state.cpp
    scheduler/test_rule_engine.cpp
    scheduler/test_epg_output.cpp
    scheduler/test_timeslot_block.cpp
    scheduler/test_natural_advance.cpp
    scheduler/test_determinism_regression.cpp
    scraper/test_scraper_manager.cpp
    scraper/test_tmdb_scraper.cpp
    source/test_jellyfin_source.cpp
    source/test_plex_source.cpp
    source/test_local_source.cpp
    source/test_sync_manager.cpp
)

# kairos_core brings in SQLiteCpp, nlohmann_json, httplib, and the src/ include
# path transitively — no additional target_include_directories needed here.
target_link_libraries(momus_kairos
    PRIVATE
    kairos_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(momus_kairos)
