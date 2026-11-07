# libopus for the audio sent to the phone. Host: system library. Zynq cross
# build: Ubuntu multiarch libopus-dev:armhf, static.
find_path(OPUS_INCLUDE_DIR opus/opus.h REQUIRED)
find_library(OPUS_LIBRARY NAMES libopus.a opus REQUIRED)
message(STATUS "Opus: ${OPUS_LIBRARY}")
add_library(Opus::opus UNKNOWN IMPORTED)
set_target_properties(Opus::opus PROPERTIES
  IMPORTED_LOCATION "${OPUS_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INCLUDE_DIR}"
  INTERFACE_LINK_LIBRARIES m)
