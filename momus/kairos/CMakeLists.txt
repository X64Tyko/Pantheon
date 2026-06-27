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
    db/test_migrations.cpp
    db/test_schema_constraints.cpp
    db/test_chapter_repository.cpp
    db/test_block_repository.cpp
    scheduler/test_cursor_state.cpp
    scheduler/test_rule_engine.cpp
    scheduler/test_epg_output.cpp
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
