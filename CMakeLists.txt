cmake_minimum_required(VERSION 3.20)

# Single source of truth for the version across all services and the web
# client (see VERSION at repo root) — bump that file only, everything else
# reads from it.
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" PANTHEON_VERSION)
project(Pantheon VERSION ${PANTHEON_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Opt-in AddressSanitizer+LeakSanitizer instrumentation for the whole build —
# off by default so normal dev/CI builds are unaffected. Enable with
# -DPANTHEON_SANITIZE=ON (Debug build recommended) to make momus_kairos's
# resource-release tests (momus/kairos/source/test_media_probe_resource_release.cpp)
# actually catch leaked allocations via LSan's process-exit check, instead of
# just exercising the code paths without verifying anything got freed.
option(PANTHEON_SANITIZE "Build with AddressSanitizer + LeakSanitizer" OFF)
if (PANTHEON_SANITIZE)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif ()

add_subdirectory(hades)
add_subdirectory(kairos)
add_subdirectory(hermes)
add_subdirectory(hephaestus)
add_subdirectory(momus)
