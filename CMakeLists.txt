# For toolchains without make (MSVC on Windows). The client itself needs no
# build step: include trace_client.hpp. This only builds and runs the tests.
cmake_minimum_required(VERSION 3.10)
project(trace_client_cpp VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

add_executable(test_trace_client test/test_trace_client.cpp)
target_link_libraries(test_trace_client Threads::Threads)
# The tests read test/install-id-fixtures and the version spots from here,
# wherever they run from (MSVC runs them from build/Release).
target_compile_definitions(test_trace_client PRIVATE TRACE_CLIENT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
  target_link_libraries(test_trace_client ws2_32)
endif()

enable_testing()
add_test(NAME test_trace_client COMMAND test_trace_client)
