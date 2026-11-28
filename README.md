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
scripts/deploy.sh qnxuser@<PI_IP>          # or omit it and set WSA_PI in local.env
```

Settings that belong to your setup (the Pi's address, the regulatory country, the SDP location) are
read from `local.env` at the repo root, which git ignores, or from the file `WSA_LOCAL_ENV` names.
Copy `local.env.example` to start one.

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
| `-p <ms>` | `10000` (`SCAN_PERIOD_MS`) | Fixed scan period, SCAN to SCAN (1000-600000). Scans start on an absolute CLOCK_MONOTONIC schedule, so scan duration doesn't affect the rate. A cycle that runs past its deadline skips the missed slot(s) and logs an overrun. |
| `-T <ns>` | `0` (`WAKE_TOLERANCE_NS_DEFAULT`) | Wake timer for the schedule. `0`: `clock_nanosleep`, aligned to the 1 ms system tick (wakes up to ~1 ms late). `1`-`999999`: QNX high-resolution timer with that tolerance (measured 5-11 µs late with `-T 1000`). Needs root. The tick itself can't be changed at run time on QNX 8. |

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
| `Scan cycle overran its N ms period` | A scan took longer than `-p`. A full 2.4 + 5 GHz scan takes about 5 s (longer without `country=`), so keep the period well above that. |
| `wpa_supplicant has no country set` | Add `country=CA` to `/boot/wpa_supplicant.conf` and reboot. |

## Testing and CI

### Host unit tests

The portable core (every source except `main.c`) also builds for Linux, where CTest runs it
against a link-time fake of libwpactrl (`tests/fakes/`) that replays recorded `SCAN_RESULTS` replies
and scan events from `tests/fixtures/`. Nothing needs the Pi or the QNX SDP.

```bash
scripts/host-test.sh                 # gcc, no sanitizer
scripts/host-test.sh clang asan      # [gcc|clang] [asan|tsan|none]
python3 -m unittest discover -s tests/scripts   # the Python scripts' tests
```

On Linux the build runs natively and needs cmake, ninja and the compiler. On macOS the script re-runs
itself in the linux/amd64 build image (`docker/Containerfile`, without the SDP), because the host
build needs `clock_nanosleep`, which macOS lacks. `tsan` only runs on native x86-64 Linux:
ThreadSanitizer aborts under Rosetta, so it runs in the hosted CI. Each compiler and sanitizer pair
builds in `build/host-<cc>-<san>/`.

### Hosted CI (GitHub Actions)

`.github/workflows/host.yml` runs on every push and on pull requests into `main` and `dev`:

| Job | What it checks |
| --- | --- |
| `build-test` | the host tests with gcc and with clang |
| `sanitizers` | clang with ASan+UBSan, and with TSan |
| `static` | cppcheck on `src/`, and clang-tidy (`.clang-tidy`) on the host-build sources |
| `scripts` | shellcheck on `scripts/*.sh`, and the Python tests |
| `version` | on `v*` tags only: the tag must equal `project(VERSION)` in `CMakeLists.txt` |
| `host` | passes only if all of the above did (this is the required check) |

`codeql.yml` runs CodeQL on pull requests into `main` and weekly, and Dependabot keeps the
SHA-pinned actions current.

### QNX build and Pi smoke run (`ci-local.sh`)

Hosted runners can't build for QNX (the SDP is licensed and x86-64 only) or reach the Pi, so
`scripts/ci-local.sh` does both locally and posts the results as commit statuses via `gh`:

```bash
scripts/ci-local.sh                      # QNX build, then a 60 s smoke run on the Pi
scripts/ci-local.sh --no-pi              # QNX build only
scripts/ci-local.sh --pi qnxuser@<PI_IP> --seconds 120
```

Must run from a clean checkout of a commit that's on GitHub (it refuses otherwise). Setup-specific settings come from `local.env`
(see `local.env.example`), and the pass limits are in `scripts/ci-thresholds.env`.

| Status | Success means | Error means |
| --- | --- | --- |
| `host` | every hosted job passed | at least one hosted job failed |
| `qnx/build` | `scripts/qnx-build.sh clean` built the binary (size and warning count in the description) | `ci-local.sh` was interrupted |
| `pi/smoke` | the app ran on the Pi with no ERROR lines, a clean shutdown, enough completed scans at the period it logged, logged wake latency with p99 under the limit, and at least one AP (`scripts/smoke-check.py`) | the Pi was unreachable or the run timed out, which isn't a failed build |

The full report (build output, the Pi's logs, the verdict) goes to `build/ci/<sha>/`. Nothing under
`build/` is committed.

Note: `pi/smoke` is reported but not required, so a merge doesn't wait on the Pi being plugged in.

### Releases

Tag `main` with `vX.Y.Z`, equal to `project(VERSION)` in `CMakeLists.txt`, add a `## [X.Y.Z]` section
to `CHANGELOG.md`, and push both. Then `scripts/ci-local.sh --release` builds the binary for that tag
and attaches it to the GitHub Release as `wifi_spectrum_analyzer-vX.Y.Z-qnx800-aarch64le`, with the
changelog section as its notes, and attaches the QNX demonstration EULA beside it (see
[License](#license)). It won't publish a binary whose `pi/smoke` failed.

Add `--dry-run` to try any of this without touching GitHub: everything is built and run, but no
status is posted and nothing is published. The script prints what it would post and leaves the
release assets and notes in `build/ci/<sha>/`, and the commit and tag needn't be pushed yet.

### Adding a test fixture

Fixtures are committed, so they must not contain real network names, BSSIDs, IP or MAC addresses.
Captures must be anonymized before they are added:

```bash
# on the Pi:  wpa_cli -i bcm0 scan_results > scan.txt
scripts/anonymize.py scan scan.txt tests/fixtures/scan/<name>.txt
scripts/anonymize.py log  run.log  tests/fixtures/timing/<name>.txt
```

BSSIDs and MACs become `02:00:00:xx:xx:xx` and SSIDs become `net-NN`, numbered in first-seen order,
and the same input always gives the same output. Check the result by eye before committing it.

## Project Structure

```
.
├── CMakeLists.txt                    Top-level build configuration
├── cmake/
│   └── qnx_aarch64.toolchain.cmake   Cross-compilation toolchain file
│
├── docker/
│   ├── Containerfile                 linux/amd64 QNX SDP build image (container/Docker)
│   └── qnx-entrypoint.sh             Sources qnxsdp-env.sh inside the container
│
├── .github/                          Hosted CI (host.yml, codeql.yml) and Dependabot
├── local.env.example                 Template for setup-specific settings (local.env)
├── CHANGELOG.md                      Changes per release
├── scripts/
│   ├── qnx-build.sh                  Cross-compile in the container
│   ├── deploy.sh                     Copy binary (+ libwpactrl) and launcher to the Pi
│   ├── host-test.sh                  Build and run the host unit tests
│   ├── ci-local.sh                   QNX build + Pi smoke run, posted as commit statuses
│   ├── measure-timing.sh             Timing capture on the Pi (runs measure-remote.sh there)
│   ├── analyze-timing.py             Timing statistics from analyzer logs
│   ├── smoke-check.py                Pass/fail verdict for a smoke-run log
│   └── anonymize.py                  Anonymize captures before committing them as fixtures
│
├── include/
│   ├── config.h                      Constants, data types, tunables
│   ├── wifi_scanner.h                Scanner module API
│   ├── channel_analyzer.h            Analyzer module API
│   ├── regdomain.h                   Country -> legal channel tables
│   ├── scan_parse.h                  SCAN_RESULTS parsing
│   ├── schedule.h                    Fixed-rate schedule maths
│   ├── thread_manager.h              Thread lifecycle API
│   └── logger.h                      Logging API
│
├── src/
│   ├── main.c                        Entry point, CLI (-i/-c), orchestration
│   ├── wifi_scanner.c                wpa_ctrl-based Wi-Fi scanning
│   ├── channel_analyzer.c            Channel scoring and ranking
│   ├── regdomain.c                   Built-in CA / US / world tables
│   ├── scan_parse.c                  SCAN_RESULTS rows -> scan entries
│   ├── schedule.c                    Next wake time and skipped slots
│   ├── thread_manager.c              POSIX RT thread management
│   └── logger.c                      Thread-safe logging
│
├── tests/                            Host unit tests, the fake libwpactrl, fixtures
└── README.md                         This file
```

## Best Practices Reference

These practices are documented in code comments throughout the project:


| Practice                   | Where                              | Why                                        |
| -------------------------- | ---------------------------------- | ------------------------------------------ |
| No malloc in RT paths      | wifi_scanner.c, channel_analyzer.c | Deterministic timing (malloc can block)     |
| PTHREAD_PRIO_INHERIT       | logger.c, main.c                   | Prevents priority inversion                |
| PTHREAD_EXPLICIT_SCHED     | thread_manager.c                   | Ensures threads get the attributes they are set with |
| clock_nanosleep(TIMER_ABSTIME) | wifi_scanner.c                 | Fixed-rate, drift-free scan schedule with overrun detection |
| poll() on wpa_ctrl fd      | wifi_scanner.c                     | Event-driven scan completion, no polling quantization |
| sigwait() pattern          | main.c                             | Safe signal handling in multithreaded apps |
| Reentrant in-place parsing | scan_parse.c (`next_field`)        | Thread-safe, no allocation, keeps empty fields (hidden SSIDs) |
| Check all return values    | All files                          | Robust error handling for RTOS reliability |


## Extending the Project

- **40/80/160 MHz channel widths**: Extend the analyzer to evaluate bonded channel pairs and their combined interference profile.
- **Continuous monitoring UI**: Add a QNX Screen or ncurses-based display showing real-time channel utilization.
- **Syslog integration**: Replace stderr logging with QNX slogger2 for persistent, structured logging.
- **REST/IPC interface**: Expose results via a QNX resource manager or a lightweight HTTP server for remote monitoring.
- **More regulatory domains**: Add tables to `src/regdomain.c` (only CA, US and world are built in), or read them from wpa_supplicant at runtime.

## License

This repository's own code, scripts and docs are licensed under the [Apache License 2.0](LICENSE).
See [NOTICE](NOTICE) for the copyright line.

The software it's built with and runs on is not in this repository and is not covered by that
license:

- **QNX Software Development Platform 8.0.** Building needs the SDP (compiler, headers and
  libraries), which is BlackBerry's software under its own license: a commercial license, or the
  free non-commercial license from the QNX Everywhere program. It is not redistributed here, and must be acquired from QNX.
- **Libraries the binary links.** QNX's C library and libsocket, and libwpactrl-2.11 from the SDP's
  wpa_supplicant package. libwpactrl comes from the open-source hostap project (BSD license). The
  build shipped in the SDP is delivered under the SDP's terms.
- **The target.** Running the app needs a QNX Neutrino 8.0 image for the Raspberry Pi 4 (the QNX
  Quick Start image), which has its own license.

Apache-2.0 lets anyone use, change and redistribute this code, including commercial use. Building or
running it still needs a QNX license that fits the use: the non-commercial license doesn't cover
commercial development or products.

**Prebuilt binaries.** Binaries attached to GitHub Releases are built with a non-commercial QNX
license. That license allows sharing them only as demonstrations for non-commercial evaluation,
with the recipient accepting its demonstration EULA, so each release carries that EULA as
`QNX-DEMO-EULA.txt`. The releases are not meant for redistribution or commercially use, build your own under your
own QNX license instead for those purposes. To publish releases, set `WSA_QNX_DEMO_EULA` in `local.env` to a text copy
of the demonstration EULA (Exhibit B of the QNX Development License, Non-Commercial); see
`local.env.example`.
