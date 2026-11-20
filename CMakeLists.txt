# =============================================================================
# Wi-Fi Spectrum Analyzer - QNX RTOS Application
# Target: Raspberry Pi 4 (BCM2711, aarch64) running QNX Neutrino 8.0
# =============================================================================
#
# Build instructions:
#   1. Source QNX SDP environment:
#        source <SDP_PATH>/qnxsdp-env.sh
#
#   2. Configure with the QNX toolchain file:
#        mkdir build && cd build
#        cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/qnx_aarch64.toolchain.cmake
#
#   3. Build:
#        cmake --build .
#
# The output binary "wifi_spectrum_analyzer" is deployed to the Raspberry Pi.
# =============================================================================

cmake_minimum_required(VERSION 3.20)

project(wifi_spectrum_analyzer
    VERSION   0.2.0
    LANGUAGES C
    DESCRIPTION "QNX RTOS Wi-Fi Spectrum Analyzer for Raspberry Pi 4"
)

# ---------------------------------------------------------------------------
# Compiler standards and warnings
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Best practice: enable strict warnings during development.
# -Wall -Wextra catch common issues; -Werror can be enabled once code is stable.
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wno-unused-parameter   # Allow unused params in stubs during development
)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.c
    src/wifi_scanner.c
    src/channel_analyzer.c
    src/thread_manager.c
    src/logger.c
    src/regdomain.c
)

# ---------------------------------------------------------------------------
# Executable target
# ---------------------------------------------------------------------------
add_executable(${PROJECT_NAME} ${SOURCES})

# Project-local headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Single source of truth for the version: project(VERSION) above.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
# Threads: on QNX Neutrino pthreads are part of libc and qcc links them
# implicitly. find_package(Threads) handles this portably (no-op on QNX,
# adds -lpthread on Linux, etc.).
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# wpa_supplicant control-interface library (QNX SDP 8.0 "WPA2/WPA3/IEEE
# 802.1X Supplicant 2.11 for io-sock" package).
#
# The SDP documents this as a shared library (libwpactrl-<ver>.so). The
# QNX 8.0.5 Quick Start image for the RPi4 ships wpa_supplicant-2.11 but its
# image manifest has no libwpactrl, so a dynamically linked binary may fail to
# load on the target. Prefer a static archive if the SDP has one. Otherwise
# link the .so and copy libwpactrl-2.11.so.1 to the target (e.g. ~/lib with
# LD_LIBRARY_PATH=$HOME/lib), unless `ls /usr/lib | grep wpactrl` on the Pi
# shows it is already there.
# The SDP installs the header VERSIONED, next to other versions:
#   $QNX_TARGET/usr/include/wpa_ctrl-2.11.h   (also wpa_ctrl-2.10.h)
# and the library under the arch dir:
#   $QNX_TARGET/aarch64le/usr/lib/libwpactrl-2.11.so -> .so.1   (no static .a)
# There's no plain wpa_ctrl.h. The header version must match the library, so
# both are pinned to WPA_SUPPLICANT_VERSION. wifi_scanner.c includes
# <wpa_ctrl-2.11.h>.
set(WPA_SUPPLICANT_VERSION 2.11)
find_path(WPACTRL_INCLUDE_DIR NAMES wpa_ctrl-${WPA_SUPPLICANT_VERSION}.h)
if(NOT WPACTRL_INCLUDE_DIR)
    message(FATAL_ERROR
        "wpa_ctrl-${WPA_SUPPLICANT_VERSION}.h not found in "
        "<root>/usr/include for roots: ${CMAKE_FIND_ROOT_PATH}\n"
        "Install \"QNX SDP 8.0 Networking - WPA2/WPA3/IEEE 802.1X Supplicant "
        "${WPA_SUPPLICANT_VERSION} for use with io-sock\" from the QNX Software Center.")
endif()
message(STATUS "wpa_ctrl header: ${WPACTRL_INCLUDE_DIR}/wpa_ctrl-${WPA_SUPPLICANT_VERSION}.h")
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${WPACTRL_INCLUDE_DIR})

find_library(WPACTRL_LIBRARY
    NAMES libwpactrl-${WPA_SUPPLICANT_VERSION}.a wpactrl-${WPA_SUPPLICANT_VERSION})
if(NOT WPACTRL_LIBRARY)
    message(FATAL_ERROR
        "libwpactrl-${WPA_SUPPLICANT_VERSION} not found in <root>/usr/lib or "
        "<root>/lib for roots: ${CMAKE_FIND_ROOT_PATH}\n"
        "Install \"QNX SDP 8.0 Networking - WPA2/WPA3/IEEE 802.1X Supplicant "
        "${WPA_SUPPLICANT_VERSION} for use with io-sock\" from the QNX Software Center.")
endif()
if(WPACTRL_LIBRARY MATCHES "\\.a$")
    message(STATUS "libwpactrl: ${WPACTRL_LIBRARY} (static)")
else()
    message(WARNING
        "libwpactrl linked dynamically (${WPACTRL_LIBRARY}). Deploy "
        "libwpactrl-2.11.so.1 to the target unless it is already installed.")
endif()

# Libraries required on QNX Neutrino:
#   - Threads::Threads : POSIX threads (portable; implicit on QNX)
#   - WPACTRL_LIBRARY  : wpa_supplicant control interface (see above)
#   - socket           : BSD socket API used by wpa_ctrl; must follow it when
#                        wpactrl is a static archive
target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
    ${WPACTRL_LIBRARY}
    socket
)

# ---------------------------------------------------------------------------
# Install target (optional - for deployment scripting)
# ---------------------------------------------------------------------------
# Installs to /usr/local/bin on the target filesystem by default.
# Override with: cmake --install . --prefix /opt/wifi_analyzer
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)
