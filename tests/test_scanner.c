/**
 * @file test_scanner.c
 * @brief The scanner's cycle against the fake libwpactrl (wifi_scanner.c).
 *
 * Built with SCAN_COMPLETE_TIMEOUT_MS=300 (see tests/CMakeLists.txt). Each
 * test scripts the fake, runs wifi_scanner_run() in a thread with a 1000 ms
 * period, waits for the effect, stops the thread as main.c does
 * (pthread_cancel + join) and then checks the shared buffer and the log.
 */

#include "fake_wpa_ctrl.h"
#include "log_capture.h"
#include "schedule.h"
#include "test.h"
#include "wifi_scanner.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#define CTRL_PATH "/fake/wpa_supplicant/bcm0"

typedef struct {
    scan_buffer_t    buf;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    wifi_scanner_t  *scanner;
    pthread_t        thread;
    int              started;
} rig_t;

static rig_t rig;

/** Set @p t to @p ms from now on CLOCK_REALTIME (for pthread_cond_timedwait). */
static void deadline_in(struct timespec *t, int ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    ts_add_ms(t, (uint32_t)ms);
}

/** Init the scanner on a fresh rig; start its thread if @p run. */
static int rig_start(int run)
{
    memset(&rig, 0, sizeof(rig));
    pthread_mutex_init(&rig.mutex, NULL);
    pthread_cond_init(&rig.cond, NULL);
    rig.scanner = wifi_scanner_init(CTRL_PATH, 1000, &rig.buf, &rig.mutex, &rig.cond);
    if (!rig.scanner) {
        return -1;
    }
    if (run) {
        rig.started = pthread_create(&rig.thread, NULL, wifi_scanner_run,
                                     rig.scanner) == 0;
    }
    return 0;
}

/** Wait up to @p ms for a publish (timestamp_ns becoming non-zero). */
static int wait_publish(int ms)
{
    struct timespec t;
    int rc = 0;
    deadline_in(&t, ms);
    pthread_mutex_lock(&rig.mutex);
    while (rig.buf.timestamp_ns == 0 && rc == 0) {
        rc = pthread_cond_timedwait(&rig.cond, &rig.mutex, &t);
    }
    int published = rig.buf.timestamp_ns != 0;
    pthread_mutex_unlock(&rig.mutex);
    return published ? 0 : -1;
}

/** Number of results the scanner has published into the rig's buffer. */
static size_t published_count(void)
{
    pthread_mutex_lock(&rig.mutex);
    size_t n = rig.buf.count;
    pthread_mutex_unlock(&rig.mutex);
    return n;
}

/** Stop the thread as main.c does (cancel, join), then shut the scanner down. */
static void rig_stop(void)
{
    if (rig.started) {
        pthread_cancel(rig.thread);
        pthread_join(rig.thread, NULL);
        rig.started = 0;
    }
    wifi_scanner_shutdown(rig.scanner);
    rig.scanner = NULL;
    pthread_cond_destroy(&rig.cond);
    pthread_mutex_destroy(&rig.mutex);
}

/** Reset the fake with a working SCAN and the basic fixture as results. */
static void script_ok(void)
{
    fake_wpa_reset();
    fake_wpa_set_reply("SCAN", 0, "OK\n");
    fake_wpa_set_reply_file("SCAN_RESULTS", "basic.txt");
}

static void test_cycle_publishes(void)
{
    script_ok();
    log_capture_begin();
    CHECK_EQ_INT(rig_start(1), 0);
    CHECK_EQ_INT(wait_publish(2000), 0);
    CHECK_EQ_INT(published_count(), 8);
    rig_stop();
    const char *log = log_capture_end();
    CHECK(log_contains(log, "Scan complete: 8 APs found"));
    CHECK_EQ_INT(fake_wpa_open_count(), 0);   /* shutdown closed both */
}

static void test_fail_busy_still_reads_results(void)
{
    script_ok();
    fake_wpa_set_reply("SCAN", 0, "FAIL-BUSY\n");
    log_capture_begin();
    CHECK_EQ_INT(rig_start(1), 0);
    CHECK_EQ_INT(wait_publish(2000), 0);
    CHECK_EQ_INT(published_count(), 8);
    rig_stop();
    (void)log_capture_end();
}

static void test_scan_failed_event(void)
{
    script_ok();
    fake_wpa_set_scan_event("<3>CTRL-EVENT-SCAN-FAILED ret=-16");
    log_capture_begin();
    CHECK_EQ_INT(rig_start(1), 0);
    CHECK_EQ_INT(wait_publish(1500), -1);
    rig_stop();
    const char *log = log_capture_end();
    CHECK(log_contains(log, "Scan failed at driver/firmware level"));
    CHECK_EQ_INT(fake_wpa_call_count("SCAN_RESULTS"), 0);
}

static void test_scan_event_timeout(void)
{
    script_ok();
    fake_wpa_set_scan_event(NULL);
    log_capture_begin();
    CHECK_EQ_INT(rig_start(1), 0);
    CHECK_EQ_INT(wait_publish(800), -1);
    rig_stop();
    const char *log = log_capture_end();
    CHECK(log_contains(log, "Timed out waiting for scan completion (300 ms)"));
}

static void test_request_timeout(void)
{
    script_ok();
    fake_wpa_set_reply("SCAN", -2, NULL);
    log_capture_begin();
    CHECK_EQ_INT(rig_start(1), 0);
    /* The first SCAN times out; the next slot (1000 ms later) tries again. */
    CHECK_EQ_INT(fake_wpa_wait_calls("SCAN", 2, 2500), 0);
    rig_stop();
    const char *log = log_capture_end();
    CHECK(log_contains(log, "wpa_ctrl_request('SCAN') timed out"));
    CHECK_EQ_INT(rig.buf.timestamp_ns, 0);
}

static void test_broken_monitor_reattaches(void)
{
    script_ok();
    log_capture_begin();
    fake_wpa_break_monitor();                 /* init never calls pending */
    CHECK_EQ_INT(rig_start(1), 0);
    CHECK_EQ_INT(wait_publish(2000), 0);
    CHECK_EQ_INT(published_count(), 8);
    CHECK_EQ_INT(fake_wpa_open_count(), 2);   /* old monitor closed, new one open */
    rig_stop();
    const char *log = log_capture_end();
    CHECK(log_contains(log, "Monitor connection lost, attempting re-attach"));
    CHECK(log_contains(log, "Monitor connection re-attached successfully"));
    CHECK_EQ_INT(fake_wpa_open_count(), 0);
}

static void test_truncation_warning(void)
{
    script_ok();
    fake_wpa_set_reply_file("SCAN_RESULTS", "truncated-4096.txt");
    log_capture_begin();
    CHECK_EQ_INT(rig_start(1), 0);
    CHECK_EQ_INT(wait_publish(2000), 0);
    CHECK(published_count() >= 73);
    rig_stop();
    const char *log = log_capture_end();
    CHECK(log_contains(log, "near wpa_supplicant's 4096-byte cap"));
}

static void test_init_fails_when_open_fails(void)
{
    scan_buffer_t buf;
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;

    fake_wpa_reset();
    log_capture_begin();
    fake_wpa_fail_open_at(1);                 /* control connection */
    CHECK(wifi_scanner_init(CTRL_PATH, 1000, &buf, &m, &c) == NULL);
    CHECK_EQ_INT(fake_wpa_open_count(), 0);

    fake_wpa_fail_open_at(2);                 /* monitor connection */
    CHECK(wifi_scanner_init(CTRL_PATH, 1000, &buf, &m, &c) == NULL);
    CHECK_EQ_INT(fake_wpa_open_count(), 0);   /* control connection closed */
    const char *log = log_capture_end();
    CHECK(log_contains(log, "failed for control connection"));
    CHECK(log_contains(log, "failed for monitor connection"));
}

static void test_get_country(void)
{
    char cc[3];

    script_ok();
    log_capture_begin();
    CHECK_EQ_INT(rig_start(0), 0);
    fake_wpa_set_reply("GET country", 0, "CA\n");
    CHECK_EQ_INT(wifi_scanner_get_country(rig.scanner, cc, sizeof(cc)), 0);
    CHECK_STR(cc, "CA");
    fake_wpa_set_reply("GET country", 0, "FAIL\n");
    CHECK_EQ_INT(wifi_scanner_get_country(rig.scanner, cc, sizeof(cc)), -1);
    rig_stop();
    (void)log_capture_end();
}

int main(void)
{
    RUN(test_cycle_publishes);
    RUN(test_fail_busy_still_reads_results);
    RUN(test_scan_failed_event);
    RUN(test_scan_event_timeout);
    RUN(test_request_timeout);
    RUN(test_broken_monitor_reattaches);
    RUN(test_truncation_warning);
    RUN(test_init_fails_when_open_fails);
    RUN(test_get_country);
    return test_finish();
}
