#include "uart_bridge.h"

#include <math.h>
#include <string.h>

#include "build_info.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/idf_additions.h"
#include "factory_reset.h"
#include "watchdog_cfg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_spi.h" /* ILI9488_PANEL_WIDTH/HEIGHT -- TOUCH_CMD_INJECT bounds check */
#include "danger_mode.h" /* danger_mode_active() -- link-loss watchdog suppression */
#include "heat_interlock.h" /* HEAT_INTERLOCK_REASON_MAX -- IO_CMD_SET_RELAY[_MASK]'s ERR_UPDATING case */
#include "kiln_io.h"
#include "kiln_io_owner.h"
#include "kilnlink/kilnlink_set_ct_cal.h"
#include "kiln_ui.h"
#include "lvgl_port.h"
#include "ota_http.h" /* ota_http_heat_blocked_by_update() -- same ERR_UPDATING case */
#include "relay_authority.h"
#include "settings.h"
#include "stack_margin.h"
#include "telemetry_log.h"
#include "thermo_owner.h"
#include "uart_task_ids.h"
#include "wifi_prov.h"
#include "zones_config_accessors.h"
#include "uart_bridge_internal.h"

static const char *TAG = "uart_bridge";

/* --------------------------------------------------------------------------
 * SYSTEM (task 6) -- unchanged from the fixture firmware
 * ------------------------------------------------------------------------ */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    uart_owner_t *owner;
} system_bridge_ctx_t;

static void system_bridge_task(void *arg)
{
    system_bridge_ctx_t *ctx = (system_bridge_ctx_t *)arg;

    /* 2026-09-08 stack-budget fix (docs/audits/2026-09-08-stack-margin-audit.md,
     * check_system_uart_bridge_stack_budget.py): `msg` used to be a plain
     * ~336-byte uart_proto_message_t stack local, present in system_bridge_task's
     * own frame on EVERY call, not just the deep factory-reset path -- moving
     * it to a heap allocation removes that fixed cost from this task's
     * measured stack depth the same way cfgfs_status_get_handler's request
     * buffers were moved off httpd_worker's stack. Allocated ONCE before the
     * loop, never freed: this task never returns (its `while (true)` runs for
     * the life of the program, same as every other bridge task), so there is
     * no per-iteration malloc/free churn and no "free on every return path"
     * to get wrong -- the one allocation IS the task's lifetime allocation.
     * Internal DRAM, not PSRAM: this buffer is touched from the
     * SYSTEM_CMD_FACTORY_RESET path, which reaches flash (factory_reset_execute()
     * -> cfg_fs_confirm_format_device()), and this codebase's convention
     * (uart_bridge_ext.c's own comment) is PSRAM-backed buffers must never be
     * read from inside a flash operation. */
    uart_proto_message_t *msg = heap_caps_malloc(sizeof(*msg), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (msg == NULL) {
        ESP_LOGE(TAG, "system_bridge_task: heap_caps_malloc(%u B, INTERNAL) failed -- system "
                      "command dispatch (relay/CT-cal/danger-mode/factory-reset/watchdog-cfg) "
                      "unavailable this boot, degrading cleanly instead of dereferencing NULL",
                 (unsigned)sizeof(*msg));
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (uart_protocol_receive(ctx->inbox, msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg->length < 1) {
            ESP_LOGW(TAG, "system: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        switch (msg->payload[0]) {
            case SYSTEM_CMD_RESTART_UART: {
                esp_err_t err = uart_owner_restart(ctx->owner);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "system: UART restarted (RX flushed) by host request");
                } else {
                    ESP_LOGW(TAG, "system: UART restart failed: %s", esp_err_to_name(err));
                }
                break;
            }
            case SYSTEM_CMD_FACTORY_RESET: {
                if (!bridge_args_ok("system", msg, 2)) {
                    break;
                }
                /* No reply either way -- see uart_task_ids.h's doc comment:
                 * the reboot itself (a fresh unsolicited GET_FW_VERSION push
                 * from INFO) is the real confirmation, and this command's own
                 * ACK is already the delivery confirmation. */
                esp_err_t err = factory_reset_execute((factory_reset_scope_t)msg->payload[1]);
                if (err == ESP_ERR_INVALID_ARG) {
                    ESP_LOGW(TAG, "system: FACTORY_RESET scope %u out of range -- rejected, nothing erased",
                             msg->payload[1]);
                } else if (err == FACTORY_RESET_ERR_MODE_GATE_REFUSED) {
                    /* system_mode_gate refused (owner decision Q3) -- a
                     * firing or autotune run is active. Nothing erased, no
                     * reboot -- distinct from the erase-failure case below,
                     * which DOES still reboot. Review fix, 2026-09-25: this
                     * used to compare against ESP_ERR_INVALID_STATE, which
                     * the erase loop itself can also return (partial erase
                     * already happened), making "nothing erased" a lie on
                     * that overlap -- see factory_reset.h's doc comment on
                     * FACTORY_RESET_ERR_MODE_GATE_REFUSED. */
                    ESP_LOGW(TAG, "system: FACTORY_RESET scope %u refused -- a firing, autotune run, "
                                  "zone current sweep or backup restore is active, or another factory reset is "
                                  "already in progress; nothing erased",
                             msg->payload[1]);
                } else if (err == FACTORY_RESET_ERR_REBOOT_FAILED) {
                    ESP_LOGE(TAG, "system: FACTORY_RESET scope %u: storage erased, reboot failed -- "
                                  "power-cycle now (rebooting inline)",
                             msg->payload[1]);
                    factory_reset_reboot_fallback();
                } else if (err != ESP_OK) {
                    ESP_LOGE(TAG, "system: FACTORY_RESET scope %u failed: %s -- a reboot follows only if the "
                                  "erase was dispatched",
                             msg->payload[1], esp_err_to_name(err));
                } else {
                    ESP_LOGW(TAG, "system: FACTORY_RESET scope %u requested by host -- erasing and rebooting",
                             msg->payload[1]);
                }
                break;
            }
            case SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED: {
                /* Query -- reply directly to whoever asked, same convention
                 * as INFO's queries (info_bridge_task() above). */
                uint8_t reply[2];
                reply[0] = SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED;
                reply[1] = watchdog_cfg_panic_disabled() ? 1 : 0;
                esp_err_t err = uart_protocol_send(ctx->proto, msg->device, msg->task_id, UART_TASK_ID_SYSTEM,
                                                   reply, sizeof(reply), BRIDGE_REPLY_ACK_TIMEOUT_MS);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "system: GET_WATCHDOG_PANIC_DISABLED reply failed: %s", esp_err_to_name(err));
                }
                break;
            }
            case SYSTEM_CMD_SET_WATCHDOG_PANIC_DISABLED: {
                if (!bridge_args_ok("system", msg, 2)) {
                    break;
                }
                bool disabled = msg->payload[1] != 0;
                esp_err_t perr = watchdog_cfg_set_panic_disabled(disabled, "UART SYSTEM_CMD_SET_WATCHDOG_PANIC_DISABLED");
                if (perr != ESP_OK) {
                    ESP_LOGW(TAG, "system: SET_WATCHDOG_PANIC_DISABLED did not persist: %s (setting lost at reboot)",
                             esp_err_to_name(perr));
                }
                break;
            }
            case SYSTEM_CMD_SET_TELEMETRY_ENABLED: {
                if (!bridge_args_ok("system", msg, 2)) {
                    break;
                }
                bool enabled = msg->payload[1] != 0;
                telemetry_log_set_enabled(enabled);
                break;
            }
            case SYSTEM_CMD_GET_TELEMETRY_ENABLED: {
                uint8_t reply[2];
                reply[0] = SYSTEM_CMD_GET_TELEMETRY_ENABLED;
                reply[1] = telemetry_log_is_enabled() ? 1 : 0;
                esp_err_t err = uart_protocol_send(ctx->proto, msg->device, msg->task_id, UART_TASK_ID_SYSTEM,
                                                   reply, sizeof(reply), BRIDGE_REPLY_ACK_TIMEOUT_MS);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "system: GET_TELEMETRY_ENABLED reply failed: %s", esp_err_to_name(err));
                }
                break;
            }
            default:
                ESP_LOGW(TAG, "system: unknown subcmd 0x%02X -- rejected", msg->payload[0]);
                bridge_reply_unsupported(ctx->proto, msg, UART_TASK_ID_SYSTEM, msg->payload[0]);
                break;
        }
    }
}

esp_err_t uart_bridge_start_system_task(uart_protocol_t *proto, uart_owner_t *owner)
{
    if (!proto || !owner) {
        return ESP_ERR_INVALID_ARG;
    }

    static system_bridge_ctx_t ctx;
    ctx.proto = proto;
    ctx.owner = owner;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_SYSTEM, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    /* Static, not a local -- stack_margin_register() below keeps this
     * pointer past this function returning, and reads through it fresh on
     * every report (see stack_margin.h), so it must outlive the call. */
    static TaskHandle_t s_system_bridge_task_handle;
    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, "system_uart_bridge", 4096, &ctx, 5,
                                                  &s_system_bridge_task_handle, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_SYSTEM);
        return ESP_ERR_NO_MEM;
    }
    /* TODO.md section 13: internal-only (plain xTaskCreatePinnedToCore(), no
     * MALLOC_CAP_SPIRAM) -- one of the six candidate stacks left unresized
     * pending a real uxTaskGetStackHighWaterMark() reading. */
    stack_margin_register("system_uart_bridge", &s_system_bridge_task_handle, 3072);
    return ESP_OK;
}
