/* wifi_prov.c split (2026-09-04, ROADMAP.md M15 A3) -- NVS load/save and the
 * one-time cross-partition/legacy-format migrations. See
 * wifi_prov_internal.h's top-of-file comment for the full split rationale
 * and file map. Move-only: no logic, ordering, naming or visibility change
 * beyond widening former `static` symbols this split's sibling files now
 * call directly. */

#include "wifi_prov_internal.h"
#include "../persist/legacy_default_nvs.h"

#include <string.h>

#include "esp_log.h"

#include "hal_esp_common.h"
#include "hal_kv.h"
/* nvs_flash.h kept for NVS_DEFAULT_PART_NAME only -- see wifi_prov.c's
 * identical comment; every actual nvs_*() call in this file below now goes
 * through hal_kv_*() instead. */
#include "nvs_flash.h"

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
 * 2026-08-13: the same reasoning was extended to the REST of the default
 * `nvs` partition's former contents. zones/rules/relay_cycles/run_state now
 * live in their own `kiln_nvs` partition and fire profiles in their own
 * `profiles_nvs` (see partitions.csv) -- each with the same scoped
 * wifi_prov_nvs_partition_init()-and-erase-ONLY-that-partition pattern this file
 * pioneered, and each doing its own one-time migration off the default
 * partition independently, the same way this module migrates Wi-Fi
 * credentials below. This module no longer initializes the default `nvs`
 * partition as a side effect for anyone else -- that was a historical
 * accident (main.c happened to call wifi_prov_start() first), and every
 * module that touches NVS now owns its own partition's init.
 *
 * The same split is what makes "reset the kiln's configuration" (erase
 * `kiln_nvs` and/or `profiles_nvs`) a thing the operator can do without
 * stranding the board off Wi-Fi, and without one corrupt section taking any
 * other section with it.
 *
 * It does NOT make the credentials immortal: `esptool erase_flash` clears the
 * whole chip and every partition goes with it. The survival matrix is:
 *   flash bootloader+partition-table+app  -> credentials survive (this is the
 *                                            common case, and the reason the
 *                                            first three partitions.csv rows
 *                                            are kept byte-identical to the
 *                                            stock table)
 *   erase the default `nvs` partition     -> credentials survive; kiln_nvs
 *                                            and profiles_nvs are UNAFFECTED
 *                                            (default `nvs` is only read now,
 *                                            during migration, never written)
 *   erase `kiln_nvs`                      -> zones/rules/relay_cycles/
 *                                            run_state lost; credentials and
 *                                            profiles survive
 *   erase `profiles_nvs`                  -> fire profiles lost; everything
 *                                            else survives
 *   esptool erase_flash                   -> nothing survives, credentials
 *                                            included */
#include "wifi_prov_nvs_keys.h" /* NVS_NAMESPACE + NVS_KEY_* (shared with legacy_default_nvs.c) */

/* Winning legacy single-network credential (pre-8.4 NVS_KEY_SSID/PASS/
 * HAS_CREDS format), set by wifi_prov_migrate_from_default_partition() and consumed
 * exactly once by nvs_load_saved_nets() to wrap it into saved_nets.nets[0]
 * the first time this build runs against an old NVS blob. Module-static
 * (rather than a parameter threaded through) because migrate_from_default_
 * partition() already has to decide "which whole config wins" for the
 * mode/AP-identity fields it does carry in s_wifi -- this rides along with
 * that same decision instead of duplicating it. Declared extern in
 * wifi_prov_internal.h: wifi_prov.c's wifi_prov_start() also sets it
 * directly on the no-cross-partition-migration path. */
struct wifi_prov_legacy_single s_legacy_single;

/* Set by wifi_prov_migrate_from_default_partition() once the mode/AP-identity
 * fields it adopted read back identically from WIFI_NVS_PARTITION; consumed by
 * nvs_load_saved_nets(), which erases the legacy default-partition keys only
 * after the credential list is persisted and read back too (one-shot
 * migration, DEV_FIRMWARE_REVIEW_5 M2). */
static bool s_legacy_erase_pending;

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
esp_err_t wifi_prov_nvs_load_from(const char *partition, bool *out_found)
{
    if (out_found) {
        *out_found = false;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, partition);
    if (err == HAL_NOT_FOUND) {
        /* Namespace/key not found (first boot) -- silently "nothing saved".
         * A genuinely missing partition table (ESP_ERR_NVS_PART_NOT_FOUND on
         * the ESP backend) is a DIFFERENT hal_status_t as of the 2026-09-06
         * flash-safety review: hal_kv_esp.c now maps it to HAL_NOT_READY, not
         * HAL_NOT_FOUND, precisely so this branch cannot also swallow it. It
         * falls into the `err != HAL_OK` branch below instead, which surfaces
         * it as a real error via hal_status_to_esp_err() rather than treating
         * it as an ordinary unprovisioned board. This call site is safe
         * either way: by the time wifi_prov_nvs_load_from() runs, WIFI_NVS_
         * PARTITION has already been brought up successfully earlier in boot
         * (this module's own partition-init call, gating every caller here),
         * so a PART_NOT_FOUND at this point would mean that partition failed
         * to mount after all -- a real fault worth surfacing, not "nothing
         * saved yet" (2026-09-05 batch 3 review confirmed the gating;
         * re-confirmed after this remap). */
        return ESP_OK;
    }
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    /* An EMPTY namespace is "not found". ESP-IDF keeps a namespace entry after
     * nvs_erase_all()/erasing every key, so recovery's Wi-Fi reset (which runs
     * nvs_erase_all on this default-partition namespace) leaves one behind. Only
     * a namespace in which at least one known key exists counts as saved data;
     * otherwise the migration would adopt an all-default legacy config and
     * override the AP identity that survives in WIFI_NVS_PARTITION. */
    static const char *const probe_keys[] = WIFI_PROV_NVS_ALL_KEYS_INIT;
    bool any_key = false;
    for (size_t i = 0; i < sizeof(probe_keys) / sizeof(probe_keys[0]) && !any_key; i++) {
        /* Type-agnostic: a typed getter returns NOT_FOUND for a key of another
         * type on target (ESP-IDF 6.0.2), so mode, the has_ flags and ip_mode (u8) and
         * saved_nets (blob) would read as absent. */
        any_key = hal_kv_key_exists(&h, probe_keys[i]) == HAL_OK;
    }
    if (!any_key) {
        hal_kv_close(&h);
        return ESP_OK;
    }
    if (out_found) {
        *out_found = true;
    }

    /* The legacy single-network keys (NVS_KEY_SSID/NVS_KEY_PASS/
     * NVS_KEY_HAS_CREDS) are no longer read here -- they're only consulted
     * by the one-time list-format migration in nvs_load_saved_nets_from(),
     * which runs after this function (and after wifi_prov_migrate_from_default_partition(),
     * on whichever single-key data wins that decision). This function no
     * longer touches s_wifi.saved_nets/active_ssid at all. */
    size_t len;
    uint8_t u8 = 0;

    /* Mode: prefer the new key. If it's missing, this is an NVS blob
     * written by pre-2026-08-11 firmware -- fall back to the old
     * "local_only" flag so a device that was in local-only mode boots back
     * into AP mode (its closest equivalent) rather than silently defaulting
     * to home mode and trying to join a network the user explicitly opted
     * out of. Never writes NVS_KEY_LOCAL_ONLY itself; the migration is
     * read-only and completes for real the next time the mode is saved
     * (nvs_save_mode() below only ever writes NVS_KEY_MODE). */
    u8 = 0;
    err = hal_kv_get_u8(&h, NVS_KEY_MODE, &u8);
    if (err == HAL_OK) {
        s_wifi.mode = u8 ? WIFI_PROV_MODE_AP : WIFI_PROV_MODE_HOME;
    } else if (err == HAL_NOT_FOUND) {
        uint8_t legacy_local_only = 0;
        hal_status_t legacy_err = hal_kv_get_u8(&h, NVS_KEY_LOCAL_ONLY, &legacy_local_only);
        if (legacy_err == HAL_OK && legacy_local_only) {
            ESP_LOGI(WIFI_PROV_TAG, "migrating legacy local_only=1 NVS flag to mode=AP");
            s_wifi.mode = WIFI_PROV_MODE_AP;
        } else {
            s_wifi.mode = WIFI_PROV_MODE_HOME;
        }
    } else {
        hal_kv_close(&h);
        return hal_status_to_esp_err(err);
    }

    len = sizeof(s_wifi.ap_ssid);
    err = hal_kv_get_str(&h, NVS_KEY_AP_SSID, s_wifi.ap_ssid, &len);
    if (err != HAL_OK && err != HAL_NOT_FOUND) {
        hal_kv_close(&h);
        return hal_status_to_esp_err(err);
    }
    if (err == HAL_NOT_FOUND) {
        s_wifi.ap_ssid[0] = '\0';
    }

    u8 = 0;
    err = hal_kv_get_u8(&h, NVS_KEY_HAS_AP_SSID, &u8);
    s_wifi.has_ap_ssid_override = (err == HAL_OK) && u8;

    len = sizeof(s_wifi.ap_password);
    err = hal_kv_get_str(&h, NVS_KEY_AP_PASS, s_wifi.ap_password, &len);
    if (err != HAL_OK && err != HAL_NOT_FOUND) {
        hal_kv_close(&h);
        return hal_status_to_esp_err(err);
    }
    if (err == HAL_NOT_FOUND) {
        s_wifi.ap_password[0] = '\0';
    }

    u8 = 0;
    err = hal_kv_get_u8(&h, NVS_KEY_HAS_AP_PASS, &u8);
    s_wifi.has_ap_password_override = (err == HAL_OK) && u8;

    /* 2026-08-20, web-GUI-only static-IP fields. Absent (pre-feature NVS
     * blob, or genuinely never configured) reads back as DHCP with empty
     * strings -- today's always-on default, unchanged. */
    u8 = 0;
    err = hal_kv_get_u8(&h, NVS_KEY_IP_MODE, &u8);
    s_wifi.ip_mode = (err == HAL_OK && u8) ? WIFI_PROV_IP_MODE_STATIC : WIFI_PROV_IP_MODE_DHCP;

    len = sizeof(s_wifi.static_ip);
    err = hal_kv_get_str(&h, NVS_KEY_STATIC_IP, s_wifi.static_ip, &len);
    if (err != HAL_OK) {
        s_wifi.static_ip[0] = '\0';
    }
    len = sizeof(s_wifi.static_netmask);
    err = hal_kv_get_str(&h, NVS_KEY_STATIC_NETMASK, s_wifi.static_netmask, &len);
    if (err != HAL_OK) {
        s_wifi.static_netmask[0] = '\0';
    }
    len = sizeof(s_wifi.static_gateway);
    err = hal_kv_get_str(&h, NVS_KEY_STATIC_GW, s_wifi.static_gateway, &len);
    if (err != HAL_OK) {
        s_wifi.static_gateway[0] = '\0';
    }
    len = sizeof(s_wifi.static_dns);
    err = hal_kv_get_str(&h, NVS_KEY_STATIC_DNS, s_wifi.static_dns, &len);
    if (err != HAL_OK) {
        s_wifi.static_dns[0] = '\0';
    }
    len = sizeof(s_wifi.static_dns2);
    err = hal_kv_get_str(&h, NVS_KEY_STATIC_DNS2, s_wifi.static_dns2, &len);
    if (err != HAL_OK) {
        s_wifi.static_dns2[0] = '\0';
    }

    hal_kv_close(&h);
    return ESP_OK;
}

/* Every writer below targets WIFI_NVS_PARTITION unconditionally. The old
 * default-partition copy is never written again, and is erased exactly once:
 * after wifi_prov_migrate_from_default_partition() has adopted it AND the
 * saved_nets record has been written and read back from WIFI_NVS_PARTITION
 * (see nvs_load_saved_nets()). Consequence, accepted: once that migration has
 * completed, a rollback to firmware that predates the partition split finds no
 * legacy credentials and boots unprovisioned. */

/* True only if `partition` holds a complete, readable saved_nets record. This
 * -- not the existence of the wifi_cfg namespace or of the mode key, both of
 * which the first migration step creates before the credential is persisted --
 * is what proves the migration finished and the legacy copy is stale. */
static bool nvs_saved_nets_record_present(const char *partition)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, partition) != HAL_OK) {
        return false;
    }
    saved_nets_blob_t blob;
    size_t len = sizeof(blob);
    hal_status_t err = hal_kv_get_blob(&h, NVS_KEY_SAVED_NETS, &blob, &len);
    hal_kv_close(&h);
    return err == HAL_OK && len == sizeof(blob) && blob.version == SAVED_NETS_VERSION &&
           blob.count <= WIFI_PROV_MAX_SAVED_NETWORKS;
}

/* Reads the legacy single-network keys (NVS_KEY_SSID/NVS_KEY_PASS/
 * NVS_KEY_HAS_CREDS) out of `partition`, with the same has_creds-missing
 * inference the pre-8.4 wifi_prov_nvs_load_from() used to apply in place: if the flag
 * key is genuinely absent, trust the presence of a non-empty ssid rather than
 * silently reporting "nothing saved" (covers a partial write that landed
 * ssid/pass but not the flag). This is read-only and exists purely to feed
 * the one-time list-format migration in nvs_load_saved_nets() -- nothing
 * else in this build ever reads these keys again, and nothing writes them. */
void nvs_load_legacy_single(const char *partition, saved_net_t *out_net, bool *out_has)
{
    memset(out_net, 0, sizeof(*out_net));
    *out_has = false;

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, partition);
    if (err != HAL_OK) {
        return; /* namespace/partition absent -- nothing legacy to find */
    }

    size_t len = sizeof(out_net->ssid);
    err = hal_kv_get_str(&h, NVS_KEY_SSID, out_net->ssid, &len);
    if (err != HAL_OK) {
        out_net->ssid[0] = '\0';
    }

    len = sizeof(out_net->password);
    err = hal_kv_get_str(&h, NVS_KEY_PASS, out_net->password, &len);
    if (err != HAL_OK) {
        out_net->password[0] = '\0';
    }

    uint8_t u8 = 0;
    err = hal_kv_get_u8(&h, NVS_KEY_HAS_CREDS, &u8);
    if (err == HAL_NOT_FOUND) {
        *out_has = out_net->ssid[0] != '\0';
    } else {
        *out_has = (err == HAL_OK) && u8;
    }

    hal_kv_close(&h);
}

/* Reads the current saved_nets_blob_t out of `partition`, applying the same
 * 3-outcome version-check pattern as zones_http.c/rules_http.c's blob
 * loaders: exact SAVED_NETS_VERSION match uses it as-is; an absent/unreadable/
 * wrong-size blob is treated as an empty list (count 0, not an error -- first
 * boot looks the same as "nothing saved"); a version NEWER than this build's
 * SAVED_NETS_VERSION means the blob was written by newer firmware (the
 * firmware-rollback case) -- refuse to load it and leave flash untouched
 * rather than risk misinterpreting fields/layout this build doesn't know
 * about, same rationale as zones_cfg_t's version check. */
/* LOW-2: set when a saved_nets record exists but could not be loaded
 * (read error, wrong size, newer version, corrupt count, open failure other
 * than "absent"). Such a record is "present but unreadable": the board is not
 * unprovisioned, add/forget are refused and the record is never overwritten.
 * factory_reset(wifi) is the escape. */
bool s_saved_nets_refused;

static esp_err_t nvs_load_saved_nets_from(const char *partition, saved_nets_blob_t *out_blob)
{
    memset(out_blob, 0, sizeof(*out_blob));
    s_saved_nets_refused = false;

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, partition);
    if (err == HAL_NOT_FOUND) {
        /* See wifi_prov_nvs_load_from()'s comment: as of the 2026-09-06
         * flash-safety remap, ESP_ERR_NVS_PART_NOT_FOUND (partition table
         * missing this partition entirely) maps to HAL_NOT_READY, not
         * HAL_NOT_FOUND, so it no longer reaches this silent branch -- it
         * falls into the generic `err != HAL_OK` branch below instead, which
         * logs a warning and STILL resolves to "treat as empty" (this
         * function has no error return of its own to distinguish the two).
         * Safe either way: this call site runs only after WIFI_NVS_PARTITION
         * has already been brought up successfully earlier in boot, so
         * reaching PART_NOT_FOUND here would itself be a genuine fault. */
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (err != HAL_OK) {
        s_saved_nets_refused = true;
        return hal_status_to_esp_err(err);
    }

    size_t len = sizeof(*out_blob);
    err = hal_kv_get_blob(&h, NVS_KEY_SAVED_NETS, out_blob, &len);
    hal_kv_close(&h);
    if (err == HAL_NOT_FOUND) {
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (err != HAL_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "saved_nets blob read from '%s' failed (%s) -- record kept, add/forget refused",
                 partition, hal_status_to_name(err));
        s_saved_nets_refused = true;
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (len != sizeof(*out_blob)) {
        ESP_LOGW(WIFI_PROV_TAG, "saved_nets blob from '%s' is the wrong size -- record kept, add/forget refused", partition);
        s_saved_nets_refused = true;
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }

    if (out_blob->version <= SAVED_NETS_VERSION) {
        /* LOW-1: a corrupt count would index past nets[]. */
        if (out_blob->count > WIFI_PROV_MAX_SAVED_NETWORKS) {
            ESP_LOGW(WIFI_PROV_TAG, "saved_nets blob from '%s' has count %u > %u -- record kept, add/forget refused",
                     partition, (unsigned)out_blob->count, (unsigned)WIFI_PROV_MAX_SAVED_NETWORKS);
            s_saved_nets_refused = true;
            memset(out_blob, 0, sizeof(*out_blob));
            out_blob->version = SAVED_NETS_VERSION;
            return ESP_OK;
        }
        /* LOW-1: force NUL termination; later code uses strlen/strcmp. */
        for (size_t i = 0; i < WIFI_PROV_MAX_SAVED_NETWORKS; i++) {
            out_blob->nets[i].ssid[sizeof(out_blob->nets[i].ssid) - 1] = '\0';
            out_blob->nets[i].password[sizeof(out_blob->nets[i].password) - 1] = '\0';
        }
        /* v1 is the first version that has ever existed -- hook point for a
         * future migration, nothing to convert yet. */
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    /* out_blob->version > SAVED_NETS_VERSION: written by newer firmware
     * (firmware-rollback case, same as zones_cfg_t) -- refuse to load,
     * flash data left untouched. */
    ESP_LOGW(WIFI_PROV_TAG, "saved_nets blob from '%s' is version %u, newer than this firmware's %u -- "
                  "refusing to load, flash data left untouched",
             partition, (unsigned)out_blob->version, (unsigned)SAVED_NETS_VERSION);
    s_saved_nets_refused = true; /* newer */
    memset(out_blob, 0, sizeof(*out_blob));
    out_blob->version = SAVED_NETS_VERSION;
    return ESP_OK;
}

esp_err_t nvs_save_saved_nets(void)
{
    s_wifi.saved_nets.version = SAVED_NETS_VERSION;

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_SAVED_NETS, &s_wifi.saved_nets, sizeof(s_wifi.saved_nets));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

esp_err_t nvs_save_mode(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_u8(&h, NVS_KEY_MODE, s_wifi.mode == WIFI_PROV_MODE_AP ? 1 : 0);
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

esp_err_t nvs_save_ap_ssid(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_str(&h, NVS_KEY_AP_SSID, s_wifi.ap_ssid);
    if (err == HAL_OK) {
        err = hal_kv_set_u8(&h, NVS_KEY_HAS_AP_SSID, s_wifi.has_ap_ssid_override ? 1 : 0);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

esp_err_t nvs_save_ap_password(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_str(&h, NVS_KEY_AP_PASS, s_wifi.ap_password);
    if (err == HAL_OK) {
        err = hal_kv_set_u8(&h, NVS_KEY_HAS_AP_PASS, s_wifi.has_ap_password_override ? 1 : 0);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

/* 2026-08-20, web-GUI-only: persists ip_mode + the three static-IP strings
 * together (they only ever change as a group -- see do_set_dhcp()/
 * do_set_static_ip()). Same partition/namespace/commit pattern as every
 * other nvs_save_*() in this file. */
esp_err_t nvs_save_ip_config(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_u8(&h, NVS_KEY_IP_MODE, s_wifi.ip_mode == WIFI_PROV_IP_MODE_STATIC ? 1 : 0);
    if (err == HAL_OK) {
        err = hal_kv_set_str(&h, NVS_KEY_STATIC_IP, s_wifi.static_ip);
    }
    if (err == HAL_OK) {
        err = hal_kv_set_str(&h, NVS_KEY_STATIC_NETMASK, s_wifi.static_netmask);
    }
    if (err == HAL_OK) {
        err = hal_kv_set_str(&h, NVS_KEY_STATIC_GW, s_wifi.static_gateway);
    }
    if (err == HAL_OK) {
        err = hal_kv_set_str(&h, NVS_KEY_STATIC_DNS, s_wifi.static_dns);
    }
    if (err == HAL_OK) {
        err = hal_kv_set_str(&h, NVS_KEY_STATIC_DNS2, s_wifi.static_dns2);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
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
hal_status_t wifi_prov_nvs_partition_init(const char *partition)
{
    /* HAL Phase 3 item 3 (hal_kv migration): hal_kv_init_partition() already
     * implements this erase-and-retry idiom. */
    return hal_kv_init_partition(partition);
}

/* One-time move of the persisted config out of the default partition's
 * wifi_cfg namespace and into WIFI_NVS_PARTITION's, for boards that were
 * provisioned by firmware predating the split. s_wifi must already hold
 * whatever WIFI_NVS_PARTITION yielded; this decides whether the old copy
 * should win and, if so, leaves s_wifi holding it and persists it to the new
 * home.
 *
 * The old copy is erased only once the credential is safely in the new home
 * (saved_nets record written and read back; see nvs_load_saved_nets()). Until
 * then it is left intact, and because the adopt decision below keys on a
 * verified saved_nets record in WIFI_NVS_PARTITION, a failed or interrupted
 * migration is retried on the next boot. Trade-off: after a completed
 * migration a rollback past the partition split boots unprovisioned. */
void wifi_prov_migrate_from_default_partition(bool found_in_wifi_nvs)
{
    /* Snapshot first: wifi_prov_nvs_load_from() writes straight into s_wifi, so the
     * speculative read of the old copy below would otherwise clobber a
     * perfectly good new-partition config if the old namespace turns out to
     * hold nothing useful. */
    struct wifi_prov_state from_wifi_nvs = s_wifi;

    /* Legacy single-network credentials, read directly -- wifi_prov_nvs_load_from()
     * itself no longer touches NVS_KEY_SSID/PASS/HAS_CREDS (see
     * nvs_load_saved_nets_from() for the current, list-based loader). These
     * are needed only to decide which side's legacy credentials should feed
     * the one-time list-format migration in nvs_load_saved_nets(): whichever
     * whole config "wins" the adopt decision below is also whichever legacy
     * credentials win, so s_legacy_single is set on every return path. */
    saved_net_t wifi_nvs_legacy_net;
    bool wifi_nvs_has_legacy = false;
    nvs_load_legacy_single(WIFI_NVS_PARTITION, &wifi_nvs_legacy_net, &wifi_nvs_has_legacy);

    bool found_in_default = false;
    esp_err_t err = wifi_prov_nvs_load_from(NVS_DEFAULT_PART_NAME, &found_in_default);
    if (err != ESP_OK || !found_in_default) {
        s_wifi = from_wifi_nvs;
        s_legacy_single.has = wifi_nvs_has_legacy;
        s_legacy_single.net = wifi_nvs_legacy_net;
        return;
    }

    saved_net_t default_legacy_net;
    bool default_has_legacy = false;
    nvs_load_legacy_single(NVS_DEFAULT_PART_NAME, &default_legacy_net, &default_has_legacy);

    /* Adopt the old copy unless the new home already holds a verified
     * saved_nets record. Namespace/mode existence (found_in_wifi_nvs) is NOT
     * the test: the first migration step (nvs_save_mode) creates the
     * namespace before the credential is persisted, so a failed or
     * interrupted migration would otherwise look "already migrated" and have
     * its still-only copy erased as stale. Once a verified record exists the
     * new location is the source of truth and the legacy copy is stale. */
    (void)found_in_wifi_nvs;
    bool adopt = !nvs_saved_nets_record_present(WIFI_NVS_PARTITION);
    if (!adopt) {
        /* The destination holds a verified saved_nets record: it is the source
         * of truth and the legacy copy is stale. Drop it so no reset path (recovery wifi reset,
         * forget-last-network) can ever resurrect it. Best effort. */
        esp_err_t stale_err = legacy_default_nvs_erase_wifi();
        if (stale_err != ESP_OK) {
            ESP_LOGW(WIFI_PROV_TAG, "erasing stale legacy wifi_cfg failed: %s", esp_err_to_name(stale_err));
        }
        s_wifi = from_wifi_nvs;
        s_legacy_single.has = wifi_nvs_has_legacy;
        s_legacy_single.net = wifi_nvs_legacy_net;
        return;
    }

    ESP_LOGI(WIFI_PROV_TAG, "migrating Wi-Fi config from the default NVS partition to '%s' (ssid '%s', mode %s)",
             WIFI_NVS_PARTITION, default_legacy_net.ssid, s_wifi.mode == WIFI_PROV_MODE_AP ? "AP" : "home");

    s_legacy_single.has = default_has_legacy;
    s_legacy_single.net = default_legacy_net;

    /* Write the mode/AP-identity fields through, same as before -- they're
     * part of the same persisted config and would otherwise silently revert
     * to defaults on the next boot, once this function stops adopting the old
     * copy. The legacy credentials themselves are NOT written back out here
     * (no nvs_save_creds() any more): s_legacy_single is only ever consumed
     * by nvs_load_saved_nets(), which persists it in the new list format, not
     * the old single-key one. Failures are logged and survivable -- s_wifi is
     * already correct for this boot either way, and the migration simply gets
     * retried next time. */
    esp_err_t save_err = nvs_save_mode();
    if (save_err == ESP_OK && s_wifi.has_ap_ssid_override) {
        save_err = nvs_save_ap_ssid();
    }
    if (save_err == ESP_OK && s_wifi.has_ap_password_override) {
        save_err = nvs_save_ap_password();
    }
    if (save_err == ESP_OK) {
        /* Read back and compare before the legacy copy may be erased. */
        struct wifi_prov_state adopted = s_wifi;
        bool rb_found = false;
        esp_err_t rb_err = wifi_prov_nvs_load_from(WIFI_NVS_PARTITION, &rb_found);
        bool same = rb_err == ESP_OK && rb_found && s_wifi.mode == adopted.mode &&
                    s_wifi.has_ap_ssid_override == adopted.has_ap_ssid_override &&
                    s_wifi.has_ap_password_override == adopted.has_ap_password_override &&
                    strcmp(s_wifi.ap_ssid, adopted.ap_ssid) == 0 &&
                    strcmp(s_wifi.ap_password, adopted.ap_password) == 0;
        s_wifi = adopted;
        if (same) {
            s_legacy_erase_pending = true;
        } else {
            ESP_LOGE(WIFI_PROV_TAG, "migration read-back from '%s' differs -- keeping the legacy copy, will retry",
                     WIFI_NVS_PARTITION);
        }
    }
    if (save_err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "migration write to '%s' failed: %s -- running from the old copy this boot, will retry",
                 WIFI_NVS_PARTITION, esp_err_to_name(save_err));
    }
}

/* Loads the current saved-networks list from WIFI_NVS_PARTITION and, the
 * first time this ever runs against a board that has no list saved yet but
 * does have a winning legacy single-network credential (s_legacy_single, set
 * by wifi_prov_migrate_from_default_partition() just before this is called), wraps
 * that single credential into nets[0]/count=1 and persists it in the new
 * format. The old default-partition copy is erased only after that record is
 * written and read back (s_legacy_erase_pending); on any failure it is kept
 * and the whole migration retries next boot. Must be called after migrate_from_default_
 * partition() so s_legacy_single reflects the cross-partition "which copy
 * wins" decision, not just whatever WIFI_NVS_PARTITION alone happened to
 * hold. */
void nvs_load_saved_nets(void)
{
    esp_err_t err = nvs_load_saved_nets_from(WIFI_NVS_PARTITION, &s_wifi.saved_nets);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "saved_nets load from '%s' failed: %s -- starting with an empty list",
                 WIFI_NVS_PARTITION, esp_err_to_name(err));
        memset(&s_wifi.saved_nets, 0, sizeof(s_wifi.saved_nets));
        s_wifi.saved_nets.version = SAVED_NETS_VERSION;
    }

    if (s_saved_nets_refused) {
        /* LOW-2: never overwrite an unreadable/newer record with a migrated one. */
        s_legacy_erase_pending = false;
    } else if (s_wifi.saved_nets.count == 0 && s_legacy_single.has && s_legacy_single.net.ssid[0] != '\0') {
        ESP_LOGI(WIFI_PROV_TAG, "migrating legacy single-network credential ('%s') to the saved-networks list",
                 s_legacy_single.net.ssid);
        s_wifi.saved_nets.nets[0] = s_legacy_single.net;
        s_wifi.saved_nets.count = 1;
        s_wifi.saved_nets.version = SAVED_NETS_VERSION;
        esp_err_t save_err = nvs_save_saved_nets();
        if (save_err != ESP_OK) {
            ESP_LOGE(WIFI_PROV_TAG, "saving migrated saved_nets list to '%s' failed: %s -- will retry next boot",
                     WIFI_NVS_PARTITION, esp_err_to_name(save_err));
            s_legacy_erase_pending = false;
        } else {
            saved_nets_blob_t rb;
            if (nvs_load_saved_nets_from(WIFI_NVS_PARTITION, &rb) != ESP_OK || rb.count != 1 ||
                strcmp(rb.nets[0].ssid, s_legacy_single.net.ssid) != 0 ||
                strcmp(rb.nets[0].password, s_legacy_single.net.password) != 0) {
                ESP_LOGE(WIFI_PROV_TAG, "saved_nets read-back differs -- keeping the legacy copy, will retry");
                s_legacy_erase_pending = false;
            }
        }
    }

    if (s_legacy_erase_pending) {
        s_legacy_erase_pending = false;
        esp_err_t er = legacy_default_nvs_erase_wifi();
        if (er == ESP_OK) {
            ESP_LOGI(WIFI_PROV_TAG, "legacy default-partition wifi_cfg migrated and erased");
        } else {
            ESP_LOGW(WIFI_PROV_TAG, "legacy wifi_cfg erase failed: %s -- will retry next boot", esp_err_to_name(er));
        }
    }
}
