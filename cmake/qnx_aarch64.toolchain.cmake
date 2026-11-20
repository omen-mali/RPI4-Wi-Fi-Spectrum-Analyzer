# =============================================================================
# QNX Neutrino RTOS Cross-Compilation Toolchain File
# Target: Raspberry Pi 4 (BCM2711) - aarch64
# =============================================================================
#
# Prerequisites:
#   - QNX SDP 8.0 installed on the host machine
#   - Environment sourced: source <SDP_PATH>/qnxsdp-env.sh
#     This sets QNX_HOST, QNX_TARGET, and adds tools to PATH.
#
# Usage:
#   mkdir build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/qnx_aarch64.toolchain.cmake
#   cmake --build .
# =============================================================================

cmake_minimum_required(VERSION 3.20)

# -- Target system identification --
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 8.0)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# -- Validate QNX SDP environment --
# QNX_HOST and QNX_TARGET must be set by sourcing qnxsdp-env.sh
if(NOT DEFINED ENV{QNX_HOST})
    message(FATAL_ERROR
        "QNX_HOST is not set. Source your QNX SDP environment first:\n"
        "  source <SDP_PATH>/qnxsdp-env.sh")
endif()

if(NOT DEFINED ENV{QNX_TARGET})
    message(FATAL_ERROR
        "QNX_TARGET is not set. Source your QNX SDP environment first:\n"
        "  source <SDP_PATH>/qnxsdp-env.sh")
endif()

set(QNX_HOST "$ENV{QNX_HOST}")
set(QNX_TARGET "$ENV{QNX_TARGET}")

# -- Compilers --
# qcc is the QNX compiler driver; it wraps GCC and ensures correct QNX ABI.
# Use -Vgcc_ntoaarch64le to target 64-bit ARM (little-endian) for the Pi 4.
set(CMAKE_C_COMPILER   "${QNX_HOST}/usr/bin/qcc")
set(CMAKE_CXX_COMPILER "${QNX_HOST}/usr/bin/q++")

set(CMAKE_C_COMPILER_TARGET   "gcc_ntoaarch64le")
set(CMAKE_CXX_COMPILER_TARGET "gcc_ntoaarch64le")

# Pass the target ABI variant to qcc/q++ for compilation AND linking.
# Without -V on the linker invocation, qcc won't add the arch-specific
# library search paths ($QNX_TARGET/aarch64le/lib, .../usr/lib).
set(CMAKE_C_FLAGS_INIT   "-Vgcc_ntoaarch64le")
set(CMAKE_CXX_FLAGS_INIT "-Vgcc_ntoaarch64le")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-Vgcc_ntoaarch64le")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Vgcc_ntoaarch64le")

# -- Sysroot --
# Points the compiler to QNX system headers and libraries for the target.
set(CMAKE_SYSROOT "${QNX_TARGET}")
# Target libraries live under $QNX_TARGET/aarch64le/{lib,usr/lib}, and the
# shared headers under $QNX_TARGET/usr/include. List the arch dir first so
# find_library() finds aarch64 libraries (e.g. libwpactrl-2.11).
set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}/aarch64le" "${QNX_TARGET}")

# -- Search path behavior --
# Search headers and libraries only under the QNX sysroot, never the host.
# Programs (e.g., code generators) can be found on the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# -- QNX-specific preprocessor defines --
# _QNX_SOURCE: enables QNX extensions beyond strict POSIX
add_definitions(-D_QNX_SOURCE)
