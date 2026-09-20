#include "factory_reset.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "cfg_fs_mount.h" /* cfg_fs_confirm_format_device() -- "all" scope formats the `cfg`
                            * LittleFS partition too, see reset_scope_t's format_cfg_fs field */
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "hal_wdt.h"
#include "nvs_key_check.h"
#include "http_form.h"
#include "ota_http.h" /* interlocks + challenge/response auth -- see reset_post_handler() */
#include "profile_executor.h" /* firing_stats_cache_invalidate_all() -- see the erase loop in
                                * execute_scope_job() below */
#include "profiles_builtin.h"
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
} reset_scope_t;

static const char *const kWifiOnly[] = { WIFI_NVS_PARTITION, NULL };
static const char *const kKilnOnly[] = { KILN_NVS_PARTITION, NULL };
static const char *const kProfilesOnly[] = { PROFILES_NVS_PARTITION, NULL };
static const char *const kAll[] = { WIFI_NVS_PARTITION, KILN_NVS_PARTITION, PROFILES_NVS_PARTITION, NULL };

static const reset_scope_t kScopes[] = {
    { "wifi", kWifiOnly, false, false },
    { "kiln", kKilnOnly, false, false },
    { "profiles", kProfilesOnly, true, false },
    { "all", kAll, true, true },
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
            ESP_LOGW(TAG, "profiles_builtin_restore_all() reported %s -- the partition erase below "
                          "clears the hidden mask regardless",
                     esp_err_to_name(restore_err));
        } else {
            ESP_LOGW(TAG, "restored every shipped fire schedule to visible");
        }
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
    xTaskCreatePinnedToCoreWithCaps(reboot_task, "factory_reset_reboot", 2048, NULL, tskIDLE_PRIORITY + 1, NULL,
                                    tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ctx.err;
}

esp_err_t factory_reset_execute(factory_reset_scope_t scope)
{
    if ((size_t)scope >= NUM_SCOPES) {
        return ESP_ERR_INVALID_ARG;
    }
    return execute_scope(&kScopes[(size_t)scope]);
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    /* Authenticate FIRST, before the interlock check below: TODO.md flagged
     * this endpoint as "strictly more destructive than POST /api/ota/esp/
     * rollback, which IS challenge-response authenticated" -- this route
     * erases zone config / Wi-Fi credentials / saved profiles and reboots,
     * and until this pass had no authentication at all. It now runs the
     * identical X-Ota-Mac header -> hex-decode -> challenge/HMAC/lockout
     * check every other mutating OTA route runs, via
     * ota_http_authenticate_request() (ota_http.h) -- the header-parsing
     * helper exported from ota_http.c specifically so this file did not have
     * to duplicate it. Its own context (OTA_HTTP_CONTEXT_FACTORY_RESET) means
     * a MAC signed for pushing/rolling back a firmware image cannot double as
     * authorization to wipe the board's configuration, and a wrong-password
     * guess here burns only this route's own 3-strikes budget, not any
     * other's -- same reasoning as every other ota_http_context_t.
     *
     * Auth before the interlock check for the same reason ota_http_check_
     * interlocks()'s own doc comment gives for every other route: an
     * unauthenticated caller must not be able to use this endpoint's refusal
     * reason (which can name a live zone temperature) to learn live kiln
     * telemetry. On refusal, ota_http_authenticate_request() has already sent
     * the response (400 for a malformed header, 403 otherwise); this handler
     * has nothing left to do but return. */
    char ip[46];
    if (!ota_http_authenticate_request(req, OTA_HTTP_CONTEXT_FACTORY_RESET, ip)) {
        return ESP_OK;
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
