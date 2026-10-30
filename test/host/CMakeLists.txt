# Host-native tests of the portable layer (no ESP-IDF):
#   cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build
cmake_minimum_required(VERSION 3.16)
project(rbx_host_tests C)
set(CMAKE_C_STANDARD 99)
add_compile_options(-Wall -Wextra -Werror)
add_executable(test_rbx_frame test_rbx_frame.c ../../src/rbx_frame.c ../../src/rbx_pilot_state.c)
target_include_directories(test_rbx_frame PRIVATE ../../include)
target_link_libraries(test_rbx_frame m)
enable_testing()
add_test(NAME rbx_frame COMMAND test_rbx_frame)
