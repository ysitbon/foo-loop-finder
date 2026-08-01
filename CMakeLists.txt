cmake_minimum_required(VERSION 3.22)
project(foo_loop_finder VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(loop_finder_core
    src/core/beat_grid.cpp
    src/core/loop_engine.cpp
    src/core/waveform.cpp)
target_include_directories(loop_finder_core PUBLIC include)

if(MSVC)
    target_compile_options(loop_finder_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(loop_finder_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(LOOP_FINDER_BUILD_TESTS "Build core unit tests" ON)
if(LOOP_FINDER_BUILD_TESTS)
    enable_testing()
    add_executable(loop_finder_tests tests/core_tests.cpp)
    target_link_libraries(loop_finder_tests PRIVATE loop_finder_core)
    add_test(NAME loop_finder_tests COMMAND loop_finder_tests)
endif()

# The official SDK ships as Visual Studio projects rather than a CMake package.
# native/foo_loop_finder.vcxproj builds the Windows component and reuses these
# core sources directly; this CMake project remains SDK-independent.
