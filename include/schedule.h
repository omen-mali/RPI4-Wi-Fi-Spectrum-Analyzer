/**
 * @file schedule.h
 * @brief CLOCK_MONOTONIC timespec arithmetic and the fixed-rate slot advance.
 *
 * Pure functions (no clock reads, no sleeping), used by the scanner's
 * fixed-rate loop and tested directly with chosen timestamps.
 */

#ifndef WIFI_SPECTRUM_SCHEDULE_H
#define WIFI_SPECTRUM_SCHEDULE_H

#include <stdint.h>
#include <time.h>

#define NSEC_PER_SEC 1000000000L

/** Add @p ms milliseconds to @p ts (tv_nsec stays normalized). */
void ts_add_ms(struct timespec *ts, uint32_t ms);

/** @return a - b in nanoseconds. */
int64_t ts_diff_ns(const struct timespec *a, const struct timespec *b);

/**
 * @brief Move a fixed-rate deadline to the next slot that's still ahead.
 *
 * Adds one period to *next (the next slot on the grid, measured from the
 * previous deadline, not from now). While that deadline isn't in the future
 * (now >= *next), adds whole periods, so the schedule stays phase-aligned
 * instead of firing back-to-back to catch up.
 *
 * @param next       In: the deadline just served. Out: the next one after now.
 * @param now        The current CLOCK_MONOTONIC time.
 * @param period_ms  Slot length.
 * @return The number of slots skipped (0 when on time).
 */
uint32_t schedule_advance(struct timespec *next, const struct timespec *now,
                          uint32_t period_ms);

#endif /* WIFI_SPECTRUM_SCHEDULE_H */
