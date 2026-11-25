/**
 * @file scan_parse.c
 * @brief SCAN_RESULTS parsing and frequency-to-channel mapping.
 *
 * wpa_supplicant's SCAN_RESULTS reply is a header line followed by one
 * tab-separated row per AP:
 *
 *     bssid / frequency / signal level / flags / ssid
 *     00:11:22:33:44:55\t2412\t-45\t[WPA2-PSK-CCMP][ESS]\tMyNetwork
 *     AA:BB:CC:DD:EE:FF\t5180\t-67\t[WPA2-PSK-CCMP][ESS]\tOtherNet
 *
 *   - bssid:        MAC address of the AP
 *   - frequency:    center frequency in MHz
 *   - signal level: RSSI in dBm (negative)
 *   - flags:        security and capability flags (not used here)
 *   - ssid:         network name, empty for a hidden network
 */

#include "scan_parse.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Internal Helper: Frequency-to-Channel Conversion
 * ========================================================================= */

/**
 * @brief Convert a Wi-Fi center frequency in MHz to a channel number.
 *
 *   2.4 GHz band:
 *     Channel 1-13:  freq = 2407 + (channel * 5)  =>  channel = (freq - 2407) / 5
 *     Channel 14:    freq = 2484  (special case)
 *
 *   5 GHz band:
 *     Channel = (freq - 5000) / 5
 *     Example: 5180 MHz => channel 36,  5745 MHz => channel 149
 *
 * @param freq_mhz  Center frequency in MHz.
 * @return Channel number, or 0 if the frequency is unrecognized.
 */
uint8_t scan_freq_to_channel(uint16_t freq_mhz)
{
    if (freq_mhz == 2484) {
        return 14;
    } else if (freq_mhz >= 2412 && freq_mhz <= 2472) {
        return (uint8_t)((freq_mhz - 2407) / 5);
    } else if (freq_mhz >= 5180 && freq_mhz <= 5825) {
        return (uint8_t)((freq_mhz - 5000) / 5);
    }

    LOG_WRN("Unknown frequency: %u MHz", freq_mhz);
    return 0;
}

/**
 * @brief Determine which band a frequency belongs to.
 *
 * @param freq_mhz  Center frequency in MHz.
 * @return BAND_2_4_GHZ, BAND_5_GHZ, or BAND_2_4_GHZ as a fallback.
 */
wifi_band_t scan_freq_to_band(uint16_t freq_mhz)
{
    if (freq_mhz >= 5000) {
        return BAND_5_GHZ;
    }
    return BAND_2_4_GHZ;
}

/* =========================================================================
 * Internal Helper: Parse SCAN_RESULTS Response
 * ========================================================================= */

/**
 * @brief Split off the next delimiter-separated field, in place.
 *
 * Unlike strtok_r(), this keeps empty fields: "a\t\tb" yields "a", "", "b".
 * That matters for SCAN_RESULTS, where hidden networks leave the SSID column
 * empty (the line ends in "\t"). strtok_r() would collapse that and return
 * NULL for the SSID.
 *
 * @param cursor  In/out: start of the remaining text; advanced past the
 *                delimiter, or set to NULL once the last field is returned.
 * @param delim   Field delimiter character.
 * @return The field (NUL-terminated in place), or NULL if no field remains.
 */
static char *next_field(char **cursor, char delim)
{
    char *start = *cursor;
    char *end;

    if (start == NULL) {
        return NULL;
    }

    end = strchr(start, delim);
    if (end) {
        *end = '\0';
        *cursor = end + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

/**
 * @brief Parse the text response from a SCAN_RESULTS command into
 *        structured scan_result_t entries.
 *
 * The response is tokenized IN PLACE (it is modified), so no allocation
 * happens on this real-time path. Lines that are missing the bssid,
 * frequency or signal field, or have an unparseable frequency, are skipped.
 * An empty or missing SSID (hidden network) gives an empty string.
 *
 * @param response     Mutable SCAN_RESULTS text (header line + rows).
 * @param results      Output array of scan_result_t entries.
 * @param max_results  Maximum number of entries to write.
 * @return Number of entries successfully parsed.
 */
size_t scan_parse_results(char *response,
                          scan_result_t *results,
                          size_t max_results)
{
    char  *cursor = response;
    char  *line;
    size_t count = 0;

    /* Skip the header line ("bssid / frequency / signal level / ..."). */
    (void)next_field(&cursor, '\n');

    while (count < max_results && (line = next_field(&cursor, '\n')) != NULL) {
        char *field_cursor = line;
        char *bssid = next_field(&field_cursor, '\t');
        char *freq  = next_field(&field_cursor, '\t');
        char *level = next_field(&field_cursor, '\t');
        char *flags = next_field(&field_cursor, '\t');
        char *ssid  = next_field(&field_cursor, '\t');
        char *endp;
        unsigned long freq_val;
        long          level_val;
        scan_result_t *r;

        (void)flags;

        if (!bssid || !freq || !level || bssid[0] == '\0') {
            continue;   /* blank or malformed line */
        }

        freq_val = strtoul(freq, &endp, 10);
        if (endp == freq || freq_val == 0 || freq_val > UINT16_MAX) {
            LOG_WRN("Skipping scan row with bad frequency '%s'", freq);
            continue;
        }

        level_val = strtol(level, &endp, 10);
        if (endp == level) {
            LOG_WRN("Skipping scan row with bad signal level '%s'", level);
            continue;
        }
        if (level_val < INT8_MIN) level_val = INT8_MIN;
        if (level_val > INT8_MAX) level_val = INT8_MAX;

        r = &results[count];
        snprintf(r->bssid, sizeof(r->bssid), "%s", bssid);
        snprintf(r->ssid,  sizeof(r->ssid),  "%s", ssid ? ssid : "");
        r->freq_mhz   = (uint16_t)freq_val;
        r->signal_dbm = (int8_t)level_val;
        r->channel    = scan_freq_to_channel(r->freq_mhz);
        r->band       = scan_freq_to_band(r->freq_mhz);

        count++;
    }

    if (count == max_results && cursor != NULL && *cursor != '\0') {
        LOG_WRN("More than %zu scan results; extra entries dropped", max_results);
    }

    return count;
}
