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
 *   #include <wpa_ctrl-2.11.h>   (versioned name in the QNX SDP)
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
#include "scan_parse.h"
#include "schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>

/*
 * On QNX, use the SDP's own header from the "WPA2/WPA3/IEEE 802.1X Supplicant
 * 2.11 for use with io-sock" package. It's installed versioned, as
 * $QNX_TARGET/usr/include/wpa_ctrl-2.11.h (there's no plain wpa_ctrl.h), and
 * must match libwpactrl-2.11 (see WPA_SUPPLICANT_VERSION in CMakeLists.txt).
 * It declares QNX-only API (e.g. wpa_ctrl_set_cmd_timeout()) that upstream
 * hostap headers lack, so don't substitute one.
 *
 * Non-QNX host builds (syntax checks, parser tests) get these prototypes
 * only and must link stub implementations.
 */
#ifdef __QNX__
#include <wpa_ctrl-2.11.h>
#include <sys/neutrino.h>   /* TimerTimeout(), _NTO_TIMEOUT_NANOSLEEP */
#else
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
int              wpa_ctrl_get_fd(struct wpa_ctrl *ctrl);
#endif

/**
 * Reply buffer size for wpa_ctrl_request().
 *
 * wpa_supplicant builds most control replies, SCAN_RESULTS included, in a
 * fixed 4096-byte buffer and silently stops adding rows when it is full
 * (roughly 50-60 APs). A larger client buffer does not help. We keep 4096 and
 * detect a full reply so truncation shows up in the log instead of skewing
 * the analysis without warning.
 */
#define WPA_REPLY_BUF_SIZE        4096

/** A reply this close to the server's 4096-byte cap is assumed truncated. */
#define WPA_REPLY_TRUNC_THRESHOLD (WPA_REPLY_BUF_SIZE - 128)

/* =========================================================================
 * Scanner Context (opaque struct defined in header)
 * ========================================================================= */

struct wifi_scanner {
    struct wpa_ctrl  *ctrl_conn;      /**< Command connection to wpa_supplicant */
    struct wpa_ctrl  *monitor_conn;   /**< Event monitoring connection */

    char              ctrl_path[WPA_CTRL_PATH_MAX]; /**< Control socket path */

    scan_buffer_t    *shared_buffer;  /**< Pointer to shared scan results */
    pthread_mutex_t  *buffer_mutex;   /**< Mutex protecting shared_buffer */
    pthread_cond_t   *buffer_cond;    /**< Condition var for new-data signal */

    /**
     * Private staging area. Results are parsed here without holding the
     * mutex, then copied into shared_buffer under it, which keeps the
     * critical section (and any priority-inheritance boost) short.
     */
    scan_result_t     staging[MAX_SCAN_RESULTS];

    uint32_t          period_ms;      /**< Fixed scan period (SCAN request to SCAN request) */
    uint64_t          cycles;         /**< Completed schedule slots */
    uint64_t          overruns;       /**< Slots skipped because a cycle ran past its deadline */
    int64_t           max_lateness_ns; /**< Worst wake-up lateness vs. deadline */
    uint32_t          wake_tolerance_ns; /**< 0: tick-aligned sleep; else high-res timer */

    volatile int      running;        /**< Flag to control the scan loop */
};

/* =========================================================================
 * Internal Helper: Absolute-Deadline Sleep
 * ========================================================================= */

/**
 * @brief Sleep until an absolute CLOCK_MONOTONIC deadline.
 *
 * tolerance_ns == 0: clock_nanosleep(TIMER_ABSTIME), tick-aligned expiry.
 * tolerance_ns  > 0: (QNX) high-resolution timer. Set the thread's timeout
 * tolerance, then block in STATE_NANOSLEEP until the absolute deadline.
 * Both are cancellation points.
 *
 * @return 0 on success, or an errno value (EINTR is returned to the caller).
 */
static int sleep_until(const struct timespec *deadline, uint32_t tolerance_ns)
{
#ifdef __QNX__
    if (tolerance_ns > 0) {
        uint64_t tol    = tolerance_ns;
        uint64_t abs_ns = (uint64_t)deadline->tv_sec * (uint64_t)NSEC_PER_SEC +
                          (uint64_t)deadline->tv_nsec;

        if (TimerTimeout(CLOCK_MONOTONIC, TIMER_TOLERANCE, NULL, &tol, NULL) == -1) {
            return errno;
        }
        if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_NANOSLEEP | TIMER_ABSTIME,
                         NULL, &abs_ns, NULL) == -1) {
            return errno;
        }
        return 0;
    }
#else
    (void)tolerance_ns;
#endif
    return clock_nanosleep(CLOCK_MONOTONIC, (int)TIMER_ABSTIME, deadline, NULL);
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

    scanner->monitor_conn = wpa_ctrl_open(scanner->ctrl_path);
    if (!scanner->monitor_conn) {
        LOG_ERR("wpa_ctrl_open(%s) failed during re-attach: %s",
                scanner->ctrl_path, strerror(errno));
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
 * Blocks in poll() on the monitor socket (wpa_ctrl_get_fd()), so the thread
 * sleeps until an event arrives and wakes as soon as it does. The total
 * wait is bounded by SCAN_COMPLETE_TIMEOUT_MS against an absolute
 * CLOCK_MONOTONIC deadline, so unrelated events (state changes, BSS
 * added/removed) don't extend it. poll() is a cancellation point.
 *
 * @param scanner  Scanner context with the monitor connection.
 * @return 0 if scan-complete event received, -1 on failure/timeout.
 */
static int wait_for_scan_complete(wifi_scanner_t *scanner)
{
    char            event_buf[256];
    size_t          event_len;
    struct timespec deadline;
    struct timespec now;
    struct pollfd   pfd;
    int             rc;

    pfd.fd     = wpa_ctrl_get_fd(scanner->monitor_conn);
    pfd.events = POLLIN;
    if (pfd.fd < 0) {
        LOG_ERR("wpa_ctrl_get_fd failed on monitor connection");
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    ts_add_ms(&deadline, SCAN_COMPLETE_TIMEOUT_MS);

    while (scanner->running) {
        int64_t remaining_ns;

        clock_gettime(CLOCK_MONOTONIC, &now);
        remaining_ns = ts_diff_ns(&deadline, &now);
        if (remaining_ns <= 0) {
            break;
        }

        /* Round up so we never spin on a sub-millisecond remainder. */
        pfd.revents = 0;
        rc = poll(&pfd, 1, (int)((remaining_ns + 999999) / 1000000));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERR("poll on monitor socket failed: %s", strerror(errno));
            return -1;
        }
        if (rc == 0) {
            break;                              /* timed out */
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_ERR("Monitor socket error (revents=0x%x)", (unsigned)pfd.revents);
            if (reattach_monitor(scanner) != 0) {
                return -1;
            }
            LOG_WRN("Monitor re-attached, but scan event was likely lost");
            return -1;
        }

        /* Drain everything that's queued; return on the first verdict. */
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
    }

    if (scanner->running) {
        LOG_WRN("Timed out waiting for scan completion (%d ms)",
                SCAN_COMPLETE_TIMEOUT_MS);
    }
    return -1;
}

/* =========================================================================
 * Public API Implementation
 * ========================================================================= */

wifi_scanner_t *wifi_scanner_init(const char *ctrl_path,
                                  uint32_t period_ms,
                                  scan_buffer_t *shared_buffer,
                                  pthread_mutex_t *buffer_mutex,
                                  pthread_cond_t *buffer_cond)
{
    wifi_scanner_t *scanner;

    if (!ctrl_path || ctrl_path[0] == '\0' ||
        strlen(ctrl_path) >= WPA_CTRL_PATH_MAX) {
        LOG_ERR("Invalid wpa_supplicant control socket path");
        return NULL;
    }

    if (period_ms < SCAN_PERIOD_MS_MIN || period_ms > SCAN_PERIOD_MS_MAX) {
        LOG_ERR("Scan period %u ms out of range [%u, %u]",
                period_ms, SCAN_PERIOD_MS_MIN, SCAN_PERIOD_MS_MAX);
        return NULL;
    }

    LOG_INF("Initializing Wi-Fi scanner (ctrl_path=%s, period=%u ms)",
            ctrl_path, period_ms);

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

    snprintf(scanner->ctrl_path, sizeof(scanner->ctrl_path), "%s", ctrl_path);
    scanner->shared_buffer = shared_buffer;
    scanner->buffer_mutex  = buffer_mutex;
    scanner->buffer_cond   = buffer_cond;
    scanner->period_ms     = period_ms;
    scanner->running       = 1;

    scanner->ctrl_conn = wpa_ctrl_open(scanner->ctrl_path);
    if (!scanner->ctrl_conn) {
        /*
         * Common causes on the QNX image:
         *   ENOENT -- wrong interface name (default bcm0; see -i), or
         *             wpa_supplicant isn't running on that interface.
         *   EACCES -- not running as root; the socket is root-only.
         */
        LOG_ERR("wpa_ctrl_open(%s) failed for control connection: %s",
                scanner->ctrl_path, strerror(errno));
        free(scanner);
        return NULL;
    }

    scanner->monitor_conn = wpa_ctrl_open(scanner->ctrl_path);
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

int wifi_scanner_set_wake_tolerance(wifi_scanner_t *scanner, uint32_t tolerance_ns)
{
    if (!scanner || tolerance_ns > WAKE_TOLERANCE_NS_MAX) {
        return -1;
    }
#ifndef __QNX__
    if (tolerance_ns > 0) {
        LOG_WRN("High-resolution wake timer is QNX-only, using clock_nanosleep");
        tolerance_ns = 0;
    }
#endif
    scanner->wake_tolerance_ns = tolerance_ns;
    return 0;
}

int wifi_scanner_get_country(wifi_scanner_t *scanner, char *out, size_t out_len)
{
    char   reply[64];
    size_t reply_len = sizeof(reply) - 1;

    if (!scanner || !out || out_len < 3) {
        return -1;
    }
    if (send_wpa_command(scanner->ctrl_conn, "GET country",
                         reply, &reply_len) != 0) {
        return -1;
    }

    /* Reply is e.g. "CA" (possibly newline-terminated) or "FAIL". */
    if (reply_len < 2 || strncmp(reply, "FAIL", 4) == 0 ||
        reply[0] == '\0' || reply[0] == '\n') {
        return -1;
    }
    out[0] = reply[0];
    out[1] = reply[1];
    out[2] = '\0';
    return 0;
}

void *wifi_scanner_run(void *arg)
{
    wifi_scanner_t *scanner = (wifi_scanner_t *)arg;
    char reply[WPA_REPLY_BUF_SIZE];
    size_t reply_len;
    struct timespec next;   /* absolute start time of the current slot */

    {
        struct timespec res = { 0, 0 };

        clock_getres(CLOCK_MONOTONIC, &res);
        LOG_INF("Scanner thread started (fixed-rate, period=%u ms)",
                scanner->period_ms);
        if (scanner->wake_tolerance_ns > 0) {
            LOG_INF("Wake timer: high-resolution, tolerance=%u ns (system tick %ld ns)",
                    scanner->wake_tolerance_ns, res.tv_nsec);
        } else {
            LOG_INF("Wake timer: tick-aligned clock_nanosleep (system tick %ld ns)",
                    res.tv_nsec);
        }
    }

    /* Slot 0 starts now; slot N starts at exactly next + N * period. */
    clock_gettime(CLOCK_MONOTONIC, &next);

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

        if (reply_len >= WPA_REPLY_TRUNC_THRESHOLD) {
            LOG_WRN("SCAN_RESULTS reply is %zu bytes, near wpa_supplicant's "
                    "4096-byte cap -- the AP list is probably truncated",
                    reply_len);
        }

        {
            /* Parse outside the lock, publish under it. */
            size_t          count = scan_parse_results(reply, scanner->staging,
                                                       MAX_SCAN_RESULTS);
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);

            pthread_mutex_lock(scanner->buffer_mutex);
            memcpy(scanner->shared_buffer->entries, scanner->staging,
                   count * sizeof(scanner->staging[0]));
            scanner->shared_buffer->count        = count;
            scanner->shared_buffer->timestamp_ns =
                (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
            pthread_cond_signal(scanner->buffer_cond);
            pthread_mutex_unlock(scanner->buffer_mutex);

            LOG_INF("Scan complete: %zu APs found", count);
        }
        
        sleep:
        {
            struct timespec now;
            uint32_t        skipped = 0;
            int             rc;
            int64_t         lateness_ns;

            /*
             * Advance the deadline by exactly one period from the previous
             * deadline (not from "now"), so per-cycle work never shifts the
             * schedule. If the cycle overran, skip whole slots to stay
             * phase-aligned instead of firing back-to-back to catch up.
             */
            scanner->cycles++;

            clock_gettime(CLOCK_MONOTONIC, &now);
            skipped = schedule_advance(&next, &now, scanner->period_ms);
            if (skipped > 0) {
                scanner->overruns += skipped;
                LOG_WRN("Scan cycle overran its %u ms period: skipped %u slot(s) "
                        "(%llu overrun(s) total)", scanner->period_ms, skipped,
                        (unsigned long long)scanner->overruns);
            }

            do {
                rc = sleep_until(&next, scanner->wake_tolerance_ns);
            } while (rc == EINTR && scanner->running);
            if (rc != 0 && rc != EINTR) {
                LOG_ERR("Schedule sleep failed: %s -- falling back to "
                        "tick-aligned clock_nanosleep", strerror(rc));
                scanner->wake_tolerance_ns = 0;
                (void)sleep_until(&next, 0);
            }

            clock_gettime(CLOCK_MONOTONIC, &now);
            lateness_ns = ts_diff_ns(&now, &next);
            if (lateness_ns > scanner->max_lateness_ns) {
                scanner->max_lateness_ns = lateness_ns;
            }
            /* Machine-readable for scripts/analyze-timing.py. */
            LOG_DBG("Wake lateness: %lld us", (long long)(lateness_ns / 1000));
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

    LOG_INF("Shutting down Wi-Fi scanner (cycles=%llu, skipped slots=%llu, "
            "max wake lateness=%lld us)",
            (unsigned long long)scanner->cycles,
            (unsigned long long)scanner->overruns,
            (long long)(scanner->max_lateness_ns / 1000));

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
