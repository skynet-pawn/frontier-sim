cmake_minimum_required(VERSION 3.20)
project(FrontierSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The simulation core: no rendering, no engine dependencies.
add_subdirectory(sim)

# Headless runner.
add_subdirectory(cli)

# Overworld viewer (needs raylib, fetched at configure time): another consumer of `frontier_sim`. Off by default so the
# simulation and its tools build with no dependencies.
option(FRONTIER_BUILD_VIEWER "Build the raylib overworld viewer" OFF)
if(FRONTIER_BUILD_VIEWER)
    add_subdirectory(viewer)
endif()

enable_testing()
add_subdirectory(tests)
