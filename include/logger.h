/**
 * @file logger.h
 * @brief Thread-safe logging utility API.
 *
 * Provides an asynchronous logging facility suitable for use from
 * real-time threads. A call formats the line (timestamped at call time) and
 * copies it into a fixed ring buffer. A dedicated writer thread
 * (LOGGER_WRITER_PRIORITY, SCHED_RR) does all output to stderr, which on QNX
 * can be redirected to a file or slogger2. Callers never block on I/O.
 *
 * Best practices:
 *   - Always use these logging functions instead of raw printf/fprintf.
 *   - The ring mutex uses PTHREAD_PRIO_INHERIT and guards only a memcpy.
 *   - If the ring is full (LOG_RING_ENTRIES), the newest message is dropped
 *     and the drop count is logged. Size the ring for the worst burst.
 *   - Lines longer than LOG_LINE_MAX are truncated with "...".
 *   - In production, raise LOG_MIN_LEVEL to LOG_WARN or LOG_ERROR to
 *     reduce overhead in real-time paths.
 */

#ifndef WIFI_SPECTRUM_LOGGER_H
#define WIFI_SPECTRUM_LOGGER_H

#include "config.h"

/**
 * @brief Initialize the logging subsystem.
 *
 * Must be called once from the main thread before any other logger call,
 * after blocking signals (the writer thread inherits the signal mask).
 * Sets up the ring mutex (PTHREAD_PRIO_INHERIT) and starts the writer
 * thread. If the writer can't be started, logging falls back to
 * synchronous writes.
 *
 * @return 0 on success, -1 on failure (mutex/condvar init failed).
 */
int logger_init(void);

/**
 * @brief Shut down the logging subsystem.
 *
 * Stops the writer thread after it has written every queued message, then
 * destroys the mutex and condvar. Call during application cleanup after all
 * threads that might log have been joined.
 */
void logger_shutdown(void);

/**
 * @brief Log a message at the specified severity level.
 *
 * Messages below LOG_MIN_LEVEL (defined in config.h) are silently dropped.
 * The output format is:
 *   [TIMESTAMP] [LEVEL] [file:line] message
 *
 * This function is thread-safe and doesn't block on I/O (only briefly on the
 * ring mutex). Output order matches call order.
 *
 * @param level   Severity level (LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR).
 * @param file    Source file name (use __FILE__).
 * @param line    Source line number (use __LINE__).
 * @param fmt     printf-style format string.
 * @param ...     Format arguments.
 */
void logger_log(log_level_t level, const char *file, int line,
                const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* -------------------------------------------------------------------------
 * Convenience macros
 * ------------------------------------------------------------------------- */

#define LOG_DBG(fmt, ...) \
    logger_log(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INF(fmt, ...) \
    logger_log(LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WRN(fmt, ...) \
    logger_log(LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERR(fmt, ...) \
    logger_log(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* WIFI_SPECTRUM_LOGGER_H */
