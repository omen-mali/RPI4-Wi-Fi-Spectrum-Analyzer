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

static void test_order_preserved(void)
{
    log_capture_begin();
    for (int i = 0; i < 1000; i++) {
        LOG_INF("seq=%d", i);
    }
    const char *log = log_capture_end();

    int next = 0, ok = 1;
    for (const char *p = log; (p = strstr(p, "seq=")) != NULL; p += 4) {
        if (atoi(p + 4) != next++) {
            ok = 0;
        }
    }
    CHECK(ok);
    CHECK_EQ_INT(next, 1000);
}

#define WRITERS 4
#define LINES_PER_WRITER 10000

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

    long lines = 0, dropped = 0;
    for (const char *p = log; *p; ) {
        const char *eol = strchr(p, '\n');
        if (!eol) {
            break;
        }
        unsigned long n;
        if (strstr(p, "] w") && strstr(p, "] w") < eol) {
            lines++;
        } else if (sscanf(p, "[logger] %lu message(s) dropped: ring full", &n) == 1) {
            dropped += (long)n;
        }
        p = eol + 1;
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
