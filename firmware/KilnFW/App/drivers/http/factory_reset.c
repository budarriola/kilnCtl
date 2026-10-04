#include "factory_reset.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h" /* esp_wifi_restore()/esp_wifi_set_storage() -- see the wifi/all scope loop
                        * in execute_scope_job() below,
                        * docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md */
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "cfg_fs_mount.h" /* cfg_fs_confirm_format_device() -- "all" scope formats the `cfg`
                            * LittleFS partition too, see reset_scope_t's format_cfg_fs field */
#include "kiln_scope_cfg_files.h" /* kiln_scope_cfg_files_delete() -- "kiln" scope cfg cleanup */
#include "profiles_scope_cfg_files.h" /* profiles_scope_cfg_files_delete() -- "profiles" scope cfg cleanup */
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "hal_wdt.h"
#include "nvs.h" /* nvs_entry_find()/nvs_entry_next() -- log_net80211_key_count() below, same
                   * primitives as diagnostics_http.c's nvs_keys_get_handler() */
#include "nvs_key_check.h"
#include "http_form.h"
#include "ota_http.h" /* interlocks -- the challenge/response auth this used to also
                        * require was retired 2026-09-29, see reset_post_handler() */
#include "ota_http_internal.h" /* ota_http_get_client_ip() declaration -- logging only */
#include "profile_executor.h" /* firing_stats_cache_invalidate_all() -- see the erase loop in
                                * execute_scope_job() below */
#include "profiles_builtin.h"
#include "relay_authority.h" /* relay_authority_heat_run_active() -- system_mode_gate below */
#include "system_mode_gate.h" /* SYS_ACTION_FACTORY_RESET -- owner decision Q3, 2026-09-25 */
#include "system_mode_gate_http.h" /* system_mode_gate_http_send_refusal() */
#include "wifi_provision_http.h"

static const char *TAG = "factory_reset";

/* Hand-declared rather than #include "uart_bridge.h"/"flash_worker.h" -- same
 * reasoning as relay_cycles.c's/safety_cfg_store.c's identical block: that
 * header pulls in hardware-bridge task declarations this file needs none of,
 * and which are not part of this module's host-test stub surface. Keep in
 * sync with uart_bridge.h/flash_worker.h by hand if either signature ever
 * changes. Used by execute_scope() (see below) so the NVS erase runs on
 * bx_flash_worker's own stack, not the caller's (system_uart_bridge or
 * httpd_worker). */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);
bool uart_bridge_ext_is_on_flash_worker(void);

/* TODO.md 8.1: "reset kiln config" is a per-partition operation now that
 * wifi_nvs/kiln_nvs/profiles_nvs are split -- the whole point of the split
 * is that no scope's erase can reach another scope's data. This module is
 * the explicit-choice front end the TODO asked for: the operator names
 * exactly one of the four scopes below, never a default, and gets exactly
 * that much destroyed.
 *
 * Each scope maps to hal_kv_erase_partition() on ONE OR MORE of the same
 * three partition names every other module in this codebase already erases
 * from its own recovery path (wifi_prov.c, zones_http.c, rules_http.c,
 * relay_cycles.c, run_state.c, profiles_http.c) -- see those files'
 * nvs_partition_init() for the pattern this deliberately reuses rather than
 * reintroducing a blanket nvs_flash_erase(), which is the exact anti-pattern
 * TODO.md 8.1 was written to retire (fixed 2026-08-12). */
#define WIFI_NVS_PARTITION "wifi_nvs"
#define KILN_NVS_PARTITION "kiln_nvs"
#define PROFILES_NVS_PARTITION "profiles_nvs"
NVS_KEY_LEN_CHECK(WIFI_NVS_PARTITION);
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(PROFILES_NVS_PARTITION);

/* application/x-www-form-urlencoded, "scope=profiles" plus headroom --
 * generous over the longest legal value ("profiles", 8 chars) the same way
 * every other POST body bound in this codebase is generous over its longest
 * legal input. Checked against Content-Length before a single byte is read. */
#define FACTORY_RESET_BODY_MAX 64
#define SCOPE_VALUE_MAX 16

/* Embedded via EMBED_TXTFILES in CMakeLists.txt -- same convention as every
 * other embedded page in this codebase. main_page.html grew a "Danger zone"
 * section that POSTs here directly; there is no separate factory_reset page. */

typedef struct {
    const char *name;
    const char *const *partitions; /* NULL-terminated */
    /* "Reset fire profiles" has to mean both halves of what a user sees on
     * the /profiles page: the user slots are cleared AND every shipped
     * schedule is visible again. The hidden-mask that removes a built-in
     * from the listings lives in profiles_nvs, so erasing the partition does
     * clear it -- but only as a side effect, and only for whoever reads it
     * after the reboot; the copy profiles_builtin.c holds in RAM would
     * still say "hidden" until then. Calling profiles_builtin_restore_all()
     * explicitly makes the intent part of the scope definition rather than
     * an accident of storage layout, and re-syncs the RAM copy. */
    bool restore_builtin_profiles;

    /* Owner decision 2026-09-07 (docs/FILESYSTEM_USER_DATA_PLAN.md section 5
     * step 1, "The reset section of the webpage should format when
     * reseting"): "all" is the one scope that also erases and reformats the
     * `cfg` LittleFS partition -- the same partition zones_config_cfg_fs.c/
     * profiles_cfg_fs.c/pref_cfg_fs.c dual-write onto. A NARROWER scope
     * (wifi/kiln/profiles) must NOT touch it: those buttons promise "erases
     * ONLY what it says" (settings_page.html's own copy), and cfg holds a
     * MIX of kiln-config-shaped and profile-shaped data today -- formatting
     * it under "kiln" or "profiles" alone would silently destroy the other
     * category too, breaking that promise. Only "all" (erase everything) is
     * honest about reaching it. */
    bool format_cfg_fs;

    /* "kiln" only: delete the cfg_fs mirrors whose NVS side lives in
     * kiln_nvs (kiln_scope_cfg_files.h). Narrower than format_cfg_fs: the
     * scope deletes ONLY its own mirror files, never formats. Without it a
     * surviving file wins the next boot's resolve and undoes the reset. */
    bool delete_kiln_cfg_files;

    /* "profiles" only: delete the cfg_fs mirrors whose NVS side lives in
     * profiles_nvs -- profile slot files and firing-history files
     * (profiles_scope_cfg_files.h). hidden.json is handled by the
     * restore_builtin_profiles branch. */
    bool delete_profiles_cfg_files;
} reset_scope_t;

static const char *const kWifiOnly[] = { WIFI_NVS_PARTITION, NULL };
static const char *const kKilnOnly[] = { KILN_NVS_PARTITION, NULL };
static const char *const kProfilesOnly[] = { PROFILES_NVS_PARTITION, NULL };
static const char *const kAll[] = { WIFI_NVS_PARTITION, KILN_NVS_PARTITION, PROFILES_NVS_PARTITION, NULL };

static const reset_scope_t kScopes[] = {
    { "wifi", kWifiOnly, false, false, false, false },
    { "kiln", kKilnOnly, false, false, true, false },
    { "profiles", kProfilesOnly, true, false, false, true },
    { "all", kAll, true, true, false, false },
};
#define NUM_SCOPES (sizeof(kScopes) / sizeof(kScopes[0]))

/* Delays briefly before esp_restart() so the "ok" response the handler
 * already queued has a chance to actually reach the client's socket buffer
 * first -- calling esp_restart() directly from the HTTP handler's own task
 * risks tearing the connection down before httpd flushes the reply, leaving
 * the operator's browser with a dropped connection instead of the
 * confirmation that the erase actually happened. Runs on its own short-lived
 * task rather than blocking the handler itself, so the handler can return
 * (and the httpd task can move on to its next connection) immediately. */
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "rebooting now to bring every module up clean against the erased partition(s)");
    hal_wdt_reboot(); /* esp_restart() under the hood on this backend; never returns -- see hal_wdt.h */
    vTaskDelete(NULL); /* defensive only: hal_wdt_reboot() is not declared noreturn (the host
                         * fake deliberately returns so tests can observe the call -- see
                         * fake_wdt.c), so this guards a real backend that somehow returns
                         * instead of falling off the end of a FreeRTOS task function. */
}

/* Bench self-check for docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md:
 * an HTTP-only nvs_list_keys poll cannot reach this window (STA drops when
 * esp_wifi_restore() tears the link down, and the reboot below follows
 * shortly after), so the board has to report the result itself. Counts
 * entries in the `nvs` partition's `net80211` namespace -- the exact
 * nvs_entry_find()/nvs_entry_next() primitives nvs_keys_get_handler()
 * (diagnostics_http.c) already uses for the same partition/namespace pair --
 * and logs the count only, never a key name or value. Runs on
 * bx_flash_worker (10240 B stack, internal SRAM); the nvs_iterator_t local
 * here is the same small, fixed-size struct that handler already carries on
 * a task stack, so this adds no measurable ceiling risk. */
static void log_net80211_key_count(const char *when)
{
    size_t count = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", "net80211", NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        count++;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    ESP_LOGW(TAG, "factory_reset: nvs/net80211 key count %s esp_wifi_restore(): %u",
             when, (unsigned)count);
}

/* Shared by both entry points (HTTP name-based lookup and the UART SYSTEM
 * task's index-based one): erases every partition in *scope, logs a WARN
 * per partition (success or failure), and unconditionally schedules the
 * delayed reboot -- whatever DID erase needs every module re-initializing
 * clean against it either way, and a partial erase left in place with the
 * old init state is worse than one that reboots and re-observes reality
 * (same reasoning reset_post_handler() always had). Returns the first
 * partition's erase error, if any. */
/* The actual erase, run ON bx_flash_worker's own internal-SRAM stack rather
 * than the caller's -- see uart_bridge_ext_run_on_flash_worker()'s doc
 * comment (flash_worker.h). This path is reachable both from an HTTP POST
 * (httpd_worker, 8192 B stack, comfortable) and from the UART SYSTEM task's
 * SYSTEM_CMD_FACTORY_RESET (system_uart_bridge, 3072 B, reporting only 884 B
 * / 28.8% free at idle -- get_stack_margin()). nvs_flash_erase_partition()'s
 * own depth has never been exercised on that task by the idle-board soak
 * (this route is destructive and not something a soak test triggers), so its
 * true peak on that stack is unmeasured; dispatching it onto the worker's
 * larger, purpose-built stack removes the question rather than betting the
 * remaining headroom on an unmeasured path -- same reasoning as
 * relay_cycles.c's reset_persist_job() and safety_cfg_store.c's
 * nvs_save_store_job(). */
typedef struct {
    const reset_scope_t *scope;
    esp_err_t err;
} execute_scope_job_ctx_t;

static void execute_scope_job(void *arg)
{
    execute_scope_job_ctx_t *ctx = (execute_scope_job_ctx_t *)arg;
    const reset_scope_t *scope = ctx->scope;
    esp_err_t first_err = ESP_OK;

    ESP_LOGW(TAG, "factory_reset: scope '%s' requested -- erasing", scope->name);

    /* Before the erase, not after: nvs_flash_erase_partition() de-initializes
     * the partition it wipes, so a save attempted afterwards would have
     * nowhere to go. Doing it here leaves the RAM mask cleared and the flash
     * copy both written and then erased -- consistent either way, and the
     * shipped schedules are visible again immediately rather than only after
     * the reboot below. */
    if (scope->restore_builtin_profiles) {
        esp_err_t restore_err = profiles_builtin_restore_all();
        if (restore_err != ESP_OK) {
            ESP_LOGW(TAG, "profiles_builtin_restore_all() reported %s -- the file is deleted below and the "
                          "partition erase clears the NVS copy",
                     esp_err_to_name(restore_err));
        } else {
            ESP_LOGW(TAG, "restored every shipped fire schedule to visible");
        }
        /* The mask also lives at /cfg/profiles/hidden.json, which the NVS
         * erase below does NOT reach; delete it unconditionally so a stale
         * file cannot win the next boot's resolve. */
        (void)profiles_builtin_discard_file();
    }

    for (size_t i = 0; scope->partitions[i] != NULL; i++) {
        const char *part = scope->partitions[i];
        hal_status_t st = hal_kv_erase_partition(part);
        if (st != HAL_OK) {
            esp_err_t err = hal_status_to_esp_err(st);
            ESP_LOGE(TAG, "hal_kv_erase_partition('%s') failed: %s", part, hal_status_to_name(st));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        } else {
            ESP_LOGW(TAG, "erased NVS partition '%s'", part);
        }
    }

    /* docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md section 3
     * part B: clear the SEPARATE copy of the STA/AP config ESP-IDF's Wi-Fi
     * driver keeps in the default `nvs` partition's nvs.net80211 namespace --
     * a partition no scope's erase loop above may touch wholesale, because
     * kiln_auth (the web admin credential, WEB_AUTH_PLAN 12b) shares it. Only
     * for scopes whose partition list includes wifi_nvs (today: "wifi" and
     * "all") -- "kiln"/"profiles" never touch Wi-Fi state and must not call
     * this. esp_wifi_restore() resets "esp_wifi_set_config related" settings
     * to default, which is the STA/AP SSID+password the app wrote via
     * wifi_prov_link.c.
     *
     * Ordering caveat (audit section 3, unresolved from source/header review
     * alone -- esp_wifi_restore()'s own doc comment does not say whether it
     * still erases the flash-backed blob once storage mode is RAM, which
     * wifi_prov.c's own fix now sets at boot): bracket the call with an
     * explicit FLASH/RAM round-trip so the restore always runs against
     * flash-backed storage regardless of whatever mode esp_wifi_init() left
     * it in, then put it back to RAM so no later esp_wifi_set_config() call
     * this boot (there are none between here and the reboot, but this is
     * cheap insurance) starts writing flash again. Confirmed on hardware only
     * via a bench nvs.net80211 dump per the audit's test plan step 6, not by
     * this change alone.
     *
     * Second thing the bench must check (code review 2026-09-21): this runs
     * inside execute_scope(), which reset_post_handler() calls BEFORE it
     * sends its "ok, rebooting" reply. esp_wifi_restore() also resets mode
     * (esp_wifi.h's own list: bandwidth, protocol, set_config-related, mode),
     * so if the driver tears the STA link down synchronously here, the
     * operator's browser gets a dropped connection instead of that reply --
     * the board still erases and still reboots, so this is a UX regression,
     * not a safety one. It is NOT fixable by moving this into reboot_task():
     * that task runs on a PSRAM stack and esp_wifi_restore() writes NVS.
     * Confirm over the WEB route (not UART) that the reply still arrives. */
    for (size_t i = 0; scope->partitions[i] != NULL; i++) {
        if (strcmp(scope->partitions[i], WIFI_NVS_PARTITION) != 0) {
            continue;
        }
        esp_err_t store_flash_err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        if (store_flash_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_storage(FLASH) before restore failed: %s", esp_err_to_name(store_flash_err));
        }
        log_net80211_key_count("before");
        esp_err_t restore_err = esp_wifi_restore();
        log_net80211_key_count("after");
        if (restore_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_restore() failed: %s -- the Wi-Fi driver's own persisted "
                     "config may survive this reset", esp_err_to_name(restore_err));
        } else {
            ESP_LOGW(TAG, "cleared the Wi-Fi driver's own persisted config (nvs.net80211)");
        }
        esp_err_t store_ram_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (store_ram_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_storage(RAM) after restore failed: %s", esp_err_to_name(store_ram_err));
        }
        break;
    }

    /* "fs_<id>" firing-history blobs live in PROFILES_NVS_PARTITION, and the
     * erase above wiped them without going through firing_stats_erase() --
     * the one path that normally keeps profile_executor_last_run_started_
     * unix_s()'s RAM cache in step with NVS. Drop the cache here so
     * GET /api/profiles cannot keep reporting pre-erase "last run" times
     * during the (delayed, and not guaranteed) reboot scheduled below.
     * Unconditional on the erase result, same best-effort reasoning as
     * firing_stats_erase() itself: if the partition is gone or half-gone,
     * a cached value from before it is wrong either way.
     * "wifi"/"kiln" never list that partition, so this only fires for
     * "profiles"/"all" -- checked against the list rather than assumed. */
    for (size_t i = 0; scope->partitions[i] != NULL; i++) {
        if (strcmp(scope->partitions[i], PROFILES_NVS_PARTITION) == 0) {
            firing_stats_cache_invalidate_all();
            break;
        }
    }

    /* Formats the `cfg` LittleFS partition too, for "all" only -- see
     * reset_scope_t's format_cfg_fs doc comment. Runs AFTER the NVS erases
     * above for the same "before vs. after" reasoning as the builtin-
     * profile restore: this whole job already runs on the flash worker (see
     * execute_scope() below), and cfg_fs_confirm_format_device() detects
     * that (uart_bridge_ext_is_on_flash_worker()) and runs its own format
     * inline instead of dispatching again -- calling it from here, rather
     * than from execute_scope() before dispatch, is what makes that
     * same-worker fast path apply. This IS the explicit operator action the
     * mount-failure contract requires (docs/FILESYSTEM_USER_DATA_PLAN.md
     * section 3 point 4) -- clicking "Factory default" already carries the
     * same confirm-dialog the danger-zone buttons all require
     * (settings_page.html's kcConfirm()), so this never goes through the
     * ask-first / awaiting-confirmation path cfg_fs_mount.c's boot-time
     * auto-format gate uses. */
    if (scope->format_cfg_fs) {
        esp_err_t cfg_fmt_err = cfg_fs_confirm_format_device();
        if (cfg_fmt_err != ESP_OK) {
            ESP_LOGE(TAG, "cfg_fs_confirm_format_device() failed during factory reset: %s",
                     esp_err_to_name(cfg_fmt_err));
            if (first_err == ESP_OK) {
                first_err = cfg_fmt_err;
            }
        } else {
            ESP_LOGW(TAG, "cfg LittleFS partition formatted as part of factory reset");
        }
    }

    /* delete_kiln_cfg_files: kiln_nvs is erased above but its dual-written
     * cfg mirrors survive, and a file would win the next boot's resolve with
     * stale data (and the RAM revs would rewrite pre-reset values). Delete
     * them here on the flash worker; a file that cannot be deleted FAILS the
     * reset rather than only logging. */
    if (scope->delete_kiln_cfg_files) {
        int n = 0;
        esp_err_t kerr = kiln_scope_cfg_files_delete(&n);
        ESP_LOGW(TAG, "kiln factory reset: %d cfg file(s) deleted", n);
        if (kerr != ESP_OK && first_err == ESP_OK) {
            first_err = kerr;
        }
    }

    /* delete_profiles_cfg_files: same reasoning as above for profiles_nvs --
     * stale slot/history files would win the next boot's resolve. */
    if (scope->delete_profiles_cfg_files) {
        int n = 0;
        esp_err_t perr = profiles_scope_cfg_files_delete(&n);
        ESP_LOGW(TAG, "profiles factory reset: %d cfg file(s) deleted", n);
        if (perr != ESP_OK && first_err == ESP_OK) {
            first_err = perr;
        }
    }

    ctx->err = first_err;
}

static esp_err_t execute_scope(const reset_scope_t *scope)
{
    execute_scope_job_ctx_t ctx = { .scope = scope, .err = ESP_FAIL };

    /* RE-ENTRANCY (flash_worker_lint.py's pattern 1): neither known caller
     * (the HTTP handler below, or the UART SYSTEM bridge task) is expected
     * to already be on the worker today, but the check is cheap and this is
     * exactly the class of bug that stays invisible until a caller changes
     * -- same guard as relay_cycles_reset()/adaptive_tune.c. */
    if (uart_bridge_ext_is_on_flash_worker()) {
        execute_scope_job(&ctx);
    } else {
        esp_err_t submit_err = uart_bridge_ext_run_on_flash_worker(execute_scope_job, &ctx);
        if (submit_err != ESP_OK) {
            ESP_LOGE(TAG, "factory_reset: could not dispatch erase to flash worker: %s",
                     esp_err_to_name(submit_err));
            return submit_err;
        }
    }

    /* 2026-08-22: PSRAM stack. reboot_task() only vTaskDelay()s and calls
     * esp_restart() -- the NVS erase above already happened (either inline
     * or on the flash worker, both blocking this call until done), so
     * nothing on this task's own stack touches flash. */
    if (xTaskCreatePinnedToCoreWithCaps(reboot_task, "factory_reset_reboot", 2048, NULL, tskIDLE_PRIORITY + 1, NULL,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        /* No reboot means the RAM state (revs included) would keep running
         * against just-erased storage and rewrite pre-reset data. Fail loud. */
        ESP_LOGE(TAG, "factory_reset: could not create the reboot task -- reset NOT complete");
        return ESP_ERR_NO_MEM;
    }
    return ctx.err;
}

esp_err_t factory_reset_execute(factory_reset_scope_t scope)
{
    if ((size_t)scope >= NUM_SCOPES) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Owner decision Q3 (docs/SYSTEM_MODE_GATE_PLAN.md, 2026-09-25,
     * gate-slices-2/4/5 spec): refuse outright while a firing or autotune run
     * is active, PAUSED included -- same unconditional rule reset_post_handler()
     * (HTTP) already enforces above via execute_scope() directly. This
     * function is UART's own entry point (uart_bridge_system.c's
     * SYSTEM_CMD_FACTORY_RESET calls it directly, never through
     * reset_post_handler()) and had NO such gate until this review fix --
     * a firing could be wiped mid-run over UART with no refusal at all.
     * Checked here rather than in the UART case itself so this is the one
     * choke point both transports that call factory_reset_execute() share;
     * reset_post_handler() is unaffected since it never calls this function. */
    {
        sys_mode_snapshot_t mode_snap = { 0 };
        relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
        char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
        mode_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_FACTORY_RESET, &mode_snap, mode_reason, sizeof(mode_reason))) {
            ESP_LOGW(TAG, "factory_reset_execute: refused by system mode gate: %s", mode_reason);
            return FACTORY_RESET_ERR_MODE_GATE_REFUSED;
        }
    }

    return execute_scope(&kScopes[(size_t)scope]);
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    /* Log the client IP FIRST, before the interlock check below:
     * route_tier_table.h's ADMIN tier is the only gate on this route (the
     * AP-password HMAC it used to also require was retired 2026-09-29 --
     * WEB_AUTH_PLAN.md item 2b, owner decision "Retire; open when login
     * off"), enforced by the dispatcher before this handler ever runs --
     * same as every other ADMIN route.
     *
     * Still logged before the interlock check for the same reason
     * ota_http_check_interlocks()'s own doc comment gives for every other
     * route: an unauthenticated caller must not be able to use this
     * endpoint's refusal reason (which can name a live zone temperature) to
     * learn live kiln telemetry. */
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip)); /* logging only -- ADMIN tier (route_tier_table.h) is the only gate, AP-password HMAC retired 2026-09-29 */

    /* Owner decision Q3 (docs/SYSTEM_MODE_GATE_PLAN.md, 2026-09-25,
     * gate-slices-2/4/5 spec): refuse outright while a firing or autotune run
     * is active, PAUSED included -- no ack, no override, unconditional. This
     * is a distinct 409 from the OTA interlock's own 428/409 check just
     * below, checked first since it can never be answered by an ack header
     * the way that one can. */
    {
        sys_mode_snapshot_t mode_snap = { 0 };
        relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
        char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
        mode_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_FACTORY_RESET, &mode_snap, mode_reason, sizeof(mode_reason))) {
            ESP_LOGW(TAG, "factory_reset from %s: refused by system mode gate: %s", ip, mode_reason);
            return system_mode_gate_http_send_refusal(req, mode_reason);
        }
    }

    /* Refuse while a firing is running/paused or any heater is commanded on
     * (audit 2026-08-27: this endpoint had NO interlock and NO authentication
     * at all). It erases the zone config -- relay maps, guard thresholds,
     * max_temp_c -- and reboots. Doing that mid-firing leaves elements hot
     * with an executor that has no configuration to control them by, and if
     * the scope includes wifi, the board also drops off the network, so the
     * operator cannot reach it to shut anything down.
     *
     * ota_http_check_interlocks() is the same gate backup_http.c's restore
     * and kiln_cfg_http.c's apply already use -- both of which write the very
     * same zones_cfg_t this erases, and neither of which is as destructive.
     * ota_http_req_ack_no_safety() carries the same "a board with no safety
     * processor must still be recoverable" escape hatch those two allow, and
     * for the same reason: a factory reset is part of how an uncommissioned
     * board gets commissioned. Every other precondition still refuses. */
    char interlock_reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req),
                                                             interlock_reason,
                                                             sizeof(interlock_reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "factory_reset from %s: refused by interlock: %s", ip, interlock_reason);
        return ota_http_send_interlock_refusal(req, gate, interlock_reason);
    }

    if (req->content_len <= 0 || req->content_len > FACTORY_RESET_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[FACTORY_RESET_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "factory_reset body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char scope_val[SCOPE_VALUE_MAX];
    int len = http_form_find_field(body, "scope", scope_val, sizeof(scope_val));
    if (len <= 0) {
        /* No default scope, on purpose -- TODO.md 8.1 flags this as a
         * genuinely destructive, hard-to-reverse action on real hardware.
         * Guessing "all" (or anything else) for a missing/oversized field
         * is exactly the failure mode an explicit-choice UI exists to rule
         * out. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "scope missing or malformed -- must be one of "
                                                          "wifi, kiln, profiles, all");
        return ESP_OK;
    }

    const reset_scope_t *scope = NULL;
    for (size_t i = 0; i < NUM_SCOPES; i++) {
        if (strcmp(scope_val, kScopes[i].name) == 0) {
            scope = &kScopes[i];
            break;
        }
    }
    if (!scope) {
        ESP_LOGW(TAG, "factory_reset request with unrecognized scope '%s'", scope_val);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unrecognized scope -- must be one of "
                                                          "wifi, kiln, profiles, all");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "factory_reset from %s: authenticated, scope '%s' accepted", ip, scope->name);
    esp_err_t first_err = execute_scope(scope);

    if (first_err != ESP_OK) {
        /* Best-effort is not good enough here: an operator who asked for a
         * wipe and silently got a partial one (e.g. "all" that only erased
         * two of three partitions) needs to know, not just see a reboot and
         * assume it worked. execute_scope() already scheduled the reboot
         * regardless. */
        char msg[64];
        snprintf(msg, sizeof(msg), "erase failed for one or more partitions: %s", esp_err_to_name(first_err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, msg);
    } else {
        /* Name what was restored, not just what was erased: "reset fire
         * profiles" that only ever says "ok" leaves the operator guessing
         * whether the shipped schedules came back. The reboot is stated
         * because every listing the UI is showing right now is about to be
         * re-read from an erased partition. */
        if (scope->restore_builtin_profiles) {
            httpd_resp_sendstr(req, "ok -- saved profiles cleared and all shipped schedules restored; "
                                    "rebooting");
        } else {
            httpd_resp_sendstr(req, "ok, rebooting");
        }
    }
    return ESP_OK;
}

esp_err_t factory_reset_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t post_uri = {
        .uri = "/api/factory_reset", .method = HTTP_POST, .handler = reset_post_handler,
    };
    esp_err_t err = kiln_http_register(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/factory_reset) failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
