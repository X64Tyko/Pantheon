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

# GCC doesn't transitively pull in <cstdint> the way MSVC does, so patch the
# two SQLiteCpp headers that use uint32_t etc. without including it.
foreach(_hdr Statement.h Column.h)
    set(_path "${sqlitecpp_SOURCE_DIR}/include/SQLiteCpp/${_hdr}")
    file(READ "${_path}" _src)
    if(NOT _src MATCHES "#include <cstdint>")
        string(REPLACE "#include <memory>" "#include <memory>\n#include <cstdint>" _src "${_src}")
        file(WRITE "${_path}" "${_src}")
    endif()
endforeach()

# ── cpp-httplib (header-only) ─────────────────────────────────────────────────
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(httplib)

# ── kairos_core (all sources except main.cpp — linkable by tests) ─────────────
file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER ALL_SOURCES EXCLUDE REGEX ".*[/\\\\]main\\.cpp$")

add_library(kairos_core STATIC ${ALL_SOURCES})

target_include_directories(kairos_core PUBLIC src)

target_link_libraries(kairos_core
    PUBLIC
    nlohmann_json::nlohmann_json
    SQLiteCpp
    httplib::httplib
)

if(UNIX)
    target_link_libraries(kairos_core PUBLIC pthread dl)
endif()

# ── kairos executable ─────────────────────────────────────────────────────────
add_executable(kairos src/main.cpp)
target_link_libraries(kairos PRIVATE kairos_core)

# When built as part of Pantheon, sequence after hades so ui-dist is ready.
if(TARGET hades)
    add_dependencies(kairos hades)
endif()
