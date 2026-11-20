/**
 * @file wifi_scanner.h
 * @brief Wi-Fi scanning module API using wpa_supplicant control interface.
 *
 * This module communicates with a running wpa_supplicant process on the
 * QNX target via the WPA Control Interface (libwpactrl). It issues scan
 * commands and parses results into the shared scan_buffer_t structure.
 *
 * Hardware context:
 *   The Raspberry Pi 4 has an onboard Cypress/Broadcom CYW43455 Wi-Fi
 *   chipset supporting 2.4 GHz and 5 GHz bands. On QNX SDP 8.0 it is driven
 *   by the io-sock driver devs-qwdi_dhd_sdio-2_11-rpi4.so, which exposes the
 *   interface "bcm0" to wpa_supplicant-2.11.
 *
 * API usage pattern:
 *   1. Call wifi_scanner_init() once at startup.
 *   2. The scanner thread calls wifi_scanner_run() which loops internally.
 *   3. On shutdown, call wifi_scanner_shutdown() to release resources.
 *
 * Dependencies:
 *   - wpa_supplicant must be running on the target with ctrl_interface set
 *     to WPA_CTRL_DIR (config.h). The control socket is owned by root, so
 *     the process must run as root (e.g. via sudo).
 *   - Link with: libwpactrl-2.11 (see CMakeLists.txt), -lsocket
 */

#ifndef WIFI_SPECTRUM_SCANNER_H
#define WIFI_SPECTRUM_SCANNER_H

#include "config.h"
#include <pthread.h>

/**
 * @brief Opaque scanner context.
 *
 * Holds the wpa_ctrl connection handles and references to the shared
 * scan buffer. Created by wifi_scanner_init().
 */
typedef struct wifi_scanner wifi_scanner_t;

/**
 * @brief Initialize the Wi-Fi scanner module.
 *
 * Opens a control connection and an event-monitoring connection to
 * wpa_supplicant. Both connections are needed:
 *   - Control connection: for sending SCAN / SCAN_RESULTS commands.
 *   - Monitor connection:  for receiving async events (scan completion).
 *
 * @param ctrl_path      Full path of the wpa_supplicant control socket for
 *                       the Wi-Fi interface (e.g. "/var/run/wpa_supplicant/bcm0").
 *                       Copied into the context; must be < WPA_CTRL_PATH_MAX.
 * @param shared_buffer  Pointer to the shared scan results buffer.
 * @param buffer_mutex   Pointer to the mutex protecting shared_buffer.
 * @param buffer_cond    Pointer to the condition variable signaled when
 *                       new scan data is available.
 *
 * @return Pointer to an allocated scanner context, or NULL on failure.
 *
 * Best practice: Call this from the main thread before spawning the
 * scanner thread, so that initialization errors are caught early.
 */
wifi_scanner_t *wifi_scanner_init(const char *ctrl_path,
                                  scan_buffer_t *shared_buffer,
                                  pthread_mutex_t *buffer_mutex,
                                  pthread_cond_t *buffer_cond);

/**
 * @brief Query wpa_supplicant's configured regulatory country ("GET country").
 *
 * Call from the main thread before the scanner thread starts (it uses the
 * same control connection).
 *
 * @param scanner  Initialized scanner context.
 * @param out      Receives the two-letter code, NUL-terminated.
 * @param out_len  Size of out (>= 3).
 * @return 0 on success, -1 if not set or the query failed.
 */
int wifi_scanner_get_country(wifi_scanner_t *scanner, char *out, size_t out_len);

/**
 * @brief Scanner thread entry point -- runs the scan loop.
 *
 * This function is designed to be passed to pthread_create(). It runs
 * an infinite loop that:
 *   1. Sends a SCAN command to wpa_supplicant.
 *   2. Waits for the scan-complete event.
 *   3. Retrieves SCAN_RESULTS and parses them.
 *   4. Writes parsed results into the shared buffer (under mutex).
 *   5. Signals the condition variable so the analyzer can wake up.
 *   6. Sleeps for SCAN_INTERVAL_SEC before repeating.
 *
 * The loop checks for POSIX cancellation points to allow clean shutdown.
 *
 * @param arg  Pointer to the wifi_scanner_t context (cast from void*).
 * @return NULL (thread return value).
 */
void *wifi_scanner_run(void *arg);

/**
 * @brief Shut down the scanner and release all resources.
 *
 * Closes wpa_ctrl connections and frees the scanner context.
 * The scanner thread should be cancelled/joined before calling this.
 *
 * @param scanner  The scanner context to destroy.
 */
void wifi_scanner_shutdown(wifi_scanner_t *scanner);

#endif /* WIFI_SPECTRUM_SCANNER_H */
