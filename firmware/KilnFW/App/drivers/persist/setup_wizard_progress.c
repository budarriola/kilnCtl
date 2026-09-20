#include "setup_wizard_progress.h"

#include <string.h>

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "hal_time.h"
#include "nvs_key_check.h"

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

#define SETUP_WIZARD_PROGRESS_VERSION 4u

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
#define SETUP_WIZARD_REMOVED_OLD_STEP_INDEX 11u

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

/* ---- version 4 (current): same per-step record, SETUP_WIZARD_STEP_COUNT
 * (13) steps -- 2026-09-19 dropped the old step 11 ("Coupling matrix
 * (optional)") and shifted every step after it down by one slot (see
 * remap_dropping_old_step_11() below and setup_wizard_progress.h's header
 * comment). Note this blob is BYTE-IDENTICAL in size to the historical v2
 * blob (both hold 13 records of setup_wizard_step_record_t) -- the two are
 * told apart by the `version` field alone, checked explicitly in
 * setup_wizard_progress_start() before either is trusted. */
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

/* Fills s_steps from a validated current-version (v4, 13-step) blob. */
static void adopt_v4(const setup_wizard_progress_blob_t *blob)
{
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        s_steps[i].state = (setup_wizard_step_state_t)blob->steps[i].state;
        s_steps[i].ts = blob->steps[i].ts;
        memcpy(s_steps[i].note, blob->steps[i].note, SETUP_WIZARD_NOTE_MAX);
        s_steps[i].note[SETUP_WIZARD_NOTE_MAX - 1] = '\0';
    }
}

/* Shared remap for every pre-v4 layout (v1, v2-legacy, v3-legacy): the old
 * step 11 ("Coupling matrix (optional)", removed 2026-09-19) sat at the same
 * index in all three, and everything after it shifts down by one slot in
 * the new (13-step) numbering. `old_count` is the source layout's own step
 * count (13 for v1/v2, 14 for v3); `has_note` says whether the source
 * per-step record carries a note field (false only for v1's 8-byte record).
 * s_steps must already be at defaults (apply_defaults()) before this runs,
 * since the removed old step 11 and any new step beyond old_count's re-index
 * range are deliberately left untouched (PENDING) -- there is nothing to
 * migrate for either. */
static void remap_dropping_old_step_11(const void *old_steps, size_t old_stride, uint8_t old_count, bool has_note)
{
    for (uint8_t old_i = 0; old_i < old_count; old_i++) {
        if (old_i == SETUP_WIZARD_REMOVED_OLD_STEP_INDEX) {
            continue; /* the coupling-matrix step itself -- discarded, not migrated */
        }
        uint8_t new_i = (old_i < SETUP_WIZARD_REMOVED_OLD_STEP_INDEX) ? old_i : (uint8_t)(old_i - 1);
        if (new_i >= SETUP_WIZARD_STEP_COUNT) {
            continue; /* should not happen for any known old_count, but never write out of bounds */
        }
        const uint8_t *rec = (const uint8_t *)old_steps + (size_t)old_i * old_stride;
        /* Every known record layout (v1 and v2/v3) starts with {state(u8),
         * reserved[3], ts(u32)} -- only the tail (note[], present or not)
         * differs, so this part is layout-agnostic. */
        uint8_t state = rec[0];
        uint32_t ts;
        memcpy(&ts, rec + 4, sizeof(ts));
        s_steps[new_i].state = (setup_wizard_step_state_t)state;
        s_steps[new_i].ts = ts;
        if (has_note) {
            memcpy(s_steps[new_i].note, rec + 8, SETUP_WIZARD_NOTE_MAX);
            s_steps[new_i].note[SETUP_WIZARD_NOTE_MAX - 1] = '\0';
        } else {
            s_steps[new_i].note[0] = '\0';
        }
    }
}

/* Migrates a validated version-1 blob forward: state/ts carry over (note
 * defaults to empty -- v1 predates that field entirely) for every step
 * except the removed old step 11, which is dropped, and every step after it
 * shifts down one slot per remap_dropping_old_step_11(). v1 predates step 13
 * ("Authentication") too, so the new step 12 is left at its already-applied
 * PENDING default -- nothing recorded for it to migrate. */
static void adopt_v1_migrate(const setup_wizard_progress_v1_t *blob)
{
    remap_dropping_old_step_11(blob->steps, sizeof(blob->steps[0]), SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT, false);
    ESP_LOGI(TAG, "migrated setup wizard progress v1 -> v%u (old step 11 dropped, steps after it shifted down, "
                  "note defaults empty, new step 12 defaults pending)",
             SETUP_WIZARD_PROGRESS_VERSION);
}

/* Migrates a validated legacy version-2 (13-step) blob forward: every stored
 * record except the removed old step 11 carries over via
 * remap_dropping_old_step_11(); v2 predates step 13 ("Authentication") too,
 * so the new step 12 is left at its already-applied PENDING default. */
static void adopt_v2_legacy_migrate(const setup_wizard_progress_v2_legacy_t *blob)
{
    remap_dropping_old_step_11(blob->steps, sizeof(blob->steps[0]), SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT, true);
    ESP_LOGI(TAG, "migrated setup wizard progress v2 (13 steps) -> v%u (old step 11 dropped, steps after it "
                  "shifted down, new step 12 defaults pending)",
             SETUP_WIZARD_PROGRESS_VERSION);
}

/* Migrates a validated legacy version-3 (14-step) blob forward: every stored
 * record except the removed old step 11 carries over via
 * remap_dropping_old_step_11(), INCLUDING old step 13 ("Authentication"),
 * which lands at new step 12 -- v3 is the one historical layout that already
 * had it recorded. */
static void adopt_v3_legacy_migrate(const setup_wizard_progress_v3_legacy_t *blob)
{
    remap_dropping_old_step_11(blob->steps, sizeof(blob->steps[0]), SETUP_WIZARD_LEGACY_V3_STEP_COUNT, true);
    ESP_LOGI(TAG, "migrated setup wizard progress v3 (14 steps) -> v%u (13 steps, old step 11 dropped, "
                  "steps after it shifted down)",
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

esp_err_t setup_wizard_progress_start(void)
{
    apply_defaults(); /* safe "nothing visited" state stands until proven otherwise below */

    hal_status_t part_err = nvs_partition_init(NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- setup wizard progress stays at defaults this boot",
                 NVS_PARTITION, hal_status_to_name(part_err));
        return ESP_OK; /* non-fatal, same convention as touch_cal_store_load()/display_power_cfg_start() */
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NVS_PARTITION);
    if (err != HAL_OK) {
        /* HAL_NOT_FOUND on a fresh board is the expected steady state. */
        return ESP_OK;
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
        return ESP_OK;
    }

    /* v4 (current) and legacy v2 are BYTE-IDENTICAL in size (both 13 records
     * of setup_wizard_step_record_t) -- `version` is the only thing that
     * tells them apart, so this size bucket must check it before anything
     * else. */
    if (len == sizeof(setup_wizard_progress_blob_t)) {
        const setup_wizard_progress_blob_t *v4 = (const setup_wizard_progress_blob_t *)&raw;
        if (v4->version == SETUP_WIZARD_PROGRESS_VERSION &&
            steps_valid(v4->steps, sizeof(v4->steps[0]), SETUP_WIZARD_STEP_COUNT)) {
            adopt_v4(v4);
            ESP_LOGI(TAG, "setup wizard progress loaded (v%u)", (unsigned)v4->version);
            return ESP_OK;
        }
        const setup_wizard_progress_v2_legacy_t *v2 = (const setup_wizard_progress_v2_legacy_t *)&raw;
        if (v2->version == 2u &&
            steps_valid(v2->steps, sizeof(v2->steps[0]), SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT)) {
            adopt_v2_legacy_migrate(v2);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is 13-step-sized but version/fields do not check out "
                      "(saw version %u, expected %u current or 2 legacy) -- defaulting",
                 (unsigned)((const setup_wizard_progress_blob_t *)&raw)->version,
                 (unsigned)SETUP_WIZARD_PROGRESS_VERSION);
        return ESP_OK;
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
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is v3-legacy-sized but version/fields do not check out -- defaulting");
        return ESP_OK;
    }

    if (len == sizeof(setup_wizard_progress_v1_t)) {
        const setup_wizard_progress_v1_t *v1 = (const setup_wizard_progress_v1_t *)&raw;
        if (v1->version == 1u && steps_valid(v1->steps, sizeof(v1->steps[0]), SETUP_WIZARD_LEGACY_V1V2_STEP_COUNT)) {
            adopt_v1_migrate(v1);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is v1-sized but version/fields do not check out -- defaulting");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "stored setup wizard progress blob is size %u (recognize v1=%u, v2/v4=%u, v3-legacy=%u) -- defaulting",
             (unsigned)len, (unsigned)sizeof(setup_wizard_progress_v1_t),
             (unsigned)sizeof(setup_wizard_progress_blob_t), (unsigned)sizeof(setup_wizard_progress_v3_legacy_t));
    return ESP_OK;
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
    hal_status_t part_err = nvs_partition_init(NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- setup wizard progress not persisted", NVS_PARTITION,
                 hal_status_to_name(part_err));
        return hal_status_to_esp_err(part_err);
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "hal_kv_open failed: %s -- setup wizard progress not persisted", hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }

    setup_wizard_progress_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = SETUP_WIZARD_PROGRESS_VERSION;
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        blob.steps[i].state = (uint8_t)s_steps[i].state;
        blob.steps[i].ts = s_steps[i].ts;
        memcpy(blob.steps[i].note, s_steps[i].note, SETUP_WIZARD_NOTE_MAX);
    }

    err = hal_kv_set_blob(&h, NVS_KEY_PROGRESS, &blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);

    if (err != HAL_OK) {
        /* Non-negotiable per this task's spec: never silently discard a
         * persist failure -- log the real esp_err_to_name() text. */
        ESP_LOGE(TAG, "could not persist setup wizard progress: %s -- will not survive a reboot",
                 hal_status_to_name(err));
    }
    return hal_status_to_esp_err(err);
}

esp_err_t setup_wizard_progress_set_step(uint8_t step_index, setup_wizard_step_state_t state, const char *note)
{
    if (step_index >= SETUP_WIZARD_STEP_COUNT || !setup_wizard_step_state_is_valid(state)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* In-RAM truth first -- live immediately regardless of whether the NVS
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
