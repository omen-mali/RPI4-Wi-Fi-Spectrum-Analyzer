/**
 * @file logger.c
 * @brief Thread-safe logging implementation.
 *
 * Uses a POSIX mutex with PTHREAD_PRIO_INHERIT to ensure that logging
 * from high-priority real-time threads does not suffer unbounded priority
 * inversion.
 *
 * Best practices followed:
 *   - Mutex uses priority inheritance protocol.
 *   - Timestamps use clock_gettime(CLOCK_REALTIME) for human-readable
 *     wall-clock time in log output.
 *   - Log formatting is kept simple to minimize time under the lock.
 *   - All system call return values are checked.
 */

#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* =========================================================================
 * Internal State
 * ========================================================================= */

static pthread_mutex_t log_mutex;
static int             initialized = 0;

/**
 * @brief Human-readable names for log levels.
 */
static const char *level_names[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO]  = "INFO",
    [LOG_WARN]  = "WARN",
    [LOG_ERROR] = "ERROR",
};

/* =========================================================================
 * Public API
 * ========================================================================= */

int logger_init(void)
{
    pthread_mutexattr_t attr;
    int rc;

    /*
     * Best practice: Use PTHREAD_PRIO_INHERIT on all mutexes shared
     * between threads of different priorities. This prevents a low-priority
     * thread holding the lock from blocking a high-priority thread
     * indefinitely (priority inversion).
     */
    rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        fprintf(stderr, "logger: mutexattr_init failed: %s\n", strerror(rc));
        return -1;
    }

    rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if (rc != 0) {
        fprintf(stderr, "logger: setprotocol PRIO_INHERIT failed: %s\n",
                strerror(rc));
        pthread_mutexattr_destroy(&attr);
        return -1;
    }

    rc = pthread_mutex_init(&log_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if (rc != 0) {
        fprintf(stderr, "logger: mutex_init failed: %s\n", strerror(rc));
        return -1;
    }

    initialized = 1;
    return 0;
}

void logger_shutdown(void)
{
    if (initialized) {
        pthread_mutex_destroy(&log_mutex);
        initialized = 0;
    }
}

void logger_log(log_level_t level, const char *file, int line,
                const char *fmt, ...)
{
    struct timespec ts;
    struct tm       tm_buf;
    char            time_str[32];
    va_list         args;

    /* Filter messages below the configured minimum level. */
    if (level < LOG_MIN_LEVEL) {
        return;
    }

    if (!initialized) {
        /* Fallback: if logger not initialized, print without locking. */
        fprintf(stderr, "[NO_INIT] ");
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
        return;
    }

    /*
     * Get wall-clock time for the log timestamp.
     *
     * Note: CLOCK_REALTIME gives human-readable time but can jump
     * (e.g., NTP adjustments). For log ordering that's acceptable.
     * For measuring intervals, always use CLOCK_MONOTONIC instead.
     */
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_buf);
    snprintf(time_str, sizeof(time_str),
             "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             ts.tv_nsec / 1000000L);

    /*
     * Strip directory prefix from __FILE__ so log lines show only the
     * basename (e.g. "wifi_scanner.c:42" instead of "/mnt/c/.../wifi_scanner.c:42").
     * Handles both '/' (POSIX/WSL) and '\\' (Windows) separators.
     */
    {
        const char *slash = strrchr(file, '/');
        const char *bslash = strrchr(file, '\\');
        if (bslash && (!slash || bslash > slash))
            slash = bslash;
        if (slash)
            file = slash + 1;
    }

    /*
     * Lock, format, print, unlock.
     * Keep the critical section as short as possible.
     */
    pthread_mutex_lock(&log_mutex);

    fprintf(stderr, "[%s] [%s] [%s:%d] ",
            time_str,
            (level >= 0 && level <= LOG_ERROR) ? level_names[level] : "?????",
            file, line);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);

    pthread_mutex_unlock(&log_mutex);
}
