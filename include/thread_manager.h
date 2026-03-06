/**
 * @file thread_manager.h
 * @brief RTOS thread lifecycle management API.
 *
 * Abstracts POSIX thread creation with QNX real-time scheduling attributes.
 * Provides a clean interface for creating, monitoring, and shutting down
 * the scanner and analyzer threads with proper priority and scheduling
 * policy configuration.
 *
 * QNX Scheduling Overview:
 *
 *   QNX Neutrino is a priority-preemptive RTOS. The highest-priority
 *   ready thread always runs. Scheduling policies only matter when
 *   multiple threads share the same priority:
 *
 *     SCHED_FIFO:     Runs until it blocks or is preempted. No timeslice.
 *                     Best for: short, deterministic I/O-bound tasks.
 *
 *     SCHED_RR:       Round-robin with a timeslice. Threads at the same
 *                     priority take turns.
 *                     Best for: compute-bound tasks sharing a priority level.
 *
 *     SCHED_SPORADIC: Provides a replenishment budget model for mixed
 *                     criticality workloads. (Advanced -- not used here.)
 *
 *   Priority range: 1 (lowest) to 255 (highest, kernel-only).
 *   Unprivileged max is typically 63. Use procmgr_ability() to raise.
 *
 * Best practices for thread creation:
 *   - Always set scheduling attributes explicitly (don't rely on defaults).
 *   - Use PTHREAD_EXPLICIT_SCHED so the new thread's attributes are taken
 *     from the attr, not inherited from the creating thread.
 *   - Set stack size explicitly to avoid surprises.
 *   - Check every pthread_* return value.
 */

#ifndef WIFI_SPECTRUM_THREAD_MANAGER_H
#define WIFI_SPECTRUM_THREAD_MANAGER_H

#include <pthread.h>

/**
 * @brief Thread configuration parameters.
 */
typedef struct {
    const char *name;              /**< Descriptive name for logging */
    void *(*entry_func)(void *);   /**< Thread entry point function */
    void *entry_arg;               /**< Argument passed to entry_func */
    int   sched_policy;            /**< SCHED_FIFO, SCHED_RR, etc. */
    int   priority;                /**< QNX thread priority (1-255) */
    size_t stack_size;             /**< Stack size in bytes (0 = default) */
} thread_config_t;

/**
 * @brief Managed thread handle.
 */
typedef struct {
    pthread_t  tid;       /**< POSIX thread ID */
    const char *name;     /**< Thread name for logging */
    int        created;   /**< 1 if thread was successfully created */
} managed_thread_t;

/**
 * @brief Create and start a real-time thread with the given configuration.
 *
 * Sets up pthread attributes (scheduling policy, priority, stack size,
 * explicit scheduling inheritance) and calls pthread_create().
 *
 * @param thread  Output: managed thread handle.
 * @param config  Thread configuration parameters.
 * @return 0 on success, -1 on failure (details logged).
 */
int thread_manager_create(managed_thread_t *thread,
                          const thread_config_t *config);

/**
 * @brief Join (wait for) a managed thread to finish.
 *
 * Blocks until the thread exits. Used during orderly shutdown.
 *
 * @param thread  The managed thread to join.
 * @return 0 on success, -1 on failure.
 */
int thread_manager_join(managed_thread_t *thread);

/**
 * @brief Cancel a managed thread and then join it.
 *
 * Sends a cancellation request to the thread and waits for it to
 * terminate. The thread must reach a POSIX cancellation point
 * (e.g., clock_nanosleep, pthread_cond_wait) to be cancelled.
 *
 * Best practice: Design threads so they periodically reach cancellation
 * points or check a 'running' flag. Avoid disabling cancellation.
 *
 * @param thread  The managed thread to cancel and join.
 * @return 0 on success, -1 on failure.
 */
int thread_manager_cancel_and_join(managed_thread_t *thread);

#endif /* WIFI_SPECTRUM_THREAD_MANAGER_H */
