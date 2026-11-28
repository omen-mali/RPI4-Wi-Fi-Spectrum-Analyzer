/**
 * @file test_logger.c
 * @brief Line format, ordering, ring overflow and shutdown flush (logger.c).
 */

#include "log_capture.h"
#include "test.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_format(void)
{
    /* [YYYY-MM-DD HH:MM:SS.mmm] [INFO] [test_logger.c:<line>] x=5 */
    log_capture_begin();
    int expected_line = __LINE__ + 1;
    LOG_INF("x=%d", 5);
    const char *log = log_capture_end();

    int y, mo, d, h, mi, s, ms, line, used = -1;
    char level[8], file[64];
    int n = sscanf(log, "[%4d-%2d-%2d %2d:%2d:%2d.%3d] [%7[^]]] [%63[^:]:%d] x=5\n%n",
                   &y, &mo, &d, &h, &mi, &s, &ms, level, file, &line, &used);
    CHECK_EQ_INT(n, 10);
    CHECK_STR(level, "INFO");
    CHECK_STR(file, "test_logger.c");        /* basename only */
    CHECK_EQ_INT(line, expected_line);
    CHECK_EQ_INT(used, (int)strlen(log));    /* exactly one line, nothing else */
    CHECK(y >= 2024 && mo >= 1 && mo <= 12 && ms >= 0 && ms <= 999);
}

/* Sum of the "[logger] N message(s) dropped: ring full" reports in log. */
static long reported_drops(const char *log)
{
    long dropped = 0;
    for (const char *p = log; (p = strstr(p, "[logger] ")) != NULL; p++) {
        unsigned long n;
        if (sscanf(p, "[logger] %lu message(s) dropped: ring full", &n) == 1) {
            dropped += (long)n;
        }
    }
    return dropped;
}

#define ORDER_LINES 1000   /* more than LOG_RING_ENTRIES, so drops may happen */

static void test_order_preserved(void)
{
    log_capture_begin();
    for (int i = 0; i < ORDER_LINES; i++) {
        LOG_INF("seq=%d", i);
    }
    const char *log = log_capture_end();

    /*
     * A burst this size can outrun the writer, and the logger then drops
     * the newest lines. Whatever arrives must still be in order, and every
     * line must be either written or reported dropped.
     */
    int delivered = 0, last = -1, ok = 1;
    for (const char *p = log; (p = strstr(p, "seq=")) != NULL; p += 4) {
        int seq = atoi(p + 4);
        if (seq <= last) {
            ok = 0;
        }
        last = seq;
        delivered++;
    }
    CHECK(ok);
    CHECK(delivered > 0);
    CHECK_EQ_INT(delivered + reported_drops(log), ORDER_LINES);
}

#define WRITERS 4
#define LINES_PER_WRITER 10000

/** Writer thread: log LINES_PER_WRITER numbered lines tagged with its id. */
static void *burst(void *arg)
{
    int id = (int)(long)arg;
    for (int i = 0; i < LINES_PER_WRITER; i++) {
        LOG_INF("w%d %d", id, i);
    }
    return NULL;
}

static void test_overflow_reports_drops(void)
{
    pthread_t t[WRITERS];

    log_capture_begin();
    for (long i = 0; i < WRITERS; i++) {
        pthread_create(&t[i], NULL, burst, (void *)i);
    }
    for (int i = 0; i < WRITERS; i++) {
        pthread_join(t[i], NULL);
    }
    const char *log = log_capture_end();

    long lines = 0, dropped = reported_drops(log);
    for (const char *p = log; (p = strstr(p, "] w")) != NULL; p += 3) {
        lines++;
    }
    /* Either everything arrived, or the drops were all reported. */
    CHECK_EQ_INT(lines + dropped, WRITERS * LINES_PER_WRITER);
    printf("   (%ld lines written, %ld reported dropped)\n", lines, dropped);
}

static void test_shutdown_flushes(void)
{
    log_capture_begin();
    for (int i = 0; i < 100; i++) {
        LOG_INF("filler %d", i);
    }
    LOG_ERR("last line %s", "before shutdown");
    const char *log = log_capture_end();
    CHECK(log_contains(log, "[ERROR]"));
    CHECK(log_contains(log, "last line before shutdown\n"));
}

int main(void)
{
    RUN(test_format);
    RUN(test_order_preserved);
    RUN(test_overflow_reports_drops);
    RUN(test_shutdown_flushes);
    return test_finish();
}
