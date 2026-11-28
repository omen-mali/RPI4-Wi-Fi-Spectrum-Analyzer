/**
 * @file test_schedule.c
 * @brief timespec helpers and the fixed-rate slot advance (schedule.c).
 */

#include "schedule.h"
#include "test.h"

/** A timespec of @p s seconds and @p ns nanoseconds. */
static struct timespec ts(time_t s, long ns)
{
    struct timespec t;
    t.tv_sec = s;
    t.tv_nsec = ns;
    return t;
}

static void test_add_ms_carries_nsec(void)
{
    struct timespec t = ts(1, 999000000L);
    ts_add_ms(&t, 2);
    CHECK_EQ_INT(t.tv_sec, 2);
    CHECK_EQ_INT(t.tv_nsec, 1000000L);
}

static void test_add_ms_large(void)
{
    struct timespec t = ts(0, 0);
    ts_add_ms(&t, 600000);
    CHECK_EQ_INT(t.tv_sec, 600);
    CHECK_EQ_INT(t.tv_nsec, 0);
}

static void test_diff_ns_sign(void)
{
    struct timespec a = ts(10, 500000000L), b = ts(9, 750000000L);
    CHECK_EQ_INT(ts_diff_ns(&a, &b), 750000000LL);
    CHECK_EQ_INT(ts_diff_ns(&b, &a), -ts_diff_ns(&a, &b));
}

static void test_advance_on_time(void)
{
    struct timespec next = ts(10, 0), now = ts(10, 2000000L);
    CHECK_EQ_INT(schedule_advance(&next, &now, 1000), 0);
    CHECK_EQ_INT(next.tv_sec, 11);
    CHECK_EQ_INT(next.tv_nsec, 0);
}

static void test_advance_one_overrun(void)
{
    struct timespec next = ts(10, 0), now = ts(11, 500000000L);
    CHECK_EQ_INT(schedule_advance(&next, &now, 1000), 1);
    CHECK_EQ_INT(next.tv_sec, 12);
    CHECK_EQ_INT(next.tv_nsec, 0);
}

static void test_advance_exact_boundary(void)
{
    /* A deadline equal to now counts as missed (>= 0). */
    struct timespec next = ts(10, 0), now = ts(11, 0);
    CHECK_EQ_INT(schedule_advance(&next, &now, 1000), 1);
    CHECK_EQ_INT(next.tv_sec, 12);
}

static void test_advance_phase_kept(void)
{
    const struct timespec start = ts(100, 250000000L);
    struct timespec next = start;
    const uint32_t period = 10000;
    /* Late by varying amounts, including several whole periods. */
    const long late_ms[] = { 0, 3, 9999, 10000, 25001, 47000 };
    for (size_t i = 0; i < sizeof(late_ms) / sizeof(late_ms[0]); i++) {
        struct timespec now = next;
        ts_add_ms(&now, (uint32_t)late_ms[i]);
        (void)schedule_advance(&next, &now, period);
        CHECK(ts_diff_ns(&next, &now) > 0);
        CHECK_EQ_INT(ts_diff_ns(&next, &start) % ((int64_t)period * 1000000), 0);
    }
}

int main(void)
{
    RUN(test_add_ms_carries_nsec);
    RUN(test_add_ms_large);
    RUN(test_diff_ns_sign);
    RUN(test_advance_on_time);
    RUN(test_advance_one_overrun);
    RUN(test_advance_exact_boundary);
    RUN(test_advance_phase_kept);
    return test_finish();
}
