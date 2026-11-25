/**
 * @file schedule.c
 * @brief timespec arithmetic and the fixed-rate slot advance (schedule.h).
 *
 * ts_add_ms() and ts_diff_ns() do CLOCK_MONOTONIC arithmetic with tv_nsec
 * kept normalized. schedule_advance() moves the scanner's next deadline along
 * a fixed grid of period-long slots and counts the slots an overrun skipped.
 * No function here reads the clock or sleeps.
 */

#include "schedule.h"

void ts_add_ms(struct timespec *ts, uint32_t ms)
{
    ts->tv_sec  += (time_t)(ms / 1000u);
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= NSEC_PER_SEC) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= NSEC_PER_SEC;
    }
}

int64_t ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (int64_t)(a->tv_sec - b->tv_sec) * NSEC_PER_SEC +
           (int64_t)(a->tv_nsec - b->tv_nsec);
}

uint32_t schedule_advance(struct timespec *next, const struct timespec *now,
                          uint32_t period_ms)
{
    uint32_t skipped = 0;

    ts_add_ms(next, period_ms);
    while (ts_diff_ns(now, next) >= 0) {
        ts_add_ms(next, period_ms);
        skipped++;
    }
    return skipped;
}
