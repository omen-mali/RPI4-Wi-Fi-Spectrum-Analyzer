/**
 * @file test_channel_analyzer.c
 * @brief Scoring and recommendation (channel_analyzer_analyze()).
 */

#include "channel_analyzer.h"
#include "regdomain.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

/** Append one AP to a scan buffer. */
static void add_ap(scan_buffer_t *s, uint8_t ch, uint16_t freq, int8_t dbm,
                   wifi_band_t band)
{
    scan_result_t *e = &s->entries[s->count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->bssid, sizeof(e->bssid), "02:00:00:00:00:%02x",
             (unsigned)s->count);
    snprintf(e->ssid, sizeof(e->ssid), "net-%02u", (unsigned)s->count);
    e->channel    = ch;
    e->freq_mhz   = freq;
    e->signal_dbm = dbm;
    e->band       = band;
}

/** Index of a channel in r->channels, or -1. */
static int find_channel(const analysis_result_t *r, uint8_t ch)
{
    for (size_t i = 0; i < r->channel_count; i++) {
        if (r->channels[i].channel_number == ch) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * The same 8 APs as tests/fixtures/scan/basic.txt, in the same order.
 * Keep the two in step.
 */
static void build_basic_scan(scan_buffer_t *s)
{
    memset(s, 0, sizeof(*s));
    add_ap(s, 1,   2412, -45, BAND_2_4_GHZ);
    add_ap(s, 1,   2412, -70, BAND_2_4_GHZ);
    add_ap(s, 6,   2437, -50, BAND_2_4_GHZ);
    add_ap(s, 6,   2437, -60, BAND_2_4_GHZ);
    add_ap(s, 6,   2437, -80, BAND_2_4_GHZ);
    add_ap(s, 11,  2462, -65, BAND_2_4_GHZ);
    add_ap(s, 36,  5180, -55, BAND_5_GHZ);
    add_ap(s, 149, 5745, -72, BAND_5_GHZ);
}

static void test_empty_scan_picks_first_bonus_channels(void)
{
    /* No APs: 1/6/11 get the non-overlap bonus and the earliest wins ties
       (strict >); 36 is the first non-DFS 5 GHz channel. */
    scan_buffer_t s;
    analysis_result_t r;
    memset(&s, 0, sizeof(s));
    channel_analyzer_analyze(regdomain_lookup("CA", NULL), &s, &r);
    CHECK_EQ_INT(r.best_channel_2_4ghz, 1);
    CHECK_EQ_INT(r.best_channel_5ghz, 36);
}

static void test_busy_1_and_6_recommends_11(void)
{
    scan_buffer_t s;
    analysis_result_t r;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < 3; i++) {
        add_ap(&s, 1, 2412, -40, BAND_2_4_GHZ);
        add_ap(&s, 6, 2437, -40, BAND_2_4_GHZ);
    }
    channel_analyzer_analyze(regdomain_lookup("CA", NULL), &s, &r);
    CHECK_EQ_INT(r.best_channel_2_4ghz, 11);
}

static void test_regdomain_filters_channels(void)
{
    scan_buffer_t s;
    analysis_result_t ca, us;
    memset(&s, 0, sizeof(s));
    channel_analyzer_analyze(regdomain_lookup("CA", NULL), &s, &ca);
    channel_analyzer_analyze(regdomain_lookup("US", NULL), &s, &us);
    for (uint8_t ch = 120; ch <= 128; ch += 4) {
        CHECK_EQ_INT(find_channel(&ca, ch), -1);
    }
    CHECK(find_channel(&us, 120) >= 0);
    CHECK(us.channel_count > ca.channel_count);
}

static void test_analyze_ignores_unknown_channels(void)
{
    /* Review Focus 3. The parser gives channel 0 for frequencies outside its
       tables; 4940 (4.9 GHz) gets BAND_2_4_GHZ (freq < 5000) and 5955
       (6 GHz) gets BAND_5_GHZ. Neither may count as an AP on a channel, and
       neither may change any channel's score. */
    scan_buffer_t base, s;
    analysis_result_t want, got;
    memset(&base, 0, sizeof(base));
    add_ap(&base, 6, 2437, -50, BAND_2_4_GHZ);
    s = base;
    add_ap(&s, 0, 4940, -40, BAND_2_4_GHZ);
    add_ap(&s, 0, 5955, -40, BAND_5_GHZ);

    channel_analyzer_analyze(regdomain_lookup("CA", NULL), &base, &want);
    channel_analyzer_analyze(regdomain_lookup("CA", NULL), &s, &got);

    long aps = 0;
    for (size_t i = 0; i < got.channel_count; i++) {
        aps += got.channels[i].ap_count;
        CHECK(got.channels[i].channel_number != 0);
    }
    CHECK_EQ_INT(aps, 1);
    CHECK_EQ_INT(got.channel_count, want.channel_count);
    for (size_t i = 0; i < got.channel_count && i < want.channel_count; i++) {
        CHECK_NEAR(got.channels[i].score, want.channels[i].score, 1e-6);
    }
    CHECK_EQ_INT(got.best_channel_2_4ghz, want.best_channel_2_4ghz);
}

static void test_golden_basic_scan(void)
{
    /* Characterization: pinned 2026-11-23 from the current code; change only
       with an intended scoring change. */
    static const struct { uint8_t ch; double score; } want[] = {
        { 1, 32.183907 }, { 3, 56.0 }, { 6, 27.968750 },
        { 11, 40.454544 }, { 36, 30.535715 }, { 149, 30.410959 },
    };
    scan_buffer_t s;
    analysis_result_t r;
    build_basic_scan(&s);
    channel_analyzer_analyze(regdomain_lookup("CA", NULL), &s, &r);

    CHECK_EQ_INT(r.best_channel_2_4ghz, 10);
    CHECK_EQ_INT(r.best_channel_5ghz, 40);
    CHECK_NEAR(r.best_score_2_4ghz, 66.0, 1e-4);
    CHECK_NEAR(r.best_score_5ghz, 85.0, 1e-4);
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int idx = find_channel(&r, want[i].ch);
        CHECK(idx >= 0);
        if (idx >= 0) {
            CHECK_NEAR(r.channels[idx].score, want[i].score, 1e-4);
        }
    }
    int aps = 0;
    for (size_t i = 0; i < r.channel_count; i++) {
        aps += r.channels[i].ap_count;
    }
    CHECK_EQ_INT(aps, 8);
}

int main(void)
{
    RUN(test_empty_scan_picks_first_bonus_channels);
    RUN(test_busy_1_and_6_recommends_11);
    RUN(test_regdomain_filters_channels);
    RUN(test_analyze_ignores_unknown_channels);
    RUN(test_golden_basic_scan);
    return test_finish();
}
