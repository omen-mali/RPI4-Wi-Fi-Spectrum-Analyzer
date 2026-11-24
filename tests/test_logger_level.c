/**
 * @file test_logger_level.c
 * @brief LOG_MIN_LEVEL filtering, with a logger built at LOG_WARN.
 */

#include "log_capture.h"
#include "test.h"

static void test_below_min_level_dropped(void)
{
    log_capture_begin();
    LOG_DBG("debug %d", 1);
    LOG_INF("info %d", 2);
    LOG_WRN("warn %d", 3);
    LOG_ERR("error %d", 4);
    const char *log = log_capture_end();
    CHECK(!log_contains(log, "debug 1"));
    CHECK(!log_contains(log, "info 2"));
    CHECK(log_contains(log, "[WARN]"));
    CHECK(log_contains(log, "warn 3"));
    CHECK(log_contains(log, "error 4"));
}

int main(void)
{
    RUN(test_below_min_level_dropped);
    return test_finish();
}
