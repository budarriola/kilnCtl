/* wifi_prov.c split (2026-09-04, ROADMAP.md M15 A3) -- NVS load/save and the
 * one-time cross-partition/legacy-format migrations. See
 * wifi_prov_internal.h's top-of-file comment for the full split rationale
 * and file map. Move-only: no logic, ordering, naming or visibility change
 * beyond widening former `static` symbols this split's sibling files now
 * call directly. */

#include "wifi_prov_internal.h"

#include <string.h>

#include "esp_log.h"

#include "nvs.h"
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
#define NVS_NAMESPACE "wifi_cfg"

#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_HAS_CREDS "has_creds"
/* Legacy single-network keys (NVS_KEY_SSID/NVS_KEY_PASS/NVS_KEY_HAS_CREDS)
 * are never written by this build any more -- see NVS_KEY_SAVED_NETS below --
 * but are still READ once, by the one-time list-format migration in
 * nvs_load_saved_nets(), for boards provisioned by firmware that predates
 * TODO.md 8.4's bounded-list rework. Left in place afterward, same rationale
 * as every other "old copy stays, never re-read" migration in this file. */
#define NVS_KEY_SAVED_NETS "saved_nets"
#define NVS_KEY_MODE "mode"           /* u8: 0 = WIFI_PROV_MODE_HOME, 1 = WIFI_PROV_MODE_AP */
#define NVS_KEY_LOCAL_ONLY "local_only" /* legacy, read-only: pre-2026-08-11 firmware's
                                          * only mode flag. Migrated into NVS_KEY_MODE the
                                          * first time this runs against an old NVS blob;
                                          * never written by this build. See nvs_load(). */
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_HAS_AP_SSID "has_ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"
#define NVS_KEY_HAS_AP_PASS "has_ap_pass"

/* 2026-08-20, web-GUI-only static-IP addition (see wifi_prov.h's "Static IP"
 * section). New keys, same partition/namespace as everything else in this
 * file -- never repurposing an existing key. Absent (first boot, or a board
 * that predates this feature) reads back as DHCP with empty strings, which
 * is exactly today's always-on default behavior. */
#define NVS_KEY_IP_MODE "ip_mode" /* u8: 0 = DHCP, 1 = STATIC */
#define NVS_KEY_STATIC_IP "static_ip"
#define NVS_KEY_STATIC_NETMASK "static_netmask"
#define NVS_KEY_STATIC_GW "static_gw"

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
    err = nvs_get_u8(h, NVS_KEY_MODE, &u8);
    if (err == ESP_OK) {
        s_wifi.mode = u8 ? WIFI_PROV_MODE_AP : WIFI_PROV_MODE_HOME;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t legacy_local_only = 0;
        esp_err_t legacy_err = nvs_get_u8(h, NVS_KEY_LOCAL_ONLY, &legacy_local_only);
        if (legacy_err == ESP_OK && legacy_local_only) {
            ESP_LOGI(WIFI_PROV_TAG, "migrating legacy local_only=1 NVS flag to mode=AP");
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

    /* 2026-08-20, web-GUI-only static-IP fields. Absent (pre-feature NVS
     * blob, or genuinely never configured) reads back as DHCP with empty
     * strings -- today's always-on default, unchanged. */
    u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_IP_MODE, &u8);
    s_wifi.ip_mode = (err == ESP_OK && u8) ? WIFI_PROV_IP_MODE_STATIC : WIFI_PROV_IP_MODE_DHCP;

    len = sizeof(s_wifi.static_ip);
    err = nvs_get_str(h, NVS_KEY_STATIC_IP, s_wifi.static_ip, &len);
    if (err != ESP_OK) {
        s_wifi.static_ip[0] = '\0';
    }
    len = sizeof(s_wifi.static_netmask);
    err = nvs_get_str(h, NVS_KEY_STATIC_NETMASK, s_wifi.static_netmask, &len);
    if (err != ESP_OK) {
        s_wifi.static_netmask[0] = '\0';
    }
    len = sizeof(s_wifi.static_gateway);
    err = nvs_get_str(h, NVS_KEY_STATIC_GW, s_wifi.static_gateway, &len);
    if (err != ESP_OK) {
        s_wifi.static_gateway[0] = '\0';
    }

    nvs_close(h);
    return ESP_OK;
}

/* Every writer below targets WIFI_NVS_PARTITION unconditionally. The old
 * default-partition copy is deliberately never written again (nor deleted --
 * see the migration note in wifi_prov_start()), so a rollback to firmware that
 * predates the split still finds the credentials it knew about, just frozen at
 * whatever they were when this build first ran. */

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

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return; /* namespace/partition absent -- nothing legacy to find */
    }

    size_t len = sizeof(out_net->ssid);
    err = nvs_get_str(h, NVS_KEY_SSID, out_net->ssid, &len);
    if (err != ESP_OK) {
        out_net->ssid[0] = '\0';
    }

    len = sizeof(out_net->password);
    err = nvs_get_str(h, NVS_KEY_PASS, out_net->password, &len);
    if (err != ESP_OK) {
        out_net->password[0] = '\0';
    }

    uint8_t u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_HAS_CREDS, &u8);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_has = out_net->ssid[0] != '\0';
    } else {
        *out_has = (err == ESP_OK) && u8;
    }

    nvs_close(h);
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
static esp_err_t nvs_load_saved_nets_from(const char *partition, saved_nets_blob_t *out_blob)
{
    memset(out_blob, 0, sizeof(*out_blob));

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(*out_blob);
    err = nvs_get_blob(h, NVS_KEY_SAVED_NETS, out_blob, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "saved_nets blob read from '%s' failed (%s) -- treating as empty",
                 partition, esp_err_to_name(err));
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (len != sizeof(*out_blob)) {
        ESP_LOGW(WIFI_PROV_TAG, "saved_nets blob from '%s' is the wrong size -- treating as empty", partition);
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }

    if (out_blob->version == SAVED_NETS_VERSION) {
        return ESP_OK; /* current version -- happy path */
    }
    if (out_blob->version < SAVED_NETS_VERSION) {
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
    memset(out_blob, 0, sizeof(*out_blob));
    out_blob->version = SAVED_NETS_VERSION;
    return ESP_OK;
}

esp_err_t nvs_save_saved_nets(void)
{
    s_wifi.saved_nets.version = SAVED_NETS_VERSION;

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_SAVED_NETS, &s_wifi.saved_nets, sizeof(s_wifi.saved_nets));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t nvs_save_mode(void)
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

esp_err_t nvs_save_ap_ssid(void)
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

esp_err_t nvs_save_ap_password(void)
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

/* 2026-08-20, web-GUI-only: persists ip_mode + the three static-IP strings
 * together (they only ever change as a group -- see do_set_dhcp()/
 * do_set_static_ip()). Same partition/namespace/commit pattern as every
 * other nvs_save_*() in this file. */
esp_err_t nvs_save_ip_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_IP_MODE, s_wifi.ip_mode == WIFI_PROV_IP_MODE_STATIC ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_STATIC_IP, s_wifi.static_ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_STATIC_NETMASK, s_wifi.static_netmask);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_STATIC_GW, s_wifi.static_gateway);
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
esp_err_t wifi_prov_nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(WIFI_PROV_TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
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

    /* Adopt the old copy when the new home has nothing at all (the true
     * first-boot-after-the-split case), or when the new home exists but has no
     * legacy credentials while the old one does -- which is what a board looks
     * like if it reached the new firmware, saved only an AP-identity override,
     * and still has its real network sitting in the old partition. Anything
     * the new partition has already recorded otherwise wins outright; the new
     * location is the source of truth from the moment it holds credentials. */
    bool adopt = !found_in_wifi_nvs || (default_has_legacy && !wifi_nvs_has_legacy);
    if (!adopt) {
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
 * format. The old single-key entries are never deleted and never read again
 * after this -- same "leave the old copy in place" rationale as every other
 * migration in this file. Must be called after migrate_from_default_
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

    if (s_wifi.saved_nets.count == 0 && s_legacy_single.has && s_legacy_single.net.ssid[0] != '\0') {
        ESP_LOGI(WIFI_PROV_TAG, "migrating legacy single-network credential ('%s') to the saved-networks list",
                 s_legacy_single.net.ssid);
        s_wifi.saved_nets.nets[0] = s_legacy_single.net;
        s_wifi.saved_nets.count = 1;
        s_wifi.saved_nets.version = SAVED_NETS_VERSION;
        esp_err_t save_err = nvs_save_saved_nets();
        if (save_err != ESP_OK) {
            ESP_LOGE(WIFI_PROV_TAG, "saving migrated saved_nets list to '%s' failed: %s -- will retry next boot",
                     WIFI_NVS_PARTITION, esp_err_to_name(save_err));
        }
    }
}
