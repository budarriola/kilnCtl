// profiles_builtin -- the shipped-in-flash Digital Fire firing-schedule
// catalogue. See profiles_builtin.h for the design rationale (why these are
// not user profiles, and why "remove" is a hide rather than a delete).
//
// FILE SPLIT, deliberate: the 28-entry / 136-segment table itself is machine
// generated from scraped source data and lives in profiles_builtin_table.inc,
// which tools/scripts/gen_builtin_profiles.py overwrites wholesale. THIS file
// is hand-written and is never touched by the generator, so regenerating the
// catalogue cannot delete the API implementation below. (The generator refuses
// outright to write anything whose name does not end in .inc, so pointing it
// at this file by mistake fails loudly instead of silently.)
#include "profiles_builtin.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "cfg_fs_status.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"

static const char *TAG = "profiles_builtin";

/* Same partition and namespace profiles_http.c uses for the user slots --
 * this data belongs to the same feature and has the same lifetime, so it
 * migrates, factory-resets and rolls back with it rather than acquiring a
 * second place to look. Key name deliberately does not collide with
 * profiles_http.c's "prof_used" / "prof0".."prof7". */
#define PROFILES_NVS_PARTITION "profiles_nvs"
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_HIDDEN "prof_bihid"
/* Rev counter for the cfg-filesystem dual-write below -- a SEPARATE key, same
 * pattern unit_pref.c's "u_pref_rev" uses (14 chars, under the 15-char cap). */
#define NVS_KEY_HIDDEN_REV "prof_bihid_rev"
NVS_KEY_LEN_CHECK(PROFILES_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_HIDDEN);
NVS_KEY_LEN_CHECK(NVS_KEY_HIDDEN_REV);

/* docs/FILESYSTEM_USER_DATA_PLAN.md section 3 item 6: the hidden-builtin mask
 * lives at /cfg/profiles/hidden.json, next to the profiles/prof<id>.json slot
 * files (profiles_cfg_fs.c). Same deviation those files carry: the ".json"
 * name is the plan's path, the content is pref_cfg_fs.h's rev-prefixed binary
 * item (4-byte LE rev + the 4-byte LE mask). NVS stays authoritative; see
 * pref_cfg_fs.h for the read-through/dual-write/tie-break policy. */
#define PROFILES_HIDDEN_FILE_PATH "profiles/hidden.json"

/* ---- The generated catalogue table --------------------------------------- */

#include "profiles_builtin_table.inc"

/* ---- Hand-written API implementation (NOT generated) ---------------------- *
 *
 * Everything below this line is maintained by hand. Do not move it into the
 * .inc above -- see this file's header comment.
 */

/* Bit i = catalogue entry i is hidden. 32 bits for 28 entries; see the header
 * for why this is not the 8-bit shape the user slots use. */
static uint32_t s_hidden_mask;
static uint32_t s_hidden_rev;

/* Any 32-bit mask is accepted -- the NVS path never range-checked it either
 * (bits past g_builtin_profile_count are inert), so the file is validated
 * exactly as NVS is: by size alone. */
static bool hidden_mask_validate(const void *bytes, size_t len)
{
    (void)bytes;
    return len == sizeof(uint32_t);
}

static bool builtin_index(uint8_t id, size_t *out_index)
{
    if (id < PROFILE_BUILTIN_ID_BASE) {
        return false;
    }
    size_t idx = (size_t)(id - PROFILE_BUILTIN_ID_BASE);
    if (idx >= g_builtin_profile_count) {
        return false;
    }
    if (out_index) {
        *out_index = idx;
    }
    return true;
}

/* Brings up the profiles partition, erasing ONLY that partition if unusable.
 * Same shape (and same rationale) as profiles_http.c's nvs_partition_init():
 * NO_FREE_PAGES / NEW_VERSION_FOUND have no other cure, and the erase must
 * stay scoped to the broken partition. Calling this when profiles_http.c has
 * already initialized the same partition is a harmless no-op returning
 * ESP_OK, so neither module has to assume the other ran first. */
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

/* Persists the mask at the next rev: file first (best-effort, logged and
 * swallowed -- also a no-op when cfg_fs is unmounted), then NVS (authoritative,
 * its status is what the caller gets). s_hidden_rev advances only once NVS
 * accepted the write, same as unit_pref_set(). */
static hal_status_t hidden_mask_save(void)
{
    uint32_t new_rev = s_hidden_rev + 1;
    uint32_t mask = s_hidden_mask;
    esp_err_t file_err = pref_cfg_fs_save(PROFILES_HIDDEN_FILE_PATH, &mask, sizeof(mask), new_rev);
    if (file_err != ESP_OK && file_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "hidden-mask file write failed: %s -- NVS remains the source of truth this boot",
                 esp_err_to_name(file_err));
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, PROFILES_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_u32(&h, NVS_KEY_HIDDEN, s_hidden_mask);
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_HIDDEN_REV, new_rev);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err == HAL_OK) {
        s_hidden_rev = new_rev;
    }
    return err;
}

esp_err_t profiles_builtin_start(void)
{
    s_hidden_mask = 0;
    s_hidden_rev = 0;

    hal_status_t part_err = nvs_partition_init(PROFILES_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- hidden-schedule choices will not persist",
                 PROFILES_NVS_PARTITION, hal_status_to_name(part_err));
        return hal_status_to_esp_err(part_err);
    }

    uint32_t nvs_mask = 0;
    uint32_t nvs_rev = 0;
    bool nvs_valid = false;
    hal_status_t load_err = HAL_OK; /* a real NVS open/read error, not merely "never written" */
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, PROFILES_NVS_PARTITION);
    if (err == HAL_NOT_FOUND) {
        /* Namespace has never been written on this partition. Same reading
         * profiles_http.c's nvs_load_all_from() gives it: not an error,
         * just "nothing configured yet". */
    } else if (err != HAL_OK) {
        load_err = err;
    } else {
        err = hal_kv_get_u32(&h, NVS_KEY_HIDDEN, &nvs_mask);
        if (err == HAL_OK) {
            nvs_valid = true;
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_HIDDEN_REV, &rev) == HAL_OK) {
                nvs_rev = rev;
            }
        } else {
            nvs_mask = 0;
            if (err != HAL_NOT_FOUND) {
                /* Missing key is explicitly not an error -- it means nothing
                 * has ever been hidden, the shipped default. */
                load_err = err;
            }
        }
        hal_kv_close(&h);
    }
    if (load_err != HAL_OK) {
        ESP_LOGW(TAG, "hidden-mask read failed: %s -- starting with nothing hidden", hal_status_to_name(load_err));
    }

    /* Hand the NVS candidate to the file-vs-NVS read-through/tie-break policy
     * (pref_cfg_fs.h). With cfg_fs unmounted this returns the NVS candidate
     * unchanged. */
    uint32_t resolved = nvs_mask;
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(PROFILES_HIDDEN_FILE_PATH, &nvs_mask, sizeof(nvs_mask), nvs_valid, nvs_rev,
                                           hidden_mask_validate, &resolved, &resolved_rev, &used_file);
    if (have_value) {
        s_hidden_mask = resolved;
        s_hidden_rev = resolved_rev;
    }

    ESP_LOGI(TAG, "%u builtin schedules, hidden mask 0x%08lx (source=%s, rev=%lu)",
             (unsigned)g_builtin_profile_count, (unsigned long)s_hidden_mask,
             have_value ? (used_file ? "file" : "NVS") : "default", (unsigned long)s_hidden_rev);
    return hal_status_to_esp_err(load_err);
}

void profiles_builtin_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                            bool *diverged)
{
    uint32_t f_mask = 0;
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw(PROFILES_HIDDEN_FILE_PATH, sizeof(f_mask), hidden_mask_validate, &f_mask, &f_rev, &f_valid);

    bool n_valid = false;
    uint32_t n_mask = 0;
    uint32_t n_rev = 0;
    if (nvs_partition_init(PROFILES_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, PROFILES_NVS_PARTITION) == HAL_OK) {
            if (hal_kv_get_u32(&h, NVS_KEY_HIDDEN, &n_mask) == HAL_OK) {
                n_valid = true;
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_HIDDEN_REV, &rev) == HAL_OK) {
                    n_rev = rev;
                }
            }
            hal_kv_close(&h);
        }
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
        *nvs_rev = n_rev;
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, f_valid && n_valid && f_mask == n_mask);
    }
}

bool profiles_builtin_id_valid(uint8_t id)
{
    return builtin_index(id, NULL);
}

const builtin_profile_t *profiles_builtin_entry(uint8_t id)
{
    size_t idx;
    if (!builtin_index(id, &idx)) {
        return NULL;
    }
    return &g_builtin_profiles[idx];
}

const char *profiles_builtin_firing_type_label(profile_firing_type_t type)
{
    switch (type) {
    case PROFILE_FIRING_BISQUE: return "Bisque";
    case PROFILE_FIRING_GLAZE:  return "Glaze";
    case PROFILE_FIRING_OTHER:  return "Other";
    default:                    return "?";
    }
}

void profiles_builtin_cone_label(int8_t cone, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    if (cone == PROFILES_BUILTIN_CONE_UNRATED) {
        snprintf(buf, buf_len, "Unrated");
        return;
    }
    /* Negative encoding stores the "0N" cones (see profiles_builtin.h) --
     * print the magnitude with the leading zero restored, not the sign. */
    if (cone < 0) {
        snprintf(buf, buf_len, "0%d", -cone);
    } else {
        snprintf(buf, buf_len, "%d", cone);
    }
}

bool profiles_builtin_get(uint8_t id, profile_t *out)
{
    size_t idx;
    if (!out || !builtin_index(id, &idx)) {
        return false;
    }
    const builtin_profile_t *b = &g_builtin_profiles[idx];

    memset(out, 0, sizeof(*out));
    strncpy(out->name, b->code, PROFILE_NAME_MAX_LEN);
    out->name[PROFILE_NAME_MAX_LEN] = '\0';
    out->zone_mask = 0; /* zone-agnostic catalogue; the caller supplies zones */
    out->segment_count = b->segment_count;
    for (uint8_t i = 0; i < b->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        out->segments[i] = b->segments[i];
    }
    return true;
}

bool profiles_builtin_is_hidden(uint8_t id)
{
    size_t idx;
    if (!builtin_index(id, &idx)) {
        return false;
    }
    return (s_hidden_mask & (1u << idx)) != 0u;
}

esp_err_t profiles_builtin_set_hidden(uint8_t id, bool hidden)
{
    size_t idx;
    if (!builtin_index(id, &idx)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t updated = hidden ? (s_hidden_mask | (1u << idx)) : (s_hidden_mask & ~(1u << idx));
    if (updated == s_hidden_mask) {
        return ESP_OK; /* already in the requested state -- no flash write */
    }
    s_hidden_mask = updated;
    hal_status_t err = hidden_mask_save();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "hidden-mask save failed: %s -- applied live but will revert on reboot",
                 hal_status_to_name(err));
    }
    return hal_status_to_esp_err(err);
}

esp_err_t profiles_builtin_restore_all(void)
{
    if (s_hidden_mask == 0) {
        return ESP_OK;
    }
    s_hidden_mask = 0;
    hal_status_t err = hidden_mask_save();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "hidden-mask save failed: %s -- restored live but will revert on reboot",
                 hal_status_to_name(err));
    }
    return hal_status_to_esp_err(err);
}
