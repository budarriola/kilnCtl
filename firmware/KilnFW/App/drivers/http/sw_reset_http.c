#include "sw_reset_http.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "hal_esp_common.h"
#include "hal_wdt.h"
#include "ota_http.h" /* interlocks + challenge/response auth -- same idiom as
                        * factory_reset.c's reset_post_handler(), see there for
                        * the full rationale on auth-before-interlock ordering */
#include "safety_link.h" /* safety_link_send_announce_reboot() -- the existing,
                           * version-independent courtesy notice; see this
                           * file's header comment on what it does NOT do */
#include "wifi_provision_http.h"

static const char *TAG = "sw_reset";

// Set once by sw_reset_http_start(), read-only after -- same pattern as
// ota_http.c's own s_io/s_thermo_bus/ota_http_safety. May be NULL on a board
// with no safety processor commissioned yet; every use below tolerates that.
static SafetyLinkClass *s_safety;

// Runs on its own short-lived task so the "ok" response already queued by
// the handler has a chance to reach the client before esp_restart() tears
// the connection down -- identical shape to factory_reset.c's reboot_task()
// and ota_http_esp.c's ota_rollback_reboot_task().
//
// Pico half: sends SAFETY_CMD_ANNOUNCE_REBOOT (existing courtesy notice,
// KILNLINK_PROTOCOL_VERSION untouched) so SaftyFW's S6b link-dead guard does
// not nuisance-trip on the ESP's brief absence -- the SAME call ota_http_esp.c
// already makes before its own esp_restart(). This is NOT a command that
// makes the Pico reboot: the wire protocol has no such command today (see
// this file's header comment). The owner asked for both processors to
// reboot; only the ESP half is implemented here. Reported, not silently
// dropped.
static void sw_reset_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));

    if (s_safety) {
        esp_err_t announce_err = safety_link_send_announce_reboot(s_safety);
        if (announce_err != ESP_OK) {
            ESP_LOGW(TAG, "sw_reset: safety_link_send_announce_reboot failed (%s) -- "
                          "rebooting anyway, S6b may nuisance-trip on the safety processor",
                     esp_err_to_name(announce_err));
        }
    } else {
        ESP_LOGW(TAG, "sw_reset: no safety link configured this boot -- announce-reboot not sent");
    }

    ESP_LOGW(TAG, "sw_reset: rebooting ESP now (no configuration touched)");
    hal_wdt_reboot(); /* esp_restart() under the hood -- never returns on real hardware */
    vTaskDelete(NULL); /* defensive only, see factory_reset.c's identical comment */
}

static esp_err_t sw_reset_post_handler(httpd_req_t *req)
{
    // Auth first, same reasoning as factory_reset.c's reset_post_handler():
    // an unauthenticated caller must not learn live kiln state from this
    // route's own refusal reason. Its own context (OTA_HTTP_CONTEXT_SW_RESET)
    // means a MAC signed for this route cannot double as authorization for
    // any other destructive or update route, and vice versa.
    char ip[46];
    if (!ota_http_authenticate_request(req, OTA_HTTP_CONTEXT_SW_RESET, ip)) {
        return ESP_OK;
    }

    // Refuse while a firing/autotune is running or any zone's heater is
    // commanded on -- the SAME gate factory_reset.c and kiln_cfg_http.c's
    // apply already use. Rebooting the ESP mid-firing drops control of live
    // heaters (the executor and its relay commands vanish with it); a
    // reboot is not gentler than a config wipe from the kiln's point of
    // view, so it gets exactly the same refusal, not a looser one.
    // ota_http_req_ack_no_safety() carries the same escape hatch factory
    // reset allows for a board with no safety processor -- a board being
    // commissioned still needs a way to clear a stuck state without JTAG.
    char interlock_reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req),
                                                             interlock_reason,
                                                             sizeof(interlock_reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "sw_reset from %s: refused by interlock: %s", ip, interlock_reason);
        return ota_http_send_interlock_refusal(req, gate, interlock_reason);
    }

    // No body to read: unlike /api/factory_reset, this route has no scope
    // to choose -- it always means "reboot both processors, touch nothing
    // else". POST with an empty body is intentional here.

    ESP_LOGW(TAG, "sw_reset from %s: authenticated, rebooting -- no config touched", ip);

    // 2026-08-22-style PSRAM-stack note: sw_reset_reboot_task() only
    // vTaskDelay()s, sends one fire-and-forget UART frame and calls
    // hal_wdt_reboot() -- no flash access on this task's own stack, so
    // unlike factory_reset.c's erase path there is no flash-worker dispatch
    // needed here.
    xTaskCreatePinnedToCoreWithCaps(sw_reset_reboot_task, "sw_reset_reboot", 3072, NULL,
                                    tskIDLE_PRIORITY + 1, NULL, tskNO_AFFINITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    httpd_resp_sendstr(req, "ok -- rebooting the ESP now (safety processor not yet reboot-able over "
                            "the link, see docs); the board will be unreachable for about 10-15 "
                            "seconds, then come back on the same address. No configuration was changed.");
    return ESP_OK;
}

esp_err_t sw_reset_http_start(SafetyLinkClass *safety_or_null)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }
    s_safety = safety_or_null;

    static const httpd_uri_t post_uri = {
        .uri = "/api/sw_reset", .method = HTTP_POST, .handler = sw_reset_post_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/sw_reset) failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
