cmake_minimum_required(VERSION 3.16)
project(nbd LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NBD_BUILD_TESTS "Build the unit tests" ON)
option(NBD_WITH_WDSP "Build WDSP and the nbd demodulator" ON)

find_package(Threads REQUIRED)

add_executable(nbsynth tools/nbsynth.cpp)
install(TARGETS nbsynth RUNTIME DESTINATION bin)

# Core DSP: no external dependency, unit tested on any host.
add_library(nbcore STATIC src/dsp.cpp src/beacon_lock.cpp)
target_include_directories(nbcore PUBLIC src)
target_compile_options(nbcore PRIVATE -Wall -Wextra)

if(NBD_BUILD_TESTS)
  enable_testing()
  add_executable(test_beacon_lock tests/test_beacon_lock.cpp)
  target_link_libraries(test_beacon_lock nbcore)
  if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME beacon_lock COMMAND test_beacon_lock)
  endif()
endif()

if(NBD_WITH_WDSP)
  include(cmake/fftw.cmake)
  file(GLOB WDSP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wdsp/src/*.c)
  if(NOT WDSP_SOURCES)
    message(FATAL_ERROR "WDSP sources missing: run 'git submodule update --init'")
  endif()
  add_library(wdsp STATIC ${WDSP_SOURCES})
  target_compile_definitions(wdsp PRIVATE _GNU_SOURCE)
  target_compile_options(wdsp PRIVATE -O3 -w)
  target_include_directories(wdsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wdsp/src)
  target_link_libraries(wdsp PUBLIC FFTW::fftw3 Threads::Threads m)

  add_executable(nbd src/main.cpp src/wdsp_rx.cpp)
  target_link_libraries(nbd PRIVATE nbcore wdsp)
  target_compile_options(nbd PRIVATE -Wall -Wextra)
  install(TARGETS nbd RUNTIME DESTINATION bin)

  if(NBD_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_pipeline tests/test_pipeline.cpp src/wdsp_rx.cpp)
    target_link_libraries(test_pipeline nbcore wdsp)
    add_test(NAME pipeline COMMAND test_pipeline)
  endif()
endif()
