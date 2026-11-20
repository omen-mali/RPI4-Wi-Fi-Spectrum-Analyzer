/**
 * @file regdomain.c
 * @brief Static regulatory-domain channel tables (see regdomain.h).
 *
 * Ranges are inclusive channel-number ranges for 20 MHz operation, derived
 * from wireless-regdb frequency rules:
 *
 *   CA (ISED RSS-247)            US (FCC Part 15)          00 (world)
 *   2402-2472  -> ch 1-11        2402-2472 -> ch 1-11      2402-2472 -> ch 1-11
 *   5150-5250  -> 36-48          5170-5250 -> 36-48        5170-5250 -> 36-48
 *   5250-5350  -> 52-64 (DFS)    5250-5330 -> 52-64 (DFS)  5250-5330 -> 52-64 (DFS)
 *   5470-5600  -> 100-116 (DFS)  5490-5730 -> 100-144(DFS) 5490-5730 -> 100-144 (DFS)
 *   5650-5730  -> 132-144 (DFS)  5735-5835 -> 149-165      5735-5835 -> 149-165
 *   5735-5835  -> 149-165
 *
 * Canada excludes 5600-5650 MHz (channels 120-128, weather radar).
 * The world domain allows 12-13 and 5 GHz only with passive scanning
 * (NO-IR). Here, it's treated as "never recommend 12-14".
 */

#include "regdomain.h"

#include <ctype.h>
#include <stddef.h>

typedef struct {
    wifi_band_t band;
    uint8_t     first;   /**< First permitted channel (inclusive) */
    uint8_t     last;    /**< Last permitted channel (inclusive) */
} chan_range_t;

struct regdomain {
    char                code[3];
    const chan_range_t *ranges;
    size_t              num_ranges;
};

static const chan_range_t ranges_world[] = {
    { BAND_2_4_GHZ,   1,  11 },
    { BAND_5_GHZ,    36,  64 },
    { BAND_5_GHZ,   100, 144 },
    { BAND_5_GHZ,   149, 165 },
};

static const chan_range_t ranges_ca[] = {
    { BAND_2_4_GHZ,   1,  11 },
    { BAND_5_GHZ,    36,  64 },
    { BAND_5_GHZ,   100, 116 },
    { BAND_5_GHZ,   132, 144 },
    { BAND_5_GHZ,   149, 165 },
};

static const chan_range_t ranges_us[] = {
    { BAND_2_4_GHZ,   1,  11 },
    { BAND_5_GHZ,    36,  64 },
    { BAND_5_GHZ,   100, 144 },
    { BAND_5_GHZ,   149, 165 },
};

#define RD(cc, tbl) { cc, tbl, sizeof(tbl) / sizeof(tbl[0]) }

static const regdomain_t regdomains[] = {
    RD("00", ranges_world),   /* must stay first: fallback */
    RD("CA", ranges_ca),
    RD("US", ranges_us),
};

#define NUM_REGDOMAINS (sizeof(regdomains) / sizeof(regdomains[0]))

int regdomain_code_is_valid(const char *country)
{
    if (!country || country[0] == '\0' || country[1] == '\0' ||
        country[2] != '\0') {
        return 0;
    }
    if (country[0] == '0' && country[1] == '0') {
        return 1;
    }
    return isalpha((unsigned char)country[0]) &&
           isalpha((unsigned char)country[1]);
}

const regdomain_t *regdomain_lookup(const char *country, int *exact)
{
    if (exact) {
        *exact = 0;
    }
    if (!regdomain_code_is_valid(country)) {
        return &regdomains[0];
    }

    for (size_t i = 0; i < NUM_REGDOMAINS; i++) {
        if (toupper((unsigned char)country[0]) == regdomains[i].code[0] &&
            toupper((unsigned char)country[1]) == regdomains[i].code[1]) {
            if (exact) {
                *exact = 1;
            }
            return &regdomains[i];
        }
    }
    return &regdomains[0];
}

const char *regdomain_code(const regdomain_t *rd)
{
    return rd ? rd->code : regdomains[0].code;
}

int regdomain_channel_allowed(const regdomain_t *rd, wifi_band_t band,
                              uint8_t channel)
{
    if (!rd) {
        rd = &regdomains[0];
    }
    for (size_t i = 0; i < rd->num_ranges; i++) {
        const chan_range_t *r = &rd->ranges[i];
        if (r->band == band && channel >= r->first && channel <= r->last) {
            return 1;
        }
    }
    return 0;
}
