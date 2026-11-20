/**
 * @file regdomain.h
 * @brief Minimal regulatory-domain (country) channel tables.
 *
 * The analyzer must only recommend channels an AP may legally use in the
 * configured country. This module answers "is 20 MHz channel N in band B
 * permitted in country CC?" from small static tables derived from the
 * Linux wireless-regdb (center frequency +/- 10 MHz must fit inside a
 * permitted frequency range).
 *
 * Only a few domains are built in, so unknown country codes fall back to the
 * conservative world domain "00" (with a warning). Add a country by adding
 * a range table and an entry in regdomains[] in regdomain.c.
 *
 * Note: this only filters the analyzer's recommendations. What the radio
 * actually scans and transmits on is set by wpa_supplicant's "country="
 * setting (/boot/wpa_supplicant.conf on the QNX Quick Start image).
 */

#ifndef WIFI_SPECTRUM_REGDOMAIN_H
#define WIFI_SPECTRUM_REGDOMAIN_H

#include "config.h"

/** Opaque handle to a regulatory domain table. */
typedef struct regdomain regdomain_t;

/**
 * @brief Look up a regulatory domain by ISO 3166-1 alpha-2 code.
 *
 * Case-insensitive. Unknown codes return the world domain "00".
 *
 * @param country     Two-letter code (e.g. "CA") or "00".
 * @param[out] exact  Optional: set to 1 if the code matched a built-in
 *                    table, 0 if the world fallback was returned.
 * @return Never NULL.
 */
const regdomain_t *regdomain_lookup(const char *country, int *exact);

/** @return The domain's code, e.g. "CA" or "00". */
const char *regdomain_code(const regdomain_t *rd);

/**
 * @brief Check whether a 20 MHz channel is permitted in the domain.
 *
 * @return 1 if permitted, 0 otherwise.
 */
int regdomain_channel_allowed(const regdomain_t *rd, wifi_band_t band,
                              uint8_t channel);

/**
 * @brief Validate a user-supplied country code: two ASCII letters or "00".
 *
 * @return 1 if well-formed, 0 otherwise.
 */
int regdomain_code_is_valid(const char *country);

#endif /* WIFI_SPECTRUM_REGDOMAIN_H */
