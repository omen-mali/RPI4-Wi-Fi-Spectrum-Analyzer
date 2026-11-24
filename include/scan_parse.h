/**
 * @file scan_parse.h
 * @brief Parse wpa_supplicant SCAN_RESULTS replies; map frequency to channel.
 *
 * Pure text processing (it only logs warnings), used by wifi_scanner.c.
 */

#ifndef WIFI_SPECTRUM_SCAN_PARSE_H
#define WIFI_SPECTRUM_SCAN_PARSE_H

#include "config.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Parse a SCAN_RESULTS reply (header line + tab-separated rows).
 *
 * Tokenizes @p response in place (it is modified). Rows missing the bssid,
 * frequency or signal field, or with an unparseable frequency or signal,
 * are skipped. A missing SSID gives an empty string.
 *
 * @param response     Mutable, NUL-terminated SCAN_RESULTS text.
 * @param results      Output array.
 * @param max_results  Capacity of @p results.
 * @return Number of entries written.
 */
size_t scan_parse_results(char *response, scan_result_t *results,
                          size_t max_results);

/** @return The channel for a center frequency in MHz, or 0 if unknown. */
uint8_t scan_freq_to_channel(uint16_t freq_mhz);

/** @return BAND_5_GHZ for 5000 MHz and above, otherwise BAND_2_4_GHZ. */
wifi_band_t scan_freq_to_band(uint16_t freq_mhz);

#endif /* WIFI_SPECTRUM_SCAN_PARSE_H */
