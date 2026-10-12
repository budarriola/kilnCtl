// profiles_favorites -- persisted "favorite" marks on firing profiles.
// See profiles_favorites.h for the design rationale (why a favorite is a
// shortcut and not a move, why two masks rather than one, and what happens
// to a favorite when the profile it points at is deleted or overwritten).
#include "profiles_favorites.h"
#include "cfgfs_file_validators.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "hal_esp_common.h"
#include "cfg_fs.h"
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
/* INFO-5: the two masks are published together under this spinlock so lock-free readers (LCD picker, httpd GETs)
 * never see a new user mask with an old builtin mask. Held only for a 20-byte copy. */
static portMUX_TYPE s_fav_mux = portMUX_INITIALIZER_UNLOCKED;
static void fav_publish(const profiles_slot_bitmap_t *user, uint32_t builtin)
{
    portENTER_CRITICAL(&s_fav_mux);
    s_fav_user = *user;
    s_fav_builtin = builtin;
    portEXIT_CRITICAL(&s_fav_mux);
}
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
 * enforces on-flash key type (a blob read against a U32-typed key ends in
 * HAL_NOT_FOUND, as does the typed host fake), so NOT_FOUND from the blob
 * probe falls through to the legacy u32 read. A missing key entirely is
 * explicitly not an error -- it means nothing has ever been favorited, the
 * shipped default. A blob of any OTHER size is corrupt (review LOW-3): it
 * returns HAL_IO rather than reading as "nothing favorited". */
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
        if (err == HAL_OK) {
            return HAL_IO; /* a blob exists at a wrong size: not "absent" */
        }
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

/* Legacy (audit L1): RAM used to change before the write; since PROFILESFX4 LOW-B it is published only after a
 * successful commit, so this stays false. Kept so a retry that finds
 * "no change" against RAM must still write. Guarded by s_save_lock. */
static bool s_fav_dirty = false;
/* Set when the user mask could not be resolved at start (failed NVS load, no cfg file): profiles_favorites_set()
 * refuses rather than writing a rev-1 file from empty RAM (review LOW-3a). Guarded by s_save_lock. */
static bool s_fav_user_unresolved = false;

/* Loads only the builtin mask (its own u32 key), independent of the user mask. */
static void favorites_read_builtin_only(uint32_t *builtin)
{
    hal_kv_handle_t h;
    if (hal_kv_init_partition(PROFILES_NVS_PARTITION) != HAL_OK ||
        hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, PROFILES_NVS_PARTITION) != HAL_OK) {
        return;
    }
    uint32_t v = 0;
    hal_status_t gerr = hal_kv_get_u32(&h, NVS_KEY_FAV_BUILTIN, &v);
    if (gerr == HAL_OK) {
        *builtin = v;
    } else if (gerr == HAL_NOT_FOUND) {
        *builtin = 0; /* INFO-4: the key is genuinely absent, so the true value is 0, not the stale RAM mask */
    }
    hal_kv_close(&h);
}

static esp_err_t favorites_load(void)
{
    /* FIRELOWFX3 LOW-2: everything is resolved into locals and published only at the end. profiles_favorites_is()
     * and _masks() read s_fav_* without s_save_lock, so a retry that is refused again must never zero them.
     * INFO-1: no explicit degraded clear here -- pref_cfg_fs_resolve() re-evaluates the registry entry on every path. */

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
        /* LOW-1: "no file, no NVS copy" is only "nothing saved yet" when the file was actually readable; an
         * unreadable prof_fav.bin (rev-unknown) must stay unresolved so no zero mask is published for it. */
        bool file_unreadable = pref_cfg_fs_rev_unknown(PROFILES_FAVORITES_FILE_PATH);
        if (nerr == HAL_OK && !file_unreadable) {
            ESP_LOGI(TAG, "no favorites saved yet");
            profiles_slot_bitmap_t zero;
            profiles_slot_bitmap_from_u32(&zero, 0);
            fav_publish(&zero, 0);
            s_fav_rev = 0;
            s_fav_dirty = false;
            s_fav_user_unresolved = false;
            return ESP_OK;
        }
        s_fav_user_unresolved = true;
        cfg_fs_degraded_set(PROFILES_FAVORITES_FILE_PATH, true); /* FIRELOWFX2 MED-1: listed in GET /api/cfgfs, HTTP save 409 */
        /* Only the builtin mask has an independent key; keep the current RAM value if it cannot be read. */
        uint32_t builtin = s_fav_builtin;
        favorites_read_builtin_only(&builtin);
        profiles_slot_bitmap_t keep_user;
        profiles_favorites_masks(&keep_user, NULL);
        fav_publish(&keep_user, builtin);
        return nerr == HAL_OK ? ESP_ERR_INVALID_STATE : hal_status_to_esp_err(nerr);
    }
    fav_publish(&resolved.user, resolved.builtin);
    s_fav_rev = rev;
    s_fav_dirty = false;
    s_fav_user_unresolved = false;
    ESP_LOGI(TAG, "favorites: saved mask 0x%02lx, builtin mask 0x%08lx (source=%s, rev=%lu)",
             (unsigned long)profiles_slot_bitmap_to_u32(&resolved.user), (unsigned long)resolved.builtin,
             used_file ? "file" : "NVS", (unsigned long)s_fav_rev);
    return ESP_OK;
}

esp_err_t profiles_favorites_start(void)
{
    return favorites_load();
}

bool profiles_favorites_is(uint8_t id)
{
    bool is_user = false;
    uint32_t builtin_bit = 0;
    if (!fav_locate(id, &is_user, &builtin_bit)) {
        return false;
    }
    profiles_slot_bitmap_t u;
    uint32_t b = 0;
    profiles_favorites_masks(&u, &b);
    if (is_user) {
        return profiles_slot_bitmap_test(&u, id);
    }
    return (b & builtin_bit) != 0;
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
    if (s_fav_user_unresolved) { /* LOW-3a: never write a file from an unresolved (empty) user mask */
        (void)favorites_load(); /* MED-1: retry the read so a transient boot failure heals without a reboot */
        if (s_fav_user_unresolved) {
            cfg_save_lock_give(&s_save_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    bool changed;
    profiles_slot_bitmap_t cur_user;
    uint32_t cur_builtin = 0;
    profiles_favorites_masks(&cur_user, &cur_builtin);
    if (is_user) {
        bool was = profiles_slot_bitmap_test(&cur_user, id);
        changed = (was != favorite);
        if (favorite) {
            profiles_slot_bitmap_set(&cur_user, id);
        } else {
            profiles_slot_bitmap_clear(&cur_user, id);
        }
    } else {
        uint32_t updated = favorite ? (cur_builtin | builtin_bit) : (cur_builtin & ~builtin_bit);
        changed = (updated != cur_builtin);
        cur_builtin = updated;
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
    fav_item_t item = { .user = cur_user, .builtin = cur_builtin };
    uint32_t new_rev = s_fav_rev + 1;
    esp_err_t err = pref_cfg_fs_commit(PROFILES_FAVORITES_FILE_PATH, &item, sizeof(item), new_rev, "profile favorites");
    if (err == ESP_OK) {
        s_fav_rev = new_rev;
        fav_publish(&cur_user, cur_builtin); /* PROFILESFX4 LOW-B: RAM changes only once the commit succeeded */
    }
    s_fav_dirty = false; /* a refused/failed commit leaves RAM equal to the file: nothing to retry */
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
    portENTER_CRITICAL(&s_fav_mux);
    if (out_user) {
        *out_user = s_fav_user;
    }
    if (out_builtin) {
        *out_builtin = s_fav_builtin;
    }
    portEXIT_CRITICAL(&s_fav_mux);
}
