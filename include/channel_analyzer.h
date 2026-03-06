/**
 * @file channel_analyzer.h
 * @brief Channel analysis module API.
 *
 * This module consumes the raw scan results produced by the Wi-Fi scanner
 * and computes per-channel metrics to determine the optimal channel for
 * each band (2.4 GHz and 5 GHz).
 *
 * Design:
 *   The analyzer runs in its own thread, sleeping on a condition variable
 *   until the scanner signals that new data is available. It then reads
 *   the shared scan buffer under a mutex lock, performs analysis, and
 *   outputs the results.
 *
 * API usage pattern:
 *   1. Call channel_analyzer_init() once at startup.
 *   2. Pass channel_analyzer_run() to pthread_create() as the thread entry.
 *   3. On shutdown, call channel_analyzer_shutdown().
 */

#ifndef WIFI_SPECTRUM_CHANNEL_ANALYZER_H
#define WIFI_SPECTRUM_CHANNEL_ANALYZER_H

#include "config.h"
#include <pthread.h>

/**
 * @brief Opaque analyzer context.
 */
typedef struct channel_analyzer channel_analyzer_t;

/**
 * @brief Initialize the channel analyzer module.
 *
 * @param shared_buffer  Pointer to the shared scan results buffer (read-only).
 * @param buffer_mutex   Pointer to the mutex protecting the shared buffer.
 * @param buffer_cond    Pointer to the condition variable signaled on new data.
 *
 * @return Pointer to an allocated analyzer context, or NULL on failure.
 */
channel_analyzer_t *channel_analyzer_init(scan_buffer_t *shared_buffer,
                                          pthread_mutex_t *buffer_mutex,
                                          pthread_cond_t *buffer_cond);

/**
 * @brief Analyzer thread entry point.
 *
 * Waits on the condition variable for new scan data, then performs
 * channel analysis and logs/outputs the optimal channel recommendations.
 *
 * @param arg  Pointer to the channel_analyzer_t context (cast from void*).
 * @return NULL (thread return value).
 */
void *channel_analyzer_run(void *arg);

/**
 * @brief Retrieve the latest analysis results.
 *
 * Thread-safe. Copies the most recent analysis_result_t into the
 * caller-provided output buffer.
 *
 * @param analyzer  The analyzer context.
 * @param out       Output buffer for the analysis results.
 * @return 0 on success, -1 if no analysis has been performed yet.
 */
int channel_analyzer_get_results(channel_analyzer_t *analyzer,
                                 analysis_result_t *out);

/**
 * @brief Shut down the analyzer and release resources.
 *
 * The analyzer thread should be cancelled/joined before calling this.
 *
 * @param analyzer  The analyzer context to destroy.
 */
void channel_analyzer_shutdown(channel_analyzer_t *analyzer);

#endif /* WIFI_SPECTRUM_CHANNEL_ANALYZER_H */
