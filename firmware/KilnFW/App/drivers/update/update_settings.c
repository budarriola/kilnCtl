#include "update_settings.h"

#include <string.h>

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"

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
_Static_assert(UPDATE_SETTINGS_REPO_MAX_LEN < REPO_BUF - 1, "max repo length must fit the blob with its NUL");

// Double buffer flipped by index: see update_settings.h's RAM note.
static char s_buf[2][REPO_BUF];
static volatile uint8_t s_live;
static volatile bool s_live_valid; // false until the first start()/set(): reads return the default
static uint32_t s_rev;

static bool is_name_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

bool update_settings_repo_is_valid(const char *repo)
{
    if (repo == NULL) {
        return false;
    }
    size_t len = strnlen(repo, UPDATE_SETTINGS_REPO_MAX_LEN + 1);
    if (len < 3 || len > UPDATE_SETTINGS_REPO_MAX_LEN) {
        return false;
    }
    const char *slash = strchr(repo, '/');
    if (slash == NULL || strchr(slash + 1, '/') != NULL) {
        return false;
    }
    size_t owner_len = (size_t)(slash - repo);
    size_t name_len = len - owner_len - 1;
    if (owner_len < 1 || owner_len > 39 || name_len < 1 || name_len > 100) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (repo[i] != '/' && !is_name_char(repo[i])) {
            return false;
        }
    }
    if (repo[0] == '-' || repo[owner_len - 1] == '-') {
        return false;
    }
    if (strstr(repo, "..") != NULL) {
        return false;
    }
    return true;
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
static void publish_ram(const char *repo)
{
    uint8_t next = (uint8_t)(s_live_valid ? (1u - s_live) : 0u);
    memset(s_buf[next], 0, REPO_BUF);
    strncpy(s_buf[next], repo, REPO_BUF - 1);
    s_live = next;
    s_live_valid = true;
}

esp_err_t update_settings_start(void)
{
    s_rev = 0;
    s_live_valid = false;

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
    if (!s_live_valid || s_buf[s_live][0] == '\0') {
        return UPDATE_SETTINGS_DEFAULT_REPO;
    }
    return s_buf[s_live];
}

bool update_settings_repo_is_default(void)
{
    return strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0;
}

esp_err_t update_settings_set(const char *repo)
{
    if (repo == NULL) {
        repo = "";
    }
    if (repo[0] != '\0' && !update_settings_repo_is_valid(repo)) {
        return ESP_ERR_INVALID_ARG; // never clamp or sanitize: refuse outright
    }

    // In-RAM truth first (live for the next reader whether or not the write below works).
    publish_ram(repo);

    uint32_t new_rev = s_rev + 1;
    update_settings_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = UPDATE_SETTINGS_VERSION;
    strncpy(blob.repo, repo, sizeof(blob.repo) - 1);

    // FILE FIRST (best effort), THEN NVS (authoritative), as display_power_cfg_set().
    esp_err_t file_err = pref_cfg_fs_save(UPDATE_SETTINGS_FILE_PATH, &blob, sizeof(blob), new_rev);
    if (file_err != ESP_OK && file_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "update repo file write failed: %s -- NVS remains the source of truth this boot",
                 esp_err_to_name(file_err));
    }

    hal_status_t part_err = hal_kv_init_partition(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- update repo not persisted", KILN_NVS_PARTITION,
                 hal_status_to_name(part_err));
        return hal_status_to_esp_err(part_err);
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "nvs open failed: %s -- update repo not persisted", hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_UPDATE_REPO, &blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_UPDATE_REPO_REV, new_rev);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "could not persist update repo: %s -- will not survive a reboot", hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    s_rev = new_rev;
    ESP_LOGI(TAG, "update repo saved: %s", update_settings_repo());
    return ESP_OK;
}

void update_settings_reset_ram_for_test(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    s_live = 0;
    s_live_valid = false;
    s_rev = 0;
}
