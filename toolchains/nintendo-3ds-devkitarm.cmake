# Nintendo 3DS bootstrap toolchain for OpenStarbound research builds.
# This preset is intentionally conservative and aimed at early bring-up.

if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO is not set. Install devkitPro and set DEVKITPRO before configuring.")
endif()

function(N3DS_NORMALIZE_HOST_PATH output input)
  set(normalized "${input}")
  string(REPLACE "\\" "/" normalized "${normalized}")

  if((CMAKE_HOST_SYSTEM_NAME STREQUAL "MSYS" OR MSYS) AND normalized MATCHES "^[A-Za-z]:/")
    find_program(N3DS_CYGPATH_EXECUTABLE NAMES cygpath)
    if(N3DS_CYGPATH_EXECUTABLE)
      execute_process(
        COMMAND "${N3DS_CYGPATH_EXECUTABLE}" -u "${normalized}"
        OUTPUT_VARIABLE cygpath_result
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(cygpath_result)
        set(normalized "${cygpath_result}")
      endif()
    endif()
  endif()

  set(${output} "${normalized}" PARENT_SCOPE)
endfunction()

n3ds_normalize_host_path(DEVKITPRO "$ENV{DEVKITPRO}")

if(DEFINED ENV{DEVKITARM})
  n3ds_normalize_host_path(DEVKITARM "$ENV{DEVKITARM}")
else()
  set(DEVKITARM "${DEVKITPRO}/devkitARM")
endif()

set(ARM_GCC "${DEVKITARM}/bin/arm-none-eabi-gcc")
set(ARM_GXX "${DEVKITARM}/bin/arm-none-eabi-g++")
set(ARM_AR "${DEVKITARM}/bin/arm-none-eabi-ar")
set(ARM_RANLIB "${DEVKITARM}/bin/arm-none-eabi-ranlib")
set(ARM_STRIP "${DEVKITARM}/bin/arm-none-eabi-strip")

foreach(ARM_TOOL ARM_GCC ARM_GXX ARM_AR ARM_RANLIB ARM_STRIP)
  if(EXISTS "${${ARM_TOOL}}.exe")
    set(${ARM_TOOL} "${${ARM_TOOL}}.exe")
  endif()
endforeach()

if(NOT EXISTS "${ARM_GCC}")
  message(FATAL_ERROR "arm-none-eabi-gcc not found at ${ARM_GCC}. Install devkitARM (3ds-dev group).")
endif()

if(NOT DEFINED PKG_CONFIG_EXECUTABLE)
  find_program(N3DS_PKG_CONFIG_EXECUTABLE
    NAMES pkg-config pkg-config.exe
    HINTS
      "${DEVKITPRO}/msys2/usr/bin"
  )

  if(N3DS_PKG_CONFIG_EXECUTABLE)
    set(PKG_CONFIG_EXECUTABLE "${N3DS_PKG_CONFIG_EXECUTABLE}" CACHE FILEPATH "pkg-config executable" FORCE)
  endif()
endif()

if(PKG_CONFIG_EXECUTABLE)
  set(ENV{PKG_CONFIG_PATH}
    "${DEVKITPRO}/portlibs/3ds/lib/pkgconfig;${DEVKITPRO}/libctru/lib/pkgconfig"
  )
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
set(CMAKE_EXE_LINKER_FLAGS_INIT "${N3DS_ARCH_FLAGS} -specs=3dsx.specs")
set(CMAKE_EXE_LINKER_FLAGS "${N3DS_ARCH_FLAGS} -specs=3dsx.specs" CACHE STRING "N3DS executable linker flags" FORCE)

# Allow CMake to find headers/libs in the devkitPro sysroots first.
set(CMAKE_FIND_ROOT_PATH
  "${DEVKITPRO}/portlibs/3ds"
  "${DEVKITPRO}/libctru"
)

set(CMAKE_PREFIX_PATH
  "${DEVKITPRO}/portlibs/3ds"
  "${DEVKITPRO}/libctru"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
