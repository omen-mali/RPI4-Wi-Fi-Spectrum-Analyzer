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
    VERSION   0.0.1
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
)

# ---------------------------------------------------------------------------
# Executable target
# ---------------------------------------------------------------------------
add_executable(${PROJECT_NAME} ${SOURCES})

# Project-local headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
# Threads: on QNX Neutrino pthreads are part of libc and qcc links them
# implicitly. find_package(Threads) handles this portably (no-op on QNX,
# adds -lpthread on Linux, etc.).
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Libraries required on QNX Neutrino:
#   - Threads::Threads : POSIX threads (portable; implicit on QNX)
#   - socket           : BSD socket API (required by wpa_ctrl under the hood)
#   - wpactrl-2.11     : QNX SDP 8.0 wpa_supplicant control interface library
target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
    socket
    wpactrl-2.11
)

# ---------------------------------------------------------------------------
# Install target (optional - for deployment scripting)
# ---------------------------------------------------------------------------
# Installs to /usr/local/bin on the target filesystem by default.
# Override with: cmake --install . --prefix /opt/wifi_analyzer
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)
