#include "update_settings.h"

#include <stdatomic.h>
#include <string.h>

#include "cfg_fs_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"
#include "update_url.h"

static const char *TAG = "update_settings";

// Same partition/namespace as display_power_cfg.c / unit_pref.c.
#define KILN_NVS_PARTITION      "kiln_nvs"
#define NVS_NAMESPACE           "kiln_cfg"
#define NVS_KEY_UPDATE_REPO     "update_repo"
#define NVS_KEY_UPDATE_REPO_REV "upd_repo_rev"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_UPDATE_REPO);
NVS_KEY_LEN_CHECK(NVS_KEY_UPDATE_REPO_REV);

#define UPDATE_SETTINGS_VERSION 1
#define REPO_BUF                64

// 64 bytes: version + 63-byte NUL-padded string. All-zero string = unset.
typedef struct {
    uint8_t version;
    char repo[REPO_BUF - 1];
} update_settings_blob_t;
_Static_assert(sizeof(update_settings_blob_t) == REPO_BUF, "blob must stay 64 bytes");
_Static_assert(UPDATE_SETTINGS_REPO_MAX_LEN == UPDATE_REPO_VALID_MAX_LEN, "one repo length cap");
_Static_assert(UPDATE_SETTINGS_REPO_MAX_LEN < REPO_BUF - 1, "max repo length must fit the blob with its NUL");

// Double buffer flipped by index: see update_settings.h's RAM note. s_live and
// s_live_valid are published with release stores and read with acquire loads,
// so a reader that sees the new index also sees the string written before it.
static char s_buf[2][REPO_BUF];
static _Atomic uint8_t s_live;
static _Atomic bool s_live_valid; // false until the first start()/set(): reads return the default
static uint32_t s_rev;            // only touched under s_write_lock (and start(), before any reader)
static bool s_persist_dirty;      // true after a set() whose NVS write failed (RAM differs from flash)

// Two locks, deliberately:
//   s_state_lock  guards the double buffer's writer side and update_settings_repo_copy().
//                 Held for a memcpy only -- NEVER across file/NVS access.
//   s_write_lock  serialises whole update_settings_set() calls (RAM publish + file + NVS), so
//                 two concurrent sets cannot persist out of order against the rev counter.
//                 Only writers ever take it; readers and repo_copy() never wait on flash.
// Both are static-storage mutexes created in update_settings_start() (boot, single-threaded);
// ensure_locks() is the fallback for a caller that runs before start().
static StaticSemaphore_t s_state_lock_storage;
static StaticSemaphore_t s_write_lock_storage;
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_write_lock;

static void ensure_locks(void)
{
    if (s_state_lock == NULL) {
        s_state_lock = xSemaphoreCreateMutexStatic(&s_state_lock_storage);
    }
    if (s_write_lock == NULL) {
        s_write_lock = xSemaphoreCreateMutexStatic(&s_write_lock_storage);
    }
}

bool update_settings_repo_is_valid(const char *repo)
{
    // One definition, shared with the WP8 fetcher (update_url.c).
    return update_repo_valid(repo);
}

// Blob validator, also the pref_cfg_fs_validate_fn_t for the file. Accepts the
// all-zero string (unset) and any valid repo, NUL-terminated and zero-padded.
static bool update_settings_validate_blob(const void *bytes, size_t len)
{
    if (bytes == NULL || len != sizeof(update_settings_blob_t)) {
        return false;
    }
    const update_settings_blob_t *b = (const update_settings_blob_t *)bytes;
    if (b->version != UPDATE_SETTINGS_VERSION) {
        return false;
    }
    size_t n = strnlen(b->repo, sizeof(b->repo));
    if (n == sizeof(b->repo)) {
        return false; // not NUL-terminated
    }
    for (size_t i = n; i < sizeof(b->repo); i++) {
        if (b->repo[i] != '\0') {
            return false; // garbage after the terminator
        }
    }
    return n == 0 || update_settings_repo_is_valid(b->repo);
}

// Publishes `repo` ("" = default) into the idle buffer, then flips to it.
// Takes s_state_lock itself; callers must not hold it.
static void publish_ram(const char *repo)
{
    ensure_locks();
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    bool valid = atomic_load_explicit(&s_live_valid, memory_order_relaxed);
    uint8_t live = atomic_load_explicit(&s_live, memory_order_relaxed);
    uint8_t next = (uint8_t)(valid ? (1u - live) : 0u);
    memset(s_buf[next], 0, REPO_BUF);
    strncpy(s_buf[next], repo, REPO_BUF - 1);
    atomic_store_explicit(&s_live, next, memory_order_release);
    atomic_store_explicit(&s_live_valid, true, memory_order_release);
    xSemaphoreGive(s_state_lock);
}

esp_err_t update_settings_start(void)
{
    ensure_locks();
    s_rev = 0;
    s_persist_dirty = false;
    atomic_store_explicit(&s_live_valid, false, memory_order_release);

    hal_status_t part_err = hal_kv_init_partition(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- update repo stays at the default this boot",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return ESP_OK;
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    bool nvs_valid = false;
    update_settings_blob_t nvs_blob;
    memset(&nvs_blob, 0, sizeof(nvs_blob));
    uint32_t nvs_rev = 0;
    if (err == HAL_NOT_FOUND) {
        // namespace never written -- default is the expected steady state
    } else if (err != HAL_OK) {
        ESP_LOGW(TAG, "nvs open failed: %s -- update repo stays at the default this boot", hal_status_to_name(err));
    } else {
        update_settings_blob_t blob;
        memset(&blob, 0, sizeof(blob));
        size_t len = sizeof(blob);
        hal_status_t rerr = hal_kv_get_blob(&h, NVS_KEY_UPDATE_REPO, &blob, &len);
        if (rerr == HAL_OK) {
            if (update_settings_validate_blob(&blob, len)) {
                nvs_valid = true;
                nvs_blob = blob;
            } else {
                ESP_LOGW(TAG, "stored update_repo blob is invalid (size %u, version %u) -- using the default",
                         (unsigned)len, (unsigned)blob.version);
            }
        } else if (rerr != HAL_NOT_FOUND) {
            ESP_LOGW(TAG, "update_repo read failed: %s -- default stays in effect this boot",
                     hal_status_to_name(rerr));
        }
        if (nvs_valid) {
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_UPDATE_REPO_REV, &rev) == HAL_OK) {
                nvs_rev = rev;
            }
        }
        hal_kv_close(&h);
    }

    update_settings_blob_t resolved = nvs_blob;
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(UPDATE_SETTINGS_FILE_PATH, &nvs_blob, sizeof(nvs_blob), nvs_valid, nvs_rev,
                                           update_settings_validate_blob, &resolved, &resolved_rev, &used_file);
    if (!have_value) {
        return ESP_OK; // neither side had anything trustworthy -- default stands
    }
    publish_ram(resolved.repo);
    s_rev = resolved_rev;
    ESP_LOGI(TAG, "update repo loaded (source=%s, rev=%lu): %s", used_file ? "file" : "NVS", (unsigned long)s_rev,
             update_settings_repo());
    return ESP_OK;
}

const char *update_settings_repo(void)
{
    if (!atomic_load_explicit(&s_live_valid, memory_order_acquire)) {
        return UPDATE_SETTINGS_DEFAULT_REPO;
    }
    uint8_t live = atomic_load_explicit(&s_live, memory_order_acquire);
    if (s_buf[live][0] == '\0') {
        return UPDATE_SETTINGS_DEFAULT_REPO;
    }
    return s_buf[live];
}

bool update_settings_repo_copy(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
    ensure_locks();
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    const char *repo = update_settings_repo();
    size_t n = strlen(repo);
    bool fits = n < cap;
    if (fits) {
        memcpy(out, repo, n + 1);
    } else {
        out[0] = '\0';
    }
    xSemaphoreGive(s_state_lock);
    return fits;
}

bool update_settings_repo_is_default(void)
{
    return strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0;
}

static esp_err_t update_settings_set_locked(const char *repo);

esp_err_t update_settings_set(const char *repo)
{
    if (repo == NULL) {
        repo = "";
    }
    if (repo[0] != '\0' && !update_settings_repo_is_valid(repo)) {
        return ESP_ERR_INVALID_ARG; // never clamp or sanitize: refuse outright
    }
    if (strcmp(repo, UPDATE_SETTINGS_DEFAULT_REPO) == 0) {
        repo = ""; // canonical form: the default is stored (and exported) as "unset"
    }
    ensure_locks();
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    esp_err_t err = update_settings_set_locked(repo);
    xSemaphoreGive(s_write_lock);
    return err;
}

// Caller holds s_write_lock (and NOT s_state_lock). The file/NVS writes below run under
// s_write_lock only, which no reader or repo_copy() ever takes.
static esp_err_t update_settings_set_locked(const char *repo)
{
    // Unchanged value: no RAM flip, no flash wear. Not when the last persist failed -- then
    // RAM and flash differ and this call is the retry.
    const char *current = "";
    if (atomic_load_explicit(&s_live_valid, memory_order_acquire)) {
        current = s_buf[atomic_load_explicit(&s_live, memory_order_acquire)];
    }
    if (!s_persist_dirty && strcmp(current, repo) == 0) {
        return ESP_OK;
    }

    // In-RAM truth first (live for the next reader whether or not the write below works).
    publish_ram(repo);
    s_persist_dirty = true;

    uint32_t new_rev = s_rev + 1;
    update_settings_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = UPDATE_SETTINGS_VERSION;
    strncpy(blob.repo, repo, sizeof(blob.repo) - 1);

    // cfg file ONLY (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"); s_persist_dirty
    // stays set on failure so an identical retry is not short-circuited above.
    esp_err_t err = pref_cfg_fs_commit(UPDATE_SETTINGS_FILE_PATH, &blob, sizeof(blob), new_rev, "update repo");
    if (err != ESP_OK) {
        return err;
    }
    s_rev = new_rev;
    s_persist_dirty = false;
    ESP_LOGI(TAG, "update repo saved: %s", update_settings_repo());
    return ESP_OK;
}

void update_settings_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                          bool *diverged)
{
    if (file_valid) {
        *file_valid = false;
    }
    if (file_rev) {
        *file_rev = 0;
    }
    if (nvs_valid) {
        *nvs_valid = false;
    }
    if (nvs_rev) {
        *nvs_rev = 0;
    }
    if (diverged) {
        *diverged = false;
    }

    update_settings_blob_t f_blob;
    memset(&f_blob, 0, sizeof(f_blob));
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw_quiet(UPDATE_SETTINGS_FILE_PATH, sizeof(f_blob), update_settings_validate_blob, &f_blob,
                               &f_rev, &f_valid);

    bool n_valid = false;
    update_settings_blob_t n_blob;
    memset(&n_blob, 0, sizeof(n_blob));
    uint32_t n_rev = 0;
    if (hal_kv_init_partition(KILN_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
            update_settings_blob_t blob;
            memset(&blob, 0, sizeof(blob));
            size_t len = sizeof(blob);
            if (hal_kv_get_blob(&h, NVS_KEY_UPDATE_REPO, &blob, &len) == HAL_OK &&
                update_settings_validate_blob(&blob, len)) {
                n_valid = true;
                n_blob = blob;
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_UPDATE_REPO_REV, &rev) == HAL_OK) {
                    n_rev = rev;
                }
            }
            hal_kv_close(&h);
        }
    }

    bool content_equal = f_valid && n_valid && (memcmp(&f_blob, &n_blob, sizeof(f_blob)) == 0);
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
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
    }
}

void update_settings_reset_ram_for_test(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    atomic_store_explicit(&s_live, 0, memory_order_release);
    atomic_store_explicit(&s_live_valid, false, memory_order_release);
    s_rev = 0;
    s_persist_dirty = false;
}
