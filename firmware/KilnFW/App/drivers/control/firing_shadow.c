// firing_shadow.c -- see firing_shadow.h for the full design/deviation
// rationale. Pure scoring + a small, self-contained NVS store; never calls
// any iter_tune_* function, never writes a gain.

#include "firing_shadow.h"

#include <string.h>

#include "esp_attr.h" /* EXT_RAM_BSS_ATTR -- see s_zone/s_current_set/s_previous_set/s_status below */
#include "esp_log.h"
#include "freertos/FreeRTOS.h" /* must precede portmacro.h -- configNUMBER_OF_CORES etc. are resolved
                                 * there first; see profile_executor_internal.h's own include order */
#include "freertos/portmacro.h" /* portMUX_TYPE -- s_status torn-read guard below */

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

// v1 -> v2 (2026-09-24 review advisory): added alloc_failed_count, a
// distinct counter for a firing_compare() malloc failure (FIRING_COMPARE_
// ALLOC_FAILED) that v1 had no field for and would otherwise have folded
// into no_matched_pairs_count, hiding a low-memory streak behind a
// legitimate "nothing to compare" streak. firing_shadow_store_start() below
// already treats any version other than the current one as never-persisted
// (fail-closed) -- a board upgrading from v1 simply starts this counter, and
// every other counter, at zero rather than misreading a v1 blob's layout.
#define FIRING_SHADOW_STORE_VERSION 2u

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
    uint32_t alloc_failed_count;
    uint8_t  last_verdict;
    uint8_t  reserved2[3];
    float    last_composite_normalised;
} firing_shadow_store_blob_t;

_Static_assert(sizeof(firing_shadow_store_blob_t) == 36,
               "firing_shadow_store_blob_t size must stay pinned");

// v1 layout (32 B, pre-2026-09-24): the same header/counters as v2 minus
// alloc_failed_count, which sat between no_matched_pairs_count and
// last_verdict -- removing it shifts last_verdict/reserved2/
// last_composite_normalised each 4 bytes earlier than their v2 offsets.
// Kept only for the one-time migration in firing_shadow_store_start() below
// (2026-09-24 review advisory: a v1 blob must not reset firings_scored to 0
// on upgrade -- step 8's "at least 5 firings scored" gate depends on this
// counter surviving the version bump).
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
} firing_shadow_store_blob_v1_t;

_Static_assert(sizeof(firing_shadow_store_blob_v1_t) == 32,
               "firing_shadow_store_blob_v1_t size must stay pinned");

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

// check_kilnfw_dram_bss_budget.ps1 (this task's coordinator review): moved off
// internal .dram0.bss into PSRAM, same pattern as autotune_engine.c's s_at.
// Plain RAM state, no PSRAM-stack-writes-NVS hazard -- the only write path
// (firing_shadow_store_persist's hal_kv_set_blob/commit) runs from
// executor_task_entry(), which is INTERNAL-stacked (profile_executor_start.c),
// never PSRAM-stacked.
static EXT_RAM_BSS_ATTR firing_shadow_zone_state_t s_zone[MAX31856_CHANNEL_COUNT];
static EXT_RAM_BSS_ATTR firing_score_set_t s_current_set;
static EXT_RAM_BSS_ATTR firing_score_set_t s_previous_set;
static bool s_have_previous;      // RAM only, per firing_shadow.h's contract -- never persisted

static EXT_RAM_BSS_ATTR firing_shadow_status_t s_status;
static bool s_status_loaded;

// Guards every access to s_status (and s_status_loaded): the executor task
// updates it field-by-field in firing_shadow_finish_firing() while the httpd
// task reads it via firing_shadow_get_status() (iter_tune_status_get_handler,
// two chunk writes) -- a plain `*out = s_status` struct copy is not atomic
// with respect to that concurrent update, so a reader could observe some
// fields from before an update and some from after (2026-09-24 review
// advisory, "torn status read"). Same pattern as profile_executor_firing_
// stats.c's s_fs_last_run_data_mux: a spinlock held only across a plain
// struct copy/assignment, never across a blocking or producer call.
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;

static void firing_shadow_store_persist(void)
{
    firing_shadow_store_blob_t blob = {0};
    blob.version = FIRING_SHADOW_STORE_VERSION;
    blob.firings_scored = s_status.firings_scored;
    blob.accept_count = s_status.accept_count;
    blob.reject_count = s_status.reject_count;
    blob.insufficient_count = s_status.insufficient_count;
    blob.no_matched_pairs_count = s_status.no_matched_pairs_count;
    blob.alloc_failed_count = s_status.alloc_failed_count;
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
    // Populate a local first: the NVS read/decode below can take a while and
    // must never run with the spinlock held (never hold a critical section
    // across a blocking/producer call), so s_status itself is only touched
    // under the lock, in one short critical section, once fully assembled.
    firing_shadow_status_t loaded;
    memset(&loaded, 0, sizeof(loaded));

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, FIRING_SHADOW_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY,
                                    FIRING_SHADOW_NVS_PARTITION);
    if (err == HAL_OK) {
        // Read the size first, so a v1-sized blob can be migrated instead of
        // being dismissed as never-persisted by an exact-v2-size check --
        // same pattern as kiln_cfg_store.c's own v1/v2 migration probe.
        size_t stored_len = 0;
        err = hal_kv_get_blob(&h, FIRING_SHADOW_NVS_KEY, NULL, &stored_len);
        if (err == HAL_OK && stored_len == sizeof(firing_shadow_store_blob_v1_t)) {
            firing_shadow_store_blob_v1_t v1;
            size_t v1_len = sizeof(v1);
            err = hal_kv_get_blob(&h, FIRING_SHADOW_NVS_KEY, &v1, &v1_len);
            hal_kv_close(&h);
            if (err == HAL_OK && v1_len == sizeof(v1) && v1.version == 1) {
                // Migration (2026-09-24 review advisory): carry every v1
                // counter forward unchanged; the only new v2 field,
                // alloc_failed_count, has no v1 history and starts at 0 --
                // it was never distinguished from no_matched_pairs_count
                // before this version existed, and re-deriving it from the
                // past would be a guess, not a fact. firings_scored (step 8's
                // "at least 5 firings scored" gate) survives the upgrade.
                loaded.firings_scored = v1.firings_scored;
                loaded.accept_count = v1.accept_count;
                loaded.reject_count = v1.reject_count;
                loaded.insufficient_count = v1.insufficient_count;
                loaded.no_matched_pairs_count = v1.no_matched_pairs_count;
                loaded.alloc_failed_count = 0;
                loaded.last_verdict = v1.last_verdict;
                loaded.last_composite_normalised = v1.last_composite_normalised;
                ESP_LOGI(TAG, "shadow_tune store migrated v1 -> v%u (firings_scored=%lu carried forward)",
                         (unsigned)FIRING_SHADOW_STORE_VERSION, (unsigned long)loaded.firings_scored);
            }
            // else: v1-sized but unreadable or not actually version 1 --
            // treated as never persisted, same as any other corrupt blob.
        } else if (err == HAL_OK && stored_len == sizeof(firing_shadow_store_blob_t)) {
            firing_shadow_store_blob_t blob;
            size_t len = sizeof(blob);
            err = hal_kv_get_blob(&h, FIRING_SHADOW_NVS_KEY, &blob, &len);
            hal_kv_close(&h);
            // missing, truncated, or unknown-version -- treated as never persisted
            if (err == HAL_OK && len == sizeof(blob) && blob.version == FIRING_SHADOW_STORE_VERSION) {
                loaded.firings_scored = blob.firings_scored;
                loaded.accept_count = blob.accept_count;
                loaded.reject_count = blob.reject_count;
                loaded.insufficient_count = blob.insufficient_count;
                loaded.no_matched_pairs_count = blob.no_matched_pairs_count;
                loaded.alloc_failed_count = blob.alloc_failed_count;
                loaded.last_verdict = blob.last_verdict;
                loaded.last_composite_normalised = blob.last_composite_normalised;
            }
        } else {
            hal_kv_close(&h);
        }
    }
    // Published only once s_status is fully populated (review fix): a reader
    // that sees s_status_loaded == true never sees a half-loaded struct.
    portENTER_CRITICAL(&s_status_mux);
    s_status = loaded;
    s_status_loaded = true;
    portEXIT_CRITICAL(&s_status_mux);
}

bool firing_shadow_get_status(firing_shadow_status_t *out)
{
    // Review fix: NO lazy load here. This is called from the httpd task
    // (iter_tune_status_get_handler()); a lazy firing_shadow_store_start()
    // from there would memset and rewrite s_status concurrently with the
    // executor task's firing_shadow_finish_firing() incrementing it, losing
    // a verdict. iter_tune_http_start() loads the store once at boot instead;
    // until then this reports "not loaded" (the route emits "shadow":null).
    //
    // The struct copy below runs under s_status_mux so it can never observe
    // a torn mix of pre-/post-update fields while the executor task is
    // concurrently updating s_status in firing_shadow_finish_firing()
    // (2026-09-24 review advisory) -- both the loaded-flag check and the
    // copy happen inside one critical section so a caller never sees
    // s_status_loaded flip true with a partially-copied struct either.
    portENTER_CRITICAL(&s_status_mux);
    bool loaded = s_status_loaded;
    if (loaded && out != NULL) {
        *out = s_status;
    }
    portEXIT_CRITICAL(&s_status_mux);
    return loaded;
}

void firing_shadow_zone_tick(uint8_t zone_index, bool actual_valid, float actual_c, float target_c,
                              bool dwelling, uint8_t segment_index, float dt_s)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return; // defensive: caller derives this from z's address within s_exec.zones
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
        // Update the whole verdict-summary struct inside one critical
        // section so a concurrent httpd-task reader (firing_shadow_
        // get_status()) can never observe a mix of pre- and post-update
        // fields (2026-09-24 review advisory, "torn status read"). No
        // blocking/producer call runs inside the lock; firing_shadow_
        // store_persist() below (which opens NVS) runs after it is released.
        portENTER_CRITICAL(&s_status_mux);
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
            case FIRING_COMPARE_ALLOC_FAILED:
                // Distinct from no_matched_pairs_count (2026-09-24 review
                // advisory): a malloc failure inside firing_compare() means
                // the comparison never ran, not that this pair had nothing
                // to say. Never scored as accept/reject/insufficient.
                s_status.alloc_failed_count++;
                break;
        }
        portEXIT_CRITICAL(&s_status_mux);
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

void firing_shadow_abandon_firing(void)
{
    // RAM only -- no NVS open/write, so this is safe from any task's stack,
    // including a PSRAM-stacked one. Deliberately leaves s_previous_set/
    // s_have_previous and the persisted verdict-summary counters untouched:
    // an abandoned, never-finished firing must not become the reference the
    // NEXT real firing is compared against, and it was never scored, so
    // there is nothing to persist.
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
    portENTER_CRITICAL(&s_status_mux);
    memset(&s_status, 0, sizeof(s_status));
    s_status_loaded = false;
    portEXIT_CRITICAL(&s_status_mux);
}
