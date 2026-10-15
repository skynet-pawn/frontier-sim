add_library(frontier_sim STATIC
    src/rng.cpp
)
target_include_directories(frontier_sim PUBLIC include)

if(MSVC)
    target_compile_options(frontier_sim PRIVATE /W4)
else()
    target_compile_options(frontier_sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
