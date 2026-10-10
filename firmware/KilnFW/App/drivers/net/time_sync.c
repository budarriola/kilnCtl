#include "time_sync.h"
#include "cfgfs_file_validators.h"

#include <string.h>
#include <time.h>

#include "cfg_fs_status.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "cfg_save_lock.h"
#include "pref_cfg_fs.h" /* item 14 (TZ), docs/FILESYSTEM_USER_DATA.md section 5
                            * step 3 close-out: TZ is a small fixed-CAPACITY string
                            * (<=TIME_SYNC_TZ_MAX_LEN bytes) with no migration chain of
                            * its own -- reuses the SAME generic bridge unit_pref.c/
                            * relay names (zones_config_store.c) share, not a bespoke
                            * module. See time_sync_tz_file_validate() below for how a
                            * NUL-padded fixed buffer maps onto pref_cfg_fs's
                            * fixed-item_size contract. */

/* Real ESP-IDF/newlib provide setenv()/tzset(); MSVC (this file's host-test
 * build) has neither under those names -- _putenv_s()/_tzset() are its
 * nearest equivalents. Device behavior is completely unchanged: this branch
 * compiles out entirely on the real toolchain. */
#ifdef _MSC_VER
#include <stdlib.h>
static void time_sync_setenv_compat(const char *name, const char *value, int overwrite)
{
    (void)overwrite; /* MSVC's _putenv_s() always overwrites -- matches our one call site's overwrite=1 */
    _putenv_s(name, value);
}
#define setenv(name, value, overwrite) time_sync_setenv_compat((name), (value), (overwrite))
#define tzset() _tzset()
#endif

static const char *TAG = "time_sync";

/* Same partition/namespace convention as unit_pref.c/zones_http.c/
 * touch_cal_store.c -- a single small persisted string belongs in the
 * existing kiln_cfg namespace, not a new one. */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE      "kiln_cfg"
#define NVS_KEY_TZ         "time_tz"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_TZ);

/* Separate tiny NVS key for the dual-write rev counter -- same reasoning as
 * NVS_KEY_ZONES_REV/NVS_KEY_RELAY_NAMES_REV: a rev counter is not part of
 * the value itself. */
#define NVS_KEY_TZ_REV "tz_rev"
NVS_KEY_LEN_CHECK(NVS_KEY_TZ_REV);

/* The `cfg` LittleFS file TZ dual-writes to, via the generic pref_cfg_fs.h
 * bridge. */
/* TIME_SYNC_TZ_FILE_PATH now lives in time_sync.h (kiln-scope reset names it). */

/* Fixed item_size pref_cfg_fs.h's contract requires -- one byte more than
 * TIME_SYNC_TZ_MAX_LEN so a maximum-length string's NUL terminator always
 * has room. Every write fills this whole buffer (memset 0 first, then the
 * string), so file content and item_size never depend on the live string's
 * actual length -- avoids the "variable-length item" complication a naive
 * string bridge would otherwise have. */
#define TZ_ITEM_SIZE (TIME_SYNC_TZ_MAX_LEN + 1)

static uint32_t s_tz_rev = 0;

/* pref_cfg_fs_validate_fn_t for the TZ file: `bytes` is always exactly
 * TZ_ITEM_SIZE raw bytes (fixed by contract, never a raw NVS string length).
 * Requires a NUL terminator within the buffer (a corrupted file might have
 * none) and re-runs time_sync_tz_is_valid() -- the EXACT same check
 * time_sync_start()'s NVS path already applies to a stored string, per this
 * task's "validated on load exactly as its NVS path validates today"
 * requirement. */
bool time_sync_tz_file_validate(const void *bytes, size_t len)
{
    if (len != TZ_ITEM_SIZE) {
        return false;
    }
    const char *s = (const char *)bytes;
    bool has_nul = false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\0') {
            has_nul = true;
            break;
        }
    }
    if (!has_nul) {
        return false;
    }
    return time_sync_tz_is_valid(s);
}

#define SNTP_DEFAULT_SERVER "pool.ntp.org"

static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static time_sync_status_t s_status; /* guarded by s_status_mux */
/* Set true only once esp_netif_sntp_init() has actually succeeded.
 * time_sync_notify_got_ip() checks this before calling esp_netif_sntp_
 * start() -- that call does not itself check whether init ever ran, and
 * unconditionally does sntp_stop();sntp_init() regardless (its own doc
 * comment), so calling it after a failed/never-run init would arm a client
 * with no server configured. owner_task()-only: written once in time_sync_
 * start(), read (never written) in time_sync_notify_got_ip(); both run on
 * wifi_prov.c's owner_task(), so no lock is needed. */
static bool s_sntp_init_ok;

/* Copied verbatim from unit_pref.c's nvs_partition_init() -- same
 * partition, same NO_FREE_PAGES/NEW_VERSION_FOUND erase-and-retry, scoped
 * to only the broken partition. HAL Phase 3 item 3 (hal_kv migration):
 * hal_kv_init_partition() already implements this erase-and-retry idiom. */
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

/* Runs on the lwIP SNTP task -- must stay fast and non-blocking, no logging
 * of anything that could itself block (ESP_LOGI is fine, it's buffered). */
static void sntp_sync_cb(struct timeval *tv)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.ever_synced = true;
    s_status.last_sync_epoch = tv ? (time_t)tv->tv_sec : time(NULL);
    portEXIT_CRITICAL(&s_status_mux);
    ESP_LOGI(TAG, "SNTP sync landed");
}

static void apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
    portENTER_CRITICAL(&s_status_mux);
    strncpy(s_status.tz, tz, TIME_SYNC_TZ_MAX_LEN);
    s_status.tz[TIME_SYNC_TZ_MAX_LEN] = '\0';
    portEXIT_CRITICAL(&s_status_mux);
}

esp_err_t time_sync_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_sntp_init_ok = false;
    s_tz_rev = 0;
    time_sync_tz_effective(NULL, s_status.tz, sizeof(s_status.tz)); /* UTC default until NVS/file says otherwise */

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    bool nvs_valid = false;
    uint32_t nvs_rev = 0;
    uint8_t nvs_item[TZ_ITEM_SIZE];
    memset(nvs_item, 0, sizeof(nvs_item));
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- defaulting to %s this boot",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err), s_status.tz);
    } else {
        hal_kv_handle_t h;
        hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
        char stored[TIME_SYNC_TZ_MAX_LEN + 1] = { 0 };
        bool have_stored = false;
        if (err == HAL_OK) {
            size_t len = sizeof(stored);
            if (hal_kv_get_str(&h, NVS_KEY_TZ, stored, &len) == HAL_OK) {
                have_stored = true;
            }
            if (have_stored && time_sync_tz_is_valid(stored)) {
                strncpy((char *)nvs_item, stored, TZ_ITEM_SIZE - 1);
                nvs_valid = true;
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_TZ_REV, &rev) == HAL_OK) {
                    nvs_rev = rev;
                }
            }
            hal_kv_close(&h);
        } else if (err != HAL_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_open_from_partition failed: %s -- defaulting to %s this boot",
                     hal_status_to_name(err), s_status.tz);
        }
    }

    /* Hand off to the generic file-vs-NVS read-through/tie-break policy
     * (pref_cfg_fs.h) -- on every board today (no `cfg` partition mounted)
     * this is a pass-through to whatever the NVS candidate above produced. */
    uint8_t resolved_item[TZ_ITEM_SIZE];
    uint32_t resolved_rev = 0;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(TIME_SYNC_TZ_FILE_PATH, nvs_item, TZ_ITEM_SIZE, nvs_valid, nvs_rev,
                                           time_sync_tz_file_validate, resolved_item, &resolved_rev, &used_file);

    char effective[TIME_SYNC_TZ_MAX_LEN + 1];
    time_sync_tz_effective(have_value ? (const char *)resolved_item : NULL, effective, sizeof(effective));
    apply_tz(effective);
    if (have_value) {
        s_tz_rev = resolved_rev;
    }
    ESP_LOGI(TAG, "timezone: %s (source=%s, rev=%lu)", s_status.tz, have_value ? (used_file ? "file" : "NVS") : "default",
             (unsigned long)s_tz_rev);

    /* start = false, wait_for_sync = false: prepared but NOT armed and NOT
     * blocking -- see this module's header comment. time_sync_notify_got_
     * ip() is what actually starts the client, once STA has an IP to send
     * the NTP request over. */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_DEFAULT_SERVER);
    config.start = false;
    config.wait_for_sync = false;
    config.sync_cb = sntp_sync_cb;
    esp_err_t sntp_err = esp_netif_sntp_init(&config);
    if (sntp_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s -- no network time this boot", esp_err_to_name(sntp_err));
        return sntp_err; /* non-fatal to the caller (logs only); the error lets it latch a startup fault */
    }
    s_sntp_init_ok = true;
    return ESP_OK;
}

void time_sync_notify_got_ip(void)
{
    /* Init never ran or failed (see time_sync_start()) -- esp_netif_sntp_
     * start() does not check this itself and would happily sntp_stop();
     * sntp_init() a client with no server configured. Bail out here instead
     * of relying on that being harmless by luck. */
    if (!s_sntp_init_ok) {
        ESP_LOGW(TAG, "SNTP client was never initialized -- not starting");
        return;
    }

    /* esp_netif_sntp_start() "start[s] it if it wasn't started during init
     * ... or restart[s] it if already started" (its own doc comment) --
     * safe and cheap to call on every GOT_IP, including a reconnect after a
     * flap. It does not wait for a sync to land -- see time_sync.h's
     * opening comment for what this call itself blocks on. */
    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_start failed: %s", esp_err_to_name(err));
    }
}

void time_sync_get_status(time_sync_status_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    out->now_epoch = out->ever_synced ? time(NULL) : 0;
}

/* Callers: httpd and backup_import's own async task (http_async_job_try_start),
 * so the rev read, RAM assign, commit and rev bump are one section
 * (docs/audits/CFG_STORE_SAVE_RACE_2026-10-09.md, MED-2). cfg_save_lock_t also
 * reserves the flash worker first -- see cfg_save_lock.h. */
static cfg_save_lock_t s_save_lock = CFG_SAVE_LOCK_INIT;

esp_err_t time_sync_set_tz(const char *tz)
{
    if (!time_sync_tz_is_valid(tz)) {
        ESP_LOGW(TAG, "refusing invalid TZ string");
        return ESP_ERR_INVALID_ARG;
    }

    /* Live immediately, same "in-RAM/live truth first" convention as
     * unit_pref_set() -- an NVS write failure below must not leave the
     * board running the OLD timezone after reporting success. */
    cfg_save_lock_take(&s_save_lock);
    apply_tz(tz);
    uint32_t new_rev = s_tz_rev + 1;

    /* cfg file ONLY (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed").
     * This used to return ESP_OK even when nothing was persisted (the NVS
     * failure branches only warned); a failed cfg write is now returned. The
     * TZ stays applied live for this boot either way. */
    uint8_t file_item[TZ_ITEM_SIZE];
    memset(file_item, 0, sizeof(file_item));
    strncpy((char *)file_item, tz, TZ_ITEM_SIZE - 1);
    esp_err_t err = pref_cfg_fs_commit(TIME_SYNC_TZ_FILE_PATH, file_item, TZ_ITEM_SIZE, new_rev, "time zone");
    if (err == ESP_OK) {
        s_tz_rev = new_rev;
    }
    cfg_save_lock_give(&s_save_lock);
    return err;
}

void time_sync_get_tz_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
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

    uint8_t f_item[TZ_ITEM_SIZE];
    memset(f_item, 0, sizeof(f_item));
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw(TIME_SYNC_TZ_FILE_PATH, TZ_ITEM_SIZE, time_sync_tz_file_validate, f_item, &f_rev, &f_valid);

    bool n_valid = false;
    uint8_t n_item[TZ_ITEM_SIZE];
    memset(n_item, 0, sizeof(n_item));
    uint32_t n_rev = 0;
    if (nvs_partition_init(KILN_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
            char stored[TIME_SYNC_TZ_MAX_LEN + 1] = { 0 };
            size_t len = sizeof(stored);
            if (hal_kv_get_str(&h, NVS_KEY_TZ, stored, &len) == HAL_OK && time_sync_tz_is_valid(stored)) {
                n_valid = true;
                strncpy((char *)n_item, stored, TZ_ITEM_SIZE - 1);
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_TZ_REV, &rev) == HAL_OK) {
                    n_rev = rev;
                }
            }
            hal_kv_close(&h);
        }
    }

    bool content_equal = f_valid && n_valid && (memcmp(f_item, n_item, sizeof(f_item)) == 0);
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
