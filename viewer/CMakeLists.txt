# Overworld viewer: a top-down map of the running simulation. It reads the sim through its const accessors only, so
# the presentation can be swapped without touching sim/. raylib is fetched at configure time (zlib licence).
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)

add_executable(frontier_viewer main.cpp)
target_link_libraries(frontier_viewer PRIVATE frontier_sim raylib)
