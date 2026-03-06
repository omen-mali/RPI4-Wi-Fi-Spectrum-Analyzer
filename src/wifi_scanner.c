/**
 * @file wifi_scanner.c
 * @brief Wi-Fi scanning implementation using the wpa_supplicant control interface.
 *
 * This is the core module that interfaces with the Wi-Fi hardware (via
 * wpa_supplicant) to perform periodic spectrum scans. The implementation
 * uses the QNX-provided libwpactrl library (WPA Control Interface).
 *
 * =========================================================================
 * KEY wpa_ctrl API REFERENCE (QNX SDP 8.0)
 * =========================================================================
 *
 *   #include <wpa_ctrl.h>
 *
 *   struct wpa_ctrl *wpa_ctrl_open(const char *ctrl_path);
 *     - Opens a connection to wpa_supplicant's UNIX domain socket.
 *     - Returns NULL on failure.
 *
 *   void wpa_ctrl_close(struct wpa_ctrl *ctrl);
 *     - Closes the connection.
 *
 *   int wpa_ctrl_attach(struct wpa_ctrl *ctrl);
 *     - Registers for unsolicited event messages (e.g., scan complete).
 *     - Returns 0 on success.
 *
 *   int wpa_ctrl_detach(struct wpa_ctrl *ctrl);
 *     - Unregisters from event messages.
 *
 *   int wpa_ctrl_request(struct wpa_ctrl *ctrl,
 *                        const char *cmd, size_t cmd_len,
 *                        char *reply, size_t *reply_len,
 *                        void (*msg_cb)(char *msg, size_t len));
 *     - Sends a command string and receives the reply.
 *     - Blocks for up to 10 seconds waiting for a response.
 *     - Returns 0 on success, -1 on error, -2 on timeout.
 *
 *   int wpa_ctrl_pending(struct wpa_ctrl *ctrl);
 *     - Returns 1 if there are pending event messages, 0 otherwise.
 *
 *   int wpa_ctrl_recv(struct wpa_ctrl *ctrl,
 *                     char *reply, size_t *reply_len);
 *     - Receives one pending event message.
 *
 * =========================================================================
 * SCAN_RESULTS FORMAT (from wpa_supplicant)
 * =========================================================================
 *
 *   The SCAN_RESULTS command returns a tab-separated table:
 *
 *     bssid / frequency / signal level / flags / ssid
 *     00:11:22:33:44:55\t2412\t-45\t[WPA2-PSK-CCMP][ESS]\tMyNetwork
 *     AA:BB:CC:DD:EE:FF\t5180\t-67\t[WPA2-PSK-CCMP][ESS]\tOtherNet
 *
 *   First line is a header that should be skipped.
 *   Fields:
 *     - bssid:       MAC address of the AP (string)
 *     - frequency:   Center frequency in MHz (integer)
 *     - signal level: RSSI in dBm (negative integer)
 *     - flags:       Security/capability flags (string, can be ignored for now)
 *     - ssid:        Network name (string, may be empty)
 *
 * =========================================================================
 */

#include "wifi_scanner.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

/*
 * Include the wpa_ctrl header from QNX SDP.
 * If building on a non-QNX host for syntax checking, you'll need a stub
 * header. See README.md for details.
 */
#ifdef __QNX__
#include <wpa_ctrl.h>
#else
/* Stub wpa_ctrl.h provided for non-QNX host-side compilation/IDE support. */
struct wpa_ctrl;
struct wpa_ctrl *wpa_ctrl_open(const char *ctrl_path);
void             wpa_ctrl_close(struct wpa_ctrl *ctrl);
int              wpa_ctrl_attach(struct wpa_ctrl *ctrl);
int              wpa_ctrl_detach(struct wpa_ctrl *ctrl);
int              wpa_ctrl_request(struct wpa_ctrl *ctrl,
                                  const char *cmd, size_t cmd_len,
                                  char *reply, size_t *reply_len,
                                  void (*msg_cb)(char *msg, size_t len));
int              wpa_ctrl_pending(struct wpa_ctrl *ctrl);
int              wpa_ctrl_recv(struct wpa_ctrl *ctrl,
                               char *reply, size_t *reply_len);
#endif

/** Maximum reply buffer size for wpa_ctrl_request(). */
#define WPA_REPLY_BUF_SIZE  4096

/* =========================================================================
 * Scanner Context (opaque struct defined in header)
 * ========================================================================= */

struct wifi_scanner {
    struct wpa_ctrl  *ctrl_conn;      /**< Command connection to wpa_supplicant */
    struct wpa_ctrl  *monitor_conn;   /**< Event monitoring connection */

    scan_buffer_t    *shared_buffer;  /**< Pointer to shared scan results */
    pthread_mutex_t  *buffer_mutex;   /**< Mutex protecting shared_buffer */
    pthread_cond_t   *buffer_cond;    /**< Condition var for new-data signal */

    volatile int      running;        /**< Flag to control the scan loop */
};

/* =========================================================================
 * Internal Helper: Frequency-to-Channel Conversion
 * ========================================================================= */

/**
 * @brief Convert a Wi-Fi center frequency in MHz to a channel number.
 *
 *   2.4 GHz band:
 *     Channel 1-13:  freq = 2407 + (channel * 5)  =>  channel = (freq - 2407) / 5
 *     Channel 14:    freq = 2484  (special case)
 *
 *   5 GHz band:
 *     Channel = (freq - 5000) / 5
 *     Example: 5180 MHz => channel 36,  5745 MHz => channel 149
 *
 * @param freq_mhz  Center frequency in MHz.
 * @return Channel number, or 0 if the frequency is unrecognized.
 */
static uint8_t freq_to_channel(uint16_t freq_mhz)
{
    if (freq_mhz == 2484) {
        return 14;
    } else if (freq_mhz >= 2412 && freq_mhz <= 2472) {
        return (uint8_t)((freq_mhz - 2407) / 5);
    } else if (freq_mhz >= 5180 && freq_mhz <= 5825) {
        return (uint8_t)((freq_mhz - 5000) / 5);
    }

    LOG_WRN("Unknown frequency: %u MHz", freq_mhz);
    return 0;
}

/**
 * @brief Determine which band a frequency belongs to.
 *
 * @param freq_mhz  Center frequency in MHz.
 * @return BAND_2_4_GHZ, BAND_5_GHZ, or BAND_2_4_GHZ as a fallback.
 */
static wifi_band_t freq_to_band(uint16_t freq_mhz)
{
    if (freq_mhz >= 5000) {
        return BAND_5_GHZ;
    }
    return BAND_2_4_GHZ;
}

/* =========================================================================
 * Internal Helper: Parse SCAN_RESULTS Response
 * ========================================================================= */

/**
 * @brief Parse the text response from a SCAN_RESULTS command into
 *        structured scan_result_t entries.
 *
 * Suggested approach:
 *   1. Tokenize the response line-by-line using strtok_r() with "\n".
 *   2. Skip the first line (header).
 *   3. For each subsequent line, tokenize by "\t" to extract fields.
 *   4. Convert frequency string to uint16_t (strtoul or atoi).
 *   5. Convert signal level string to int8_t (atoi).
 *   6. Use freq_to_channel() and freq_to_band() to populate derived fields.
 *   7. Copy BSSID and SSID with strncpy (always null-terminate!).
 *   8. Stop when MAX_SCAN_RESULTS is reached.
 *
 * Best practices:
 *   - Use strtok_r() (reentrant) instead of strtok() for thread safety.
 *   - Validate each field before conversion to avoid crashes on malformed data.
 *   - Do NOT use dynamic memory allocation here (real-time path).
 *
 * @param response     The raw text response from SCAN_RESULTS.
 * @param results      Output array of scan_result_t entries.
 * @param max_results  Maximum number of entries to write.
 * @return Number of entries successfully parsed.
 */
static size_t parse_scan_results(const char *response,
                                 scan_result_t *results,
                                 size_t max_results)
{
    char *line_ctx = NULL;
    char *copy = strdup(response);
    char *line = strtok_r(copy, "\n", &line_ctx);

    if (line) {
        line = strtok_r(NULL, "\n", &line_ctx);
    }

    size_t count = 0;
    
    while (line != NULL && count < max_results) {
        char *field_ctx = NULL;
        char *bssid = strtok_r(line, "\t", &field_ctx);
        char *freq = strtok_r(NULL, "\t", &field_ctx);
        char *level = strtok_r(NULL, "\t", &field_ctx);
        char *flags = strtok_r(NULL, "\t", &field_ctx);
        char *ssid = strtok_r(NULL, "\t", &field_ctx);

        if (bssid && freq && level) {
            snprintf(results[count].bssid, sizeof(results[count].bssid), "%s", bssid);
            results[count].freq_mhz = strtoul(freq, NULL, 10);
            results[count].signal_dbm = strtol(level, NULL, 10);
            results[count].channel = freq_to_channel(results[count].freq_mhz);
            results[count].band = freq_to_band(results[count].freq_mhz);
            strncpy(results[count].ssid, ssid, MAX_SSID_LEN);

            count++;
        }

        line = strtok_r(NULL, "\n", &line_ctx);
    }

    return count;
}

/* =========================================================================
 * Internal Helper: Send Command to wpa_supplicant
 * ========================================================================= */

/**
 * @brief Send a command string to wpa_supplicant and receive the reply.
 *
 * This is a thin wrapper around wpa_ctrl_request() with logging.
 *
 * @param ctrl       wpa_ctrl control connection.
 * @param cmd        Command string (e.g., "SCAN", "SCAN_RESULTS").
 * @param reply      Buffer to receive the reply.
 * @param reply_len  In: size of reply buffer. Out: length of received reply.
 * @return 0 on success, -1 on error.
 */
static int send_wpa_command(struct wpa_ctrl *ctrl, const char *cmd,
                            char *reply, size_t *reply_len)
{
    int rc;

    LOG_DBG("Sending wpa_supplicant command: %s", cmd);

    rc = wpa_ctrl_request(ctrl, cmd, strlen(cmd), reply, reply_len, NULL);

    if (rc == -2) {
        LOG_ERR("wpa_ctrl_request('%s') timed out", cmd);
        return -1;
    } else if (rc < 0) {
        LOG_ERR("wpa_ctrl_request('%s') failed: rc=%d", cmd, rc);
        return -1;
    }

    /* Null-terminate the reply for safe string operations. */
    if (*reply_len < WPA_REPLY_BUF_SIZE) {
        reply[*reply_len] = '\0';
    } else {
        reply[WPA_REPLY_BUF_SIZE - 1] = '\0';
    }

    return 0;
}

/* =========================================================================
 * Internal Helper: Re-attach Monitor Connection
 * ========================================================================= */

/**
 * @brief Re-establish the monitor connection if it has been dropped.
 *
 * wpa_supplicant silently detaches monitor connections whose outgoing
 * socket buffer overflows (e.g., events accumulated during our sleep
 * interval). When this happens, wpa_ctrl_pending() starts returning -1.
 * The only recovery is to close the old connection and open+attach a new one.
 *
 * @param scanner  Scanner context.
 * @return 0 on success, -1 if re-attachment failed.
 */
static int reattach_monitor(wifi_scanner_t *scanner)
{
    LOG_WRN("Monitor connection lost, attempting re-attach");

    if (scanner->monitor_conn) {
        wpa_ctrl_detach(scanner->monitor_conn);
        wpa_ctrl_close(scanner->monitor_conn);
        scanner->monitor_conn = NULL;
    }

    scanner->monitor_conn = wpa_ctrl_open(WPA_CTRL_IFACE_PATH);
    if (!scanner->monitor_conn) {
        LOG_ERR("wpa_ctrl_open failed during re-attach: %s", strerror(errno));
        return -1;
    }

    if (wpa_ctrl_attach(scanner->monitor_conn) != 0) {
        LOG_ERR("wpa_ctrl_attach failed during re-attach");
        wpa_ctrl_close(scanner->monitor_conn);
        scanner->monitor_conn = NULL;
        return -1;
    }

    LOG_INF("Monitor connection re-attached successfully");
    return 0;
}

/* =========================================================================
 * Internal Helper: Drain Pending Monitor Events
 * ========================================================================= */

/**
 * @brief Drain all pending events from the monitor connection.
 *
 * Must be called before issuing a new SCAN command so that
 * wait_for_scan_complete() only sees events from the new scan, not
 * stale leftovers from wpa_supplicant's background activity.
 *
 * If wpa_ctrl_pending() returns -1 (broken connection), triggers
 * automatic re-attachment.
 *
 * @param scanner  Scanner context with the monitor connection.
 * @return 0 if the monitor connection is healthy, -1 if broken and
 *         re-attachment failed.
 */
static int drain_pending_events(wifi_scanner_t *scanner)
{
    char event_buf[256];
    size_t event_len;
    int drained = 0;
    int rc;

    while ((rc = wpa_ctrl_pending(scanner->monitor_conn)) > 0) {
        event_len = sizeof(event_buf) - 1;
        if (wpa_ctrl_recv(scanner->monitor_conn, event_buf, &event_len) == 0) {
            event_buf[event_len] = '\0';
            drained++;
            LOG_DBG("Drained stale event: %s", event_buf);
        } else {
            break;
        }
    }

    if (rc < 0) {
        LOG_WRN("wpa_ctrl_pending returned error -- monitor connection is broken");
        return reattach_monitor(scanner);
    }

    if (drained > 0) {
        LOG_DBG("Drained %d stale event(s) from monitor connection", drained);
    }

    return 0;
}

/* =========================================================================
 * Internal Helper: Wait for Scan Completion Event
 * ========================================================================= */

/**
 * @brief Wait for the CTRL-EVENT-SCAN-RESULTS event on the monitor connection.
 *
 * After sending a SCAN command, wpa_supplicant performs the scan
 * asynchronously. When complete, it sends one of:
 *   <3>CTRL-EVENT-SCAN-RESULTS   (success)
 *   <3>CTRL-EVENT-SCAN-FAILED    (driver/firmware error)
 *
 * @param scanner  Scanner context with the monitor connection.
 * @return 0 if scan-complete event received, -1 on failure/timeout.
 */
static int wait_for_scan_complete(wifi_scanner_t *scanner)
{
    char event_buf[256];
    size_t event_len;
    struct timespec poll_interval = { .tv_sec = 0, .tv_nsec = 100000000 };
    int timeout_iterations = 300;
    int rc;

    for (int i = 0; i < timeout_iterations && scanner->running; i++) {
        while ((rc = wpa_ctrl_pending(scanner->monitor_conn)) > 0) {
            event_len = sizeof(event_buf) - 1;
            if (wpa_ctrl_recv(scanner->monitor_conn, event_buf, &event_len) != 0) {
                break;
            }
            event_buf[event_len] = '\0';
            LOG_DBG("Monitor event: %s", event_buf);

            if (strstr(event_buf, "CTRL-EVENT-SCAN-RESULTS")) {
                LOG_DBG("Scan complete event received");
                return 0;
            }
            if (strstr(event_buf, "CTRL-EVENT-SCAN-FAILED")) {
                LOG_ERR("Scan failed at driver/firmware level");
                return -1;
            }
        }

        if (rc < 0) {
            LOG_ERR("Monitor connection broken while waiting for scan event");
            if (reattach_monitor(scanner) != 0) {
                return -1;
            }
            LOG_WRN("Monitor re-attached, but scan event was likely lost");
            return -1;
        }

        clock_nanosleep(CLOCK_MONOTONIC, 0, &poll_interval, NULL);
    }

    LOG_WRN("Timed out waiting for scan completion (%d iterations)",
            timeout_iterations);
    return -1;
}

/* =========================================================================
 * Public API Implementation
 * ========================================================================= */

wifi_scanner_t *wifi_scanner_init(scan_buffer_t *shared_buffer,
                                  pthread_mutex_t *buffer_mutex,
                                  pthread_cond_t *buffer_cond)
{
    wifi_scanner_t *scanner;

    LOG_INF("Initializing Wi-Fi scanner (ctrl_path=%s)", WPA_CTRL_IFACE_PATH);

    /*
     * Allocate the scanner context.
     *
     * Note: This allocation happens once during initialization (not in a
     * real-time path), so malloc() is acceptable here. In the scan loop
     * itself, we use only fixed-size stack/static buffers.
     */
    scanner = (wifi_scanner_t *)calloc(1, sizeof(wifi_scanner_t));
    
    if (!scanner) {
        LOG_ERR("Failed to allocate scanner context");
        return NULL;
    }

    scanner->shared_buffer = shared_buffer;
    scanner->buffer_mutex  = buffer_mutex;
    scanner->buffer_cond   = buffer_cond;
    scanner->running       = 1;

    scanner->ctrl_conn = wpa_ctrl_open(WPA_CTRL_IFACE_PATH);
    if (!scanner->ctrl_conn) {
        LOG_ERR("wpa_ctrl_open failed for control connection: %s",
                strerror(errno));
        free(scanner);
        return NULL;
    }

    scanner->monitor_conn = wpa_ctrl_open(WPA_CTRL_IFACE_PATH);
    if (!scanner->monitor_conn) {
        LOG_ERR("wpa_ctrl_open failed for monitor connection: %s",
                strerror(errno));
        wpa_ctrl_close(scanner->ctrl_conn);
        free(scanner);
        return NULL;
    }

    if (wpa_ctrl_attach(scanner->monitor_conn) != 0) {
        LOG_ERR("wpa_ctrl_attach failed");
        wpa_ctrl_close(scanner->monitor_conn);
        wpa_ctrl_close(scanner->ctrl_conn);
        free(scanner);
        return NULL;
    }

    /*
     * Probe wpa_supplicant state to catch misconfiguration early.
     * A healthy interface shows wpa_state=DISCONNECTED (or INACTIVE).
     * If the interface is down or the driver isn't loaded, this will
     * show an error state that makes the problem obvious in the log.
     */
    {
        char status_reply[WPA_REPLY_BUF_SIZE];
        size_t status_len = sizeof(status_reply) - 1;

        if (send_wpa_command(scanner->ctrl_conn, "STATUS",
                             status_reply, &status_len) == 0) {
            LOG_INF("wpa_supplicant STATUS at init:\n%s", status_reply);
        } else {
            LOG_WRN("Could not query wpa_supplicant STATUS at init");
        }
    }

    LOG_INF("Wi-Fi scanner initialized successfully");
    return scanner;
}

void *wifi_scanner_run(void *arg)
{
    wifi_scanner_t *scanner = (wifi_scanner_t *)arg;
    char reply[WPA_REPLY_BUF_SIZE];
    size_t reply_len;

    LOG_INF("Scanner thread started");

    while (scanner->running) {
        if (drain_pending_events(scanner) != 0) {
            LOG_ERR("Monitor connection broken and re-attach failed, "
                    "retrying next cycle");
            goto sleep;
        }

        reply_len = sizeof(reply) - 1;

        if (send_wpa_command(scanner->ctrl_conn, "SCAN", reply, &reply_len) != 0) {
            LOG_ERR("Failed to trigger scan, retrying after interval");
            goto sleep;
        }

        if (strncmp(reply, "OK", 2) == 0) {
            LOG_DBG("SCAN accepted");
        } else if (strstr(reply, "FAIL-BUSY")) {
            LOG_DBG("SCAN busy -- previous scan still in progress, waiting for results");
        } else {
            LOG_WRN("Unexpected SCAN response: %s", reply);
            goto sleep;
        }
        
        if (wait_for_scan_complete(scanner) != 0) {
            LOG_WRN("Scan completion wait failed, will try SCAN_RESULTS anyway");
            goto sleep;
        }

        reply_len = sizeof(reply) - 1;
        if (send_wpa_command(scanner->ctrl_conn, "SCAN_RESULTS", reply, &reply_len) != 0) {
            LOG_ERR("Failed to retrieve scan results");
            goto sleep;
        }

        pthread_mutex_lock(scanner->buffer_mutex);
        {
            scanner->shared_buffer->count = parse_scan_results(reply, scanner->shared_buffer->entries, MAX_SCAN_RESULTS);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            scanner->shared_buffer->timestamp_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
            LOG_INF("Scan complete: %zu APs found", scanner->shared_buffer->count);
        }
        pthread_cond_signal(scanner->buffer_cond);
        pthread_mutex_unlock(scanner->buffer_mutex);
        
        sleep:
        {
            struct timespec next;
            clock_gettime(CLOCK_MONOTONIC, &next);
            next.tv_sec += SCAN_INTERVAL_SEC;
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
    }

    LOG_INF("Scanner thread exiting");
    return NULL;
}

void wifi_scanner_shutdown(wifi_scanner_t *scanner)
{
    if (!scanner) {
        return;
    }

    LOG_INF("Shutting down Wi-Fi scanner");

    scanner->running = 0;

    if (scanner->monitor_conn) {
        wpa_ctrl_detach(scanner->monitor_conn);
        wpa_ctrl_close(scanner->monitor_conn);
        scanner->monitor_conn = NULL;
    }
    if (scanner->ctrl_conn) {
        wpa_ctrl_close(scanner->ctrl_conn);
        scanner->ctrl_conn = NULL;
    }
    free(scanner);
    LOG_INF("Wi-Fi scanner shut down");
}
