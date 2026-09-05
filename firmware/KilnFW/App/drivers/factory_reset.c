#include "factory_reset.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "http_form.h"
#include "ota_state.h" /* interlocks + challenge/response auth -- see reset_post_handler() */
#include "profiles_builtin.h"
#include "wifi_provision_state.h"

static const char *TAG = "factory_reset";

/* TODO.md 8.1: "reset kiln config" is a per-partition operation now that
 * wifi_nvs/kiln_nvs/profiles_nvs are split -- the whole point of the split
 * is that no scope's erase can reach another scope's data. This module is
 * the explicit-choice front end the TODO asked for: the operator names
 * exactly one of the four scopes below, never a default, and gets exactly
 * that much destroyed.
 *
 * Each scope maps to nvs_flash_erase_partition() on ONE OR MORE of the same
 * three partition names every other module in this codebase already erases
 * from its own recovery path (wifi_prov.c, zones_http.c, rules_http.c,
 * relay_cycles.c, run_state.c, profiles_http.c) -- see those files'
 * nvs_partition_init() for the pattern this deliberately reuses rather than
 * reintroducing a blanket nvs_flash_erase(), which is the exact anti-pattern
 * TODO.md 8.1 was written to retire (fixed 2026-08-12). */
#define WIFI_NVS_PARTITION "wifi_nvs"
#define KILN_NVS_PARTITION "kiln_nvs"
#define PROFILES_NVS_PARTITION "profiles_nvs"

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
} reset_scope_t;

static const char *const kWifiOnly[] = { WIFI_NVS_PARTITION, NULL };
static const char *const kKilnOnly[] = { KILN_NVS_PARTITION, NULL };
static const char *const kProfilesOnly[] = { PROFILES_NVS_PARTITION, NULL };
static const char *const kAll[] = { WIFI_NVS_PARTITION, KILN_NVS_PARTITION, PROFILES_NVS_PARTITION, NULL };

static const reset_scope_t kScopes[] = {
    { "wifi", kWifiOnly, false },
    { "kiln", kKilnOnly, false },
    { "profiles", kProfilesOnly, true },
    { "all", kAll, true },
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
    esp_restart();
}

/* Shared by both entry points (HTTP name-based lookup and the UART SYSTEM
 * task's index-based one): erases every partition in *scope, logs a WARN
 * per partition (success or failure), and unconditionally schedules the
 * delayed reboot -- whatever DID erase needs every module re-initializing
 * clean against it either way, and a partial erase left in place with the
 * old init state is worse than one that reboots and re-observes reality
 * (same reasoning reset_post_handler() always had). Returns the first
 * partition's erase error, if any. */
static esp_err_t execute_scope(const reset_scope_t *scope)
{
    ESP_LOGW(TAG, "factory_reset: scope '%s' requested -- erasing", scope->name);
    esp_err_t first_err = ESP_OK;

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
        esp_err_t err = nvs_flash_erase_partition(part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase_partition('%s') failed: %s", part, esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        } else {
            ESP_LOGW(TAG, "erased NVS partition '%s'", part);
        }
    }

    /* 2026-08-22: PSRAM stack. reboot_task() only vTaskDelay()s and calls
     * esp_restart() -- the NVS erase this function name suggests already
     * happened above, in the CALLER's context, before this task is even
     * created, so nothing on this task's own stack touches flash. */
    xTaskCreatePinnedToCoreWithCaps(reboot_task, "factory_reset_reboot", 2048, NULL, tskIDLE_PRIORITY + 1, NULL,
                                    tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return first_err;
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
    esp_err_t err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/factory_reset) failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
