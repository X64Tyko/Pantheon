add_executable(momus_hermes
    test_log_buffer.cpp
)

target_link_libraries(momus_hermes
    PRIVATE
    hermes_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(momus_hermes)
