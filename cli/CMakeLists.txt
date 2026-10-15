find_package(Threads REQUIRED)

add_executable(frontier_cli main.cpp)
target_link_libraries(frontier_cli PRIVATE frontier_sim)

# Runs one scenario under many seeds and reports distributions.
add_executable(frontier_batch batch.cpp)
target_link_libraries(frontier_batch PRIVATE frontier_sim Threads::Threads)
