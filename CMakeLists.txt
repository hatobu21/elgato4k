cmake_minimum_required(VERSION 3.24)
project(Vela VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VELA_BUILD_TESTS "Build the core unit tests" ON)
option(VELA_BUILD_DEMO "Build the command-line pipeline simulation" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Platform-neutral logic: pacing, latency statistics, auto colour, planning.
add_library(vela_core STATIC
  core/src/AutoColor.cpp
  core/src/ContentCadence.cpp
  core/src/FramePacer.cpp
  core/src/LatencyMonitor.cpp
  core/src/ModePlanner.cpp
  core/src/RollingStats.cpp
  core/src/Settings.cpp)
target_include_directories(vela_core PUBLIC core/include)

if(VELA_BUILD_TESTS)
  enable_testing()
  add_executable(vela_core_tests
    core/tests/TestMain.cpp
    core/tests/AutoColorTests.cpp
    core/tests/ContentCadenceTests.cpp
    core/tests/FramePacerTests.cpp
    core/tests/LatencyMonitorTests.cpp
    core/tests/ModePlannerTests.cpp)
  target_link_libraries(vela_core_tests PRIVATE vela_core)
  add_test(NAME vela_core_tests COMMAND vela_core_tests)
endif()

if(VELA_BUILD_DEMO)
  add_executable(vela_sim demo/vela_sim.cpp)
  target_link_libraries(vela_sim PRIVATE vela_core)
endif()

# The application itself: D3D12, Media Foundation, NGX, WebView2. Windows only.
if(WIN32)
  add_subdirectory(win)
endif()
