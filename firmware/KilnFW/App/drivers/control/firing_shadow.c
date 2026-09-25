// firing_shadow.c -- see firing_shadow.h for the full design/deviation
// rationale. Pure scoring + a small, self-contained NVS store; never calls
// any iter_tune_* function, never writes a gain.

#include "firing_shadow.h"

#include <string.h>

#include "esp_log.h"

#include "hal_kv.h"
#include "nvs_key_check.h"
#include "zones_config_accessors.h" /* zone_model_at(), zones_config_get_progress_band_c(),
                                     * MAX31856_CHANNEL_COUNT (via MAX31856.h) */

static const char *TAG = "firing_shadow";

// Own, independent NVS namespace in the same "kiln_nvs" data partition every
// other non-safety-critical store in this tree uses (iter_tune_store.c's own
// header comment explains why kiln_nvs specifically) -- deliberately never
// iter_tune's own "iter_tune" namespace, and this store's blob is NOT
// iter_tune_store_blob_t (no version bump there; see this task's own
// instruction not to touch that store at all).
#define FIRING_SHADOW_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(FIRING_SHADOW_NVS_PARTITION);
#define FIRING_SHADOW_NVS_NAMESPACE "shadow_tune" // 11 chars
NVS_KEY_LEN_CHECK(FIRING_SHADOW_NVS_NAMESPACE);
#define FIRING_SHADOW_NVS_KEY "sdwblob"
NVS_KEY_LEN_CHECK(FIRING_SHADOW_NVS_KEY);

#define FIRING_SHADOW_STORE_VERSION 1u

// Compact verdict-summary blob -- counts only, never a score set. Pinned
// size/layout, same convention iter_tune_store_blob_t uses.
typedef struct {
    uint8_t  version;
    uint8_t  reserved[3];
    uint32_t firings_scored;
    uint32_t accept_count;
    uint32_t reject_count;
    uint32_t insufficient_count;
    uint32_t no_matched_pairs_count;
    uint8_t  last_verdict;
    uint8_t  reserved2[3];
    float    last_composite_normalised;
} firing_shadow_store_blob_t;

_Static_assert(sizeof(firing_shadow_store_blob_t) == 32,
               "firing_shadow_store_blob_t size must stay pinned");

// Per-zone in-progress segment tracking. RAM only.
typedef struct {
    bool have_segment_index;
    uint8_t current_segment_index;
    bool seg_active;
    firing_score_seg_t seg;
    bool have_last_target;
    float last_target_c;
    bool zone_captured; // capture-transient latch, reset once per firing (see header)
} firing_shadow_zone_state_t;

static firing_shadow_zone_state_t s_zone[MAX31856_CHANNEL_COUNT];
static firing_score_set_t s_current_set;
static firing_score_set_t s_previous_set;
static bool s_have_previous;      // RAM only, per firing_shadow.h's contract -- never persisted

static firing_shadow_status_t s_status;
static bool s_status_loaded;

static void firing_shadow_store_persist(void)
{
    firing_shadow_store_blob_t blob = {0};
    blob.version = FIRING_SHADOW_STORE_VERSION;
    blob.firings_scored = s_status.firings_scored;
    blob.accept_count = s_status.accept_count;
    blob.reject_count = s_status.reject_count;
    blob.insufficient_count = s_status.insufficient_count;
    blob.no_matched_pairs_count = s_status.no_matched_pairs_count;
    blob.last_verdict = s_status.last_verdict;
    blob.last_composite_normalised = s_status.last_composite_normalised;

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, FIRING_SHADOW_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE,
                                    FIRING_SHADOW_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "store_persist: hal_kv_open failed: %s", hal_status_to_name(err));
        return;
    }
    err = hal_kv_set_blob(&h, FIRING_SHADOW_NVS_KEY, &blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "store_persist: write failed: %s -- this firing's verdict summary was not saved",
                 hal_status_to_name(err));
    }
}

void firing_shadow_store_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status_loaded = true;

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, FIRING_SHADOW_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY,
                                    FIRING_SHADOW_NVS_PARTITION);
    if (err != HAL_OK) {
        return; // never persisted yet on this board -- all-zero status is correct
    }
    firing_shadow_store_blob_t blob;
    size_t len = sizeof(blob);
    err = hal_kv_get_blob(&h, FIRING_SHADOW_NVS_KEY, &blob, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(blob) || blob.version != FIRING_SHADOW_STORE_VERSION) {
        return; // missing, truncated, or unknown-version -- treated as never persisted
    }
    s_status.firings_scored = blob.firings_scored;
    s_status.accept_count = blob.accept_count;
    s_status.reject_count = blob.reject_count;
    s_status.insufficient_count = blob.insufficient_count;
    s_status.no_matched_pairs_count = blob.no_matched_pairs_count;
    s_status.last_verdict = blob.last_verdict;
    s_status.last_composite_normalised = blob.last_composite_normalised;
}

bool firing_shadow_get_status(firing_shadow_status_t *out)
{
    if (!s_status_loaded) {
        firing_shadow_store_start();
    }
    if (out != NULL) {
        *out = s_status;
    }
    return true;
}

void firing_shadow_zone_tick(uint8_t zone_index, bool actual_valid, float actual_c, float target_c,
                              bool dwelling, uint8_t segment_index, float dt_s)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return; // defensive: caller derives this by pointer arithmetic against s_exec.zones
    }
    if (!actual_valid) {
        return; // excluded sample -- not scored, does not advance the open segment either
    }

    firing_shadow_zone_state_t *zs = &s_zone[zone_index];
    bool new_segment = !zs->have_segment_index || segment_index != zs->current_segment_index;
    if (new_segment) {
        if (zs->seg_active) {
            firing_score_set_finish_segment(&s_current_set, &zs->seg);
            zs->seg_active = false;
        }
        // Approximate commanded rate as the instantaneous target-temperature
        // slope at this, the segment's first tick -- see firing_shadow.h
        // point 2 for why this differs from reading the profile step table
        // directly.
        float commanded_rate_c_per_hr = 0.0f;
        if (zs->have_last_target && dt_s > 0.0f) {
            commanded_rate_c_per_hr = (target_c - zs->last_target_c) / dt_s * 3600.0f;
        }
        float k_dc = 0.0f, tau_s = 0.0f, dead_time_s = 0.0f;
        (void)zone_model_at(zone_index, target_c, &k_dc, &tau_s, &dead_time_s);
        float band_c = 0.0f;
        if (!zones_config_get_progress_band_c(zone_index, &band_c) || band_c <= 0.0f) {
            band_c = 2.0f; // documented fallback -- no dedicated tracking-band accessor exists
        }
        firing_score_cfg_t cfg = {0};
        cfg.band_c = band_c;
        firing_score_seg_begin(&zs->seg, &cfg, zone_index, commanded_rate_c_per_hr, target_c,
                                dead_time_s, tau_s);
        zs->seg_active = true;
        zs->current_segment_index = segment_index;
        zs->have_segment_index = true;
    }

    (void)dwelling; // segment kind is derived by firing_score_classify() from the estimated
                     // rate, not trusted directly from the caller's dwelling flag
    firing_score_seg_tick(&zs->seg, &zs->zone_captured, target_c, actual_c, /*saturated_high=*/false, dt_s);
    zs->last_target_c = target_c;
    zs->have_last_target = true;
}

void firing_shadow_finish_firing(void)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        firing_shadow_zone_state_t *zs = &s_zone[zi];
        if (zs->seg_active) {
            firing_score_set_finish_segment(&s_current_set, &zs->seg);
            zs->seg_active = false;
        }
    }

    if (!s_status_loaded) {
        firing_shadow_store_start();
    }

    if (s_have_previous) {
        firing_compare_result_t result;
        firing_compare_verdict_t verdict = firing_compare(&s_previous_set, &s_current_set, NULL, &result);
        s_status.firings_scored++;
        s_status.last_verdict = (uint8_t)verdict;
        s_status.last_composite_normalised = result.composite_normalised;
        switch (verdict) {
            case FIRING_COMPARE_ACCEPT:
                s_status.accept_count++;
                break;
            case FIRING_COMPARE_REJECT_DEGRADED:
                s_status.reject_count++;
                break;
            case FIRING_COMPARE_INSUFFICIENT:
                s_status.insufficient_count++;
                break;
            case FIRING_COMPARE_NO_MATCHED_PAIRS:
                s_status.no_matched_pairs_count++;
                break;
        }
        ESP_LOGI(TAG, "shadow verdict: %u (composite_normalised=%.3f), %u scored so far",
                 (unsigned)verdict, (double)result.composite_normalised,
                 (unsigned)s_status.firings_scored);
        firing_shadow_store_persist();
    }
    // else: first firing scored this boot has nothing to compare against --
    // it simply becomes the reference below, no verdict recorded.

    s_previous_set = s_current_set; // RAM-only reference, never persisted (see firing_shadow.h)
    s_have_previous = true;
    memset(&s_current_set, 0, sizeof(s_current_set));
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        memset(&s_zone[zi], 0, sizeof(s_zone[zi]));
    }
}

void firing_shadow_reset_for_test(void)
{
    memset(&s_zone, 0, sizeof(s_zone));
    memset(&s_current_set, 0, sizeof(s_current_set));
    memset(&s_previous_set, 0, sizeof(s_previous_set));
    s_have_previous = false;
    memset(&s_status, 0, sizeof(s_status));
    s_status_loaded = false;
}
