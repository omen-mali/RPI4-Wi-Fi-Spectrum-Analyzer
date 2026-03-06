# Wi-Fi Spectrum Analyzer -- QNX RTOS on Raspberry Pi 4

A real-time Wi-Fi spectrum analysis application for the Raspberry Pi 4 running QNX Neutrino RTOS. Scans 2.4 GHz and 5 GHz bands and recommends the optimal channel based on interference analysis.

## Architecture

```
main.c                     Startup, signal handling, thread orchestration
  |
  +-- wifi_scanner.c       Polls wpa_supplicant for scan data (SCHED_FIFO thread)
  |     |
  |     +-- wpa_ctrl API   libwpactrl-2.11 (SCAN / SCAN_RESULTS commands)
  |
  +-- channel_analyzer.c   Scores channels, picks optimal (SCHED_RR thread)
  |
  +-- thread_manager.c     POSIX thread creation with RT scheduling attributes
  |
  +-- logger.c             Thread-safe, priority-inherit mutex-protected logging
  |
  +-- config.h             All constants, data structures, tunables
```

The scanner and analyzer communicate through a shared `scan_buffer_t` protected by a POSIX mutex with `PTHREAD_PRIO_INHERIT`. A condition variable signals the analyzer when new data is ready.

## Prerequisites

### Host Machine (Development)

- **QNX SDP 8.0** -- Download from the [QNX Software Center](https://www.qnx.com/download/)
  - Install **QNX® SDP 8.0 Networking - WPA2/WPA3/IEEE 802.1X Supplicant 2.11 for use with io-sock**
- **QNX BSP for Raspberry Pi 4 BCM2711** -- Available in the QNX Software Center (search "Raspberry Pi")
- **CMake 3.20+** -- Build system generator
- A Linux or Windows host with the QNX SDP installed
  - If on Windows, install **QNX SDP Host Tools (Linux 64-bit)**, and build using a Linux environment (e.g. WSL)

### Target (Raspberry Pi 4)

- Raspberry Pi 4 (2GB+ RAM)
- 8 GB+ microSD card flashed with the QNX BSP image (`ifs-rpi4.bin`)
- Network connectivity (Ethernet for SSH; Wi-Fi interface for scanning)
- `wpa_supplicant` running with a configured control interface

## Setting Up the QNX SDP Environment

```bash
# Source the QNX SDP environment (sets QNX_HOST, QNX_TARGET, PATH)
source /path/to/qnx-sdp-8.0/qnxsdp-env.sh

# Verify the environment
echo $QNX_HOST    # Should point to the host tools directory
echo $QNX_TARGET  # Should point to the target sysroot
which qcc         # Should find the QNX compiler driver
```

## Building

```bash
# From the project root directory:
mkdir build && cd build

# Configure with the QNX cross-compilation toolchain
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/qnx_aarch64.toolchain.cmake

# Build
cmake --build .
```

The output binary is `build/wifi_spectrum_analyzer`.

## Deploying to the Raspberry Pi

### 1. Prepare the SD Card

Follow the QNX BSP documentation to flash `ifs-rpi4.bin` onto the microSD card. The BSP includes the QNX Neutrino kernel, drivers, and base filesystem.

### 2. Transfer the Binary

```bash
# Via SCP (ensure sshd is running on the Pi)
scp build/wifi_spectrum_analyzer root@<PI_IP>:/tmp/

# Or mount the SD card and copy directly to the filesystem image
```

### 3. Configure wpa_supplicant on the Target

Ensure `/etc/wpa_supplicant.conf` exists with at least:

```
ctrl_interface=/var/run/wpa_supplicant
update_config=1
```

Start the Wi-Fi stack on the target:

```bash
# Start the network stack (if not already running)
io-pkt-v6-hc -d bcm43xx

# Wait for the interface to come up
ifconfig wlan0 up

# Start wpa_supplicant
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
```

### 4. Run the Application

```bash
# On the Raspberry Pi (via SSH or serial console)
chmod +x /tmp/wifi_spectrum_analyzer
/tmp/wifi_spectrum_analyzer
```

The application logs to stderr. Press `Ctrl+C` to stop.

## Project Structure

```
.
├── CMakeLists.txt                    Top-level build configuration
├── cmake/
│   └── qnx_aarch64.toolchain.cmake  Cross-compilation toolchain file
├── include/
│   ├── config.h                      Constants, data types, tunables
│   ├── wifi_scanner.h                Scanner module API
│   ├── channel_analyzer.h            Analyzer module API
│   ├── thread_manager.h              Thread lifecycle API
│   └── logger.h                      Logging API
├── src/
│   ├── main.c                        Entry point and orchestration
│   ├── wifi_scanner.c                wpa_ctrl-based Wi-Fi scanning
│   ├── channel_analyzer.c            Channel scoring and ranking
│   ├── thread_manager.c              POSIX RT thread management
│   └── logger.c                      Thread-safe logging
└── README.md                         This file
```

## Best Practices Reference

These practices are documented in code comments throughout the project:


| Practice                   | Where                              | Why                                        |
| -------------------------- | ---------------------------------- | ------------------------------------------ |
| No malloc in RT paths      | wifi_scanner.c, channel_analyzer.c | Deterministic timing; malloc can block     |
| PTHREAD_PRIO_INHERIT       | logger.c, main.c                   | Prevents priority inversion                |
| PTHREAD_EXPLICIT_SCHED     | thread_manager.c                   | Ensures threads get the attributes you set |
| clock_nanosleep(MONOTONIC) | wifi_scanner.c                     | Drift-free periodic scanning               |
| sigwait() pattern          | main.c                             | Safe signal handling in multithreaded apps |
| strtok_r() over strtok()   | wifi_scanner.c                     | Thread-safe string parsing                 |
| Check all return values    | All files                          | Robust error handling for RTOS reliability |


## Extending the Project

- **40/80/160 MHz channel widths**: Extend the analyzer to evaluate bonded channel pairs and their combined interference profile.
- **Continuous monitoring UI**: Add a QNX Screen or ncurses-based display showing real-time channel utilization.
- **Syslog integration**: Replace stderr logging with QNX slogger2 for persistent, structured logging.
- **REST/IPC interface**: Expose results via a QNX resource manager or a lightweight HTTP server for remote monitoring.
- **Regulatory domain awareness**: Filter channel lists based on the configured country code.

