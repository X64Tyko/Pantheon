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

add_subdirectory(hades)
add_subdirectory(kairos)
add_subdirectory(hermes)
add_subdirectory(hephaestus)
add_subdirectory(momus)
