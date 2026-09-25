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
#include "thermo_owner.h"
#include "uart_task_ids.h"
#include "wifi_prov.h"
#include "zones_config_accessors.h"
#include "uart_bridge_internal.h"

static const char *TAG = "uart_bridge";

/* --------------------------------------------------------------------------
 * UI_TEST (task 14) -- LVGL tap-target introspection / click-by-name for a
 * PC-side UI regression harness. See uart_task_ids.h's UI_TEST_CMD_* doc
 * comment for the wire format; kiln_ui.c owns the actual widget-tree walk
 * and injection, this task is purely marshalling.
 * ------------------------------------------------------------------------ */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} ui_test_bridge_ctx_t;

static void ui_test_bridge_task(void *arg)
{
    ui_test_bridge_ctx_t *ctx = (ui_test_bridge_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "ui_test: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        uint8_t subcmd = msg.payload[0];
        size_t reply_len = 0;
        bool rejected = false;

        switch (subcmd) {
            case UI_TEST_CMD_GET_CURRENT_PAGE: {
                const char *name = kiln_ui_current_page();
                reply[0] = UI_TEST_CMD_GET_CURRENT_PAGE;
                reply_len = bridge_put_lstring(reply, BRIDGE_REPLY_MAX, 1, name);
                break;
            }
            case UI_TEST_CMD_LIST_TAP_TARGETS: {
                /* Collected straight to a stack array first rather than
                 * streamed into `reply` as they're found: the collector
                 * doesn't know the wire encoding, and encoding each target
                 * (length-prefixed name + 5 more bytes) as it's collected
                 * would tangle kiln_ui.c's tree walk with this file's byte
                 * layout for no benefit -- the walk is already bounded (see
                 * kiln_ui.h's doc comment) so the extra copy is cheap.
                 *
                 * Dispatched onto lvgl_port_task (lvgl_port_collect_tap_
                 * targets()) rather than calling kiln_ui_collect_tap_targets()
                 * directly here on ui_test_bridge_task's own thread -- that
                 * direct call used to walk the live LVGL tree off lvgl_port_
                 * task, the same bug class TOUCH_CMD_LOG_TAP_TARGETS hit
                 * (IllegalInstruction panic, 2026-09-19) before lvgl_port_
                 * request_tap_dump() fixed it; this command needs the actual
                 * list back, not a fire-and-forget dump, hence the new
                 * bounded-wait dispatcher instead of reusing that one. A
                 * dispatch timeout (lvgl_port_task itself wedged or merely
                 * behind) reports the same way a genuinely truncated walk
                 * would -- count 0, truncated true -- rather than a distinct
                 * wire shape; see lvgl_port_collect_tap_targets()'s own doc
                 * comment. */
                kiln_ui_tap_target_t targets[32];
                bool collect_truncated = false;
                size_t n = lvgl_port_collect_tap_targets(targets,
                                                         sizeof(targets) / sizeof(targets[0]),
                                                         &collect_truncated);

                reply[0] = UI_TEST_CMD_LIST_TAP_TARGETS;
                size_t o = 3; /* byte1 (count), byte2 (truncated) filled in once the wire-fit
                               * count is known -- see the header-bytes comment below */
                size_t emitted = 0;
                bool wire_truncated = collect_truncated;
                for (size_t i = 0; i < n; i++) {
                    const kiln_ui_tap_target_t *t = &targets[i];
                    /* Each entry needs at least 1 (name_len) + 4 (cx/cy) + 1
                     * (hidden) = 6 bytes even with an empty name; bail before
                     * bridge_put_lstring truncates a NAME instead of simply
                     * omitting the whole entry, which would desync the
                     * decoder (a truncated name looks like a shorter but
                     * still-valid entry, not like "list stops here"). */
                    if (o + 6 > BRIDGE_REPLY_MAX) {
                        wire_truncated = true;
                        break;
                    }
                    size_t entry_start = o;
                    o = bridge_put_lstring(reply, BRIDGE_REPLY_MAX, o, t->name);
                    if (o + 5 > BRIDGE_REPLY_MAX) {
                        o = entry_start;
                        wire_truncated = true;
                        break;
                    }
                    bridge_put_u16_le(&reply[o], (uint16_t)t->cx);
                    o += 2;
                    bridge_put_u16_le(&reply[o], (uint16_t)t->cy);
                    o += 2;
                    reply[o++] = t->hidden ? 1u : 0u;
                    emitted++;
                }
                /* byte1 (count) and byte2 (truncated) are fixed-offset header
                 * bytes reserved at o=1,2 before the loop above but only
                 * written here, once `emitted`/`wire_truncated` are final --
                 * they were never part of the growing `o` cursor the loop
                 * advances, so writing them now doesn't disturb any entry
                 * already encoded past them. The count byte can only hold
                 * 255; the array cap above (32) is already well under that,
                 * so the `> 255` arm is a defensive floor, not a case
                 * expected to bite in practice. */
                reply[1] = (uint8_t)(emitted > 255 ? 255 : emitted);
                reply[2] = (uint8_t)(wire_truncated || emitted > 255 ? 1u : 0u);
                reply_len = o;
                break;
            }
            case UI_TEST_CMD_CLICK_BY_NAME: {
                if (!bridge_args_ok("ui_test", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_UI_TEST, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                /* Name is NOT null-terminated on the wire (matches
                 * DISPLAY_CMD_PRINT's convention) -- copy+terminate into a
                 * local buffer before handing it to kiln_ui_click_by_name(),
                 * which expects a C string. */
                char name[32];
                size_t name_len = (size_t)msg.length - 1;
                if (name_len >= sizeof(name)) {
                    name_len = sizeof(name) - 1;
                }
                memcpy(name, &msg.payload[1], name_len);
                name[name_len] = '\0';

                int16_t cx = 0, cy = 0;
                kiln_ui_click_result_t result = kiln_ui_click_by_name(name, &cx, &cy);

                uint8_t wire_result;
                switch (result) {
                    case KILN_UI_CLICK_OK:         wire_result = UI_TEST_CLICK_OK; break;
                    case KILN_UI_CLICK_AMBIGUOUS:  wire_result = UI_TEST_CLICK_AMBIGUOUS; break;
                    case KILN_UI_CLICK_HIDDEN:     wire_result = UI_TEST_CLICK_HIDDEN; break;
                    case KILN_UI_CLICK_SWALLOWED:  wire_result = UI_TEST_CLICK_SWALLOWED; break;
                    case KILN_UI_CLICK_VERDICT_UNKNOWN: wire_result = UI_TEST_CLICK_VERDICT_UNKNOWN; break;
                    case KILN_UI_CLICK_INJECT_FAILED: wire_result = UI_TEST_CLICK_INJECT_FAILED; break;
                    case KILN_UI_CLICK_OFFSCREEN: wire_result = UI_TEST_CLICK_OFFSCREEN; break;
                    case KILN_UI_CLICK_NOT_FOUND:
                    default:                       wire_result = UI_TEST_CLICK_NOT_FOUND; break;
                }

                reply[0] = UI_TEST_CMD_CLICK_BY_NAME;
                reply[1] = wire_result;
                bridge_put_u16_le(&reply[2], (uint16_t)cx);
                bridge_put_u16_le(&reply[4], (uint16_t)cy);
                reply_len = 6;
                break;
            }
            default:
                ESP_LOGW(TAG, "ui_test: unknown subcmd 0x%02X -- rejected", subcmd);
                bridge_reply_unsupported(ctx->proto, &msg, UART_TASK_ID_UI_TEST, subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason */
        }
        if (reply_len > 0) {
            bridge_reply(ctx->proto, &msg, UART_TASK_ID_UI_TEST, reply, reply_len);
        }
    }
}

esp_err_t uart_bridge_start_ui_test_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }

    static ui_test_bridge_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_UI_TEST, BRIDGE_INBOX_LEN,
                                                &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    /* PSRAM stack, same reasoning as touch_bridge_task above. Sized larger
     * than that task: this one's CLICK_BY_NAME case calls into
     * kiln_ui_click_by_name(), which holds its own 32-entry
     * kiln_ui_tap_target_t array (32 * 38 bytes ~= 1.2KB) on top of this
     * task's own 32-entry array (~1.2KB) in the LIST_TAP_TARGETS case above
     * -- those two never coexist on the stack at once, but between them and
     * uart_proto_message_t/reply[BRIDGE_REPLY_MAX] this task's frames run
     * well past what touch_bridge_task's 3072 leaves headroom for. 8192
     * was picked to keep clear margin rather than trimmed to a measured
     * minimum; check uxTaskGetStackHighWaterMark() if this ever needs to
     * shrink. */
    static TaskHandle_t s_ui_test_bridge_task_handle; /* lives for the program's duration, same as ctx */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(ui_test_bridge_task, "ui_test_uart_bridge",
                                                         8192, &ctx, 5, &s_ui_test_bridge_task_handle, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_UI_TEST);
        return ESP_ERR_NO_MEM;
    }
    stack_margin_register("ui_test_uart_bridge", &s_ui_test_bridge_task_handle, 8192);
    return ESP_OK;
}
