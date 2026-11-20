/**
 * @file config.h
 * @brief Project-wide configuration, constants, and data type definitions.
 *
 * This header centralizes all tunable parameters so they can be adjusted
 * without modifying application logic. It also defines the core data
 * structures shared between the scanner and analyzer modules.
 *
 * Best practices:
 *   - Keep all "magic numbers" here as named constants.
 *   - Use fixed-size arrays (not dynamic allocation) for real-time safety.
 *   - Prefer stdint.h types (int32_t, etc.) for explicit width guarantees.
 */

#ifndef WIFI_SPECTRUM_CONFIG_H
#define WIFI_SPECTRUM_CONFIG_H

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Build / Platform Guards
 * ========================================================================= */

/**
 * Application version, injected by CMake from project(VERSION ...).
 * Builds that bypass CMake (e.g. host-side syntax checks) report "unknown".
 */
#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

#ifndef __QNX__
    #warning "This project targets QNX Neutrino RTOS. Building on other platforms may require stubs."
#endif

/* =========================================================================
 * Wi-Fi Band and Channel Definitions
 * ========================================================================= */

/**
 * @brief Wi-Fi frequency band identifiers.
 */
typedef enum {
    BAND_2_4_GHZ = 0,  /**< 2.4 GHz ISM band (802.11b/g/n) (1-13) */
    BAND_5_GHZ   = 1,  /**< 5 GHz UNII band (802.11a/n/ac) (36-165) */
    BAND_DFS_GHZ = 2,  /**< 5 GHz DFS band (802.11a/n/ac) (52-144) */
    BAND_COUNT   = 3
} wifi_band_t;

/**
 * 2.4 GHz band: channels 1-14
 *   - Channels 1, 6, 11 are the only non-overlapping 20 MHz channels.
 *   - Each channel is 5 MHz apart; each occupies ~22 MHz of bandwidth.
 *   - Channel 14 (2484 MHz) is Japan-only; include for completeness.
 *
 * 5 GHz band: channels 36-165 (common UNII-1 through UNII-3)
 *   - Channels are spaced 20 MHz apart; no overlap at 20 MHz width.
 *   - DFS channels (52-144) may be restricted depending on regulatory domain.
 */

/** Maximum number of channels per band we track. */
#define MAX_CHANNELS_2_4GHZ    14
#define MAX_CHANNELS_5GHZ      25   /* Common 5 GHz channels */
#define MAX_CHANNELS_TOTAL     (MAX_CHANNELS_2_4GHZ + MAX_CHANNELS_5GHZ)

/**
 * @brief Static information about a single Wi-Fi channel.
 */
typedef struct {
    uint8_t  channel_number;    /**< Channel number (e.g. 1, 6, 36, 149) */
    uint16_t center_freq_mhz;  /**< Center frequency in MHz (e.g. 2412, 5180) */
    wifi_band_t band;           /**< Which band this channel belongs to */
} channel_info_t;

/* =========================================================================
 * Scan Result Structures
 * ========================================================================= */

/** Maximum number of access points we store per scan cycle. */
#define MAX_SCAN_RESULTS  128

/** Maximum length of an SSID (IEEE 802.11 limit is 32 bytes). */
#define MAX_SSID_LEN      33  /* 32 chars + null terminator */

/** BSSID string length: "AA:BB:CC:DD:EE:FF" + null. */
#define BSSID_STR_LEN     18

/**
 * @brief A single scan result entry representing one detected access point.
 *
 * Populated by the wifi_scanner module from wpa_supplicant SCAN_RESULTS.
 */
typedef struct {
    char     bssid[BSSID_STR_LEN];  /**< AP MAC address as string */
    char     ssid[MAX_SSID_LEN];    /**< Network name (may be empty for hidden) */
    uint16_t freq_mhz;              /**< Operating frequency in MHz */
    uint8_t  channel;               /**< Derived channel number */
    int8_t   signal_dbm;            /**< Signal level in dBm (negative value) */
    wifi_band_t band;               /**< Band this AP operates on */
} scan_result_t;

/**
 * @brief Container for a complete scan cycle's results.
 *
 * This structure is shared between the scanner (producer) and analyzer
 * (consumer) threads. Access MUST be protected by the accompanying mutex.
 *
 * Best practice: Use a condition variable to signal the analyzer when
 * new data is available, rather than polling.
 */
typedef struct {
    scan_result_t entries[MAX_SCAN_RESULTS]; /**< Fixed-size result buffer */
    size_t        count;                     /**< Number of valid entries */
    uint64_t      timestamp_ns;              /**< Scan completion time (CLOCK_MONOTONIC) */
} scan_buffer_t;

/* =========================================================================
 * Channel Analysis Structures
 * ========================================================================= */

/**
 * @brief Per-channel analysis metrics computed by the analyzer module.
 */
typedef struct {
    uint8_t  channel_number;
    uint16_t freq_mhz;
    wifi_band_t band;

    uint16_t ap_count;          /**< Number of APs detected on this channel */
    int8_t   avg_signal_dbm;    /**< Average signal strength of APs (dBm) */
    int8_t   max_signal_dbm;    /**< Strongest AP on this channel (dBm) */

    /**
     * Channel score: higher is better (fewer APs, weaker interference).
     * The scoring algorithm is implemented in channel_analyzer.c.
     */
    float    score;
} channel_metric_t;

/**
 * @brief Analysis results: the best channel recommendation per band.
 */
typedef struct {
    channel_metric_t channels[MAX_CHANNELS_TOTAL]; /**< Metrics for all channels */
    size_t           channel_count;                 /**< How many channels were analyzed */

    uint8_t best_channel_2_4ghz;  /**< Recommended channel in 2.4 GHz band */
    uint8_t best_channel_5ghz;    /**< Recommended channel in 5 GHz band */
    float   best_score_2_4ghz;    /**< Score of the best 2.4 GHz channel */
    float   best_score_5ghz;      /**< Score of the best 5 GHz channel */
} analysis_result_t;

/* =========================================================================
 * Tunable Runtime Parameters
 * ========================================================================= */

/**
 * Scan period in milliseconds: the time from one SCAN request to the next.
 *
 * The scanner runs on a fixed-rate schedule. Cycle N starts at
 * t0 + N * period, using clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) on
 * a deadline that advances by exactly one period, so scan duration and
 * wpa_supplicant latency don't change the rate or accumulate drift.
 *
 * A full scan (2.4 + 5 GHz) on the CYW43455 (Raspberry Pi 4 Model B) takes 
 * ~5 s, so the period must leave headroom. A cycle that runs past its 
 * deadline counts as an overrun: the missed slot(s) are skipped and the 
 * schedule stays phase-aligned.
 *
 * Override at runtime with "-p <ms>" (SCAN_PERIOD_MS_MIN .. SCAN_PERIOD_MS_MAX).
 */
#define SCAN_PERIOD_MS          10000u
#define SCAN_PERIOD_MS_MIN       1000u
#define SCAN_PERIOD_MS_MAX     600000u

/**
 * Upper bound on waiting for CTRL-EVENT-SCAN-RESULTS after SCAN is accepted.
 * The wait blocks in poll() on the monitor socket, so completion is seen
 * as soon as the event arrives.
 */
#define SCAN_COMPLETE_TIMEOUT_MS 30000

/**
 * Wake-up timer tolerance for the scan schedule, in nanoseconds.
 *
 * 0 (default): clock_nanosleep(TIMER_ABSTIME). On QNX its expiry is aligned
 *    to the system tick (1 ms by default, set only at boot by procnto -C;
 *    ClockPeriod() can't change it at run time in QNX 8), so the wake-up
 *    lands up to one tick after the deadline.
 * 1 .. WAKE_TOLERANCE_NS_MAX: QNX high-resolution timer. TimerTimeout()
 *    with TIMER_TOLERANCE, then an absolute _NTO_TIMEOUT_NANOSLEEP. The
 *    timer hardware is programmed for the deadline itself. Needs
 *    PROCMGR_AID_HIGH_RESOLUTION_TIMER (root has it by default). Ignored on
 *    non-QNX hosts.
 *
 * Override at runtime with "-T <ns>".
 */
#define WAKE_TOLERANCE_NS_DEFAULT 0u
#define WAKE_TOLERANCE_NS_MAX     999999u

/**
 * wpa_supplicant control interface.
 *
 * wpa_supplicant creates one UNIX domain socket per interface, named after
 * the interface, inside the ctrl_interface directory:
 *
 *     <WPA_CTRL_DIR>/<interface>     e.g. /var/run/wpa_supplicant/bcm0
 *
 * On the QNX 8.0 Quick Start image for the RPi4, io-sock loads the
 * qwdi_dhd_sdio-2_11-rpi4 driver, which names the onboard CYW43455 radio
 * "bcm0" (Ethernet is "genet0"). The image's wpa_supplicant.conf sets
 * ctrl_interface=/var/run/wpa_supplicant.
 *
 * The interface can be overridden at runtime with "-i <iface>".
 */
#define WPA_CTRL_DIR          "/var/run/wpa_supplicant"
#define WIFI_IFACE_DEFAULT    "bcm0"

/** Max interface name length incl. NUL (matches IFNAMSIZ). */
#define WIFI_IFACE_NAME_MAX   16

/** Max control socket path length incl. NUL (sizeof(sockaddr_un.sun_path)). */
#define WPA_CTRL_PATH_MAX     104

/**
 * Default regulatory domain (ISO 3166-1 alpha-2) used to filter which
 * channels the analyzer may recommend. Override at runtime with "-c <CC>".
 * Keep it consistent with "country=" in the target's wpa_supplicant.conf,
 * which controls what the radio itself is allowed to use.
 */
#define WIFI_COUNTRY_DEFAULT  "CA"

/**
 * Thread scheduling priorities.
 *
 * QNX priority range: 1 (lowest) to 255 (highest, kernel-level).
 * Unprivileged processes are typically capped at priority 63.
 * Use procmgr_ability() to request higher priorities if needed.
 *
 * Best practice:
 *   - Scanner thread needs higher priority (time-critical I/O).
 *   - Analyzer thread is compute-bound and can run at lower priority.
 *   - Never use priority 255 (reserved for the kernel).
 *   - Leave headroom above your highest priority for system threads.
 */
#define SCANNER_THREAD_PRIORITY   15
#define ANALYZER_THREAD_PRIORITY  10

/**
 * Thread stack sizes.
 *
 * Best practice: Use the minimum stack size that your function call depth
 * requires. Default POSIX minimum is PTHREAD_STACK_MIN (usually 8 KB on QNX).
 * 64 KB is generous for these workloads.
 */
#define SCANNER_STACK_SIZE   (64 * 1024)
#define ANALYZER_STACK_SIZE  (64 * 1024)

/* =========================================================================
 * Logging Configuration
 * ========================================================================= */

/**
 * @brief Log severity levels.
 */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

/** Minimum log level to emit. Messages below this level are suppressed. */
#define LOG_MIN_LEVEL  LOG_DEBUG

/**
 * Asynchronous logger (logger.c).
 *
 * Callers format the line on their own stack and copy it into a fixed ring
 * of LOG_RING_ENTRIES slots (LOG_LINE_MAX bytes each, longer lines are
 * truncated with "..."). A writer thread does all the stderr I/O, so a
 * real-time thread never blocks on a slow file or terminal (stderr is often
 * redirected to a file on the SD card). If the ring is full, the newest
 * message is dropped and counted, and the writer reports the count. Callers
 * never wait for space.
 *
 * The writer runs at LOGGER_WRITER_PRIORITY (SCHED_RR): above default user
 * load (10) so the log keeps draining, below the scanner (15). Its I/O is
 * never done under the ring mutex.
 */
#define LOG_RING_ENTRIES        256   /* power of two */
#define LOG_LINE_MAX            512
#define LOGGER_WRITER_PRIORITY  11

#endif /* WIFI_SPECTRUM_CONFIG_H */
