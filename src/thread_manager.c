/**
 * @file thread_manager.c
 * @brief RTOS thread lifecycle management implementation.
 *
 * Creates POSIX threads with explicit real-time scheduling attributes
 * for QNX Neutrino. Handles the full attribute setup required for
 * deterministic thread behavior.
 *
 * =========================================================================
 * POSIX THREAD ATTRIBUTE SETUP SEQUENCE
 * =========================================================================
 *
 * When creating a real-time thread on QNX, the following attributes MUST
 * be configured in order:
 *
 *   1. pthread_attr_init()             -- Initialize attribute object.
 *   2. pthread_attr_setinheritsched()  -- Set to PTHREAD_EXPLICIT_SCHED so
 *      the thread uses OUR attributes, not the parent's.
 *   3. pthread_attr_setschedpolicy()   -- Set SCHED_FIFO or SCHED_RR.
 *   4. pthread_attr_setschedparam()    -- Set the priority.
 *   5. pthread_attr_setstacksize()     -- Set the stack size.
 *   6. pthread_create()                -- Create the thread.
 *   7. pthread_attr_destroy()          -- Clean up the attribute object.
 *
 * If you skip setinheritsched(PTHREAD_EXPLICIT_SCHED), the new thread
 * inherits the creating thread's scheduling attributes, which is almost
 * never what you want in a real-time application.
 *
 * =========================================================================
 */

#include "thread_manager.h"
#include "logger.h"

#include <string.h>
#include <errno.h>
#include <sched.h>

int thread_manager_create(managed_thread_t *thread,
                          const thread_config_t *config)
{
    pthread_attr_t attr;
    struct sched_param sched;
    int rc;

    if (!thread || !config || !config->entry_func) {
        LOG_ERR("thread_manager_create: invalid arguments");
        return -1;
    }

    thread->name    = config->name ? config->name : "unnamed";
    thread->created = 0;

    LOG_INF("Creating thread '%s' (policy=%d, priority=%d, stack=%zu)",
            thread->name, config->sched_policy, config->priority,
            config->stack_size);

    /* Step 1: Initialize thread attributes. */
    rc = pthread_attr_init(&attr);
    if (rc != 0) {
        LOG_ERR("pthread_attr_init failed for '%s': %s",
                thread->name, strerror(rc));
        return -1;
    }

    /*
     * Step 2: CRITICAL -- Set explicit scheduling inheritance.
     *
     * Without this, the new thread inherits the calling thread's
     * scheduling policy and priority, ignoring everything we set below.
     */
    rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    if (rc != 0) {
        LOG_ERR("pthread_attr_setinheritsched failed for '%s': %s",
                thread->name, strerror(rc));
        pthread_attr_destroy(&attr);
        return -1;
    }

    /* Step 3: Set the scheduling policy. */
    rc = pthread_attr_setschedpolicy(&attr, config->sched_policy);
    if (rc != 0) {
        LOG_ERR("pthread_attr_setschedpolicy(%d) failed for '%s': %s",
                config->sched_policy, thread->name, strerror(rc));
        pthread_attr_destroy(&attr);
        return -1;
    }

    /* Step 4: Set the thread priority. */
    memset(&sched, 0, sizeof(sched));
    sched.sched_priority = config->priority;
    rc = pthread_attr_setschedparam(&attr, &sched);
    if (rc != 0) {
        LOG_ERR("pthread_attr_setschedparam(prio=%d) failed for '%s': %s",
                config->priority, thread->name, strerror(rc));
        pthread_attr_destroy(&attr);
        return -1;
    }

    /* Step 5: Set the stack size (if specified). */
    if (config->stack_size > 0) {
        rc = pthread_attr_setstacksize(&attr, config->stack_size);
        if (rc != 0) {
            LOG_ERR("pthread_attr_setstacksize(%zu) failed for '%s': %s",
                    config->stack_size, thread->name, strerror(rc));
            pthread_attr_destroy(&attr);
            return -1;
        }
    }

    /* Step 6: Create the thread. */
    rc = pthread_create(&thread->tid, &attr, config->entry_func, config->entry_arg);
    if (rc != 0) {
        LOG_ERR("pthread_create failed for '%s': %s",
                thread->name, strerror(rc));
        /*
         * Common failure causes:
         *   - EPERM: Insufficient privilege for the requested priority.
         *     Fix: Use procmgr_ability() in main to grant PROCMGR_AID_PRIORITY,
         *     or lower the requested priority.
         *   - EAGAIN: System resource limits (too many threads, out of memory).
         *   - EINVAL: Invalid attribute value (bad policy/priority combo).
         */
        pthread_attr_destroy(&attr);
        return -1;
    }

    /* Step 7: Clean up attributes (the thread is already running). */
    pthread_attr_destroy(&attr);

    thread->created = 1;
    LOG_INF("Thread '%s' created successfully (tid=%lu)",
            thread->name, (unsigned long)thread->tid);

    return 0;
}

int thread_manager_join(managed_thread_t *thread)
{
    int rc;

    if (!thread || !thread->created) {
        return -1;
    }

    LOG_INF("Joining thread '%s'...", thread->name);

    rc = pthread_join(thread->tid, NULL);
    if (rc != 0) {
        LOG_ERR("pthread_join failed for '%s': %s",
                thread->name, strerror(rc));
        return -1;
    }

    thread->created = 0;
    LOG_INF("Thread '%s' joined", thread->name);
    return 0;
}

int thread_manager_cancel_and_join(managed_thread_t *thread)
{
    int rc;

    if (!thread || !thread->created) {
        return -1;
    }

    LOG_INF("Cancelling thread '%s'...", thread->name);

    /*
     * Send a cancellation request. The thread will terminate the next time
     * it reaches a POSIX cancellation point (e.g., clock_nanosleep,
     * pthread_cond_wait, read, write, etc.).
     *
     * Best practice: Ensure all long-running thread loops contain at least
     * one cancellation point per iteration. If loop body has no natural
     * cancellation points, insert pthread_testcancel() explicitly.
     */
    rc = pthread_cancel(thread->tid);
    if (rc != 0) {
        LOG_ERR("pthread_cancel failed for '%s': %s",
                thread->name, strerror(rc));
        return -1;
    }

    /* Wait for the thread to actually terminate. */
    rc = pthread_join(thread->tid, NULL);
    if (rc != 0) {
        LOG_ERR("pthread_join after cancel failed for '%s': %s",
                thread->name, strerror(rc));
        return -1;
    }

    thread->created = 0;
    LOG_INF("Thread '%s' cancelled and joined", thread->name);
    return 0;
}
