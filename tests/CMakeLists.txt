add_executable(determinism_test determinism_test.cpp)
target_link_libraries(determinism_test PRIVATE frontier_sim)
target_compile_definitions(determinism_test PRIVATE FRONTIER_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_test(NAME determinism COMMAND determinism_test)
