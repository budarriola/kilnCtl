#include "safety_cfg_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "kilnlink/kilnlink_config_page.h"

static const char *TAG = "safety_cfg_store";

/* Same partition/namespace convention as zones_http.c/kiln_cfg_store.c -- see
 * this file's header comment for the full "survives an ordinary reflash"
 * rationale. Key distinguishes this module's blob from the others already
 * sharing the "kiln_cfg" namespace (zones_cfg/kilncfgs/...). */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_SAFETY_CFG "safetycfg"

/* Bump whenever safety_cfg_store_blob_t's on-flash layout changes -- mirrors
 * ZONES_CFG_VERSION/KILN_CFG_STORE_VERSION's role in their own files. */
#define SAFETY_CFG_STORE_VERSION 1u

/* CONFIG_REFERENCE.md secs 1-5 / COMMISSIONING.md sec 2.1's param_id table,
 * in that document's own order -- table POSITION is what
 * safety_cfg_store_get_by_index() iterates and what the persisted blob's
 * entries[] array is indexed by, so this order must never be reshuffled
 * (only ever appended to) once any board has saved a cache against it: doing
 * so would silently remap every already-cached value to the WRONG field on
 * next load. Ids themselves are the permanent identity on the wire
 * (COMMISSIONING.md sec 2.1: "ids are permanent"); this table's row order is
 * this cache's OWN, separate permanence rule for the identical reason. */
typedef struct {
    uint16_t id;
    uint8_t type; /* KILNLINK_PARAM_TYPE_* */
    const char *name;
} safety_cfg_table_row_t;

static const safety_cfg_table_row_t SAFETY_CFG_PARAM_TABLE[SAFETY_CFG_PARAM_COUNT] = {
    /* sec 1 -- commissioning, no compiled-in default */
    { 0x0101, KILNLINK_PARAM_TYPE_U8, "tc_source" },
    { 0x0102, KILNLINK_PARAM_TYPE_U8, "borrowed_zone_index" },
    { 0x0103, KILNLINK_PARAM_TYPE_U8, "tc_placement_mode" },
    { 0x0104, KILNLINK_PARAM_TYPE_F32, "abs_max_temp_c" },
    { 0x0105, KILNLINK_PARAM_TYPE_U8, "tc_type" },
    { 0x0106, KILNLINK_PARAM_TYPE_U8, "ct_channel_map[0]" },
    { 0x0107, KILNLINK_PARAM_TYPE_U8, "ct_channel_map[1]" },
    { 0x0108, KILNLINK_PARAM_TYPE_U8, "ct_channel_map[2]" },
    /* sec 2 -- temperature guards */
    { 0x0201, KILNLINK_PARAM_TYPE_F32, "firing_margin_c" },
    { 0x0202, KILNLINK_PARAM_TYPE_F32, "overshoot_margin_c" },
    { 0x0203, KILNLINK_PARAM_TYPE_U16, "overshoot_time_s" },
    { 0x0204, KILNLINK_PARAM_TYPE_F32, "max_rate_c_per_min" },
    { 0x0205, KILNLINK_PARAM_TYPE_U16, "rate_window_s" },
    { 0x0206, KILNLINK_PARAM_TYPE_U16, "blind_grace_s" },
    { 0x0207, KILNLINK_PARAM_TYPE_U16, "frozen_window_s" },
    { 0x0208, KILNLINK_PARAM_TYPE_F32, "tc_disagreement_c" },
    { 0x0209, KILNLINK_PARAM_TYPE_U16, "tc_disagreement_time_s" },
    { 0x020A, KILNLINK_PARAM_TYPE_F32, "tc_expected_offset_c" },
    { 0x020B, KILNLINK_PARAM_TYPE_F32, "cj_warn_c" },
    { 0x020C, KILNLINK_PARAM_TYPE_F32, "cj_max_c" },
    { 0x020D, KILNLINK_PARAM_TYPE_U16, "cj_time_s" },
    { 0x020E, KILNLINK_PARAM_TYPE_U16, "borrowed_stale_s" },
    { 0x020F, KILNLINK_PARAM_TYPE_U16, "borrowed_stale_trip_s" },
    { 0x0210, KILNLINK_PARAM_TYPE_U8, "borrowed_type_expected" },
    /* sec 3 -- current channels */
    { 0x0301, KILNLINK_PARAM_TYPE_F32, "i_present_a" },
    { 0x0302, KILNLINK_PARAM_TYPE_U16, "zero_counts[0]" },
    { 0x0303, KILNLINK_PARAM_TYPE_U16, "zero_counts[1]" },
    { 0x0304, KILNLINK_PARAM_TYPE_U16, "zero_counts[2]" },
    { 0x0305, KILNLINK_PARAM_TYPE_U16, "correlation_window_s" },
    { 0x0306, KILNLINK_PARAM_TYPE_U16, "stuck_on_time_s" },
    { 0x0307, KILNLINK_PARAM_TYPE_U16, "trip_verify_s" },
    { 0x0308, KILNLINK_PARAM_TYPE_F32, "k_ct_v_per_a[0]" },
    { 0x0309, KILNLINK_PARAM_TYPE_F32, "k_ct_v_per_a[1]" },
    { 0x030A, KILNLINK_PARAM_TYPE_F32, "k_ct_v_per_a[2]" },
    { 0x030B, KILNLINK_PARAM_TYPE_F32, "gain[0]" },
    { 0x030C, KILNLINK_PARAM_TYPE_F32, "gain[1]" },
    { 0x030D, KILNLINK_PARAM_TYPE_F32, "gain[2]" },
    { 0x030E, KILNLINK_PARAM_TYPE_F32, "mains_voltage_v" },
    { 0x030F, KILNLINK_PARAM_TYPE_U16, "power_window_s" },
    { 0x0310, KILNLINK_PARAM_TYPE_F32, "ct_cal[0].gain" },
    { 0x0311, KILNLINK_PARAM_TYPE_F32, "ct_cal[1].gain" },
    { 0x0312, KILNLINK_PARAM_TYPE_F32, "ct_cal[2].gain" },
    { 0x0313, KILNLINK_PARAM_TYPE_F32, "ct_cal[0].offset" },
    { 0x0314, KILNLINK_PARAM_TYPE_F32, "ct_cal[1].offset" },
    { 0x0315, KILNLINK_PARAM_TYPE_F32, "ct_cal[2].offset" },
    { 0x0316, KILNLINK_PARAM_TYPE_BOOL, "ct_cal[0].calibrated" },
    { 0x0317, KILNLINK_PARAM_TYPE_BOOL, "ct_cal[1].calibrated" },
    { 0x0318, KILNLINK_PARAM_TYPE_BOOL, "ct_cal[2].calibrated" },
    /* sec 4 -- link and liveness */
    { 0x0401, KILNLINK_PARAM_TYPE_U16, "context_max_age_s" },
    { 0x0402, KILNLINK_PARAM_TYPE_U16, "link_timeout_s" },
    { 0x0403, KILNLINK_PARAM_TYPE_U16, "link_dead_hard_s" },
    { 0x0404, KILNLINK_PARAM_TYPE_U16, "mainfault_debounce_ms" },
    { 0x0405, KILNLINK_PARAM_TYPE_U16, "telemetry_period_ms" },
    /* sec 5 -- timing and system */
    { 0x0501, KILNLINK_PARAM_TYPE_U16, "startup_grace_s" },
    { 0x0502, KILNLINK_PARAM_TYPE_U16, "estop_debounce_ms" },
    { 0x0503, KILNLINK_PARAM_TYPE_U16, "watchdog_timeout_ms" },
    { 0x0504, KILNLINK_PARAM_TYPE_U16, "config_check_period_s" },
};

typedef struct {
    uint8_t set;
    kilnlink_param_value_t value;
} safety_cfg_entry_t;

typedef struct {
    uint8_t version;
    uint16_t config_crc; /* 0 = never fetched */
    safety_cfg_entry_t entries[SAFETY_CFG_PARAM_COUNT];
} safety_cfg_store_blob_t;

static safety_cfg_store_blob_t s_store;
/* Set at safety_cfg_store_init() (a fresh NVS load counts as "just fetched",
 * same "the cache is only ever as stale as it honestly reports" discipline
 * this header documents) and again on every successful
 * safety_cfg_store_refetch() -- safety_cfg_store_fetched_ms_ago() measures
 * against this, never against anything persisted (a wall-clock timestamp
 * would need a synced RTC this board does not have; esp_timer_get_time()'s
 * monotonic microsecond counter needs none). */
static int64_t s_fetched_at_us = -1;

static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

static void reset_to_defaults(void)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.version = SAFETY_CFG_STORE_VERSION;
    s_store.config_crc = 0; /* never fetched */
}

/* Three-outcome load, same discipline zones_http.c's nvs_load_from() and
 * kiln_cfg_store.c's nvs_load_store() document -- current/migrate/refuse.
 * SAFETY_CFG_STORE_VERSION==1 is the only version this build has ever
 * written, so there is no migration chain yet (mirrors kiln_cfg_store.c's own
 * "only version 1 exists today" state); a future bump needs one here, same as
 * those two files. */
static void nvs_load_store(void)
{
    reset_to_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return; /* ESP_ERR_NVS_NOT_FOUND (never saved) or partition trouble -- defaults stand */
    }

    safety_cfg_store_blob_t loaded;
    size_t len = sizeof(loaded);
    err = nvs_get_blob(h, NVS_KEY_SAFETY_CFG, &loaded, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return; /* nothing stored, or unreadable -- defaults stand */
    }
    if (len < sizeof(loaded.version)) {
        ESP_LOGW(TAG, "safety_cfg_store blob is too short to contain a version -- treating as unreadable");
        return;
    }
    if (loaded.version == SAFETY_CFG_STORE_VERSION) {
        if (len != sizeof(loaded)) {
            ESP_LOGW(TAG, "safety_cfg_store blob claims current version but is the wrong size -- "
                          "treating as unreadable");
            return;
        }
        s_store = loaded;
        return;
    }
    if (loaded.version < SAFETY_CFG_STORE_VERSION) {
        /* No migration chain exists yet -- unreachable until a future bump
         * adds one (see this function's header comment). Fail safe rather
         * than guess at a layout this build has never written: reset stands,
         * same choice kiln_cfg_store.c's nvs_load_store() makes for "wrong
         * size or an unknown version". */
        ESP_LOGW(TAG, "safety_cfg_store blob is version %u with no migration path to %u -- "
                      "resetting to an empty cache",
                 (unsigned)loaded.version, (unsigned)SAFETY_CFG_STORE_VERSION);
        return;
    }
    /* loaded.version > SAFETY_CFG_STORE_VERSION: firmware-rollback case, same
     * as zones_http.c/kiln_cfg_store.c -- refuse to load, flash left
     * untouched, this boot runs with an empty cache. Every param reads
     * set=false until the next successful refetch, which is a strictly safer
     * failure than reinterpreting a newer layout's byte offsets as this
     * version's fields (COMMISSIONING.md sec 1's identical reasoning for the
     * Pico's own config_store). */
    ESP_LOGW(TAG, "safety_cfg_store blob is version %u, newer than this firmware's %u -- "
                  "refusing to load, flash data left untouched",
             (unsigned)loaded.version, (unsigned)SAFETY_CFG_STORE_VERSION);
}

static esp_err_t nvs_save_store(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    s_store.version = SAFETY_CFG_STORE_VERSION;
    err = nvs_set_blob(h, NVS_KEY_SAFETY_CFG, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static int index_for_id(uint16_t id)
{
    for (size_t i = 0; i < SAFETY_CFG_PARAM_COUNT; i++) {
        if (SAFETY_CFG_PARAM_TABLE[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t safety_cfg_store_init(void)
{
    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- safety commissioning cache will not persist",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        reset_to_defaults();
        return part_err;
    }
    nvs_load_store();
    /* A load from NVS counts as "fetched" ONLY if it actually restored a
     * fetched record. config_crc == 0 is this blob's own documented "never
     * fetched" marker (see safety_cfg_store_blob_t), which is what a first
     * boot on new firmware, an erased partition, or a refused-version blob
     * all leave behind -- and stamping the clock in those cases made
     * safety_cfg_store_fetched_ms_ago() report a freshness for values that
     * were never fetched from anywhere. Observed live on the bench
     * 2026-08-22: a board that had never held a cache reported
     * "fetched 15 s ago" next to a parameter list that was entirely unset.
     *
     * That is the same class of mistake this whole subsystem is built to
     * avoid -- an honest "unknown" replaced by a plausible-looking number --
     * and it also made the UINT32_MAX "never" case unreachable in practice,
     * so the API's documented `"fetched_ms_ago": null` could never appear. */
    if (s_store.config_crc != 0) {
        s_fetched_at_us = esp_timer_get_time();
    }
    return ESP_OK;
}

uint16_t safety_cfg_store_cached_crc(void)
{
    return s_store.config_crc;
}

uint32_t safety_cfg_store_fetched_ms_ago(void)
{
    if (s_fetched_at_us < 0) {
        return UINT32_MAX;
    }
    int64_t elapsed_us = esp_timer_get_time() - s_fetched_at_us;
    if (elapsed_us < 0) {
        elapsed_us = 0; /* clock went backwards somehow -- never report a negative age */
    }
    int64_t elapsed_ms = elapsed_us / 1000;
    if (elapsed_ms > (int64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)elapsed_ms;
}

size_t safety_cfg_store_param_count(void)
{
    return SAFETY_CFG_PARAM_COUNT;
}

bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    if (!out || index >= SAFETY_CFG_PARAM_COUNT) {
        return false;
    }
    const safety_cfg_table_row_t *row = &SAFETY_CFG_PARAM_TABLE[index];
    const safety_cfg_entry_t *entry = &s_store.entries[index];
    out->param_id = row->id;
    out->name = row->name;
    out->type = row->type;
    out->set = entry->set != 0;
    out->value = entry->value; /* meaningless when !out->set -- caller's job to check first */
    return true;
}

bool safety_cfg_store_lookup(uint16_t param_id, uint8_t *out_type, const char **out_name)
{
    int idx = index_for_id(param_id);
    if (idx < 0) {
        return false;
    }
    if (out_type) {
        *out_type = SAFETY_CFG_PARAM_TABLE[idx].type;
    }
    if (out_name) {
        *out_name = SAFETY_CFG_PARAM_TABLE[idx].name;
    }
    return true;
}

bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc)
{
    if (!link) {
        return false;
    }

    /* Staged into a scratch copy first -- an interrupted refetch (a page
     * request times out or fails to decode partway through) must leave the
     * PREVIOUS cache exactly as it was, never a mix of old and new pages with
     * no way to tell which is which. Nothing touches s_store until every
     * page has been read successfully. */
    safety_cfg_store_blob_t scratch;
    memset(&scratch, 0, sizeof(scratch));
    scratch.version = SAFETY_CFG_STORE_VERSION;
    scratch.config_crc = config_crc;

    uint8_t page_index = 0;
    for (;;) {
        kilnlink_config_page_t page;
        esp_err_t err = safety_link_get_config_page(link, page_index, &page);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "safety_cfg_store_refetch: page %u failed (%s) -- cache left unchanged",
                     (unsigned)page_index, esp_err_to_name(err));
            return false;
        }
        for (uint8_t i = 0; i < page.entry_count; i++) {
            const kilnlink_config_page_entry_t *e = &page.entries[i];
            int idx = index_for_id(e->param_id);
            if (idx < 0) {
                /* COMMISSIONING.md sec 2: version-tolerant -- an id this
                 * build's table predates (a newer Pico) is simply not
                 * something this cache can show; skipped, not an error. */
                continue;
            }
            scratch.entries[idx].set = 1;
            scratch.entries[idx].value = e->value;
        }
        if (!page.more) {
            break;
        }
        page_index++;
        if (page_index == 0) {
            /* uint8_t wrapped -- 256 pages is not a real page count for a
             * 57-entry table (even at the smallest 4-byte entries that is
             * ~14 pages worst case); a Pico claiming more forever is a
             * protocol fault, not something to loop on forever. */
            ESP_LOGE(TAG, "safety_cfg_store_refetch: page_index wrapped without more==0 -- aborting");
            return false;
        }
    }

    s_store = scratch;
    s_fetched_at_us = esp_timer_get_time();
    esp_err_t save_err = nvs_save_store();
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "safety_cfg_store_refetch: fetched OK but nvs_save_store failed (%s) -- live "
                      "but will not survive a reboot",
                 esp_err_to_name(save_err));
    }
    ESP_LOGI(TAG, "safety_cfg_store_refetch: refreshed cache, config_crc=0x%04X", (unsigned)config_crc);
    return true;
}

bool safety_cfg_store_maybe_refetch(SafetyLinkClass *link, uint16_t live_config_crc)
{
    if (s_store.config_crc == live_config_crc) {
        return false; /* steady state -- no UART traffic at all, by design */
    }
    return safety_cfg_store_refetch(link, live_config_crc);
}
