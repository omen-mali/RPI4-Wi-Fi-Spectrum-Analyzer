/**
 * @file test_scan_parse.c
 * @brief SCAN_RESULTS parsing and frequency mapping (scan_parse.c).
 */

#include "scan_parse.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** A reply buffer the size of the scanner's (WPA_REPLY_BUF_SIZE). */
typedef struct { char text[4096]; } reply_t;

/** Load tests/fixtures/scan/<name> into r, NUL-terminated. Aborts if missing. */
static void load(reply_t *r, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/scan/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open fixture %s\n", path);
        exit(2);
    }
    size_t n = fread(r->text, 1, sizeof(r->text) - 1, f);
    fclose(f);
    r->text[n] = '\0';
}

static scan_result_t results[MAX_SCAN_RESULTS];

static size_t parse_fixture(const char *name, size_t max)
{
    reply_t r;
    load(&r, name);
    memset(results, 0xA5, sizeof(results));   /* garbage, to catch unset fields */
    return scan_parse_results(r.text, results, max);
}

static void test_parse_basic(void)
{
    size_t n = parse_fixture("basic.txt", MAX_SCAN_RESULTS);
    CHECK_EQ_INT(n, 8);
    CHECK_STR(results[0].bssid, "02:00:00:00:00:01");
    CHECK_STR(results[0].ssid, "net-01");
    CHECK_EQ_INT(results[0].freq_mhz, 2412);
    CHECK_EQ_INT(results[0].channel, 1);
    CHECK_EQ_INT(results[0].band, BAND_2_4_GHZ);
    CHECK_EQ_INT(results[0].signal_dbm, -45);
    CHECK_STR(results[3].ssid, "");               /* hidden network */
    CHECK_EQ_INT(results[7].channel, 149);
    CHECK_EQ_INT(results[7].band, BAND_5_GHZ);
}

static void test_parse_empty(void)
{
    CHECK_EQ_INT(parse_fixture("empty.txt", MAX_SCAN_RESULTS), 0);
}

static void test_parse_malformed(void)
{
    size_t n = parse_fixture("malformed.txt", MAX_SCAN_RESULTS);
    CHECK_EQ_INT(n, 2);
    CHECK_STR(results[0].bssid, "02:00:00:00:01:05");
    CHECK_EQ_INT(results[0].signal_dbm, -61);
    CHECK_STR(results[1].bssid, "02:00:00:00:01:06");
    CHECK_EQ_INT(results[1].channel, 36);
}

static void test_parse_unknown_freq(void)
{
    size_t n = parse_fixture("unknown-freq.txt", MAX_SCAN_RESULTS);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_INT(results[0].channel, 14);
    CHECK_EQ_INT(results[0].band, BAND_2_4_GHZ);
    CHECK_EQ_INT(results[1].channel, 0);          /* 4940 */
    CHECK_EQ_INT(results[1].band, BAND_2_4_GHZ);  /* current rule: < 5000 */
    CHECK_EQ_INT(results[2].channel, 0);          /* 5955 */
    CHECK_EQ_INT(results[2].band, BAND_5_GHZ);
    CHECK_EQ_INT(scan_freq_to_channel(5825), 165);
    CHECK_EQ_INT(scan_freq_to_channel(2472), 13);
}

static int freq_in_tables(uint16_t f)
{
    return (f >= 2412 && f <= 2484) || (f >= 5180 && f <= 5825);
}

static void test_parse_truncated_4096(void)
{
    /* A reply cut at the server's 4096-byte buffer (4095 bytes + NUL) in
       the middle of a row. */
    reply_t r;
    load(&r, "truncated-4096.txt");
    CHECK_EQ_INT(strlen(r.text), 4095);

    size_t complete = 0;
    for (const char *p = r.text; (p = strchr(p, '\n')) != NULL; p++) {
        complete++;
    }
    complete -= 1;                                /* the header line */

    memset(results, 0xA5, sizeof(results));
    size_t n = scan_parse_results(r.text, results, MAX_SCAN_RESULTS);
    CHECK(n == complete || n == complete + 1);    /* partial row: skipped or whole-looking */
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ_INT(strnlen(results[i].bssid, sizeof(results[i].bssid)), 17);
        CHECK(freq_in_tables(results[i].freq_mhz));
        CHECK(memchr(results[i].ssid, '\0', sizeof(results[i].ssid)) != NULL);
    }
}

static void test_parse_escaped_and_long_ssid(void)
{
    /* wpa_supplicant escapes tabs and non-ASCII bytes in the SSID column
       as text, those must stay one field. */
    size_t n = parse_fixture("escaped.txt", MAX_SCAN_RESULTS);
    CHECK_EQ_INT(n, 4);
    CHECK_STR(results[0].ssid, "a\\tb");
    CHECK_STR(results[1].ssid, "\\xe2\\x9c\\x93");
    CHECK_EQ_INT(strlen(results[2].ssid), MAX_SSID_LEN - 1);
    CHECK_EQ_INT(results[2].ssid[0], 'L');
    CHECK_STR(results[3].bssid, "02:00:00:00:02:04");   /* next row still parses */
    CHECK_EQ_INT(results[3].freq_mhz, 5180);
}

static void test_parse_max_results(void)
{
    CHECK_EQ_INT(parse_fixture("basic.txt", 3), 3);
    CHECK_STR(results[2].bssid, "02:00:00:00:00:03");
}

int main(void)
{
    RUN(test_parse_basic);
    RUN(test_parse_empty);
    RUN(test_parse_malformed);
    RUN(test_parse_unknown_freq);
    RUN(test_parse_truncated_4096);
    RUN(test_parse_escaped_and_long_ssid);
    RUN(test_parse_max_results);
    return test_finish();
}
