add_library(frontier_sim STATIC
    src/capture.cpp
    src/config.cpp
    src/contracts.cpp
    src/crime.cpp
    src/event_log.cpp
    src/express.cpp
    src/save.cpp
    src/farms.cpp
    src/houses.cpp
    src/jobs.cpp
    src/law.cpp
    src/merchants.cpp
    src/news.cpp
    src/rng.cpp
    src/simulation.cpp
    src/stats.cpp
    src/terrain.cpp
    src/traders.cpp
    src/world.cpp
)
target_include_directories(frontier_sim PUBLIC include)
# Header-only JSON parser (MIT), vendored so the sim has no build-time dependencies.
target_include_directories(frontier_sim SYSTEM PUBLIC third_party)

if(MSVC)
    target_compile_options(frontier_sim PRIVATE /W4 /utf-8)
else()
    target_compile_options(frontier_sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
