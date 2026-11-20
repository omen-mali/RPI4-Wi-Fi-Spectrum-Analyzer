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
  |     |
  |     +-- regdomain.c    Country (-c, default CA) -> legal channels
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
- **CMake 3.20+** -- Build system generator
- A host that can run the x86-64 Linux SDP tools:
  - **macOS (Apple silicon):** Apple `container` or Docker Desktop/OrbStack. See [Building](#building).
  - **Linux x86-64:** native.
  - **Windows:** install **QNX SDP Host Tools (Linux 64-bit)** and build in WSL.

### Target (Raspberry Pi 4)

- Raspberry Pi 4 (2GB+ RAM)
- 16 GB+ microSD card flashed with the **QNX 8.0 Quick Start Target Image for RPi4**
  (QNX Software Center: *Reference Images*, `com.qnx.qnx800.quickstart.rpi4`; developed against 8.0.5)
- Network connectivity (Ethernet or Wi-Fi for SSH; the onboard Wi-Fi radio `bcm0` is used for scanning)
- Optional: 3.3 V USB-to-TTL serial cable for the console

The Quick Start image already starts `io-sock` (with the `genet` Ethernet and
`qwdi_dhd_sdio-2_11-rpi4` Wi-Fi drivers), `dhcpcd`, `wpa_supplicant-2.11`, and `sshd`.
You don't need to start the network stack by hand.

## Building

The build needs the SDP package *"Networking - WPA2/WPA3/IEEE 802.1X Supplicant 2.11 for use with
io-sock"*. It installs a **versioned** header, `$QNX_TARGET/usr/include/wpa_ctrl-2.11.h` (there's no
plain `wpa_ctrl.h`), and `$QNX_TARGET/aarch64le/usr/lib/libwpactrl-2.11.so`. CMake fails with a clear
message if either is missing. The repo doesn't vendor a copy, so the QNX header is always used.

### macOS (Apple silicon): container build (recommended)

The QNX SDP 8.0 host tools exist only for x86-64 Linux and Windows. On a Mac, run them in a
`linux/amd64` container under Rosetta. The SDP (`~/qnx800`) and license (`~/.qnx`) are
bind-mounted, not copied into the image.

```bash
scripts/qnx-build.sh          # builds the image on first run, then cross-compiles into build/qnx
scripts/qnx-build.sh shell    # shell with qcc/QNX_TARGET set, for poking around
scripts/qnx-build.sh clean    # wipe build/qnx and rebuild
```

The script prefers Apple's [`container`](https://github.com/apple/container) CLI (macOS 26+,
`container system start`) and falls back to Docker Desktop or OrbStack. Force one with
`CONTAINER_RUNTIME=docker`. The image is just Ubuntu 24.04 plus CMake/Ninja; see
`docker/Containerfile`.

### Linux x86-64 host (native)

```bash
source ~/qnx800/qnxsdp-env.sh
cmake -S . -B build/qnx -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/qnx_aarch64.toolchain.cmake
cmake --build build/qnx
```

The output binary is `build/qnx/wifi_spectrum_analyzer`.

### libwpactrl on the target

The QNX 8.0.5 Quick Start image doesn't include `libwpactrl`. CMake links a static
`libwpactrl-2.11.a` if the SDP has one. Otherwise it links the `.so` (with a warning), and
`scripts/deploy.sh` copies `libwpactrl-2.11.so.1` to `~/lib` on the Pi along with a launcher that
sets `LD_LIBRARY_PATH`.

## Deploying to the Raspberry Pi

### 1. Prepare the SD Card

Flash the Quick Start image (`.img`) to the whole card with Raspberry Pi Imager or balenaEtcher.
The card gets a FAT boot partition (`QNX_RPI4`) plus QNX6 system and data partitions.

Then edit these files on the boot partition before first boot. At boot they're copied into `/data/var/etc/settings/`.

| File | Purpose |
| --- | --- |
| `config.txt` | Pi firmware config. Keep `enable_uart=1` and `force_turbo=1` (and `core_freq=500`): the serial console runs on the mini UART, whose baud rate depends on the core clock. Don't add `dtoverlay=disable-bt`/`miniuart-bt` or overclock settings. |
| `network` | `HOSTNAME=...`, `IP_ADDR=dhcp`, `TIME_SERVERS=...` |
| `wpa_supplicant.conf` | Must keep `ctrl_interface=/var/run/wpa_supplicant`. Add `country=CA` (or your country) so the radio uses your regulatory domain instead of the passive world domain, plus a `network={ ssid=... psk=... }` block to join Wi-Fi. |

Eject the card cleanly on **macOS** *specifically*; an unclean eject can truncate files on the FAT partition.

### 2. Connect

- **SSH:** `ssh qnxuser@<hostname>.local` or `ssh qnxuser@<PI_IP>`. The default password is `qnxuser`; change it with `passwd`.
- **Serial console:** 3.3 V USB-TTL cable, adapter RX to pin 8 (GPIO14), adapter TX to pin 10 (GPIO15),
  GND to pin 6, VCC not connected. 115200 8N1, e.g. `screen /dev/tty.usbserial-XXXX 115200`.
  The console is a root shell on `/dev/ser1`.

Check the Wi-Fi side:

```bash
ifconfig bcm0                    # the onboard Wi-Fi interface (not wlan0)
ls -l /var/run/wpa_supplicant/   # should contain the bcm0 control socket
```

### 3. Transfer the Binary

```bash
scripts/deploy.sh                          # default target: qnxuser@qnx-wifi-analyzer.local
scripts/deploy.sh qnxuser@<PI_IP>
```

This installs `~/bin/wifi_spectrum_analyzer`, a `~/bin/wifi-analyzer` launcher, and (for a dynamic
build) `~/lib/libwpactrl-2.11.so.1`. Avoid `/tmp`: on this image it's `/data/tmp` and gets cleaned at boot.

### 4. Run the Application

The wpa_supplicant control socket is root-only, and SCHED_FIFO/`procmgr_ability()` need root,
so run it with `sudo` (or from the serial root console):

```bash
sudo ~/bin/wifi-analyzer                 # bcm0, country CA (the defaults)
sudo ~/bin/wifi-analyzer -c US           # other regulatory domain
sudo ~/bin/wifi-analyzer -i <iface>      # other interface
```

| Option | Default | Meaning |
| --- | --- | --- |
| `-i <iface>` | `bcm0` | Interface whose socket `/var/run/wpa_supplicant/<iface>` is used |
| `-c <CC>` | `CA` (`WIFI_COUNTRY_DEFAULT` in `config.h`) | Regulatory domain: only channels legal there are recommended. Built-in tables: `CA`, `US`, `00` (world). Unknown codes fall back to `00`. |

Canada, for example, excludes 2.4 GHz channels 12-14 and 5 GHz channels 120-128 (weather radar).
At startup the app reads wpa_supplicant's `country` and warns if it's unset or differs from `-c`.
The radio's own regulatory domain comes only from `country=` in `/boot/wpa_supplicant.conf`.

The application logs to stderr. Press `Ctrl+C` to stop.

### Troubleshooting

| Log message | Likely cause |
| --- | --- |
| `wpa_ctrl_open(...) failed ... No such file or directory` | Wrong interface name, or wpa_supplicant isn't running on it. Check `ls /var/run/wpa_supplicant/`. |
| `wpa_ctrl_open(...) failed ... Permission denied` | Not running as root. |
| `SCAN_RESULTS reply is ... near wpa_supplicant's 4096-byte cap` | Too many APs nearby; wpa_supplicant truncates `SCAN_RESULTS` at about 50-60 entries. |
| `could not load library libwpactrl-2.11.so.1` | Linked dynamically and the `.so` isn't on the target, or the binary was started directly instead of through `~/bin/wifi-analyzer`. Rerun `scripts/deploy.sh`. |
| `wpa_supplicant has no country set` | Add `country=CA` to `/boot/wpa_supplicant.conf` and reboot. |

## Project Structure

```
.
├── CMakeLists.txt                    Top-level build configuration
├── cmake/
│   └── qnx_aarch64.toolchain.cmake  Cross-compilation toolchain file
├── docker/
│   ├── Containerfile                 linux/amd64 QNX SDP build image (container/Docker)
│   └── qnx-entrypoint.sh             Sources qnxsdp-env.sh inside the container
├── scripts/
│   ├── qnx-build.sh                  Cross-compile in the container
│   └── deploy.sh                     Copy binary (+ libwpactrl) and launcher to the Pi
├── include/
│   ├── config.h                      Constants, data types, tunables
│   ├── wifi_scanner.h                Scanner module API
│   ├── channel_analyzer.h            Analyzer module API
│   ├── regdomain.h                   Country -> legal channel tables
│   ├── thread_manager.h              Thread lifecycle API
│   └── logger.h                      Logging API
├── src/
│   ├── main.c                        Entry point, CLI (-i/-c), orchestration
│   ├── wifi_scanner.c                wpa_ctrl-based Wi-Fi scanning
│   ├── channel_analyzer.c            Channel scoring and ranking
│   ├── regdomain.c                   Built-in CA / US / world tables
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
| Reentrant in-place parsing | wifi_scanner.c (`next_field`)      | Thread-safe, no allocation, keeps empty fields (hidden SSIDs) |
| Check all return values    | All files                          | Robust error handling for RTOS reliability |


## Extending the Project

- **40/80/160 MHz channel widths**: Extend the analyzer to evaluate bonded channel pairs and their combined interference profile.
- **Continuous monitoring UI**: Add a QNX Screen or ncurses-based display showing real-time channel utilization.
- **Syslog integration**: Replace stderr logging with QNX slogger2 for persistent, structured logging.
- **REST/IPC interface**: Expose results via a QNX resource manager or a lightweight HTTP server for remote monitoring.
- **More regulatory domains**: Add tables to `src/regdomain.c` (only CA, US and world are built in), or read them from wpa_supplicant at runtime.

