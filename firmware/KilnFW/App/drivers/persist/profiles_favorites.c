// profiles_favorites -- persisted "favorite" marks on firing profiles.
// See profiles_favorites.h for the design rationale (why a favorite is a
// shortcut and not a move, why two masks rather than one, and what happens
// to a favorite when the profile it points at is deleted or overwritten).
#include "profiles_favorites.h"

#include <string.h>

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "profiles_builtin.h"
#include "profiles_types.h"

static const char *TAG = "profiles_fav";

/* Same partition and namespace profiles_http.c uses for the user slots and
 * profiles_builtin.c uses for the hidden mask -- see the header. Both key
 * names are checked against NVS's 15-character limit below; "prof_favusr" is
 * 11 characters and "prof_favbi" is 10. */
#define PROFILES_NVS_PARTITION "profiles_nvs"
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_FAV_USER "prof_favusr"
#define NVS_KEY_FAV_BUILTIN "prof_favbi"
NVS_KEY_LEN_CHECK(PROFILES_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_FAV_USER);
NVS_KEY_LEN_CHECK(NVS_KEY_FAV_BUILTIN);

/* Bit i = user slot i is favorited. Only PROFILES_MAX_COUNT bits are ever
 * set, but a u32 is what hal_kv stores natively. */
static uint32_t s_fav_user;
/* Bit i = builtin catalogue index i is favorited. 32 bits for the 28 shipped
 * entries, the same shape (and for the same reason) as profiles_builtin.c's
 * hidden mask. */
static uint32_t s_fav_builtin;

_Static_assert(PROFILES_MAX_COUNT <= 32, "user favorite mask is a uint32_t");

/* Resolves an id in either namespace to the mask that holds it and the bit
 * within that mask. Returns false for an id that names no profile at all. */
static bool fav_locate(uint8_t id, uint32_t **out_mask, uint32_t *out_bit)
{
    if (id < PROFILES_MAX_COUNT) {
        if (out_mask) {
            *out_mask = &s_fav_user;
        }
        if (out_bit) {
            *out_bit = 1u << id;
        }
        return true;
    }
    if (profiles_builtin_id_valid(id)) {
        if (out_mask) {
            *out_mask = &s_fav_builtin;
        }
        if (out_bit) {
            *out_bit = 1u << (uint32_t)(id - PROFILE_BUILTIN_ID_BASE);
        }
        return true;
    }
    return false;
}

static hal_status_t favorites_save(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, PROFILES_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_u32(&h, NVS_KEY_FAV_USER, s_fav_user);
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_FAV_BUILTIN, s_fav_builtin);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

/* Reads one mask key. A missing key is explicitly not an error -- it means
 * that namespace has nothing favorited, which is the shipped default. */
static hal_status_t favorites_load_key(hal_kv_handle_t *h, const char *key, uint32_t *out)
{
    uint32_t v = 0;
    hal_status_t err = hal_kv_get_u32(h, key, &v);
    if (err == HAL_NOT_FOUND) {
        return HAL_OK;
    }
    if (err == HAL_OK) {
        *out = v;
    }
    return err;
}

esp_err_t profiles_favorites_start(void)
{
    s_fav_user = 0;
    s_fav_builtin = 0;

    /* Calling this when profiles_http.c or profiles_builtin.c has already
     * initialized the same partition is a harmless no-op returning HAL_OK,
     * so this module does not have to assume either of them ran first. */
    hal_status_t part_err = hal_kv_init_partition(PROFILES_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- favorites will not persist", PROFILES_NVS_PARTITION,
                 hal_status_to_name(part_err));
        return hal_status_to_esp_err(part_err);
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, PROFILES_NVS_PARTITION);
    if (err == HAL_NOT_FOUND) {
        /* Namespace has never been written on this partition -- not an
         * error, just "nothing configured yet". */
        ESP_LOGI(TAG, "no favorites saved yet");
        return ESP_OK;
    }
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }

    err = favorites_load_key(&h, NVS_KEY_FAV_USER, &s_fav_user);
    if (err == HAL_OK) {
        err = favorites_load_key(&h, NVS_KEY_FAV_BUILTIN, &s_fav_builtin);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "favorites read failed: %s -- starting with none", hal_status_to_name(err));
        s_fav_user = 0;
        s_fav_builtin = 0;
        return hal_status_to_esp_err(err);
    }

    ESP_LOGI(TAG, "favorites: saved mask 0x%02lx, builtin mask 0x%08lx", (unsigned long)s_fav_user,
             (unsigned long)s_fav_builtin);
    return ESP_OK;
}

bool profiles_favorites_is(uint8_t id)
{
    uint32_t *mask = NULL;
    uint32_t bit = 0;
    if (!fav_locate(id, &mask, &bit)) {
        return false;
    }
    return (*mask & bit) != 0;
}

esp_err_t profiles_favorites_set(uint8_t id, bool favorite)
{
    uint32_t *mask = NULL;
    uint32_t bit = 0;
    if (!fav_locate(id, &mask, &bit)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t updated = favorite ? (*mask | bit) : (*mask & ~bit);
    if (updated == *mask) {
        /* No change -- nothing to write. Saying OK here keeps an unfavorite
         * of something that was never favorited from reporting a failure. */
        return ESP_OK;
    }
    *mask = updated;

    hal_status_t err = favorites_save();
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "favorite save failed: %s -- applied live but will revert on reboot",
                 hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    return ESP_OK;
}

void profiles_favorites_masks(uint32_t *out_user, uint32_t *out_builtin)
{
    if (out_user) {
        *out_user = s_fav_user;
    }
    if (out_builtin) {
        *out_builtin = s_fav_builtin;
    }
}
