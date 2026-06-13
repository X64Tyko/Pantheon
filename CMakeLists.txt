cmake_minimum_required(VERSION 3.20)
project(Pantheon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build order matters: hades produces ui-dist that kairos serves at runtime.
# CMake sequences hades before kairos via add_dependencies at the bottom.
add_subdirectory(hades)
add_subdirectory(kairos)
add_subdirectory(hermes)
add_subdirectory(hephaestus)

# Ensure the UI is built before the kairos binary is linked so a fresh
# build always produces a deployable server + UI together.
if(TARGET hades AND TARGET kairos)
    add_dependencies(kairos hades)
endif()
