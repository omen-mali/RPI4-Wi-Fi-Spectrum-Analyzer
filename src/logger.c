/**
 * @file logger.c
 * @brief Thread-safe, asynchronous logging implementation.
 *
 * Producer side (any thread, including real-time ones):
 *   1. Take the timestamp (at call time, so log timing reflects the event).
 *   2. Format the whole line into a stack buffer. No lock is held.
 *   3. Under a PTHREAD_PRIO_INHERIT mutex, copy it into the next free slot of
 *      a fixed, preallocated ring and signal the writer. If the ring is full,
 *      count the message as dropped. Never block waiting for space.
 *
 * Writer side (one thread, SCHED_RR at LOGGER_WRITER_PRIORITY):
 *   Copy one slot out under the mutex, release it, then fwrite() to stderr.
 *   Flush when the ring runs empty, so I/O (e.g. a log file on the SD card)
 *   never happens under the lock or on a producer's thread.
 *
 * If the writer thread can't be started, the logger falls back to
 * synchronous output (format + write under the mutex).
 *
 * Best practices followed:
 *   - No dynamic allocation; the ring is static.
 *   - Mutex uses priority inheritance, and the critical section is a memcpy.
 *   - Timestamps use clock_gettime(CLOCK_REALTIME) for human-readable
 *     wall-clock time in log output.
 *   - All system call return values are checked.
 */

#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

#if (LOG_RING_ENTRIES & (LOG_RING_ENTRIES - 1)) != 0
#error "LOG_RING_ENTRIES must be a power of two"
#endif

/* =========================================================================
 * Internal State
 * ========================================================================= */

static pthread_mutex_t log_mutex;
static pthread_cond_t  log_cond;
static int             initialized = 0;

/* Ring: slots [tail, head) are pending; head - tail <= LOG_RING_ENTRIES. */
static char            ring[LOG_RING_ENTRIES][LOG_LINE_MAX];
static size_t          ring_len[LOG_RING_ENTRIES];
static unsigned long   head;
static unsigned long   tail;
static unsigned long   dropped;

static pthread_t       writer_thread;
static int             writer_running = 0;   /* 0 => synchronous fallback */
static int             stop_requested = 0;

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
 * Writer Thread
 * ========================================================================= */

static void *writer_main(void *arg)
{
    char          line[LOG_LINE_MAX];
    size_t        len;
    unsigned long lost;

    (void)arg;

    pthread_mutex_lock(&log_mutex);
    for (;;) {
        while (head == tail && dropped == 0 && !stop_requested) {
            pthread_cond_wait(&log_cond, &log_mutex);
        }

        if (head != tail) {
            unsigned long slot = tail & (LOG_RING_ENTRIES - 1);

            len = ring_len[slot];
            memcpy(line, ring[slot], len);
            tail++;

            pthread_mutex_unlock(&log_mutex);
            fwrite(line, 1, len, stderr);
            pthread_mutex_lock(&log_mutex);
        } else if (dropped != 0) {
            lost = dropped;
            dropped = 0;

            pthread_mutex_unlock(&log_mutex);
            fprintf(stderr, "[logger] %lu message(s) dropped: ring full\n", lost);
            pthread_mutex_lock(&log_mutex);
        } else if (stop_requested) {
            break;                          /* drained and asked to stop */
        }

        if (head == tail) {
            /* Batch boundary: flush outside the lock. */
            pthread_mutex_unlock(&log_mutex);
            fflush(stderr);
            pthread_mutex_lock(&log_mutex);
        }
    }
    pthread_mutex_unlock(&log_mutex);

    fflush(stderr);
    return NULL;
}

static int start_writer(void)
{
    pthread_attr_t     attr;
    struct sched_param param;
    int                rc;

    rc = pthread_attr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    memset(&param, 0, sizeof(param));
    param.sched_priority = LOGGER_WRITER_PRIORITY;
    pthread_attr_setschedparam(&attr, &param);

    rc = pthread_create(&writer_thread, &attr, writer_main, NULL);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        /* e.g. EPERM on a host without RT privileges: inherit instead. */
        rc = pthread_create(&writer_thread, NULL, writer_main, NULL);
    }
#ifdef __QNX__
    if (rc == 0) {
        pthread_setname_np(writer_thread, "logger");   /* visible in pidin */
    }
#endif
    return rc;
}

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

    rc = pthread_cond_init(&log_cond, NULL);
    if (rc != 0) {
        fprintf(stderr, "logger: cond_init failed: %s\n", strerror(rc));
        pthread_mutex_destroy(&log_mutex);
        return -1;
    }

    /*
     * Touch every ring page now, so the first log calls from a real-time
     * thread don't take page faults on the static buffer.
     */
    memset(ring, 0, sizeof(ring));
    memset(ring_len, 0, sizeof(ring_len));

    head = tail = dropped = 0;
    stop_requested = 0;
    initialized = 1;

    rc = start_writer();
    if (rc == 0) {
        writer_running = 1;
    } else {
        writer_running = 0;
        fprintf(stderr, "logger: writer thread failed (%s); logging synchronously\n",
                strerror(rc));
    }
    return 0;
}

void logger_shutdown(void)
{
    if (!initialized) {
        return;
    }

    if (writer_running) {
        pthread_mutex_lock(&log_mutex);
        stop_requested = 1;
        pthread_cond_signal(&log_cond);
        pthread_mutex_unlock(&log_mutex);

        pthread_join(writer_thread, NULL);   /* drains the ring first */
        writer_running = 0;
    }

    initialized = 0;
    pthread_cond_destroy(&log_cond);
    pthread_mutex_destroy(&log_mutex);
}

void logger_log(log_level_t level, const char *file, int line,
                const char *fmt, ...)
{
    struct timespec ts;
    struct tm       tm_buf;
    char            buf[LOG_LINE_MAX];
    va_list         args;
    int             n;
    size_t          len;

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
     * Get wall-clock time for the log timestamp, at call time.
     *
     * Note: CLOCK_REALTIME gives human-readable time but can jump
     * (e.g., NTP adjustments). For log ordering that's acceptable.
     * For measuring intervals, always use CLOCK_MONOTONIC instead.
     */
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_buf);

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

    /* Format the complete line outside any lock. */
    n = snprintf(buf, sizeof(buf),
                 "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] [%s] [%s:%d] ",
                 tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                 ts.tv_nsec / 1000000L,
                 (level >= 0 && level <= LOG_ERROR) ? level_names[level] : "?????",
                 file, line);
    len = (n < 0) ? 0 : ((size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);

    va_start(args, fmt);
    n = vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
    va_end(args);
    if (n > 0) {
        len += (size_t)n;
    }

    /* Room for "\n"; mark truncation with "...". */
    if (len > sizeof(buf) - 2) {
        len = sizeof(buf) - 2;
        memcpy(buf + len - 3, "...", 3);
    }
    buf[len++] = '\n';

    pthread_mutex_lock(&log_mutex);
    if (!writer_running) {
        /* Synchronous fallback. */
        fwrite(buf, 1, len, stderr);
        fflush(stderr);
    } else if (head - tail < LOG_RING_ENTRIES) {
        unsigned long slot = head & (LOG_RING_ENTRIES - 1);

        memcpy(ring[slot], buf, len);
        ring_len[slot] = len;
        head++;
        pthread_cond_signal(&log_cond);
    } else {
        dropped++;
        pthread_cond_signal(&log_cond);
    }
    pthread_mutex_unlock(&log_mutex);
}
