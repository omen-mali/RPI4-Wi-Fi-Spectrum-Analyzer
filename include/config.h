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
 * Scan interval in seconds. Controls how frequently the scanner thread
 * triggers a new Wi-Fi scan.
 *
 * Best practice: Use clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)
 * for precise, drift-free periodic scanning. Avoid sleep()/usleep().
 */
#define SCAN_INTERVAL_SEC   5

/**
 * WPA supplicant control interface socket path.
 * This is the UNIX domain socket used to communicate with wpa_supplicant.
 *
 * On QNX, the default path depends on the wpa_supplicant configuration.
 * Adjust to match your target's /etc/wpa_supplicant.conf ctrl_interface setting.
 */
#define WPA_CTRL_IFACE_PATH  "/var/run/wpa_supplicant/wlan0"

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

#endif /* WIFI_SPECTRUM_CONFIG_H */
