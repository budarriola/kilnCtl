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
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "profiles_builtin";

/* Same partition and namespace profiles_http.c uses for the user slots --
 * this data belongs to the same feature and has the same lifetime, so it
 * migrates, factory-resets and rolls back with it rather than acquiring a
 * second place to look. Key name deliberately does not collide with
 * profiles_http.c's "prof_used" / "prof0".."prof7". */
#define PROFILES_NVS_PARTITION "profiles_nvs"
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_HIDDEN "prof_bihid"

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

static esp_err_t hidden_mask_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, NVS_KEY_HIDDEN, s_hidden_mask);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t profiles_builtin_start(void)
{
    s_hidden_mask = 0;

    esp_err_t part_err = nvs_partition_init(PROFILES_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- hidden-schedule choices will not persist",
                 PROFILES_NVS_PARTITION, esp_err_to_name(part_err));
        return part_err;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace has never been written on this partition. Same reading
         * profiles_http.c's nvs_load_all_from() gives it: not an error,
         * just "nothing configured yet". */
        ESP_LOGI(TAG, "%u builtin schedules, none hidden (no saved choices yet)",
                 (unsigned)g_builtin_profile_count);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint32_t mask = 0;
    err = nvs_get_u32(h, NVS_KEY_HIDDEN, &mask);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Missing key is explicitly not an error -- it means nothing has ever
         * been hidden, which is the shipped default. */
        err = ESP_OK;
    } else if (err == ESP_OK) {
        s_hidden_mask = mask;
    } else {
        ESP_LOGW(TAG, "hidden-mask read failed: %s -- starting with nothing hidden", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "%u builtin schedules, hidden mask 0x%08lx", (unsigned)g_builtin_profile_count,
             (unsigned long)s_hidden_mask);
    return ESP_OK;
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
    esp_err_t err = hidden_mask_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hidden-mask save failed: %s -- applied live but will revert on reboot",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t profiles_builtin_restore_all(void)
{
    if (s_hidden_mask == 0) {
        return ESP_OK;
    }
    s_hidden_mask = 0;
    esp_err_t err = hidden_mask_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hidden-mask save failed: %s -- restored live but will revert on reboot",
                 esp_err_to_name(err));
    }
    return err;
}
