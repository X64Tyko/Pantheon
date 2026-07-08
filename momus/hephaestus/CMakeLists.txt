add_executable(momus_hephaestus
    test_log_buffer.cpp
)

target_link_libraries(momus_hephaestus
    PRIVATE
    hephaestus_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(momus_hephaestus)
