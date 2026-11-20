/**
 * @file main.c
 * @brief Application entry point for the Wi-Fi Spectrum Analyzer.
 *
 * Orchestrates initialization of all modules, creates the shared data
 * structures, spawns real-time threads, and handles clean shutdown on
 * SIGINT/SIGTERM.
 *
 * =========================================================================
 * STARTUP SEQUENCE
 * =========================================================================
 *
 *   1. Block signals in the main thread (so child threads inherit the mask).
 *   2. Initialize the logger.
 *   3. (QNX-specific) Request elevated privileges via procmgr_ability().
 *   4. Initialize the shared scan buffer, mutex (with PRIO_INHERIT), and
 *      condition variable.
 *   5. Initialize the Wi-Fi scanner module.
 *   6. Initialize the channel analyzer module.
 *   7. Spawn the scanner thread (SCHED_FIFO, high priority).
 *   8. Spawn the analyzer thread (SCHED_RR, lower priority).
 *   9. Wait for a termination signal (SIGINT or SIGTERM) using sigwait().
 *  10. Cancel and join both threads.
 *  11. Shut down modules and clean up.
 *
 * =========================================================================
 * SIGNAL HANDLING BEST PRACTICES
 * =========================================================================
 *
 * On QNX (and POSIX in general), the recommended approach for signal
 * handling in multi-threaded applications is:
 *
 *   - Block all signals in all threads (threads inherit the mask from
 *     the thread that created them).
 *   - Dedicate one thread (here, the main thread) to call sigwait(),
 *     which synchronously waits for a signal.
 *   - This avoids the many pitfalls of async signal handlers (limited
 *     set of async-signal-safe functions, reentrancy issues, etc.).
 *
 * =========================================================================
 */

#include "config.h"
#include "logger.h"
#include "wifi_scanner.h"
#include "channel_analyzer.h"
#include "thread_manager.h"
#include "regdomain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

/*
 * QNX-specific header for process manager abilities (privilege escalation).
 * Only included when building for QNX.
 */
#ifdef __QNX__
#include <sys/procmgr.h>
#endif

/* =========================================================================
 * Shared State
 * ========================================================================= */

/** Shared scan results buffer (written by scanner, read by analyzer). */
static scan_buffer_t   shared_scan_buffer;

/** Mutex protecting the shared scan buffer (PTHREAD_PRIO_INHERIT). */
static pthread_mutex_t scan_buffer_mutex;

/** Condition variable signaled when new scan data is available. */
static pthread_cond_t  scan_buffer_cond;

/* =========================================================================
 * Internal: Initialize Shared Resources
 * ========================================================================= */

/**
 * @brief Initialize the shared scan buffer, mutex, and condition variable.
 *
 * The mutex is created with PTHREAD_PRIO_INHERIT to prevent priority
 * inversion between the scanner (high priority) and analyzer (low priority)
 * threads.
 *
 * @return 0 on success, -1 on failure.
 */
static int init_shared_resources(void)
{
    pthread_mutexattr_t mutex_attr;
    int rc;

    /* Zero-initialize the scan buffer. */
    memset(&shared_scan_buffer, 0, sizeof(shared_scan_buffer));

    /* Create the mutex with priority inheritance protocol. */
    rc = pthread_mutexattr_init(&mutex_attr);
    if (rc != 0) {
        LOG_ERR("mutexattr_init failed: %s", strerror(rc));
        return -1;
    }

    rc = pthread_mutexattr_setprotocol(&mutex_attr, PTHREAD_PRIO_INHERIT);
    if (rc != 0) {
        LOG_ERR("mutexattr_setprotocol(PRIO_INHERIT) failed: %s",
                strerror(rc));
        pthread_mutexattr_destroy(&mutex_attr);
        return -1;
    }

    rc = pthread_mutex_init(&scan_buffer_mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);
    if (rc != 0) {
        LOG_ERR("mutex_init for scan buffer failed: %s", strerror(rc));
        return -1;
    }

    /* Create the condition variable (default attributes are fine). */
    rc = pthread_cond_init(&scan_buffer_cond, NULL);
    if (rc != 0) {
        LOG_ERR("cond_init for scan buffer failed: %s", strerror(rc));
        pthread_mutex_destroy(&scan_buffer_mutex);
        return -1;
    }

    return 0;
}

/**
 * @brief Destroy the shared mutex and condition variable.
 */
static void cleanup_shared_resources(void)
{
    pthread_cond_destroy(&scan_buffer_cond);
    pthread_mutex_destroy(&scan_buffer_mutex);
}

/* =========================================================================
 * Internal: QNX Privilege Setup
 * ========================================================================= */

/**
 * @brief Request elevated privileges needed for real-time operation.
 *
 * On QNX, unprivileged processes have a priority ceiling (typically 63).
 * To use higher priorities, the process must have PROCMGR_AID_PRIORITY.
 * Similarly, certain I/O operations may require PROCMGR_AID_IO.
 *
 * Best practice: Request only the abilities you actually need, and drop
 * them after thread creation if they're no longer required.
 *
 * @return 0 on success, -1 on failure.
 */
static int setup_qnx_privileges(void)
{
#ifdef __QNX__
    LOG_INF("QNX privilege setup");

    if (procmgr_ability(0, PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PRIORITY, 
        PROCMGR_AID_EOL) != 0) {
            LOG_WRN("procmgr_ability(PRIORITY) failed: %s -- "
                "thread priorities may be capped", strerror(errno));
    } else {
        LOG_INF("QNX privilege setup (PRIORITY) successful");
    }

    if (procmgr_ability(0, PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_IO, 
        PROCMGR_AID_EOL) != 0) {
            LOG_WRN("procmgr_ability(IO) failed: %s -- "
                "direct I/O access may be capped", strerror(errno));
    } else {
        LOG_INF("QNX privilege setup (IO) successful");
    }
#else
    LOG_INF("Not running on QNX -- skipping privilege setup");
#endif
    return 0;
}

/* =========================================================================
 * Internal: Signal Handling Setup
 * ========================================================================= */

/**
 * @brief Block SIGINT and SIGTERM in the calling thread.
 *
 * Since threads inherit the signal mask from their creator, calling this
 * before pthread_create() ensures the child threads also have these
 * signals blocked. Only the main thread will handle them via sigwait().
 *
 * @param[out] wait_set  Filled with the set of signals to wait for.
 * @return 0 on success, -1 on failure.
 */
static int block_termination_signals(sigset_t *wait_set)
{
    sigemptyset(wait_set);
    sigaddset(wait_set, SIGINT);
    sigaddset(wait_set, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, wait_set, NULL) != 0) {
        LOG_ERR("pthread_sigmask failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/* =========================================================================
 * Internal: Command-Line Parsing
 * ========================================================================= */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Wi-Fi Spectrum Analyzer v" APP_VERSION "\n"
            "Usage: %s [-i <interface>] [-c <country>]\n"
            "  -i <interface>  Wi-Fi interface whose wpa_supplicant control socket\n"
            "                  to use (default: %s -> %s/%s)\n"
            "  -c <country>    ISO 3166-1 alpha-2 regulatory domain used to pick\n"
            "                  legal channels (default: %s; built in: CA, US, 00=world)\n"
            "  -h              Show this help\n",
            prog, WIFI_IFACE_DEFAULT, WPA_CTRL_DIR, WIFI_IFACE_DEFAULT,
            WIFI_COUNTRY_DEFAULT);
}

/**
 * @brief Parse argv: build the control socket path and pick the country.
 *
 * @param[out] ctrl_path      Receives "<WPA_CTRL_DIR>/<iface>".
 * @param      ctrl_path_len  Size of ctrl_path.
 * @param[out] country        Receives the -c value (or the default).
 * @return 0 to continue, 1 if help was printed, -1 on invalid arguments.
 */
static int parse_args(int argc, char *argv[], char *ctrl_path, size_t ctrl_path_len,
                      const char **country)
{
    const char *iface = WIFI_IFACE_DEFAULT;
    int opt;
    int n;

    *country = WIFI_COUNTRY_DEFAULT;

    while ((opt = getopt(argc, argv, "i:c:h")) != -1) {
        switch (opt) {
        case 'i':
            iface = optarg;
            break;
        case 'c':
            if (!regdomain_code_is_valid(optarg)) {
                fprintf(stderr, "Invalid country code '%s' (expected e.g. CA, US, 00)\n",
                        optarg);
                return -1;
            }
            *country = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 1;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    if (iface[0] == '\0' || strlen(iface) >= WIFI_IFACE_NAME_MAX ||
        strchr(iface, '/') != NULL) {
        fprintf(stderr, "Invalid interface name '%s'\n", iface);
        return -1;
    }

    n = snprintf(ctrl_path, ctrl_path_len, "%s/%s", WPA_CTRL_DIR, iface);
    if (n < 0 || (size_t)n >= ctrl_path_len) {
        fprintf(stderr, "Control socket path too long\n");
        return -1;
    }

    return 0;
}

/* =========================================================================
 * main()
 * ========================================================================= */

int main(int argc, char *argv[])
{
    sigset_t          wait_set;
    int               caught_signal;
    wifi_scanner_t    *scanner   = NULL;
    channel_analyzer_t *analyzer = NULL;
    managed_thread_t  scanner_thread;
    managed_thread_t  analyzer_thread;
    int               exit_code = EXIT_SUCCESS;
    char              ctrl_path[WPA_CTRL_PATH_MAX];
    const char        *country = NULL;
    const regdomain_t *regdomain;
    int               country_known;

    /* ----- Step 0: Parse command line ----- */
    switch (parse_args(argc, argv, ctrl_path, sizeof(ctrl_path), &country)) {
    case 0:  break;
    case 1:  return EXIT_SUCCESS;
    default: return EXIT_FAILURE;
    }
    regdomain = regdomain_lookup(country, &country_known);

    /* ----- Step 1: Block signals before creating any threads ----- */
    if (block_termination_signals(&wait_set) != 0) {
        fprintf(stderr, "Fatal: failed to block signals\n");
        return EXIT_FAILURE;
    }

    /* ----- Step 2: Initialize the logger ----- */
    if (logger_init() != 0) {
        fprintf(stderr, "Fatal: failed to initialize logger\n");
        return EXIT_FAILURE;
    }

    LOG_INF("=== Wi-Fi Spectrum Analyzer v%s starting ===", APP_VERSION);

    /* ----- Step 3: QNX-specific privilege setup ----- */
    if (setup_qnx_privileges() != 0) {
        LOG_ERR("Privilege setup failed");
        exit_code = EXIT_FAILURE;
        goto cleanup_logger;
    }

    /* ----- Step 4: Initialize shared resources ----- */
    if (init_shared_resources() != 0) {
        LOG_ERR("Failed to initialize shared resources");
        exit_code = EXIT_FAILURE;
        goto cleanup_logger;
    }

    /* ----- Step 5: Initialize Wi-Fi scanner module ----- */
    scanner = wifi_scanner_init(ctrl_path,
                                &shared_scan_buffer,
                                &scan_buffer_mutex,
                                &scan_buffer_cond);
    if (!scanner) {
        LOG_ERR("Failed to initialize Wi-Fi scanner");
        exit_code = EXIT_FAILURE;
        goto cleanup_shared;
    }

    /* ----- Step 5b: Regulatory domain sanity check ----- */
    if (!country_known) {
        LOG_WRN("No built-in channel table for country '%s'; using the "
                "conservative world domain (00)", country);
    }
    {
        char radio_cc[3];

        if (wifi_scanner_get_country(scanner, radio_cc, sizeof(radio_cc)) != 0) {
            LOG_WRN("wpa_supplicant has no country set -- the radio uses the "
                    "world domain (passive on many 5 GHz channels). Add "
                    "'country=%s' to /boot/wpa_supplicant.conf and reboot.",
                    country);
        } else if (strncasecmp(radio_cc, country, 2) != 0) {
            LOG_WRN("Country mismatch: wpa_supplicant=%s, analyzer=%s. "
                    "Recommendations follow %s.",
                    radio_cc, country, regdomain_code(regdomain));
        } else {
            LOG_INF("Regulatory domain: %s (matches wpa_supplicant)",
                    regdomain_code(regdomain));
        }
    }

    /* ----- Step 6: Initialize channel analyzer module ----- */
    analyzer = channel_analyzer_init(regdomain,
                                     &shared_scan_buffer,
                                     &scan_buffer_mutex,
                                     &scan_buffer_cond);
    if (!analyzer) {
        LOG_ERR("Failed to initialize channel analyzer");
        exit_code = EXIT_FAILURE;
        goto cleanup_scanner;
    }

    /* ----- Step 7: Spawn the scanner thread ----- */
    {
        thread_config_t scanner_cfg = {
            .name         = "wifi_scanner",
            .entry_func   = wifi_scanner_run,
            .entry_arg    = scanner,
            .sched_policy = SCHED_FIFO,
            .priority     = SCANNER_THREAD_PRIORITY,
            .stack_size   = SCANNER_STACK_SIZE,
        };

        if (thread_manager_create(&scanner_thread, &scanner_cfg) != 0) {
            LOG_ERR("Failed to create scanner thread");
            exit_code = EXIT_FAILURE;
            goto cleanup_analyzer;
        }
    }

    /* ----- Step 8: Spawn the analyzer thread ----- */
    {
        thread_config_t analyzer_cfg = {
            .name         = "channel_analyzer",
            .entry_func   = channel_analyzer_run,
            .entry_arg    = analyzer,
            .sched_policy = SCHED_RR,
            .priority     = ANALYZER_THREAD_PRIORITY,
            .stack_size   = ANALYZER_STACK_SIZE,
        };

        if (thread_manager_create(&analyzer_thread, &analyzer_cfg) != 0) {
            LOG_ERR("Failed to create analyzer thread");
            /* Cancel the already-running scanner thread before exiting. */
            thread_manager_cancel_and_join(&scanner_thread);
            exit_code = EXIT_FAILURE;
            goto cleanup_analyzer;
        }
    }

    /* ----- Step 9: Wait for termination signal ----- */
    LOG_INF("Application running. Press Ctrl+C or send SIGTERM to stop.");

    /*
     * sigwait() blocks until one of the signals in wait_set is delivered.
     * This is the recommended POSIX pattern for multi-threaded signal
     * handling -- synchronous, safe, no async-signal-safety concerns.
     */
    if (sigwait(&wait_set, &caught_signal) != 0) {
        LOG_ERR("sigwait failed: %s", strerror(errno));
    } else {
        LOG_INF("Caught signal %d (%s), shutting down...",
                caught_signal,
                caught_signal == SIGINT ? "SIGINT" : "SIGTERM");
    }

    /* ----- Step 10: Cancel and join threads ----- */
    LOG_INF("Stopping threads...");
    thread_manager_cancel_and_join(&scanner_thread);
    thread_manager_cancel_and_join(&analyzer_thread);

    /* ----- Step 11: Clean up modules ----- */
cleanup_analyzer:
    if (analyzer) {
        channel_analyzer_shutdown(analyzer);
    }
cleanup_scanner:
    if (scanner) {
        wifi_scanner_shutdown(scanner);
    }
cleanup_shared:
    cleanup_shared_resources();
cleanup_logger:
    LOG_INF("=== Wi-Fi Spectrum Analyzer stopped (exit_code=%d) ===",
            exit_code);
    logger_shutdown();

    return exit_code;
}
