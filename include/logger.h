/**
 * @file logger.h
 * @brief Thread-safe logging utility API.
 *
 * Provides a simple, mutex-protected logging facility suitable for use
 * from multiple real-time threads. All log output goes to stderr (which
 * on QNX can be redirected to slogger2 for persistent logging).
 *
 * Best practices:
 *   - Always use these logging functions instead of raw printf/fprintf.
 *   - The logger mutex uses PTHREAD_PRIO_INHERIT to prevent priority
 *     inversion when a high-priority thread logs while a low-priority
 *     thread holds the lock.
 *   - Keep log messages short to minimize time spent holding the mutex.
 *   - In production, raise LOG_MIN_LEVEL to LOG_WARN or LOG_ERROR to
 *     reduce overhead in real-time paths.
 */

#ifndef WIFI_SPECTRUM_LOGGER_H
#define WIFI_SPECTRUM_LOGGER_H

#include "config.h"

/**
 * @brief Initialize the logging subsystem.
 *
 * Must be called once from the main thread before any other logger call.
 * Sets up the internal mutex with PTHREAD_PRIO_INHERIT protocol.
 *
 * @return 0 on success, -1 on failure (mutex init failed).
 */
int logger_init(void);

/**
 * @brief Shut down the logging subsystem.
 *
 * Destroys the internal mutex. Call during application cleanup after all
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
 * This function is thread-safe.
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
