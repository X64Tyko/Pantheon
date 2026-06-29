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

# GCC doesn't pull <cstdint> in implicitly the way MSVC does; force-include it
# when compiling SQLiteCpp so uint32_t etc. are visible without touching sources.
target_compile_options(SQLiteCpp PRIVATE -include cstdint)

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

find_package(OpenSSL REQUIRED)

# Guard: during the Docker layer-split build the configure step runs before
# src/ is copied in, leaving ALL_SOURCES empty. Use INTERFACE in that case so
# the target exists for the executable to link against unconditionally.
if(ALL_SOURCES)
    add_library(kairos_core STATIC ${ALL_SOURCES})
    target_include_directories(kairos_core PUBLIC src)
    target_link_libraries(kairos_core
        PUBLIC
        nlohmann_json::nlohmann_json
        SQLiteCpp
        httplib::httplib
        OpenSSL::SSL OpenSSL::Crypto
    )
    if(UNIX)
        target_link_libraries(kairos_core PUBLIC pthread dl crypt)
    endif()
    target_compile_definitions(kairos_core PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
else()
    add_library(kairos_core INTERFACE)
    target_include_directories(kairos_core INTERFACE src)
    target_link_libraries(kairos_core
        INTERFACE
        nlohmann_json::nlohmann_json
        SQLiteCpp
        httplib::httplib
        OpenSSL::SSL OpenSSL::Crypto
    )
    if(UNIX)
        target_link_libraries(kairos_core INTERFACE pthread dl crypt)
    endif()
    target_compile_definitions(kairos_core INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
endif()

# KAIROS_DEV: enable CORS wildcard and other dev-only behaviour.
# Set automatically for Debug builds; override with -DKAIROS_DEV=ON/OFF.
if(NOT DEFINED KAIROS_DEV)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type_upper)
    if(_build_type_upper STREQUAL "DEBUG")
        set(KAIROS_DEV ON)
    else()
        set(KAIROS_DEV OFF)
    endif()
endif()
if(KAIROS_DEV)
    target_compile_definitions(kairos_core PUBLIC KAIROS_DEV)
endif()

# ── kairos executable ─────────────────────────────────────────────────────────
# Guard: same Docker layer-split case — no main.cpp during configure-only step.
# CONFIGURE_DEPENDS on ALL_SOURCES causes a re-configure when src/ is added,
# so the full target graph is created before the actual build runs.
if(ALL_SOURCES)
    add_executable(kairos src/main.cpp)
    target_link_libraries(kairos PRIVATE kairos_core)
    if(TARGET hades)
        add_dependencies(kairos hades)
    endif()
endif()
