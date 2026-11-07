# Cross build for the Zynq-7000 (dual Cortex-A9, NEON, hard float) used by
# the Pluto, the FishBall 7020 and their clones. Needs the Ubuntu packages
# gcc-arm-linux-gnueabihf, g++-arm-linux-gnueabihf and libfftw3-dev:armhf.
# The binary is linked statically so it runs whatever libc the firmware has.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
