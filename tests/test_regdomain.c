/**
 * @file test_regdomain.c
 * @brief Country lookup, code validation and channel tables (regdomain.c).
 */

#include "regdomain.h"
#include "test.h"

static void test_lookup_exact(void)
{
    int exact = -1;
    const regdomain_t *rd = regdomain_lookup("ca", &exact); /* case-insensitive */
    CHECK(rd != NULL);
    CHECK_EQ_INT(exact, 1);
    CHECK_STR(regdomain_code(rd), "CA");
}

static void test_lookup_unknown_falls_back_to_world(void)
{
    int exact = -1;
    const regdomain_t *rd = regdomain_lookup("ZZ", &exact);
    CHECK_STR(regdomain_code(rd), "00");
    CHECK_EQ_INT(exact, 0);
}

static void test_code_validation(void)
{
    CHECK(regdomain_code_is_valid("CA"));
    CHECK(regdomain_code_is_valid("00"));
    CHECK(!regdomain_code_is_valid("C"));
    CHECK(!regdomain_code_is_valid("CAN"));
    CHECK(!regdomain_code_is_valid("C1"));
    CHECK(!regdomain_code_is_valid(""));
}

static void test_ca_excludes_120_to_128(void)
{
    const regdomain_t *ca = regdomain_lookup("CA", NULL);
    CHECK(regdomain_channel_allowed(ca, BAND_5_GHZ, 116));
    CHECK(!regdomain_channel_allowed(ca, BAND_5_GHZ, 120));
    CHECK(!regdomain_channel_allowed(ca, BAND_5_GHZ, 128));
    CHECK(regdomain_channel_allowed(ca, BAND_5_GHZ, 132));
    CHECK(!regdomain_channel_allowed(ca, BAND_2_4_GHZ, 12));
}

static void test_us_allows_120(void)
{
    CHECK(regdomain_channel_allowed(regdomain_lookup("US", NULL), BAND_5_GHZ, 120));
}

int main(void)
{
    RUN(test_lookup_exact);
    RUN(test_lookup_unknown_falls_back_to_world);
    RUN(test_code_validation);
    RUN(test_ca_excludes_120_to_128);
    RUN(test_us_allows_120);
    return test_finish();
}
