cmake_minimum_required(VERSION 3.20)
project(Hephaestus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ── cpp-httplib (header-only) ─────────────────────────────────────────────────
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(httplib)

find_package(OpenSSL REQUIRED)

# ── hephaestus_core ───────────────────────────────────────────────────────────
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*[/\\\\]main\\.cpp$")

if(CORE_SOURCES)
    add_library(hephaestus_core STATIC ${CORE_SOURCES})
    target_include_directories(hephaestus_core PUBLIC src)
    target_link_libraries(hephaestus_core
        PUBLIC
        nlohmann_json::nlohmann_json
        httplib::httplib
        OpenSSL::SSL OpenSSL::Crypto
    )
    if(UNIX)
        target_link_libraries(hephaestus_core PUBLIC pthread)
    endif()
    target_compile_definitions(hephaestus_core PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
else()
    add_library(hephaestus_core INTERFACE)
    target_include_directories(hephaestus_core INTERFACE src)
    target_link_libraries(hephaestus_core
        INTERFACE
        nlohmann_json::nlohmann_json
        httplib::httplib
        OpenSSL::SSL OpenSSL::Crypto
    )
    if(UNIX)
        target_link_libraries(hephaestus_core INTERFACE pthread)
    endif()
    target_compile_definitions(hephaestus_core INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
endif()

# ── hephaestus executable ─────────────────────────────────────────────────────
if(CORE_SOURCES)
    add_executable(hephaestus src/main.cpp)
    target_include_directories(hephaestus PRIVATE src)
    target_link_libraries(hephaestus PRIVATE hephaestus_core)
endif()
