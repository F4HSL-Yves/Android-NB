# FFTW3 (double precision), needed by WDSP.
# Host builds use the system library. Cross builds for the Zynq find the
# armhf static library from Ubuntu multiarch (libfftw3-dev:armhf), selected
# by CMAKE_LIBRARY_ARCHITECTURE in cmake/toolchain-armhf.cmake.
find_path(FFTW3_INCLUDE_DIR fftw3.h REQUIRED)
find_library(FFTW3_LIBRARY NAMES libfftw3.a fftw3 REQUIRED)
message(STATUS "FFTW3: ${FFTW3_LIBRARY}")
add_library(FFTW::fftw3 UNKNOWN IMPORTED)
set_target_properties(FFTW::fftw3 PROPERTIES
  IMPORTED_LOCATION "${FFTW3_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${FFTW3_INCLUDE_DIR}")
