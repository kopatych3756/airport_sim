cmake_minimum_required(VERSION 3.10)
project(airport_runway_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(airport_sim
    src/main.c
    src/config.c
    src/io.c
    src/simulation.c
)

target_include_directories(airport_sim PRIVATE src)
target_compile_options(airport_sim PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME nominal COMMAND airport_sim --config ${CMAKE_SOURCE_DIR}/configs/test_nominal.conf --no-log --quiet)
add_test(NAME emergency COMMAND airport_sim --config ${CMAKE_SOURCE_DIR}/configs/test_emergency.conf --no-log --quiet)
add_test(NAME closure COMMAND airport_sim --config ${CMAKE_SOURCE_DIR}/configs/test_closure.conf --no-log --quiet)
add_test(NAME incompatible COMMAND airport_sim --config ${CMAKE_SOURCE_DIR}/configs/test_incompatible.conf --no-log --quiet)
add_test(NAME safety COMMAND airport_sim --config ${CMAKE_SOURCE_DIR}/configs/test_safety.conf --no-log --quiet)
add_test(NAME duplicate_aircraft COMMAND airport_sim --config ${CMAKE_SOURCE_DIR}/configs/test_duplicate.conf --no-log --quiet)
set_tests_properties(duplicate_aircraft PROPERTIES WILL_FAIL TRUE)
