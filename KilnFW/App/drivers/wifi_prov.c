#include "wifi_prov.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"
#include "wifi_provision_http.h"

static const char *TAG = "wifi_prov";

#define NVS_NAMESPACE "wifi_cfg"

/* Dedicated NVS partition for Wi-Fi credentials -- see partitions.csv, which
 * appends it at 0x187000 in flash that the stock single-app-large layout left
 * unused above the app image.
 *
 * Why it isn't just another namespace in the default `nvs` partition (which is
 * where every version of this file before 2026-08-12 put it): NVS corruption
 * recovery is partition-wide, not namespace-wide. The only cure for
 * ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND is to erase the
 * entire partition. The code below used to answer that with a blanket
 * nvs_flash_erase(), which took zones, rules, profiles, the run-state
 * breadcrumb AND the Wi-Fi credentials with it -- so a corrupt profile blob
 * knocked the board off the network at exactly the moment the operator needed
 * the network to go fix it. Splitting the credentials into their own partition
 * means neither wipe can reach the other, in either direction.
 *
 * The same split is what makes "reset the kiln's configuration" (erase the
 * default `nvs`) a thing the operator can do without stranding the board.
 *
 * It does NOT make the credentials immortal: `esptool erase_flash` clears the
 * whole chip and this partition goes with it, exactly like every other one.
 * The survival matrix is:
 *   flash bootloader+partition-table+app  -> credentials survive (this is the
 *                                            common case, and the reason the
 *                                            first three partitions.csv rows
 *                                            are kept byte-identical to the
 *                                            stock table)
 *   erase the default `nvs` partition     -> credentials survive, kiln config
 *                                            is lost
 *   esptool erase_flash                   -> nothing survives, credentials
 *                                            included */
#define WIFI_NVS_PARTITION "wifi_nvs"

#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_HAS_CREDS "has_creds"
#define NVS_KEY_MODE "mode"           /* u8: 0 = WIFI_PROV_MODE_HOME, 1 = WIFI_PROV_MODE_AP */
#define NVS_KEY_LOCAL_ONLY "local_only" /* legacy, read-only: pre-2026-08-11 firmware's
                                          * only mode flag. Migrated into NVS_KEY_MODE the
                                          * first time this runs against an old NVS blob;
                                          * never written by this build. See nvs_load(). */
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_HAS_AP_SSID "has_ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"
#define NVS_KEY_HAS_AP_PASS "has_ap_pass"

/* Named (rather than anonymous) so the migration path in wifi_prov_start()
 * can take a whole-struct snapshot of what the new partition yielded before
 * speculatively re-loading over it from the old one. */
static struct wifi_prov_state {
    bool started;
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;
    esp_timer_handle_t ap_fallback_timer; /* one-shot; brings the AP back if a
                                           * reconnect doesn't land in time */

    char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
    bool has_creds;
    wifi_prov_mode_t mode;

    /* The fallback AP's OWN identity -- distinct from the station
     * credentials above, which are for joining a *different* network. Only
     * meaningful when the corresponding has_*_override is set; otherwise
     * apply_ap_config() falls back to the compile-time WIFI_AP_SSID /
     * WIFI_AP_DEFAULT_PASSWORD Kconfig values. Kept separate from has_creds
     * so an operator can override just the AP's own identity without
     * touching (or requiring) any saved station network. */
    char ap_ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    bool has_ap_ssid_override;
    char ap_password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
    bool has_ap_password_override;

    wifi_prov_state_t state;
} s_wifi;

/* ---- NVS -------------------------------------------------------------- */

/* Reads the whole persisted config out of `partition`'s NVS_NAMESPACE into
 * s_wifi. Parameterized on the partition (rather than hard-wired to
 * WIFI_NVS_PARTITION) purely so the one-time migration in wifi_prov_start()
 * can point the exact same reader -- legacy-key handling and all -- at the old
 * default-partition copy without duplicating any of it.
 *
 * *out_found reports whether the namespace existed at all, which is what the
 * migration keys off: an absent namespace means "nothing was ever saved here",
 * which is distinct from "saved, but with has_creds == 0". */
static esp_err_t nvs_load_from(const char *partition, bool *out_found)
{
    if (out_found) {
        *out_found = false;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        /* NOT_FOUND: nothing saved yet -- first boot. PART_NOT_FOUND: this
         * build's partition table isn't on the chip (e.g. a new app flashed
         * without the new table). Neither is an error worth refusing to bring
         * Wi-Fi up over -- the board just comes up unprovisioned. */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (out_found) {
        *out_found = true;
    }

    size_t len = sizeof(s_wifi.ssid);
    err = nvs_get_str(h, NVS_KEY_SSID, s_wifi.ssid, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_wifi.ssid[0] = '\0';
    }

    len = sizeof(s_wifi.password);
    err = nvs_get_str(h, NVS_KEY_PASS, s_wifi.password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_wifi.password[0] = '\0';
    }

    uint8_t u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_HAS_CREDS, &u8);
    s_wifi.has_creds = (err == ESP_OK) && u8;

    /* Mode: prefer the new key. If it's missing, this is an NVS blob
     * written by pre-2026-08-11 firmware -- fall back to the old
     * "local_only" flag so a device that was in local-only mode boots back
     * into AP mode (its closest equivalent) rather than silently defaulting
     * to home mode and trying to join a network the user explicitly opted
     * out of. Never writes NVS_KEY_LOCAL_ONLY itself; the migration is
     * read-only and completes for real the next time the mode is saved
     * (nvs_save_mode() below only ever writes NVS_KEY_MODE). */
    u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_MODE, &u8);
    if (err == ESP_OK) {
        s_wifi.mode = u8 ? WIFI_PROV_MODE_AP : WIFI_PROV_MODE_HOME;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t legacy_local_only = 0;
        esp_err_t legacy_err = nvs_get_u8(h, NVS_KEY_LOCAL_ONLY, &legacy_local_only);
        if (legacy_err == ESP_OK && legacy_local_only) {
            ESP_LOGI(TAG, "migrating legacy local_only=1 NVS flag to mode=AP");
            s_wifi.mode = WIFI_PROV_MODE_AP;
        } else {
            s_wifi.mode = WIFI_PROV_MODE_HOME;
        }
    } else {
        nvs_close(h);
        return err;
    }

    len = sizeof(s_wifi.ap_ssid);
    err = nvs_get_str(h, NVS_KEY_AP_SSID, s_wifi.ap_ssid, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_wifi.ap_ssid[0] = '\0';
    }

    u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_HAS_AP_SSID, &u8);
    s_wifi.has_ap_ssid_override = (err == ESP_OK) && u8;

    len = sizeof(s_wifi.ap_password);
    err = nvs_get_str(h, NVS_KEY_AP_PASS, s_wifi.ap_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_wifi.ap_password[0] = '\0';
    }

    u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_HAS_AP_PASS, &u8);
    s_wifi.has_ap_password_override = (err == ESP_OK) && u8;

    nvs_close(h);
    return ESP_OK;
}

/* Every writer below targets WIFI_NVS_PARTITION unconditionally. The old
 * default-partition copy is deliberately never written again (nor deleted --
 * see the migration note in wifi_prov_start()), so a rollback to firmware that
 * predates the split still finds the credentials it knew about, just frozen at
 * whatever they were when this build first ran. */
static esp_err_t nvs_save_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_SSID, s_wifi.ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, s_wifi.password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_HAS_CREDS, s_wifi.has_creds ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_mode(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_MODE, s_wifi.mode == WIFI_PROV_MODE_AP ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_ap_ssid(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_AP_SSID, s_wifi.ap_ssid);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_HAS_AP_SSID, s_wifi.has_ap_ssid_override ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_ap_password(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_AP_PASS, s_wifi.ap_password);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_HAS_AP_PASS, s_wifi.has_ap_password_override ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Brings up one NVS partition, erasing ONLY that partition if its contents are
 * unusable. The scoping is the entire point of the 2026-08-12 split: the old
 * code answered a recovery condition with a blanket nvs_flash_erase(), whose
 * blast radius was the default partition -- credentials, zones, rules,
 * profiles and run_state all at once. Now each partition's recovery can only
 * destroy its own contents, so a corrupt default NVS cannot take the Wi-Fi
 * credentials with it and a corrupt wifi_nvs cannot take the kiln config.
 *
 * NO_FREE_PAGES / NEW_VERSION_FOUND genuinely have no other cure -- NVS cannot
 * mount at all in either state -- so erasing is the only way forward; the
 * requirement is just that it stays inside the partition that is actually
 * broken. */
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

/* One-time move of the persisted config out of the default partition's
 * wifi_cfg namespace and into WIFI_NVS_PARTITION's, for boards that were
 * provisioned by firmware predating the split. s_wifi must already hold
 * whatever WIFI_NVS_PARTITION yielded; this decides whether the old copy
 * should win and, if so, leaves s_wifi holding it and persists it to the new
 * home.
 *
 * The old copy is intentionally left in place rather than deleted: someone
 * rolling back to pre-split firmware to chase a regression should still find a
 * board that can join its network. The cost is a stale duplicate that this
 * build never reads again after the migration and never writes at all --
 * cheap, and strictly safer than the alternative. */
static void migrate_from_default_partition(bool found_in_wifi_nvs)
{
    /* Snapshot first: nvs_load_from() writes straight into s_wifi, so the
     * speculative read of the old copy below would otherwise clobber a
     * perfectly good new-partition config if the old namespace turns out to
     * hold nothing useful. */
    struct wifi_prov_state from_wifi_nvs = s_wifi;

    bool found_in_default = false;
    esp_err_t err = nvs_load_from(NVS_DEFAULT_PART_NAME, &found_in_default);
    if (err != ESP_OK || !found_in_default) {
        s_wifi = from_wifi_nvs;
        return;
    }

    /* Adopt the old copy when the new home has nothing at all (the true
     * first-boot-after-the-split case), or when the new home exists but has no
     * credentials while the old one does -- which is what a board looks like
     * if it reached the new firmware, saved only an AP-identity override, and
     * still has its real network sitting in the old partition. Anything the
     * new partition has already recorded otherwise wins outright; the new
     * location is the source of truth from the moment it holds credentials. */
    bool adopt = !found_in_wifi_nvs || (s_wifi.has_creds && !from_wifi_nvs.has_creds);
    if (!adopt) {
        s_wifi = from_wifi_nvs;
        return;
    }

    ESP_LOGI(TAG, "migrating Wi-Fi config from the default NVS partition to '%s' (ssid '%s', mode %s)",
             WIFI_NVS_PARTITION, s_wifi.ssid, s_wifi.mode == WIFI_PROV_MODE_AP ? "AP" : "home");

    /* Write everything through, not just the credentials: the mode and the AP
     * identity overrides are part of the same persisted config and would
     * otherwise silently revert to defaults on the next boot, once this
     * function stops adopting the old copy. Failures are logged and survivable
     * -- s_wifi is already correct for this boot either way, and the migration
     * simply gets retried next time. */
    esp_err_t save_err = nvs_save_creds();
    if (save_err == ESP_OK) {
        save_err = nvs_save_mode();
    }
    if (save_err == ESP_OK && s_wifi.has_ap_ssid_override) {
        save_err = nvs_save_ap_ssid();
    }
    if (save_err == ESP_OK && s_wifi.has_ap_password_override) {
        save_err = nvs_save_ap_password();
    }
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "migration write to '%s' failed: %s -- running from the old copy this boot, will retry",
                 WIFI_NVS_PARTITION, esp_err_to_name(save_err));
    }
}

/* ---- Wi-Fi driver config helpers -------------------------------------- */

static void apply_ap_config(void)
{
    wifi_config_t ap_cfg = { 0 };
    const char *ssid = s_wifi.has_ap_ssid_override ? s_wifi.ap_ssid : WIFI_AP_SSID;
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    const char *pw = s_wifi.has_ap_password_override ? s_wifi.ap_password : WIFI_AP_DEFAULT_PASSWORD;
    if (strlen(pw) >= 8) {
        strncpy((char *)ap_cfg.ap.password, pw, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        /* WPA2-PSK requires an 8-63 char password; a shorter configured
         * default would otherwise fail esp_wifi_set_config outright. Open is
         * safer than silently refusing to start the AP a phone needs to
         * reach in order to provision the board at all. */
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        /* wifi_prov_set_ap_password() itself refuses a 1-7 char override, so
         * this only fires for a misconfigured KILNCTL_WIFI_AP_DEFAULT_PASSWORD
         * (Kconfig isn't validated the same way) or a deliberate empty
         * override (open AP). */
        ESP_LOGW(TAG, "AP password is shorter than 8 chars -- AP is OPEN");
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
    }
}

static void apply_sta_config(void)
{
    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid, s_wifi.ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, s_wifi.password, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = strlen(s_wifi.password) > 0 ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
    }
}

static void cancel_ap_fallback_timer(void)
{
    if (s_wifi.ap_fallback_timer) {
        /* esp_timer_stop on an already-stopped one-shot timer is a no-op
         * error we don't care about. */
        esp_timer_stop(s_wifi.ap_fallback_timer);
    }
}

static void ap_fallback_timer_cb(void *arg)
{
    (void)arg;
    if (s_wifi.state == WIFI_PROV_STATE_CONNECTED) {
        return; /* reconnected before the timer fired */
    }
    ESP_LOGW(TAG, "station join did not land within the timeout -- bringing the fallback AP up");
    apply_ap_config();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
    }
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
}

static void start_ap_fallback_timer(void)
{
    if (!s_wifi.ap_fallback_timer) {
        return;
    }
    cancel_ap_fallback_timer();
    esp_err_t err =
        esp_timer_start_once(s_wifi.ap_fallback_timer, (uint64_t)WIFI_STA_CONNECT_TIMEOUT_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

/* Common to wifi_prov_set_credentials() and wifi_prov_set_mode(HOME):
 * starts (or restarts) a station join attempt against the saved
 * credentials, AP staying up alongside it until the join is confirmed. Only
 * meaningful when s_wifi.has_creds is already true and mode is already
 * WIFI_PROV_MODE_HOME -- callers are responsible for having gotten there
 * first. This is the "don't strand the phone" guarantee: the AP is never
 * torn down before IP_EVENT_STA_GOT_IP confirms the join actually worked. */
static void start_sta_join(void)
{
    apply_sta_config();
    cancel_ap_fallback_timer();
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;
    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(mode_err));
    }
    start_ap_fallback_timer();
    esp_wifi_connect();
}

/* ---- Event handlers ----------------------------------------------------
 * Everything here runs on the default event loop's own task, never on a
 * caller's stack -- this is the mechanism that keeps wifi_prov_start()
 * non-blocking. Nothing in this handler touches kiln_io/safety_link. */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        if (s_wifi.has_creds && s_wifi.mode == WIFI_PROV_MODE_HOME) {
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi.mode == WIFI_PROV_MODE_AP || !s_wifi.has_creds) {
            return; /* not attempting station at all */
        }
        bool was_connected = (s_wifi.state == WIFI_PROV_STATE_CONNECTED);
        s_wifi.state = was_connected ? WIFI_PROV_STATE_RECONNECTING : WIFI_PROV_STATE_CONNECTING;
        ESP_LOGI(TAG, "station disconnected, retrying join");
        esp_wifi_connect();
        if (!s_wifi.ap_fallback_timer) {
            return;
        }
        /* Only (re)arm the fallback timer if it isn't already counting down
         * from a previous disconnect -- esp_timer_start_once on an already
         * running timer returns ESP_ERR_INVALID_STATE, which is fine to
         * ignore, but re-arming would keep pushing the AP fallback out on a
         * flapping link instead of ever bringing it back. */
        esp_timer_start_once(s_wifi.ap_fallback_timer,
                              (uint64_t)WIFI_STA_CONNECT_TIMEOUT_MS * 1000);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    cancel_ap_fallback_timer();
    ESP_LOGI(TAG, "station joined, dropping fallback AP");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
    }
    s_wifi.state = WIFI_PROV_STATE_CONNECTED;
}

/* ---- Public API --------------------------------------------------------- */

esp_err_t wifi_prov_start(void)
{
    if (s_wifi.started) {
        return ESP_OK;
    }

    /* This module is still the de-facto owner of DEFAULT-partition NVS
     * bring-up for the whole firmware: zones_http, rules_http, profiles_http,
     * run_state and relay_cycles all call nvs_open() without ever initializing
     * the partition themselves, and app_main calls wifi_prov_start() before
     * any of them. Splitting the credentials out did not change that -- both
     * partitions get initialized here, just through strictly separate recovery
     * paths so one being unmountable can never cost the other its contents. */
    esp_err_t default_err = nvs_partition_init(NVS_DEFAULT_PART_NAME);
    if (default_err != ESP_OK) {
        /* Not fatal to Wi-Fi: the credentials live somewhere else now, so the
         * board can still come up on the network and be talked to -- which is
         * exactly the situation the split exists to preserve. The kiln-config
         * readers that depend on this partition will fail their own nvs_open()
         * calls and fall back to their defaults, as they already do on a blank
         * partition. */
        ESP_LOGE(TAG, "default NVS init failed: %s -- zones/rules/profiles/run_state will not persist",
                 esp_err_to_name(default_err));
    }

    esp_err_t err = nvs_partition_init(WIFI_NVS_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- Wi-Fi credentials cannot persist",
                 WIFI_NVS_PARTITION, esp_err_to_name(err));
        /* Deliberately not a return: an unusable credential partition means
         * nothing persists, but the AP still has to come up so the board can
         * be reached and reprovisioned at all. Falling through leaves s_wifi
         * zeroed, i.e. unprovisioned/home -- the first-boot state. */
    }

    bool found_in_wifi_nvs = false;
    if (err == ESP_OK) {
        err = nvs_load_from(WIFI_NVS_PARTITION, &found_in_wifi_nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi_cfg load from '%s' failed: %s -- starting unprovisioned",
                     WIFI_NVS_PARTITION, esp_err_to_name(err));
            s_wifi.has_creds = false;
            s_wifi.mode = WIFI_PROV_MODE_HOME;
        } else if (default_err == ESP_OK) {
            /* Only worth attempting when the default partition actually
             * mounted -- there is nothing to migrate from otherwise. */
            migrate_from_default_partition(found_in_wifi_nvs);
        }
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* ESP_ERR_INVALID_STATE means a default loop already exists -- fine,
     * some other subsystem may have created it first. Anything else is a
     * real failure. */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi.ap_netif = esp_netif_create_default_wifi_ap();
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_wifi.ap_netif || !s_wifi.sta_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_* failed");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = &ap_fallback_timer_cb,
        .name = "wifi_ap_fallback",
    };
    err = esp_timer_create(&timer_args, &s_wifi.ap_fallback_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_create failed: %s -- no AP-fallback-on-reconnect-timeout", esp_err_to_name(err));
        s_wifi.ap_fallback_timer = NULL;
    }

    apply_ap_config();
    if (s_wifi.mode == WIFI_PROV_MODE_AP) {
        s_wifi.state = WIFI_PROV_STATE_AP_MODE;
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        ESP_LOGI(TAG, "AP mode: AP '%s' only, station never attempted", wifi_prov_get_ap_ssid());
    } else if (s_wifi.has_creds) {
        apply_sta_config();
        s_wifi.state = WIFI_PROV_STATE_CONNECTING;
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        start_ap_fallback_timer();
        ESP_LOGI(TAG, "attempting station join to '%s', AP '%s' available meanwhile", s_wifi.ssid,
                 wifi_prov_get_ap_ssid());
    } else {
        s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        ESP_LOGI(TAG, "no saved credentials: AP '%s' for first-boot provisioning", wifi_prov_get_ap_ssid());
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_provision_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_provision_http_start failed: %s -- Wi-Fi is up but not provisionable over HTTP",
                 esp_err_to_name(err));
        /* Not fatal to wifi_prov itself -- Wi-Fi bring-up already
         * succeeded and this module's own hard requirement (never touch
         * relay/safety state) doesn't depend on the HTTP server. */
    }

    s_wifi.started = true;
    return ESP_OK;
}

wifi_prov_state_t wifi_prov_get_state(void)
{
    return s_wifi.state;
}

wifi_prov_mode_t wifi_prov_get_mode(void)
{
    return s_wifi.mode;
}

const char *wifi_prov_get_saved_ssid(void)
{
    return s_wifi.ssid;
}

const char *wifi_prov_get_ap_ssid(void)
{
    return s_wifi.has_ap_ssid_override ? s_wifi.ap_ssid : WIFI_AP_SSID;
}

esp_err_t wifi_prov_set_credentials(const char *ssid, size_t ssid_len, const char *password,
                                    size_t password_len)
{
    if (!ssid || ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!password || password_len > WIFI_PROV_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_wifi.ssid, ssid, ssid_len);
    s_wifi.ssid[ssid_len] = '\0';
    memcpy(s_wifi.password, password, password_len);
    s_wifi.password[password_len] = '\0';
    s_wifi.has_creds = true;

    esp_err_t err = nvs_save_creds();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_creds failed: %s -- credentials will not survive a reboot",
                 esp_err_to_name(err));
        /* Still attempt the join below -- the operator asked for this
         * network right now, whether or not it persists. */
    }

    if (s_wifi.mode == WIFI_PROV_MODE_AP) {
        /* Submitting real credentials is an explicit choice to join a
         * network -- it supersedes a previously-set AP-mode preference the
         * same way picking a network in the Network settings page (TODO.md
         * section 4) would. Without this, AP mode had no way back out
         * through the HTTP API: the provisioning page's mode toggle set it,
         * but nothing ever cleared it again from this path. */
        ESP_LOGI(TAG, "credentials submitted while in AP mode -- switching to home mode");
        s_wifi.mode = WIFI_PROV_MODE_HOME;
        esp_err_t mode_err = nvs_save_mode();
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_save_mode failed: %s -- choice will not survive a reboot",
                     esp_err_to_name(mode_err));
        }
    }

    start_sta_join();
    return ESP_OK;
}

esp_err_t wifi_prov_set_mode(wifi_prov_mode_t mode)
{
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_wifi.mode = mode;
    esp_err_t err = nvs_save_mode();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_mode failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
    }

    if (mode == WIFI_PROV_MODE_AP) {
        cancel_ap_fallback_timer();
        s_wifi.state = WIFI_PROV_STATE_AP_MODE;
        apply_ap_config();
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(mode_err));
        }
        ESP_LOGI(TAG, "AP mode enabled: station never attempted");
    } else if (s_wifi.has_creds) {
        ESP_LOGI(TAG, "home mode enabled, resuming join to saved network");
        start_sta_join();
    } else {
        s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
        ESP_LOGI(TAG, "home mode enabled, no saved network to join");
    }
    return ESP_OK;
}

esp_err_t wifi_prov_set_ap_ssid(const char *ssid, size_t ssid_len)
{
    if (!ssid || ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        /* The AP always needs to be reachable by something -- an empty
         * SSID isn't a valid "leave it alone" no-op here the way it can be
         * for a station password (open network); refuse it outright. */
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_wifi.ap_ssid, ssid, ssid_len);
    s_wifi.ap_ssid[ssid_len] = '\0';
    s_wifi.has_ap_ssid_override = true;

    esp_err_t err = nvs_save_ap_ssid();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_ap_ssid failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
        /* Still apply it live below -- the operator asked for this right
         * now, whether or not it persists past a reboot. */
    }

    apply_ap_config();
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        /* Mirrors wifi_prov_set_ap_password()'s pattern: esp_wifi_set_config
         * alone doesn't kick already-associated clients off a running AP, so
         * re-applying the same mode forces the radio to pick up the new
         * SSID immediately instead of only after the next boot/mode change.
         * Station side (if APSTA) is untouched. */
        esp_err_t mode_err = esp_wifi_set_mode(mode);
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(mode_err));
        }
    }
    ESP_LOGI(TAG, "AP SSID changed to '%s'", s_wifi.ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_prov_set_ap_password(const char *password, size_t password_len)
{
    if (!password || password_len > WIFI_PROV_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (password_len > 0 && password_len < 8) {
        /* WPA2-PSK requires 8-63 chars; a non-empty password shorter than
         * that can never work on real hardware, so refuse it outright here
         * rather than silently falling back to an open AP the way a bad
         * compile-time Kconfig default does in apply_ap_config() (there is
         * no request to fail in that case, just a build to warn about). */
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_wifi.ap_password, password, password_len);
    s_wifi.ap_password[password_len] = '\0';
    s_wifi.has_ap_password_override = true;

    esp_err_t err = nvs_save_ap_password();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_ap_password failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
        /* Still apply it live below -- the operator asked for this right
         * now, whether or not it persists past a reboot. */
    }

    apply_ap_config();
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        /* Mirrors wifi_prov_set_ap_ssid()'s pattern: esp_wifi_set_config
         * alone doesn't kick already-associated clients off a running AP, so
         * re-applying the same mode forces the radio to pick up the new PSK
         * immediately instead of only after the next boot/mode change.
         * Station side (if APSTA) is untouched. */
        esp_err_t mode_err = esp_wifi_set_mode(mode);
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(mode_err));
        }
    }
    ESP_LOGI(TAG, "AP password changed (%s)", password_len > 0 ? "WPA2-PSK" : "open");
    return ESP_OK;
}

bool wifi_prov_is_sta_connected(void)
{
    return s_wifi.state == WIFI_PROV_STATE_CONNECTED;
}

esp_err_t wifi_prov_get_sta_ip(char *out, size_t out_cap)
{
    if (!out || out_cap < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!s_wifi.started || s_wifi.state != WIFI_PROV_STATE_CONNECTED || !s_wifi.sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_wifi.sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    if (out_cap < 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_ip4addr_ntoa(&ip_info.ip, out, (uint32_t)out_cap);
    return ESP_OK;
}

esp_err_t wifi_prov_scan(wifi_prov_scan_result_t *results, size_t max_results, size_t *out_count)
{
    if (!results || max_results == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wifi.mode == WIFI_PROV_MODE_AP) {
        /* AP mode means no station-radio activity at all, and a scan --
         * even though it never joins anything -- still means bringing the
         * STA interface up. Refuse rather than bend that guarantee for a
         * settings-page convenience. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return err;
    }
    if (mode == WIFI_MODE_AP) {
        /* Unprovisioned: only the AP interface is up. Bring STA up too so a
         * scan is possible -- this does not by itself connect to anything;
         * on_wifi_event() only calls esp_wifi_connect() when has_creds is
         * true, and unprovisioned means it isn't. */
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 50,
        .scan_time.active.max = 150,
    };
    err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return ESP_OK;
    }
    if (found > max_results) {
        found = (uint16_t)max_results;
    }

    static wifi_ap_record_t records[20];
    uint16_t to_fetch = found > (sizeof(records) / sizeof(records[0]))
                            ? (uint16_t)(sizeof(records) / sizeof(records[0]))
                            : found;
    err = esp_wifi_scan_get_ap_records(&to_fetch, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t n = to_fetch < max_results ? to_fetch : max_results;
    for (size_t i = 0; i < n; i++) {
        strncpy(results[i].ssid, (const char *)records[i].ssid, WIFI_PROV_SSID_MAX_LEN);
        results[i].ssid[WIFI_PROV_SSID_MAX_LEN] = '\0';
        results[i].rssi = records[i].rssi;
        results[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
    }
    *out_count = n;
    return ESP_OK;
}
