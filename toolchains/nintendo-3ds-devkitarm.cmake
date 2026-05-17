# Nintendo 3DS bootstrap toolchain for OpenStarbound research builds.
# This preset is intentionally conservative and aimed at early bring-up.

if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO is not set. Install devkitPro and set DEVKITPRO before configuring.")
endif()

set(DEVKITPRO "$ENV{DEVKITPRO}")

if(DEFINED ENV{DEVKITARM})
  set(DEVKITARM "$ENV{DEVKITARM}")
else()
  set(DEVKITARM "${DEVKITPRO}/devkitARM")
endif()

set(ARM_GCC "${DEVKITARM}/bin/arm-none-eabi-gcc")
set(ARM_GXX "${DEVKITARM}/bin/arm-none-eabi-g++")
set(ARM_AR "${DEVKITARM}/bin/arm-none-eabi-ar")
set(ARM_RANLIB "${DEVKITARM}/bin/arm-none-eabi-ranlib")
set(ARM_STRIP "${DEVKITARM}/bin/arm-none-eabi-strip")

if(WIN32)
  set(ARM_GCC "${ARM_GCC}.exe")
  set(ARM_GXX "${ARM_GXX}.exe")
  set(ARM_AR "${ARM_AR}.exe")
  set(ARM_RANLIB "${ARM_RANLIB}.exe")
  set(ARM_STRIP "${ARM_STRIP}.exe")
endif()

if(NOT EXISTS "${ARM_GCC}")
  message(FATAL_ERROR "arm-none-eabi-gcc not found at ${ARM_GCC}. Install devkitARM (3ds-dev group).")
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR armv6k)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER "${ARM_GCC}")
set(CMAKE_CXX_COMPILER "${ARM_GXX}")
set(CMAKE_ASM_COMPILER "${ARM_GCC}")
set(CMAKE_AR "${ARM_AR}")
set(CMAKE_RANLIB "${ARM_RANLIB}")
set(CMAKE_STRIP "${ARM_STRIP}")

# Baseline New 3DS-friendly ARM flags. Tune later per module/profile.
set(N3DS_ARCH_FLAGS "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft")
set(CMAKE_C_FLAGS_INIT "${N3DS_ARCH_FLAGS} -D_3DS")
set(CMAKE_CXX_FLAGS_INIT "${N3DS_ARCH_FLAGS} -D_3DS")

# Allow CMake to find headers/libs in the devkitPro sysroots first.
set(CMAKE_FIND_ROOT_PATH
  "${DEVKITPRO}/portlibs/3ds"
  "${DEVKITPRO}/libctru"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
