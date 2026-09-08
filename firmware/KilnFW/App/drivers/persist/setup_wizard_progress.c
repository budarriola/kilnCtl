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

#define SETUP_WIZARD_PROGRESS_VERSION 2u

/* ---- version 1 (historical): {state, ts} per step, no note ---- */
typedef struct {
    uint8_t state;
    uint8_t reserved[3];
    uint32_t ts;
} setup_wizard_step_v1_t;

typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    setup_wizard_step_v1_t steps[SETUP_WIZARD_STEP_COUNT];
} setup_wizard_progress_v1_t;

_Static_assert(sizeof(setup_wizard_step_v1_t) == 8, "setup_wizard_step_v1_t layout changed");
_Static_assert(sizeof(setup_wizard_progress_v1_t) == 4 + SETUP_WIZARD_STEP_COUNT * 8,
               "setup_wizard_progress_v1_t layout changed");

/* ---- version 2 (current): v1's per-step record + a tail-appended note ----
 * "Tail-append" means the NEW field is appended at the end of the PER-STEP
 * record (mirroring zones_config_migrate.c's own rule for its per-zone
 * structs), not merely at the end of the outer blob -- a per-step append
 * would not be a valid single memcpy if it were done any other way. */
typedef struct {
    uint8_t state;
    uint8_t reserved[3];
    uint32_t ts;
    char note[SETUP_WIZARD_NOTE_MAX];
} setup_wizard_step_record_t;

typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    setup_wizard_step_record_t steps[SETUP_WIZARD_STEP_COUNT];
} setup_wizard_progress_blob_t;

_Static_assert(sizeof(setup_wizard_step_record_t) == 8 + SETUP_WIZARD_NOTE_MAX,
               "setup_wizard_step_record_t layout changed -- bump SETUP_WIZARD_PROGRESS_VERSION");
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

/* Fills s_steps from a validated version-2 blob. */
static void adopt_v2(const setup_wizard_progress_blob_t *blob)
{
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        s_steps[i].state = (setup_wizard_step_state_t)blob->steps[i].state;
        s_steps[i].ts = blob->steps[i].ts;
        memcpy(s_steps[i].note, blob->steps[i].note, SETUP_WIZARD_NOTE_MAX);
        s_steps[i].note[SETUP_WIZARD_NOTE_MAX - 1] = '\0';
    }
}

/* Migrates a validated version-1 blob forward: state/ts carry over exactly,
 * note defaults to empty (nothing in v1 could have set it -- the field did
 * not exist yet). */
static void adopt_v1_migrate(const setup_wizard_progress_v1_t *blob)
{
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        s_steps[i].state = (setup_wizard_step_state_t)blob->steps[i].state;
        s_steps[i].ts = blob->steps[i].ts;
        s_steps[i].note[0] = '\0';
    }
    ESP_LOGI(TAG, "migrated setup wizard progress v1 -> v%u (per-step note defaults empty)",
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

    /* Read into the widest known layout's buffer; the actual decoded length
     * tells us which version (if any) it matches. */
    setup_wizard_progress_blob_t raw;
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

    if (len == sizeof(setup_wizard_progress_blob_t)) {
        const setup_wizard_progress_blob_t *v2 = &raw;
        if (v2->version == SETUP_WIZARD_PROGRESS_VERSION &&
            steps_valid(v2->steps, sizeof(v2->steps[0]), SETUP_WIZARD_STEP_COUNT)) {
            adopt_v2(v2);
            ESP_LOGI(TAG, "setup wizard progress loaded (v%u)", (unsigned)v2->version);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is v%u-sized but version/fields do not check out -- defaulting",
                 (unsigned)v2->version);
        return ESP_OK;
    }

    if (len == sizeof(setup_wizard_progress_v1_t)) {
        const setup_wizard_progress_v1_t *v1 = (const setup_wizard_progress_v1_t *)&raw;
        if (v1->version == 1u && steps_valid(v1->steps, sizeof(v1->steps[0]), SETUP_WIZARD_STEP_COUNT)) {
            adopt_v1_migrate(v1);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stored setup wizard progress blob is v1-sized but version/fields do not check out -- defaulting");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "stored setup wizard progress blob is size %u (recognize v1=%u, v%u=%u) -- defaulting",
             (unsigned)len, (unsigned)sizeof(setup_wizard_progress_v1_t), (unsigned)SETUP_WIZARD_PROGRESS_VERSION,
             (unsigned)sizeof(setup_wizard_progress_blob_t));
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
