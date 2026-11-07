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
add_library(nbcore STATIC src/dsp.cpp src/beacon_lock.cpp src/spectrum.cpp)
target_include_directories(nbcore PUBLIC src)
# Fast float math: lets GCC vectorize the FIR loops (NEON on the Zynq, which
# is not IEEE compliant) and inline complex products. The DSP never relies
# on NaN or infinity.
target_compile_options(nbcore PRIVATE -Wall -Wextra -O3 -ffast-math)

# Board access over TCP: iiod (AD9363, DMA) and maia-httpd (DDC).
add_library(nbpluto STATIC src/iiod_client.cpp src/http_client.cpp src/pluto_source.cpp src/maia_waterfall.cpp)
target_include_directories(nbpluto PUBLIC src)
target_link_libraries(nbpluto PUBLIC nbcore Threads::Threads)
target_compile_options(nbpluto PRIVATE -Wall -Wextra)

if(NBD_BUILD_TESTS)
  enable_testing()
  add_executable(test_beacon_lock tests/test_beacon_lock.cpp)
  target_link_libraries(test_beacon_lock nbcore)
  add_executable(test_spectrum tests/test_spectrum.cpp)
  target_link_libraries(test_spectrum nbcore)
  add_executable(test_pluto tests/test_pluto.cpp)
  target_link_libraries(test_pluto nbpluto)
  if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME beacon_lock COMMAND test_beacon_lock)
    add_test(NAME pluto COMMAND test_pluto)
    add_test(NAME spectrum COMMAND test_spectrum)
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

  include(cmake/opus.cmake)
  add_library(nbnet STATIC src/ws_server.cpp src/protocol.cpp src/opus_audio.cpp)
  target_include_directories(nbnet PUBLIC src)
  target_link_libraries(nbnet PUBLIC nbpluto Opus::opus Threads::Threads)
  target_compile_options(nbnet PRIVATE -Wall -Wextra)

  add_executable(nbd src/main.cpp src/wdsp_rx.cpp)
  target_link_libraries(nbd PRIVATE nbnet wdsp)
  target_compile_options(nbd PRIVATE -Wall -Wextra)
  install(TARGETS nbd RUNTIME DESTINATION bin)

  add_executable(nbbench tools/nbbench.cpp src/wdsp_rx.cpp)
  target_link_libraries(nbbench PRIVATE nbnet wdsp)
  install(TARGETS nbbench RUNTIME DESTINATION bin)

  if(NBD_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_pipeline tests/test_pipeline.cpp src/wdsp_rx.cpp)
    target_link_libraries(test_pipeline nbcore wdsp)
    add_test(NAME pipeline COMMAND test_pipeline)
    add_executable(test_ws tests/test_ws.cpp)
    target_link_libraries(test_ws nbnet)
    add_test(NAME websocket COMMAND test_ws)
  endif()
endif()
