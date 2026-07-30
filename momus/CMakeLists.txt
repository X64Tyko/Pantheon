cmake_minimum_required(VERSION 3.20)

# Single source of truth for the version (see VERSION at repo root) — bump
# that file only. Momus isn't containerized standalone, so this only ever
# runs as part of the monorepo build, where CMAKE_SOURCE_DIR is the repo root.
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" PANTHEON_VERSION)
project(Momus VERSION ${PANTHEON_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(kairos)
add_subdirectory(hermes)
add_subdirectory(hephaestus)
