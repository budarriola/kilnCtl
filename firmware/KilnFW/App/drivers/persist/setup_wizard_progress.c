#include "setup_wizard_progress.h"
#include "cfgfs_file_validators.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cfg_fs_status.h"
#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "hal_time.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"
#include "persist_scratch.h"

static const char *TAG = "setup_wiz_progress";

/* Own partition/namespace, same isolation rationale touch_cal_store.c's
 * header comment gives -- a corrupt/rejected record here must never be able
 * to take zones/safety config down with it, or vice versa. All three
 * literals are <=15 chars, checked at compile time. */
#define NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE "setup_wiz"
#define NVS_KEY_PROGRESS "progress_v1"
NVS_KEY_LEN_CHECK(NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_PROGRESS);

#define SETUP_WIZARD_PROGRESS_VERSION 5u

/* Historical on-disk step count for the v1 and v2 blob layouts below --
 * FROZEN at 13, deliberately never tied to the live SETUP_WIZARD_STEP_COUNT
 * (which grew to 14 on 2026-09-18 alongside the wizard page's 14th step, then
 * shrank back to 13 on 2026-09-19 when a *different* step, 11, was removed --
 * see remap_dropping_old_step_11() below; v1/v2's own 13 steps are NOT the
 * same 13 steps as today's, they still include the old step 11). If this
 * were instead `SETUP_WIZARD_STEP_COUNT`, a future change of that constant
 * would silently reinterpret an old board's actual on-disk v1/v2 blob size,
 * breaking version detection for boards that never got this far -- the same
 * "reset one side of a pair" class this codebase already has four confirmed
 * instances of (see setup_wizard_progress.h's header comment / MEMORY.md
 * project_reset_one_side_bug_class). */
#define SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT 13u

/* Historical on-disk step count for the v3 blob layout (2026-09-18's 14-step
 * era, ids 0..13) -- frozen for the exact same reason as the constant above:
 * SETUP_WIZARD_STEP_COUNT itself moved on (back to 13) the very next day. */
#define SETUP_WIZARD_LEGACY_V3_STEP_COUNT 14u

/* The old step index removed 2026-09-19 ("Coupling matrix (optional)") --
 * present in both the v1/v2 (13-step) and v3 (14-step) on-disk layouts, at
 * the same index in both since it predates the v3 step-13 addition. */
#define SETUP_WIZARD_REMOVED_OLD_STEP_11_INDEX 11u

/* Historical on-disk step count for the v4 blob layout (2026-09-19's
 * "drop old step 11" era, ids 0..12) -- frozen for the same reason as the
 * constants above: SETUP_WIZARD_STEP_COUNT itself moved on again (to 12) the
 * same day, when a second, separate step was folded away. */
#define SETUP_WIZARD_LEGACY_V4_STEP_COUNT 13u

/* The old step index removed 2026-09-19, second pass ("Current sensing:
 * install & calibrate", folded into step 7). This index is the same 8 in
 * every source layout this migrates from (v1/v2/v3's pre-step-11-drop
 * numbering and v4's post-step-11-drop numbering alike), because 8 < 11 in
 * both -- dropping step 11 never moves step 8. */
#define SETUP_WIZARD_REMOVED_OLD_STEP_8_INDEX 8u

/* ---- version 1 (historical): {state, ts} per step, no note ---- */
typedef struct {
    uint8_t state;
    uint8_t reserved[3];
    uint32_t ts;
} setup_wizard_step_v1_t;

typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    setup_wizard_step_v1_t steps[SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT];
} setup_wizard_progress_v1_t;

_Static_assert(sizeof(setup_wizard_step_v1_t) == 8, "setup_wizard_step_v1_t layout changed");
_Static_assert(sizeof(setup_wizard_progress_v1_t) == 4 + SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT * 8,
               "setup_wizard_progress_v1_t layout changed");

/* Per-step record shape, used by BOTH the historical 13-step v2 blob and the
 * current 14-step v3 blob below -- only the step COUNT changed between v2
 * and v3, not this record's own layout, so one typedef serves both blob
 * structs (each with its own array length). "Tail-append" (v1 -> v2) means
 * the NEW field is appended at the end of the PER-STEP record (mirroring
 * zones_config_migrate.c's own rule for its per-zone structs), not merely
 * at the end of the outer blob -- a per-step append would not be a valid
 * single memcpy if it were done any other way. */
typedef struct {
    uint8_t state;
    uint8_t reserved[3];
    uint32_t ts;
    char note[SETUP_WIZARD_NOTE_MAX];
} setup_wizard_step_record_t;

_Static_assert(sizeof(setup_wizard_step_record_t) == 8 + SETUP_WIZARD_NOTE_MAX,
               "setup_wizard_step_record_t layout changed -- bump SETUP_WIZARD_PROGRESS_VERSION");

/* ---- version 2 (historical): 13-step blob of setup_wizard_step_record_t --
 * kept around ONLY so a board that persisted a v2 blob before the 2026-09-18
 * step-count fix can still be migrated forward without its progress being
 * silently discarded. Frozen at SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT for the
 * same reason as setup_wizard_progress_v1_t above. */
typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    setup_wizard_step_record_t steps[SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT];
} setup_wizard_progress_v2_legacy_t;

_Static_assert(sizeof(setup_wizard_progress_v2_legacy_t) ==
                   4 + SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT * (8 + SETUP_WIZARD_NOTE_MAX),
               "setup_wizard_progress_v2_legacy_t layout changed");

/* ---- version 3 (historical, 2026-09-18..2026-09-19): same per-step record,
 * 14 steps -- that fix appended step 13 (web-auth's "Authentication
 * (optional)") at the true tail of the steps array. Kept around ONLY so a
 * board that persisted a v3 blob in that one-day window can still be
 * migrated forward (see remap_dropping_old_step_11() below) instead of
 * having its progress silently discarded. */
typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    setup_wizard_step_record_t steps[SETUP_WIZARD_LEGACY_V3_STEP_COUNT];
} setup_wizard_progress_v3_legacy_t;

_Static_assert(sizeof(setup_wizard_progress_v3_legacy_t) ==
                   4 + SETUP_WIZARD_LEGACY_V3_STEP_COUNT * (8 + SETUP_WIZARD_NOTE_MAX),
               "setup_wizard_progress_v3_legacy_t layout changed");

/* ---- version 4 (historical, 2026-09-19 morning..2026-09-19 evening): same
 * per-step record, 13 steps -- dropped the old step 11 ("Coupling matrix
 * (optional)") and shifted every step after it down by one slot. Kept around
 * ONLY so a board that persisted a v4 blob in that window still migrates
 * forward (dropping the second removed step, old step 8) instead of having
 * its progress silently discarded. Note this blob is BYTE-IDENTICAL in size
 * to the historical v2 blob (both hold 13 records of
 * setup_wizard_step_record_t) -- the two are told apart by the `version`
 * field alone, checked explicitly in setup_wizard_progress_start() before
 * either is trusted. */
typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    setup_wizard_step_record_t steps[SETUP_WIZARD_LEGACY_V4_STEP_COUNT];
} setup_wizard_progress_v4_legacy_t;

_Static_assert(sizeof(setup_wizard_progress_v4_legacy_t) ==
                   4 + SETUP_WIZARD_LEGACY_V4_STEP_COUNT * (8 + SETUP_WIZARD_NOTE_MAX),
               "setup_wizard_progress_v4_legacy_t layout changed");

/* ---- version 5 (current): same per-step record, SETUP_WIZARD_STEP_COUNT
 * (12) steps -- 2026-09-19 additionally dropped the old step 8 ("Current
 * sensing: install & calibrate", folded into step 7's safety-processor
 * commissioning screen) and shifted every step after it down by one more
 * slot (see remap_drop_index() below and setup_wizard_progress.h's header
 * comment). */
typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    setup_wizard_step_record_t steps[SETUP_WIZARD_STEP_COUNT];
} setup_wizard_progress_blob_t;

_Static_assert(sizeof(setup_wizard_progress_blob_t) == 4 + SETUP_WIZARD_STEP_COUNT * (8 + SETUP_WIZARD_NOTE_MAX),
               "setup_wizard_progress_blob_t layout changed -- bump SETUP_WIZARD_PROGRESS_VERSION");

static setup_wizard_step_t s_steps[SETUP_WIZARD_STEP_COUNT];

static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

static void apply_defaults(void)
{
    memset(s_steps, 0, sizeof(s_steps));
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        s_steps[i].state = SETUP_WIZ_STEP_PENDING;
    }
}

/* Fills s_steps from a validated current-version (v5, 12-step) blob. */
static void adopt_v5(const setup_wizard_progress_blob_t *blob)
{
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        s_steps[i].state = (setup_wizard_step_state_t)blob->steps[i].state;
        s_steps[i].ts = blob->steps[i].ts;
        memcpy(s_steps[i].note, blob->steps[i].note, SETUP_WIZARD_NOTE_MAX);
        s_steps[i].note[SETUP_WIZARD_NOTE_MAX - 1] = '\0';
    }
}

/* General "drop one step index, shift everything after it down one slot"
 * remap, shared by every migration below. `dest` must already be at
 * PENDING/empty defaults for its whole `dest_count` -- the dropped index and
 * any source index beyond `dest_count`'s re-index range are deliberately
 * left untouched, since there is nothing to migrate for either. `old_count`
 * is the source layout's own step count; `has_note` says whether the source
 * per-step record carries a note field (false only for v1's 8-byte record).
 * Every known record layout starts with {state(u8), reserved[3], ts(u32)} --
 * only the tail (note[], present or not) differs, so this is layout-agnostic
 * beyond that. */
/* The v1 -> v5 and v2/v3 -> v5 paths above run this twice, using an
 * intermediate array of the IN-RAM setup_wizard_step_t as the second pass's
 * SOURCE -- so that struct, not just the on-disk records, has to keep the
 * {state at 0, ts at 4, note at 8} shape this function reads by byte offset.
 * It does today only because setup_wizard_step_state_t is a 4-byte enum on
 * this target; nothing else pins it, so pin it here rather than let a future
 * enum/packing change silently migrate garbage. */
_Static_assert(offsetof(setup_wizard_step_t, ts) == 4,
               "setup_wizard_step_t.ts moved -- remap_drop_index() reads it at byte offset 4");
_Static_assert(offsetof(setup_wizard_step_t, note) == 8,
               "setup_wizard_step_t.note moved -- remap_drop_index() reads it at byte offset 8");
_Static_assert(sizeof(setup_wizard_step_t) == 8 + SETUP_WIZARD_NOTE_MAX,
               "setup_wizard_step_t grew padding -- remap_drop_index() strides over it");
_Static_assert(sizeof(setup_wizard_step_state_t) == 4,
               "setup_wizard_step_state_t is no longer 4 bytes -- remap_drop_index() reads the state "
               "byte at offset 0, which is only the enum's value on a 4-byte little-endian enum");
static void remap_drop_index(setup_wizard_step_t *dest, uint8_t dest_count,
                              const void *old_steps, size_t old_stride, uint8_t old_count,
                              uint8_t drop_index, bool has_note)
{
    for (uint8_t old_i = 0; old_i < old_count; old_i++) {
        if (old_i == drop_index) {
            continue; /* the removed step itself -- discarded, not migrated */
        }
        uint8_t new_i = (old_i < drop_index) ? old_i : (uint8_t)(old_i - 1);
        if (new_i >= dest_count) {
            continue; /* should not happen for any known old_count, but never write out of bounds */
        }
        const uint8_t *rec = (const uint8_t *)old_steps + (size_t)old_i * old_stride;
        uint8_t state = rec[0];
        uint32_t ts;
        memcpy(&ts, rec + 4, sizeof(ts));
        dest[new_i].state = (setup_wizard_step_state_t)state;
        dest[new_i].ts = ts;
        if (has_note) {
            memcpy(dest[new_i].note, rec + 8, SETUP_WIZARD_NOTE_MAX);
            dest[new_i].note[SETUP_WIZARD_NOTE_MAX - 1] = '\0';
        } else {
            dest[new_i].note[0] = '\0';
        }
    }
}

/* Migrates a validated version-1 blob all the way to v5: first drops old
 * step 11 ("Coupling matrix") into a 13-step intermediate, then drops old
 * step 8 ("Current sensing: install & calibrate", same index either side of
 * the step-11 drop since 8 < 11) from that intermediate into s_steps. Note
 * defaults to empty throughout -- v1 predates that field entirely. */
static void adopt_v1_migrate(const setup_wizard_progress_v1_t *blob)
{
    setup_wizard_step_t tmp[SETUP_WIZARD_LEGACY_V4_STEP_COUNT];
    memset(tmp, 0, sizeof(tmp));
    remap_drop_index(tmp, SETUP_WIZARD_LEGACY_V4_STEP_COUNT,
                      blob->steps, sizeof(blob->steps[0]), SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT,
                      SETUP_WIZARD_REMOVED_OLD_STEP_11_INDEX, false);
    remap_drop_index(s_steps, SETUP_WIZARD_STEP_COUNT,
                      tmp, sizeof(tmp[0]), SETUP_WIZARD_LEGACY_V4_STEP_COUNT,
                      SETUP_WIZARD_REMOVED_OLD_STEP_8_INDEX, true);
    ESP_LOGI(TAG, "migrated setup wizard progress v1 -> v%u (old steps 11 and 8 dropped, later steps shifted "
                  "down, note defaults empty)",
             SETUP_WIZARD_PROGRESS_VERSION);
}

/* Migrates a validated legacy version-2 (13-step) blob the same two-step way
 * as v1 above, except the source already carries notes. */
static void adopt_v2_legacy_migrate(const setup_wizard_progress_v2_legacy_t *blob)
{
    setup_wizard_step_t tmp[SETUP_WIZARD_LEGACY_V4_STEP_COUNT];
    memset(tmp, 0, sizeof(tmp));
    remap_drop_index(tmp, SETUP_WIZARD_LEGACY_V4_STEP_COUNT,
                      blob->steps, sizeof(blob->steps[0]), SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT,
                      SETUP_WIZARD_REMOVED_OLD_STEP_11_INDEX, true);
    remap_drop_index(s_steps, SETUP_WIZARD_STEP_COUNT,
                      tmp, sizeof(tmp[0]), SETUP_WIZARD_LEGACY_V4_STEP_COUNT,
                      SETUP_WIZARD_REMOVED_OLD_STEP_8_INDEX, true);
    ESP_LOGI(TAG, "migrated setup wizard progress v2 (13 steps) -> v%u (old steps 11 and 8 dropped, later steps "
                  "shifted down)",
             SETUP_WIZARD_PROGRESS_VERSION);
}

/* Migrates a validated legacy version-3 (14-step) blob the same way,
 * INCLUDING old step 13 ("Authentication"), which lands at new step 11. */
static void adopt_v3_legacy_migrate(const setup_wizard_progress_v3_legacy_t *blob)
{
    setup_wizard_step_t tmp[SETUP_WIZARD_LEGACY_V4_STEP_COUNT];
    memset(tmp, 0, sizeof(tmp));
    remap_drop_index(tmp, SETUP_WIZARD_LEGACY_V4_STEP_COUNT,
                      blob->steps, sizeof(blob->steps[0]), SETUP_WIZARD_LEGACY_V3_STEP_COUNT,
                      SETUP_WIZARD_REMOVED_OLD_STEP_11_INDEX, true);
    remap_drop_index(s_steps, SETUP_WIZARD_STEP_COUNT,
                      tmp, sizeof(tmp[0]), SETUP_WIZARD_LEGACY_V4_STEP_COUNT,
                      SETUP_WIZARD_REMOVED_OLD_STEP_8_INDEX, true);
    ESP_LOGI(TAG, "migrated setup wizard progress v3 (14 steps) -> v%u (old steps 11 and 8 dropped, later steps "
                  "shifted down)",
             SETUP_WIZARD_PROGRESS_VERSION);
}

/* Migrates a validated legacy version-4 (13-step, old step 11 already
 * dropped) blob forward by dropping the second removed step, old step 8. */
static void adopt_v4_legacy_migrate(const setup_wizard_progress_v4_legacy_t *blob)
{
    remap_drop_index(s_steps, SETUP_WIZARD_STEP_COUNT,
                      blob->steps, sizeof(blob->steps[0]), SETUP_WIZARD_LEGACY_V4_STEP_COUNT,
                      SETUP_WIZARD_REMOVED_OLD_STEP_8_INDEX, true);
    ESP_LOGI(TAG, "migrated setup wizard progress v4 (13 steps) -> v%u (12 steps, old step 8 dropped, steps "
                  "after it shifted down)",
             SETUP_WIZARD_PROGRESS_VERSION);
}

/* Validates every step's own fields regardless of which on-disk version
 * produced them -- a size/version match alone is not enough, the same
 * "field-level range check on top of the size/version check" discipline
 * display_power_cfg.c's validator documents. */
static bool steps_valid(const void *steps, size_t step_stride, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const uint8_t *state_byte = (const uint8_t *)steps + i * step_stride;
        if (!setup_wizard_step_state_is_valid((setup_wizard_step_state_t)*state_byte)) {
            return false;
        }
    }
    return true;
}

/* Reads the legacy NVS record (every historical layout, migrated forward) into
 * s_steps. Returns true only when s_steps now holds a decoded record. */
static bool nvs_legacy_load(void)
{
    hal_status_t part_err = nvs_partition_init(NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- setup wizard progress stays at defaults this boot",
                 NVS_PARTITION, hal_status_to_name(part_err));
        return false; /* non-fatal, same convention as touch_cal_store_load()/display_power_cfg_start() */
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NVS_PARTITION);
    if (err != HAL_OK) {
        /* HAL_NOT_FOUND on a fresh board is the expected steady state. */
        return false;
    }

    /* Read into the widest known layout's buffer (v3-legacy, 14 steps); the
     * actual decoded length tells us which version (if any) it matches. */
    setup_wizard_progress_v3_legacy_t raw;
    memset(&raw, 0, sizeof(raw));
    size_t len = sizeof(raw);
    err = hal_kv_get_blob(&h, NVS_KEY_PROGRESS, &raw, &len);
    hal_kv_close(&h);

    if (err != HAL_OK) {
        if (err != HAL_NOT_FOUND) {
            ESP_LOGW(TAG, "setup wizard progress read failed: %s -- defaults stay in effect this boot",
                     hal_status_to_name(err));
        }
        return false;
    }

    /* v5 (current, 12-step) is its OWN size now -- unlike the old v2/v4
     * pairing, dropping a second step means the current blob is no longer
     * byte-identical to any legacy one, so this bucket only ever holds v5. */
    if (len == sizeof(setup_wizard_progress_blob_t)) {
        const setup_wizard_progress_blob_t *v5 = (const setup_wizard_progress_blob_t *)&raw;
        if (v5->version == SETUP_WIZARD_PROGRESS_VERSION &&
            steps_valid(v5->steps, sizeof(v5->steps[0]), SETUP_WIZARD_STEP_COUNT)) {
            adopt_v5(v5);
            ESP_LOGI(TAG, "setup wizard progress loaded (v%u)", (unsigned)v5->version);
            return true;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is %u-step-sized but version/fields do not check out "
                      "(saw version %u, expected %u) -- defaulting",
                 (unsigned)SETUP_WIZARD_STEP_COUNT, (unsigned)v5->version, (unsigned)SETUP_WIZARD_PROGRESS_VERSION);
        return false;
    }

    /* Legacy v2 (13 steps, pre-2026-09-18) and legacy v4 (13 steps,
     * 2026-09-19's brief between-removals window) are BYTE-IDENTICAL in
     * size -- `version` is the only thing that tells them apart, so this
     * size bucket must check it before anything else. */
    if (len == sizeof(setup_wizard_progress_v4_legacy_t)) {
        const setup_wizard_progress_v4_legacy_t *v4 = (const setup_wizard_progress_v4_legacy_t *)&raw;
        if (v4->version == 4u &&
            steps_valid(v4->steps, sizeof(v4->steps[0]), SETUP_WIZARD_LEGACY_V4_STEP_COUNT)) {
            adopt_v4_legacy_migrate(v4);
            return true;
        }
        const setup_wizard_progress_v2_legacy_t *v2 = (const setup_wizard_progress_v2_legacy_t *)&raw;
        if (v2->version == 2u &&
            steps_valid(v2->steps, sizeof(v2->steps[0]), SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT)) {
            adopt_v2_legacy_migrate(v2);
            return true;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is 13-step-sized but version/fields do not check out "
                      "(saw version %u, expected 4 or 2 legacy) -- defaulting",
                 (unsigned)((const setup_wizard_progress_v4_legacy_t *)&raw)->version);
        return false;
    }

    /* Legacy v3 blob (14 steps, 2026-09-18..2026-09-19 only) -- must be
     * checked before the "unrecognized size" fallback so a board that
     * persisted during that one-day window still migrates forward instead
     * of being silently defaulted away. */
    if (len == sizeof(setup_wizard_progress_v3_legacy_t)) {
        const setup_wizard_progress_v3_legacy_t *v3 = &raw;
        if (v3->version == 3u &&
            steps_valid(v3->steps, sizeof(v3->steps[0]), SETUP_WIZARD_LEGACY_V3_STEP_COUNT)) {
            adopt_v3_legacy_migrate(v3);
            return true;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is v3-legacy-sized but version/fields do not check out -- defaulting");
        return false;
    }

    if (len == sizeof(setup_wizard_progress_v1_t)) {
        const setup_wizard_progress_v1_t *v1 = (const setup_wizard_progress_v1_t *)&raw;
        if (v1->version == 1u && steps_valid(v1->steps, sizeof(v1->steps[0]), SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT)) {
            adopt_v1_migrate(v1);
            return true;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is v1-sized but version/fields do not check out -- defaulting");
        return false;
    }

    ESP_LOGW(TAG, "stored setup wizard progress blob is size %u (recognize v1=%u, v2/v4-legacy=%u, v3-legacy=%u, "
                  "v5-current=%u) -- defaulting",
             (unsigned)len, (unsigned)sizeof(setup_wizard_progress_v1_t),
             (unsigned)sizeof(setup_wizard_progress_v4_legacy_t), (unsigned)sizeof(setup_wizard_progress_v3_legacy_t),
             (unsigned)sizeof(setup_wizard_progress_blob_t));
    return false;
}

/* Quiet read of the current-layout (v5) NVS record only, for the status poll:
 * never touches s_steps, never logs. `raw` is caller-provided scratch (heap in
 * the status poll, which runs on the httpd task). */
static bool nvs_v5_read_quiet(setup_wizard_progress_blob_t *out, setup_wizard_progress_v3_legacy_t *raw)
{
    if (nvs_partition_init(NVS_PARTITION) != HAL_OK) {
        return false;
    }
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NVS_PARTITION) != HAL_OK) {
        return false;
    }
    memset(raw, 0, sizeof(*raw));
    size_t len = sizeof(*raw);
    hal_status_t err = hal_kv_get_blob(&h, NVS_KEY_PROGRESS, raw, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(*out)) {
        return false;
    }
    memcpy(out, raw, sizeof(*out));
    return out->version == SETUP_WIZARD_PROGRESS_VERSION && steps_valid(out->steps, sizeof(out->steps[0]), SETUP_WIZARD_STEP_COUNT);
}

static void steps_to_blob(setup_wizard_progress_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->version = SETUP_WIZARD_PROGRESS_VERSION;
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        blob->steps[i].state = (uint8_t)s_steps[i].state;
        blob->steps[i].ts = s_steps[i].ts;
        memcpy(blob->steps[i].note, s_steps[i].note, SETUP_WIZARD_NOTE_MAX);
    }
}

bool setup_wizard_progress_file_validate(const void *bytes, size_t len)
{
    if (!bytes || len != sizeof(setup_wizard_progress_blob_t)) {
        return false;
    }
    const setup_wizard_progress_blob_t *b = (const setup_wizard_progress_blob_t *)bytes;
    return b->version == SETUP_WIZARD_PROGRESS_VERSION &&
           steps_valid(b->steps, sizeof(b->steps[0]), SETUP_WIZARD_STEP_COUNT);
}

/* rev of the cfg file as last verified on flash; the legacy NVS record has no
 * rev and competes at 0, so any file this build wrote beats it. */
static uint32_t s_rev;

esp_err_t setup_wizard_progress_start(void)
{
    apply_defaults(); /* safe "nothing visited" state stands until proven otherwise below */
    s_rev = 0;

    /* Read-through (pref_cfg_fs.h): the legacy NVS record (any historical
     * layout, migrated to v5 in RAM) is the fallback candidate; the cfg file
     * wins on a strictly higher rev. A fallback NVS candidate is migrated into
     * the file when cfg is mounted. Writes never go back to NVS. */
    bool nvs_ok = nvs_legacy_load();
    setup_wizard_progress_blob_t nvs_blob;
    steps_to_blob(&nvs_blob);

    setup_wizard_progress_blob_t resolved;
    uint32_t rev = 0;
    bool used_file = false;
    if (pref_cfg_fs_resolve(SETUP_WIZARD_PROGRESS_FILE_PATH, &nvs_blob, sizeof(nvs_blob), nvs_ok, 0,
                            setup_wizard_progress_file_validate, &resolved, &rev, &used_file)) {
        if (used_file) {
            adopt_v5(&resolved);
        }
        s_rev = rev;
        ESP_LOGI(TAG, "setup wizard progress loaded (source=%s, rev=%lu)", used_file ? "file" : "NVS",
                 (unsigned long)rev);
    } else {
        apply_defaults();
    }
    return ESP_OK;
}

void setup_wizard_progress_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid,
                                                uint32_t *nvs_rev, bool *diverged)
{
    /* Heap, not stack: this runs on the httpd task (GET /api/cfgfs), and the
     * two blobs plus the legacy-sized read scratch are ~1.5 KB. */
    struct {
        setup_wizard_progress_blob_t f;
        setup_wizard_progress_blob_t n;
        setup_wizard_progress_v3_legacy_t raw;
    } *w = persist_scratch_alloc(sizeof(*w));
    uint32_t f_rev = 0;
    bool f_valid = false;
    bool n_valid = false;
    bool content_equal = false;
    if (w != NULL) {
        pref_cfg_fs_load_raw_quiet(SETUP_WIZARD_PROGRESS_FILE_PATH, sizeof(w->f), setup_wizard_progress_file_validate, &w->f, &f_rev,
                                   &f_valid);
        memset(&w->n, 0, sizeof(w->n));
        n_valid = nvs_v5_read_quiet(&w->n, &w->raw);
        content_equal = f_valid && n_valid && memcmp(&w->f, &w->n, sizeof(w->f)) == 0;
        free(w);
    }
    if (file_valid) {
        *file_valid = f_valid;
    }
    if (file_rev) {
        *file_rev = f_rev;
    }
    if (nvs_valid) {
        *nvs_valid = n_valid;
    }
    if (nvs_rev) {
        *nvs_rev = 0; /* the NVS record has no rev key */
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
    }
}

void setup_wizard_progress_get_all(setup_wizard_step_t out[SETUP_WIZARD_STEP_COUNT])
{
    memcpy(out, s_steps, sizeof(s_steps));
}

esp_err_t setup_wizard_progress_get_step(uint8_t step_index, setup_wizard_step_t *out)
{
    if (!out || step_index >= SETUP_WIZARD_STEP_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_steps[step_index];
    return ESP_OK;
}

static esp_err_t persist_all(void)
{
    /* cfg file ONLY (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"):
     * no NVS write follows, and a failure is returned, never masked. The rev
     * advances only after a verified write. */
    setup_wizard_progress_blob_t blob;
    steps_to_blob(&blob);
    uint32_t new_rev = s_rev + 1;
    esp_err_t err = pref_cfg_fs_commit(SETUP_WIZARD_PROGRESS_FILE_PATH, &blob, sizeof(blob), new_rev,
                                       "setup wizard progress");
    if (err == ESP_OK) {
        s_rev = new_rev;
    }
    return err;
}

esp_err_t setup_wizard_progress_set_step(uint8_t step_index, setup_wizard_step_state_t state, const char *note)
{
    if (step_index >= SETUP_WIZARD_STEP_COUNT || !setup_wizard_step_state_is_valid(state)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* In-RAM truth first -- live immediately regardless of whether the cfg
     * write below succeeds, same ordering as display_power_cfg_set()/
     * touch_cal_store_save(). */
    s_steps[step_index].state = state;
    s_steps[step_index].ts = (uint32_t)(hal_time_now_us() / 1000000ULL);
    if (note && note[0] != '\0') {
        strncpy(s_steps[step_index].note, note, SETUP_WIZARD_NOTE_MAX - 1);
        s_steps[step_index].note[SETUP_WIZARD_NOTE_MAX - 1] = '\0';
    } else {
        s_steps[step_index].note[0] = '\0';
    }

    esp_err_t err = persist_all();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "setup wizard step %u -> state=%u%s%s", (unsigned)step_index, (unsigned)state,
                 s_steps[step_index].note[0] ? " note=" : "", s_steps[step_index].note);
    }
    return err;
}
