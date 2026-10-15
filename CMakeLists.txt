cmake_minimum_required(VERSION 3.20)
project(FrontierSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The simulation core: no rendering, no engine dependencies.
add_subdirectory(sim)

# Headless runner. A viewer, when it exists, is another consumer of `frontier_sim`.
add_subdirectory(cli)

enable_testing()
add_subdirectory(tests)
