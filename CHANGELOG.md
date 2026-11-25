# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) (while the version is 0.x, a minor bump may break
things). Each release's tag `vX.Y.Z` equals `project(VERSION)` in `CMakeLists.txt`.

## [Unreleased]

### Added
- The project is licensed under the Apache License 2.0 (`LICENSE`, `NOTICE`). The README's License
  section covers the QNX software it's built with and the terms for prebuilt release binaries.
- Host unit tests under CTest: the portable core (`wsa_core`, every source except `main.c`) builds
  for Linux and runs against a link-time fake of libwpactrl that replays recorded scan replies and
  events. `scripts/host-test.sh` runs them natively on Linux, or in the build container on macOS.
- Tests for the regulatory tables, scan parsing (including a reply cut at wpa_supplicant's 4096-byte
  cap), the channel analyzer, the fixed-rate schedule, the scanner's cycle (timeouts, `FAIL-BUSY`,
  monitor re-attach) and the logger (format, order, overflow accounting, shutdown flush, level filter).
- Hosted CI on GitHub Actions: gcc and clang builds, ASan+UBSan and TSan, cppcheck, clang-tidy,
  shellcheck, the Python tests and a tag/version check, summarized by one required `host` check;
  CodeQL and Dependabot.
- `scripts/ci-local.sh`: builds the QNX binary and runs a smoke test on the Pi, posting `qnx/build`
  and `pi/smoke` commit statuses; `--release` attaches the binary and the QNX demonstration EULA
  to a tagged GitHub Release, and `--dry-run` does it all without posting or publishing.
- `scripts/smoke-check.py` (smoke-run verdict), `scripts/anonymize.py` (anonymizes captures for
  fixtures) and `analyze-timing.py --json`.
- `local.env` (git-ignored, template `local.env.example`) for setup-specific settings: the Pi, the
  regulatory country and the SDP location.
- On-target timing capture (`scripts/measure-timing.sh`, which runs `measure-remote.sh` on the Pi)
  and log-based timing analysis (`scripts/analyze-timing.py`) (9011a8b).

### Changed
- Scanner: scans run on a fixed-rate schedule (absolute wake times, skipped slots counted on
  overrun), scan completion is event-driven, and the wake timer can be high-resolution (f7d2956).
- Logger: an asynchronous ring buffer with a dedicated writer thread; when the ring is full, new
  lines are dropped and counted rather than making the real-time threads wait (62137dc).
- Scan parsing, the schedule maths and one analysis pass are separate, testable units
  (`scan_parse.c`, `schedule.c`, `channel_analyzer_analyze()`).
- `deploy.sh` and `measure-timing.sh` take the Pi from the command line or `WSA_PI` instead of a
  built-in host, and the timing runs pass the country from `WSA_COUNTRY`.

### Fixed
- An AP on a frequency outside the channel tables (for example 4.9 GHz) no longer adds overlap
  penalties to channels 1-4, which could change the recommended 2.4 GHz channel.

## [0.2.0] - 2026-11-20

Brings the code in line with the QNX 8.0.5 Quick Start image for the RPi4 (io-sock and the
`qwdi_dhd_sdio-2_11-rpi4` Wi-Fi driver), and fixes the bugs found bringing it up on hardware (debc381).

### Added
- Regulatory-domain filtering (`regdomain.c`): only channels legal in the selected country are
  scored and recommended. Built-in tables for CA (the default), US and 00 (world), selected via
  `-c <CC>`.
- At startup, the app reads wpa_supplicant's country and warns if it's unset or differs.
- A warning when a `SCAN_RESULTS` reply nears wpa_supplicant's 4096-byte cap, past which the AP list
  is cut off.
- `scripts/qnx-build.sh` and `docker/Containerfile`: a linux/amd64 SDP build environment for Apple
  `container` or Docker. `scripts/deploy.sh`: copies the binary, `libwpactrl-2.11.so.1` and a
  launcher to the Pi.

### Changed
- The default interface is `bcm0`, io-sock's name for the radio (`wlan0` doesn't exist there), overridden with `-i`.
- Builds against the SDP's `<wpa_ctrl-2.11.h>` and libwpactrl-2.11, found explicitly by CMake, with
  a static archive preferred.
- `project(VERSION)` is the single version source, used by the startup log line and `-h`.
- Clearer `wpa_ctrl_open()` errors, with hints for a missing socket or missing root.
- The README is rewritten for the Quick Start image.

### Fixed
- The analyzer thread no longer re-analyzes the same scan in a busy loop, and instead waits for a new scan.
- Scan results are parsed in place, with no allocation per scan (the old parser leaked) and with
  empty fields kept, so hidden SSIDs no longer crash the parse. They're parsed into a staging buffer
  and copied in under the lock, which keeps the critical section short.

### Removed
- The vendored `include/wpa_ctrl.h` (hostap 2017), which shadowed the SDP's header.

[Unreleased]: https://github.com/omen-mali/RPI4-Wi-Fi-Spectrum-Analyzer/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/omen-mali/RPI4-Wi-Fi-Spectrum-Analyzer/releases/tag/v0.2.0
