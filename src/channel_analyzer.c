/**
 * @file channel_analyzer.c
 * @brief Channel analysis implementation -- determines optimal Wi-Fi channels.
 *
 * =========================================================================
 * IMPLEMENTATION LAYOUT
 * =========================================================================
 *
 *   1. channel_analyzer_init()        -- Allocate context, set up channel table
 *   2. build_channel_table()          -- Populate known channels for both bands
 *   3. compute_channel_metrics()      -- Aggregate scan data per channel
 *   4. score_channels()               -- Apply scoring algorithm
 *   5. find_best_channels()           -- Pick the winner per band
 *   6. channel_analyzer_run()         -- Consumer thread loop
 *   7. channel_analyzer_get_results() -- Thread-safe result retrieval
 *   8. channel_analyzer_shutdown()    -- Cleanup
 *
 * =========================================================================
 * SCORING ALGORITHM DESIGN NOTES
 * =========================================================================
 *
 * The goal is to identify the channel with the least interference and most
 * available bandwidth. Factors considered:
 *
 * 1. AP Count:
 *    Fewer APs on a channel means less contention. Weigh heavily.
 *
 * 2. Signal Strength of Neighbors:
 *    Strong neighboring APs cause more interference than weak, distant ones.
 *    The signal strength of the neighbors is used as a penalty to the score.
 *
 * 3. Overlapping Channel Interference (2.4 GHz only):
 *    In the 2.4 GHz band, each 20 MHz channel occupies ~22 MHz, so
 *    adjacent channels overlap. For example, an AP on channel 3 interferes
 *    with channels 1-5. Only channels 1, 6, and 11 are non-overlapping.
 *
 * 4. DFS Channels (5 GHz):
 *    Channels 52-144 are DFS (Dynamic Frequency Selection) channels that
 *    may be unavailable if radar is detected. A slight bonus is given to
 *    channels outside of the DFS range.
 *
 * 5. Channel Width:
 *    This starter focuses on 20 MHz channels. For 40/80/160 MHz analysis,
 *    you would need to check bonded channel pairs and their combined
 *    interference -- this is a natural extension, but not yet implemented.
 *
 * =========================================================================
 */

#include "channel_analyzer.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

/* =========================================================================
 * Scoring Weights (tune these based on empirical testing)
 * ========================================================================= */

/** Weight for AP count factor (fewer APs = higher score). */
#define W_AP_COUNT         50.0f

/** Weight for signal strength factor (weaker neighbors = higher score). */
#define W_SIGNAL_STRENGTH  30.0f

/**
 * Weight for overlapping channel interference penalty (2.4 GHz only).
 * Applied per overlapping AP, scaled by proximity in channel space.
 */
#define W_OVERLAP_PENALTY  10.0f

/** Bonus for being a non-overlapping 2.4 GHz channel (1, 6, 11). */
#define W_NON_OVERLAP_BONUS 15.0f

/** Bonus for being a non-DFS 5 GHz channel (outside of 52-144). */
#define W_NON_DFS_BONUS 5.0f

/* =========================================================================
 * Known Channel Tables
 * ========================================================================= */

/**
 * 2.4 GHz channels (worldwide common set, channels 1-13).
 * Channel 14 is Japan-only; include it but it may be filtered by regulation.
 */
static const channel_info_t channels_2_4ghz[] = {
    {  1, 2412, BAND_2_4_GHZ },
    {  2, 2417, BAND_2_4_GHZ },
    {  3, 2422, BAND_2_4_GHZ },
    {  4, 2427, BAND_2_4_GHZ },
    {  5, 2432, BAND_2_4_GHZ },
    {  6, 2437, BAND_2_4_GHZ },
    {  7, 2442, BAND_2_4_GHZ },
    {  8, 2447, BAND_2_4_GHZ },
    {  9, 2452, BAND_2_4_GHZ },
    { 10, 2457, BAND_2_4_GHZ },
    { 11, 2462, BAND_2_4_GHZ },
    { 12, 2467, BAND_2_4_GHZ },
    { 13, 2472, BAND_2_4_GHZ },
    { 14, 2484, BAND_2_4_GHZ },
};

/**
 * 5 GHz channels (common UNII-1, UNII-2, UNII-2e, UNII-3).
 *
 * Note: Channels 52-144 are DFS channels. Whether they are usable depends
 * on the regulatory domain and whether radar has been detected.
 */
static const channel_info_t channels_5ghz[] = {
    {  36, 5180, BAND_5_GHZ },  /* UNII-1 */
    {  40, 5200, BAND_5_GHZ },
    {  44, 5220, BAND_5_GHZ },
    {  48, 5240, BAND_5_GHZ },
    {  52, 5260, BAND_5_GHZ },  /* UNII-2 (DFS) */
    {  56, 5280, BAND_5_GHZ },
    {  60, 5300, BAND_5_GHZ },
    {  64, 5320, BAND_5_GHZ },
    { 100, 5500, BAND_5_GHZ },  /* UNII-2e (DFS) */
    { 104, 5520, BAND_5_GHZ },
    { 108, 5540, BAND_5_GHZ },
    { 112, 5560, BAND_5_GHZ },
    { 116, 5580, BAND_5_GHZ },
    { 120, 5600, BAND_5_GHZ },
    { 124, 5620, BAND_5_GHZ },
    { 128, 5640, BAND_5_GHZ },
    { 132, 5660, BAND_5_GHZ },
    { 136, 5680, BAND_5_GHZ },
    { 140, 5700, BAND_5_GHZ },
    { 144, 5720, BAND_5_GHZ },
    { 149, 5745, BAND_5_GHZ },  /* UNII-3 */
    { 153, 5765, BAND_5_GHZ },
    { 157, 5785, BAND_5_GHZ },
    { 161, 5805, BAND_5_GHZ },
    { 165, 5825, BAND_5_GHZ },
};

static const channel_info_t dfs_channels_5ghz[] = {
    {  52, 5260, BAND_5_GHZ },  /* UNII-2 (DFS) */
    {  56, 5280, BAND_5_GHZ },
    {  60, 5300, BAND_5_GHZ },
    {  64, 5320, BAND_5_GHZ },
    { 100, 5500, BAND_5_GHZ },  /* UNII-2e (DFS) */
    { 104, 5520, BAND_5_GHZ },
    { 108, 5540, BAND_5_GHZ },
    { 112, 5560, BAND_5_GHZ },
    { 116, 5580, BAND_5_GHZ },
    { 120, 5600, BAND_5_GHZ },
    { 124, 5620, BAND_5_GHZ },
    { 128, 5640, BAND_5_GHZ },
    { 132, 5660, BAND_5_GHZ },
    { 136, 5680, BAND_5_GHZ },
    { 140, 5700, BAND_5_GHZ },
    { 144, 5720, BAND_5_GHZ },
};

#define NUM_CHANNELS_2_4GHZ  (sizeof(channels_2_4ghz) / sizeof(channels_2_4ghz[0]))
#define NUM_CHANNELS_5GHZ    (sizeof(channels_5ghz)   / sizeof(channels_5ghz[0]))
#define NUM_DFS_CHANNELS_5GHZ    (sizeof(dfs_channels_5ghz) / sizeof(dfs_channels_5ghz[0]))

/* =========================================================================
 * Analyzer Context
 * ========================================================================= */

struct channel_analyzer {
    scan_buffer_t   *shared_buffer;   /**< Pointer to shared scan data */
    pthread_mutex_t *buffer_mutex;    /**< Mutex for shared_buffer */
    pthread_cond_t  *buffer_cond;     /**< Condition var for new-data signal */

    analysis_result_t latest_result;  /**< Most recent analysis output */
    pthread_mutex_t   result_mutex;   /**< Protects latest_result */
    int               has_result;     /**< True once first analysis completes */

    volatile int      running;        /**< Loop control flag */
};

/* =========================================================================
 * Internal: Compute Per-Channel Metrics
 * ========================================================================= */

/**
 * @brief Aggregate scan results into per-channel metrics.
 *
 * For each known channel (from the channel tables above):
 *   1. Initialize ap_count = 0, signal sums to 0, max_signal to -128.
 *   2. Iterate over scan results.
 *   3. For each AP whose frequency matches this channel's frequency,
 *      increment ap_count, accumulate signal for averaging, track max.
 *   4. Compute avg_signal_dbm = total_signal / ap_count (if ap_count > 0).
 *
 * @param scan_data    The scan results to analyze.
 * @param metrics      Output array of channel_metric_t (pre-allocated).
 * @param channel_count Output: number of channels populated.
 */
static void compute_channel_metrics(const scan_buffer_t *scan_data,
                                    channel_metric_t *metrics,
                                    size_t *channel_count)
{
    (void)scan_data;
    (void)metrics;
    *channel_count = 0;

    size_t idx = 0;

    for (size_t ch = 0; ch < NUM_CHANNELS_2_4GHZ; ch++) {
        channel_metric_t *metric = &metrics[idx++];

        metric->channel_number = channels_2_4ghz[ch].channel_number;
        metric->freq_mhz       = channels_2_4ghz[ch].center_freq_mhz;
        metric->band            = BAND_2_4_GHZ;
        metric->ap_count        = 0;
        metric->avg_signal_dbm  = 0;
        metric->max_signal_dbm  = -128;
        metric->score           = 0.0f;

        int32_t signal_sum = 0;

        for (size_t ap = 0; ap < scan_data->count; ap++) {
            if (scan_data->entries[ap].freq_mhz == channels_2_4ghz[ch].center_freq_mhz) {
                metric->ap_count++;
                signal_sum += scan_data->entries[ap].signal_dbm;

                if (scan_data->entries[ap].signal_dbm > metric->max_signal_dbm) {
                    metric->max_signal_dbm = scan_data->entries[ap].signal_dbm;
                }
            }
        }

        if (metric->ap_count > 0) {
            metric->avg_signal_dbm = (int8_t)(signal_sum / (int32_t)metric->ap_count);
        }
    }

    for (size_t ch = 0; ch < NUM_CHANNELS_5GHZ; ch++) {
        channel_metric_t *metric = &metrics[idx++];

        metric->channel_number = channels_5ghz[ch].channel_number;
        metric->freq_mhz       = channels_5ghz[ch].center_freq_mhz;
        metric->band            = BAND_5_GHZ;
        metric->ap_count        = 0;
        metric->avg_signal_dbm  = 0;
        metric->max_signal_dbm  = -128;
        metric->score           = 0.0f;

        int32_t signal_sum = 0;

        for (size_t ap = 0; ap < scan_data->count; ap++) {
            if (scan_data->entries[ap].freq_mhz == channels_5ghz[ch].center_freq_mhz) {
                metric->ap_count++;
                signal_sum += scan_data->entries[ap].signal_dbm;

                if (scan_data->entries[ap].signal_dbm > metric->max_signal_dbm) {
                    metric->max_signal_dbm = scan_data->entries[ap].signal_dbm;
                }
            }
        }

        if (metric->ap_count > 0) {
            metric->avg_signal_dbm = (int8_t)(signal_sum / (int32_t)metric->ap_count);
        }
    }

    *channel_count = idx;
}

/* =========================================================================
 * Internal: Score Channels
 * ========================================================================= */

/**
 * @brief Check if a 2.4 GHz channel is in the non-overlapping set (1, 6, 11).
 */
static int is_non_overlapping_24ghz(uint8_t channel)
{
    return (channel == 1 || channel == 6 || channel == 11);
}

/**
 * @brief Check if a 5 GHz channel is in the non-DFS set (outside of 52-144).
 */
static int is_non_dfs_5ghz(uint8_t channel)
{
    return (channel < 52 || channel > 144);
}

/**
 * @brief Compute the overlap penalty for a 2.4 GHz channel.
 *
 * In the 2.4 GHz band, each channel occupies ~22 MHz and channels are
 * spaced 5 MHz apart. So channel N overlaps with channels N-4 to N+4.
 *
 * For each AP detected on an overlapping (but different) channel:
 *   penalty += weight * (5 - abs(our_channel - ap_channel)) / 5.0
 *
 * This gives maximum penalty for the same channel (handled by ap_count)
 * and decreasing penalty for channels further away.
 *
 * @param target_channel  The channel being scored.
 * @param scan_data       All scan results.
 * @return A non-negative overlap penalty value.
 */
static float compute_overlap_penalty(uint8_t target_channel,
                                     const scan_buffer_t *scan_data)
{
    (void)target_channel;
    (void)scan_data;

    float penalty = 0.0f;

    for (size_t i = 0; i < scan_data->count; i++) {
        if (scan_data->entries[i].band != BAND_2_4_GHZ) continue;

        int ch_diff = abs((int)target_channel - (int)scan_data->entries[i].channel);

        if (ch_diff > 0 && ch_diff <= 4) {
            float proximity = (5.0f - (float)ch_diff) / 5.0f;
            penalty += proximity;
        }
    }

    return penalty;
}

/**
 * @brief Apply the scoring algorithm to all channel metrics.
 *
 * For each channel, compute:
 *   score = W_AP_COUNT * (1.0 / (1.0 + ap_count))
 *         + W_SIGNAL_STRENGTH * (1.0 / (1.0 + abs(avg_signal_dbm)))
 *         - overlap_penalty         (2.4 GHz only)
 *         + non_overlap_bonus       (2.4 GHz channels 1, 6, 11 only)
 *
 * Higher score = better channel.
 *
 * For 5 GHz channels:
 *   score = W_AP_COUNT * (1.0 / (1.0 + ap_count))
 *         + W_SIGNAL_STRENGTH * (1.0 / (1.0 + abs(avg_signal_dbm)))
           + non_dfs_bonus          (5 GHz channels outside of 52-144 only)
 *
 * (No overlap penalty because 5 GHz 20 MHz channels don't overlap.)
 *
 * @param metrics        Array of channel metrics (modified in place).
 * @param channel_count  Number of channels in the array.
 * @param scan_data      Raw scan data (needed for overlap calculation).
 */
static void score_channels(channel_metric_t *metrics, size_t channel_count,
                           const scan_buffer_t *scan_data)
{
    for (size_t i = 0; i < channel_count; i++) {
        float score = 0.0f;

        score += W_AP_COUNT * (1.0f / (1.0f + (float)metrics[i].ap_count));
        score += W_SIGNAL_STRENGTH * (1.0f / (1.0f + (float)abs(metrics[i].avg_signal_dbm)));

        if (metrics[i].band == BAND_2_4_GHZ) {
            float penalty = compute_overlap_penalty(metrics[i].channel_number, scan_data);
            score -= W_OVERLAP_PENALTY * penalty;

            if (is_non_overlapping_24ghz(metrics[i].channel_number)) {
                score += W_NON_OVERLAP_BONUS;
            }
        } else if (metrics[i].band == BAND_5_GHZ) {
            if (is_non_dfs_5ghz(metrics[i].channel_number)) {
                score += W_NON_DFS_BONUS;
            }
        }

        metrics[i].score = score;
    }
}

/* =========================================================================
 * Internal: Find Best Channels
 * ========================================================================= */

/**
 * @brief Find the highest-scoring channel for each band.
 *
 * @param result  The analysis_result_t to populate with best-channel info.
 */
static void find_best_channels(analysis_result_t *result)
{
    result->best_score_2_4ghz = -1e9f;
    result->best_score_5ghz   = -1e9f;
    result->best_channel_2_4ghz = 0;
    result->best_channel_5ghz   = 0;

    for (size_t i = 0; i < result->channel_count; i++) {
        channel_metric_t *m = &result->channels[i];

        if (m->band == BAND_2_4_GHZ && m->score > result->best_score_2_4ghz) {
            result->best_score_2_4ghz   = m->score;
            result->best_channel_2_4ghz = m->channel_number;
        }

        if (m->band == BAND_5_GHZ && m->score > result->best_score_5ghz) {
            result->best_score_5ghz   = m->score;
            result->best_channel_5ghz = m->channel_number;
        }
    }
}

/* =========================================================================
 * Public API Implementation
 * ========================================================================= */

channel_analyzer_t *channel_analyzer_init(scan_buffer_t *shared_buffer,
                                          pthread_mutex_t *buffer_mutex,
                                          pthread_cond_t *buffer_cond)
{
    channel_analyzer_t *analyzer;
    pthread_mutexattr_t attr;
    int rc;

    LOG_INF("Initializing channel analyzer");

    analyzer = (channel_analyzer_t *)calloc(1, sizeof(channel_analyzer_t));
    if (!analyzer) {
        LOG_ERR("Failed to allocate analyzer context");
        return NULL;
    }

    analyzer->shared_buffer = shared_buffer;
    analyzer->buffer_mutex  = buffer_mutex;
    analyzer->buffer_cond   = buffer_cond;
    analyzer->running       = 1;
    analyzer->has_result    = 0;

    /* Initialize the result mutex with priority inheritance. */
    rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        LOG_ERR("mutexattr_init failed for result_mutex");
        free(analyzer);
        return NULL;
    }
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    rc = pthread_mutex_init(&analyzer->result_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if (rc != 0) {
        LOG_ERR("mutex_init failed for result_mutex");
        free(analyzer);
        return NULL;
    }

    LOG_INF("Channel analyzer initialized");
    return analyzer;
}

void *channel_analyzer_run(void *arg)
{
    channel_analyzer_t *analyzer = (channel_analyzer_t *)arg;

    LOG_INF("Analyzer thread started");
    
    while (analyzer->running) {

        // Wait for new scan data
        pthread_mutex_lock(analyzer->buffer_mutex);

        while (analyzer->shared_buffer->count == 0 && analyzer->running) {
            pthread_cond_wait(analyzer->buffer_cond, analyzer->buffer_mutex);
        }

        // Copy scan data locally (minimize lock hold time)
        scan_buffer_t local_copy;
        memcpy(&local_copy, analyzer->shared_buffer, sizeof(scan_buffer_t));
        pthread_mutex_unlock(analyzer->buffer_mutex);
        
        // Compute metrics
        analysis_result_t result;
        memset(&result, 0, sizeof(result));
        compute_channel_metrics(&local_copy, result.channels, &result.channel_count);
        // Score channels
        score_channels(result.channels, result.channel_count, &local_copy);
        find_best_channels(&result);
        
        // Store results and log
        pthread_mutex_lock(&analyzer->result_mutex);
        memcpy(&analyzer->latest_result, &result, sizeof(result));
        analyzer->has_result = 1;
        pthread_mutex_unlock(&analyzer->result_mutex);

        LOG_INF("Analysis complete: Best 2.4GHz=ch%u (score=%.1f), "
                "Best 5GHz=ch%u (score=%.1f)",
                result.best_channel_2_4ghz, result.best_score_2_4ghz,
                result.best_channel_5ghz, result.best_score_5ghz);

        // Print full channel table for debugging
        for (size_t i = 0; i < result.channel_count; i++) {
            LOG_DBG("  Ch %3u (%u MHz) [%s]: APs=%u, avg=%ddBm, score=%.1f",
                    result.channels[i].channel_number,
                    result.channels[i].freq_mhz,
                    result.channels[i].band == BAND_2_4_GHZ ? "2.4G" : "5G",
                    result.channels[i].ap_count,
                    result.channels[i].avg_signal_dbm,
                    result.channels[i].score);
        }
    }

    LOG_INF("Analyzer thread exiting");
    return NULL;
}

int channel_analyzer_get_results(channel_analyzer_t *analyzer,
                                 analysis_result_t *out)
{
    if (!analyzer || !out) {
        return -1;
    }

    pthread_mutex_lock(&analyzer->result_mutex);
    if (!analyzer->has_result) {
        pthread_mutex_unlock(&analyzer->result_mutex);
        return -1;
    }
    memcpy(out, &analyzer->latest_result, sizeof(analysis_result_t));
    pthread_mutex_unlock(&analyzer->result_mutex);

    return 0;
}

void channel_analyzer_shutdown(channel_analyzer_t *analyzer)
{
    if (!analyzer) {
        return;
    }

    LOG_INF("Shutting down channel analyzer");
    analyzer->running = 0;
    pthread_mutex_destroy(&analyzer->result_mutex);
    free(analyzer);
    LOG_INF("Channel analyzer shut down");
}
