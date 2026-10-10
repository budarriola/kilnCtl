// profiles_favorites -- persisted "favorite" marks on firing profiles.
// See profiles_favorites.h for the design rationale (why a favorite is a
// shortcut and not a move, why two masks rather than one, and what happens
// to a favorite when the profile it points at is deleted or overwritten).
#include "profiles_favorites.h"
#include "cfgfs_file_validators.h"

#include <string.h>

#include "esp_log.h"
#include "hal_esp_common.h"
#include "cfg_fs_status.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"
#include "cfg_save_lock.h"
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

/* Bit i = user slot i is favorited. Widened uint32_t -> profiles_slot_bitmap_t
 * (docs/PROFILE_SLOTS_100.md section 7 task 1) so an id up to the
 * 128-id ceiling can be addressed once PROFILES_MAX_COUNT is later raised --
 * the plan's Status section names the old `user_mask & (1u << i)` scalar
 * test in favorites_list_get_handler() as undefined behavior once `i`
 * reaches 32. Still at 8 slots today, so only word[0] bits 0..7 are ever
 * set; persisted exactly as before (favorites_save()/favorites_load_key()
 * below read/write only that one word via hal_kv_get/set_u32, so the NVS
 * bytes stay byte-identical to before this change). */
static profiles_slot_bitmap_t s_fav_user;
/* Bit i = builtin catalogue index i is favorited. 32 bits for the 28 shipped
 * entries, the same shape (and for the same reason) as profiles_builtin.c's
 * hidden mask -- not affected by the user-slot count, so this one stays a
 * plain uint32_t. */
static uint32_t s_fav_builtin;
/* rev of the cfg file as last verified on flash (0 = never written). The NVS
 * copy carries no rev of its own, so it always competes at rev 0 and any
 * file this build wrote (rev >= 1) beats it. */
static uint32_t s_fav_rev;

/* The cfg file's item: both masks in one 20-byte record, so a save is one
 * atomic file write (profiles_favorites.h). */
typedef struct {
    profiles_slot_bitmap_t user;
    uint32_t builtin;
} fav_item_t;
_Static_assert(sizeof(fav_item_t) <= PREF_CFG_FS_MAX_ITEM, "favorites item must fit pref_cfg_fs");

/* Any mask is acceptable content (an empty mask is the shipped default), so
 * validation is the exact-size check pref_cfg_fs already does before calling
 * this; it exists to satisfy the helper's contract. */
bool profiles_favorites_file_validate(const void *bytes, size_t len)
{
    return bytes != NULL && len == sizeof(fav_item_t);
}

/* Resolves an id in either namespace. Returns false for an id that names no
 * profile at all. `*out_is_user` tells the caller which namespace the id
 * landed in; `*out_builtin_bit` is only meaningful when it comes back
 * false. The two masks are different types now (profiles_slot_bitmap_t vs a
 * plain uint32_t), so they can no longer share one out-pointer the way a
 * single scalar mask pointer used to. */
static bool fav_locate(uint8_t id, bool *out_is_user, uint32_t *out_builtin_bit)
{
    if (id < PROFILES_MAX_COUNT) {
        if (out_is_user) {
            *out_is_user = true;
        }
        return true;
    }
    if (profiles_builtin_id_valid(id)) {
        if (out_is_user) {
            *out_is_user = false;
        }
        if (out_builtin_bit) {
            *out_builtin_bit = 1u << (uint32_t)(id - PROFILE_BUILTIN_ID_BASE);
        }
        return true;
    }
    return false;
}

/* Reads NVS_KEY_FAV_USER, accepting EITHER the new 16-byte
 * profiles_slot_bitmap_t blob OR a pre-task-6 board's old single uint32_t
 * (word[0] only). Same two-branch dispatch as profiles_http.c's
 * used_bitmap_load() and for the same reason: the real ESP-IDF NVS backend
 * enforces on-flash key type (a blob read against a U32-typed key fails
 * with HAL_INVALID_ARG), while the host fake backend answers a size
 * mismatch instead. A missing key entirely is explicitly not an error -- it
 * means nothing has ever been favorited, the shipped default. */
static hal_status_t favorites_load_user_mask(hal_kv_handle_t *h, profiles_slot_bitmap_t *out)
{
    size_t len = 0;
    hal_status_t err = hal_kv_get_blob(h, NVS_KEY_FAV_USER, NULL, &len);
    if (err == HAL_OK && len == sizeof(*out)) {
        size_t full_len = sizeof(*out);
        return hal_kv_get_blob(h, NVS_KEY_FAV_USER, out, &full_len);
    }
    /* NOT_FOUND is NOT "absent" yet: on target (and the now-typed host fake) a blob read of a key stored
     * as U32 ends in NOT_FOUND, so the legacy uint32 form must be tried before declaring the key missing. */
    uint32_t legacy = 0;
    hal_status_t legacy_err = hal_kv_get_u32(h, NVS_KEY_FAV_USER, &legacy);
    if (legacy_err == HAL_NOT_FOUND) {
        return HAL_OK;
    }
    if (legacy_err != HAL_OK) {
        return legacy_err;
    }
    profiles_slot_bitmap_from_u32(out, legacy);
    return HAL_OK;
}

/* Reads one plain uint32_t mask key (the builtin mask, unaffected by the
 * user-slot count). A missing key is explicitly not an error -- it means
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

/* Reads the legacy NVS copy. *found is true only when at least one of the two
 * keys exists (a never-written namespace is "nothing favorited", not a
 * candidate to migrate). */
static hal_status_t favorites_read_nvs(profiles_slot_bitmap_t *user, uint32_t *builtin, bool *found)
{
    *found = false;
    profiles_slot_bitmap_from_u32(user, 0);
    *builtin = 0;
    hal_status_t perr = hal_kv_init_partition(PROFILES_NVS_PARTITION);
    if (perr != HAL_OK) {
        return perr;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, PROFILES_NVS_PARTITION);
    if (err == HAL_NOT_FOUND) {
        return HAL_OK;
    }
    if (err != HAL_OK) {
        return err;
    }
    size_t probe = 0;
    bool have_user = hal_kv_get_blob(&h, NVS_KEY_FAV_USER, NULL, &probe) == HAL_OK;
    uint32_t legacy_probe = 0;
    if (!have_user && hal_kv_get_u32(&h, NVS_KEY_FAV_USER, &legacy_probe) == HAL_OK) {
        have_user = true;
    }
    uint32_t b_probe = 0;
    bool have_builtin = hal_kv_get_u32(&h, NVS_KEY_FAV_BUILTIN, &b_probe) == HAL_OK;
    err = favorites_load_user_mask(&h, user);
    if (err == HAL_OK) {
        err = favorites_load_key(&h, NVS_KEY_FAV_BUILTIN, builtin);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        profiles_slot_bitmap_from_u32(user, 0);
        *builtin = 0;
        return err;
    }
    *found = have_user || have_builtin;
    return HAL_OK;
}

/* Set when RAM was changed but the write failed (audit L1): a retry that finds
 * "no change" against RAM must still write. Guarded by s_save_lock. */
static bool s_fav_dirty = false;

esp_err_t profiles_favorites_start(void)
{
    profiles_slot_bitmap_from_u32(&s_fav_user, 0);
    s_fav_builtin = 0;
    s_fav_rev = 0;
    s_fav_dirty = false;

    /* Read-through (pref_cfg_fs.h): the cfg file wins on a strictly higher
     * rev; otherwise the legacy NVS copy stands and, when cfg is mounted, is
     * migrated into the file. Writes never go back to NVS. */
    fav_item_t nvs_item;
    bool nvs_found = false;
    hal_status_t nerr = favorites_read_nvs(&nvs_item.user, &nvs_item.builtin, &nvs_found);
    if (nerr != HAL_OK) {
        ESP_LOGW(TAG, "legacy favorites read from '%s' failed: %s", PROFILES_NVS_PARTITION, hal_status_to_name(nerr));
    }

    fav_item_t resolved;
    uint32_t rev = 0;
    bool used_file = false;
    bool have = pref_cfg_fs_resolve(PROFILES_FAVORITES_FILE_PATH, &nvs_item, sizeof(nvs_item),
                                    nerr == HAL_OK && nvs_found, 0, profiles_favorites_file_validate, &resolved, &rev, &used_file);
    if (!have) {
        if (nerr == HAL_OK) {
            ESP_LOGI(TAG, "no favorites saved yet");
            return ESP_OK;
        }
        return hal_status_to_esp_err(nerr);
    }
    s_fav_user = resolved.user;
    s_fav_builtin = resolved.builtin;
    s_fav_rev = rev;
    ESP_LOGI(TAG, "favorites: saved mask 0x%02lx, builtin mask 0x%08lx (source=%s, rev=%lu)",
             (unsigned long)profiles_slot_bitmap_to_u32(&s_fav_user), (unsigned long)s_fav_builtin,
             used_file ? "file" : "NVS", (unsigned long)s_fav_rev);
    return ESP_OK;
}

bool profiles_favorites_is(uint8_t id)
{
    bool is_user = false;
    uint32_t builtin_bit = 0;
    if (!fav_locate(id, &is_user, &builtin_bit)) {
        return false;
    }
    if (is_user) {
        return profiles_slot_bitmap_test(&s_fav_user, id);
    }
    return (s_fav_builtin & builtin_bit) != 0;
}

/* Callers: httpd and the LCD picker task. Covers the bitmap edit too, so the blob
 * committed is never a mix of two callers' edits. Leaf lock. */
static cfg_save_lock_t s_save_lock = CFG_SAVE_LOCK_INIT;

esp_err_t profiles_favorites_set(uint8_t id, bool favorite)
{
    bool is_user = false;
    uint32_t builtin_bit = 0;
    if (!fav_locate(id, &is_user, &builtin_bit)) {
        return ESP_ERR_INVALID_ARG;
    }

    cfg_save_lock_take(&s_save_lock);
    if (cfg_save_lock_reset_refused()) { /* factory reset in flight: nothing may persist, RAM stays as is */
        cfg_save_lock_give(&s_save_lock);
        return ESP_ERR_INVALID_STATE;
    }
    bool changed;
    if (is_user) {
        bool was = profiles_slot_bitmap_test(&s_fav_user, id);
        changed = (was != favorite);
        if (favorite) {
            profiles_slot_bitmap_set(&s_fav_user, id);
        } else {
            profiles_slot_bitmap_clear(&s_fav_user, id);
        }
    } else {
        uint32_t updated = favorite ? (s_fav_builtin | builtin_bit) : (s_fav_builtin & ~builtin_bit);
        changed = (updated != s_fav_builtin);
        s_fav_builtin = updated;
    }

    if (!changed && !s_fav_dirty) {
        /* No change -- nothing to write. Saying OK here keeps an unfavorite
         * of something that was never favorited from reporting a failure. */
        cfg_save_lock_give(&s_save_lock);
        return ESP_OK;
    }

    /* cfg file ONLY (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"):
     * no NVS write follows, and a failure is returned, never masked. The rev
     * advances only after a verified write. */
    fav_item_t item = { .user = s_fav_user, .builtin = s_fav_builtin };
    uint32_t new_rev = s_fav_rev + 1;
    esp_err_t err = pref_cfg_fs_commit(PROFILES_FAVORITES_FILE_PATH, &item, sizeof(item), new_rev, "profile favorites");
    if (err == ESP_OK) {
        s_fav_rev = new_rev;
    }
    s_fav_dirty = (err != ESP_OK);
    cfg_save_lock_give(&s_save_lock);
    return err;
}

void profiles_favorites_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                             bool *diverged)
{
    fav_item_t f;
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw_quiet(PROFILES_FAVORITES_FILE_PATH, sizeof(f), profiles_favorites_file_validate, &f, &f_rev, &f_valid);

    fav_item_t n;
    bool n_found = false;
    bool n_valid = favorites_read_nvs(&n.user, &n.builtin, &n_found) == HAL_OK && n_found;
    bool content_equal = f_valid && n_valid && memcmp(&f, &n, sizeof(f)) == 0;
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
        *nvs_rev = 0; /* the NVS copy has no rev key */
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
    }
}

void profiles_favorites_masks(profiles_slot_bitmap_t *out_user, uint32_t *out_builtin)
{
    if (out_user) {
        *out_user = s_fav_user;
    }
    if (out_builtin) {
        *out_builtin = s_fav_builtin;
    }
}
