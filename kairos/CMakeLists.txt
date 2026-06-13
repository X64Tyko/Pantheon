cmake_minimum_required(VERSION 3.20)
project(Kairos VERSION 0.1.0 LANGUAGES CXX)

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

# ── SQLiteCpp (bundles sqlite3 amalgamation — no system dep) ──────────────────
set(SQLITECPP_RUN_CPPLINT    OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK   OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INTERNAL_SQLITE ON  CACHE BOOL "" FORCE)
FetchContent_Declare(
    SQLiteCpp
    GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
    GIT_TAG        3.3.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SQLiteCpp)

# ── cpp-httplib (header-only) ─────────────────────────────────────────────────
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(httplib)

# ── kairos ────────────────────────────────────────────────────────────────────
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(kairos ${SOURCES})

target_include_directories(kairos PRIVATE src)

target_link_libraries(kairos
    PRIVATE
    nlohmann_json::nlohmann_json
    SQLiteCpp
    httplib::httplib
)

if(UNIX)
    target_link_libraries(kairos PRIVATE pthread dl)
endif()

# When built as part of Pantheon, sequence after hades so ui-dist is ready.
if(TARGET hades)
    add_dependencies(kairos hades)
endif()
