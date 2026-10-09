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

/* Raised 3072 -> 4096 2026-09-24 after bench SK-02 measured 440 B
 * high-water free against the 512 B absolute floor. PSRAM stack (see
 * uart_bridge_start_touch_task() below), so this costs 0 B of internal
 * DRAM. Used by both the task creation call and its stack_margin_register()
 * call so they cannot drift apart. */
#define TOUCH_UART_BRIDGE_STACK_BYTES 4096

/* --------------------------------------------------------------------------
 * DISPLAY (task 4) -- removed 2026-08-27, confirmed dead code: main.c never
 * calls uart_bridge_start_display_task() (LVGL owns the ILI9488 outright
 * now; see TODO.md 10.1). DISPLAY_CMD_* and UART_TASK_ID_DISPLAY stay
 * defined in uart_task_ids.h as wire-protocol constants -- kilnctrl's
 * gui.py/actions.py Display panel still speaks them, even though nothing on
 * this side answers.
 * ------------------------------------------------------------------------ */

/* --------------------------------------------------------------------------
 * TOUCH (task 13) -- NS2009 touch controller / screen_idle state machine
 * ------------------------------------------------------------------------ */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    screen_idle_t *idle;
} touch_bridge_ctx_t;

static void touch_bridge_task(void *arg)
{
    touch_bridge_ctx_t *ctx = (touch_bridge_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        /* Whether a TOUCH_CMD_INJECT message reaches this point is now
         * provable on demand instead of by a per-message push log: a
         * successful injection increments lvgl_port's
         * injected_delivered_count (read back via TOUCH_CMD_GET_STATE, see
         * that case below), which only happens once this task has dequeued
         * the message, decoded it, and called lvgl_port_inject_touch(). */
        if (msg.length < 1) {
            ESP_LOGW(TAG, "touch: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;
        size_t reply_len = 0;
        bool rejected = false;

        switch (subcmd) {
            case TOUCH_CMD_GET_STATE: {
                bool screen_on = false;
                uint32_t idle_ms = 0;
                err = screen_idle_get_state(ctx->idle, &screen_on, &idle_ms);
                if (err == ESP_OK) {
                    reply[0] = TOUCH_CMD_GET_STATE;
                    reply[1] = screen_on ? 1u : 0u;
                    reply[2] = (uint8_t)(idle_ms & 0xFFu);
                    reply[3] = (uint8_t)((idle_ms >> 8) & 0xFFu);
                    reply[4] = (uint8_t)((idle_ms >> 16) & 0xFFu);
                    reply[5] = (uint8_t)((idle_ms >> 24) & 0xFFu);
                    reply_len = 6;

                    /* APPENDED FIELDS ONLY past this point (2026-08-21) --
                     * pull-based touch/UI diagnostics, added to settle
                     * whether input is really stuck disabled, whether
                     * kiln_ui_show() ever reaches its exit, and whether
                     * touch_read_cb is ever polled, without relying on
                     * uart_log_bridge's queue (which was proven to drop
                     * lines during exactly the boot burst under
                     * investigation -- see lvgl_port.c and kiln_ui.c's
                     * counter comments). An older PC-side build only reads
                     * bytes [0..5]; this never reorders or resizes them, only
                     * grows reply_len, so a short-reply-tolerant reader keeps
                     * working against a newer board and vice versa.
                     *
                     * Layout (little-endian u32s), bytes [6..22]:
                     *   [6]      input_enabled (1 = LVGL indev enabled)
                     *   [7..10]  touch_read_cb_count
                     *   [11..14] injected_delivered_count
                     *   [15..18] kiln_ui_show entries
                     *   [19..22] kiln_ui_show completed exits
                     * 17 bytes appended; 6 + 17 = 23 total reply bytes, well
                     * under UART_PROTO_MAX_PAYLOAD (253) -- see this file's
                     * BRIDGE_REPLY_MAX, sized to that same constant. Two more
                     * fields (indev_exists at [23], timer_handler_calls at
                     * [24..27]) briefly lived past this point as a one-off
                     * root-cause probe for the LVGL-task-never-starts bug;
                     * removed 2026-08-21 once that bug was fixed and the
                     * Python decoder was confirmed to never read either. */
                    bool input_enabled = false;
                    uint32_t read_cb_count = 0, injected_count = 0;
                    lvgl_port_get_touch_diag(&input_enabled, &read_cb_count, &injected_count);
                    uint32_t show_entries = 0, show_exits = 0;
                    kiln_ui_get_show_diag(&show_entries, &show_exits);

                    reply[6] = input_enabled ? 1u : 0u;
                    bridge_put_u32_le(&reply[7], read_cb_count);
                    bridge_put_u32_le(&reply[11], injected_count);
                    bridge_put_u32_le(&reply[15], show_entries);
                    bridge_put_u32_le(&reply[19], show_exits);
                    /* indev_exists and timer_handler_calls used to be
                     * appended here (bytes [23] and [24..27]) as a one-off
                     * root-cause probe for the LVGL-task-never-starts bug.
                     * That bug is fixed (static .bss stack, see the fix
                     * commit) and the Python decoder never read either field
                     * -- removed 2026-08-21 rather than carried forward as
                     * permanent wire format. reply_len shrinks from 28 to 23
                     * to match; the decoder stays tolerant of a shorter reply
                     * from older firmware regardless. */
                    reply_len = 23;

                    /* APPENDED FIELDS ONLY past this point (2026-09-24) --
                     * tap-swallow observability, see uart_task_ids.h's
                     * TOUCH_CMD_GET_STATE doc comment for the exact layout.
                     * screen_idle_get_power_diag() can only fail with
                     * ESP_ERR_INVALID_STATE (idle not ready yet) or
                     * ESP_ERR_TIMEOUT (lock contention) -- either way this
                     * simply leaves reply_len at 23 and the caller gets the
                     * pre-2026-09-24 reply shape, same "older/degraded
                     * firmware" tolerance the decoder already has to have. */
                    /* check_all_task_stack_budgets.ps1: touch_bridge_task's own
                     * frame is tight (ceiling 2176 B) -- these three locals are
                     * `static` rather than on-stack, per this codebase's
                     * established convention for a task whose stack is already
                     * near budget (see this file's module docstring / CLAUDE.md's
                     * "Fix by moving large locals off the named task's own
                     * stack"). Safe: touch_bridge_task is a single, non-reentrant
                     * consumer of its own inbox queue -- nothing else writes
                     * these between this assignment and its use two lines below. */
                    static display_power_state_t s_power_diag_state;
                    static uint32_t s_power_diag_swallow_count;
                    static screen_idle_swallow_reason_t s_power_diag_last_reason;
                    if (screen_idle_get_power_diag(ctx->idle, &s_power_diag_state,
                                                   &s_power_diag_swallow_count,
                                                   &s_power_diag_last_reason) == ESP_OK) {
                        uint8_t wire_power_state;
                        switch (s_power_diag_state) {
                            case DISPLAY_POWER_OFF:        wire_power_state = TOUCH_POWER_STATE_OFF; break;
                            case DISPLAY_POWER_ERROR_HOLD: wire_power_state = TOUCH_POWER_STATE_ERROR_HOLD; break;
                            case DISPLAY_POWER_ON:
                            default:                        wire_power_state = TOUCH_POWER_STATE_ON; break;
                        }
                        reply[23] = wire_power_state;
                        bridge_put_u32_le(&reply[24], s_power_diag_swallow_count);
                        switch (s_power_diag_last_reason) {
                            case SCREEN_IDLE_SWALLOW_WAKE:       reply[28] = TOUCH_SWALLOW_REASON_WAKE; break;
                            case SCREEN_IDLE_SWALLOW_ERROR_HOLD: reply[28] = TOUCH_SWALLOW_REASON_ERROR_HOLD; break;
                            case SCREEN_IDLE_SWALLOW_NONE:
                            default:                              reply[28] = TOUCH_SWALLOW_REASON_NONE; break;
                        }
                        reply_len = 29;
                    }
                }
                break;
            }
            case TOUCH_CMD_INJECT: {
                if (!bridge_args_ok("touch", &msg, 6)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_TOUCH, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                uint16_t inj_x = bridge_u16_le(&msg.payload[1]);
                uint16_t inj_y = bridge_u16_le(&msg.payload[3]);
                bool inj_pressed = msg.payload[5] != 0;
                /* Bounds the point to the panel. Until 2026-08-24 this
                 * accepted anything a uint16 can hold: injecting (9999,9999)
                 * on the bench returned "ok - touch injected" and
                 * TOUCH_CMD_GET_STATE's injected_delivered counter went from
                 * 0 to 107, so the operator saw both a success reply AND
                 * positive delivery evidence for a touch that cannot hit any
                 * widget -- LVGL simply hit-tests nothing out there and
                 * discards it. That is the same class of defect this task's
                 * reject replies exist to remove: an action that did nothing
                 * looking exactly like one that worked. A coordinate typo is
                 * the likely real cause, and it used to be invisible.
                 *
                 * The check is deliberately rotation-agnostic. The panel's
                 * frame memory is always 320x480 (ILI9488.h) and rotation
                 * only swaps which axis is which, so a valid point satisfies
                 * either (x<320 && y<480) or (x<480 && y<320). This task has
                 * no display handle and so cannot ask which rotation is
                 * live; accepting the union means a point valid only in the
                 * OTHER rotation still gets through, which is a deliberate
                 * false-accept rather than a missed bound. It costs nothing
                 * real -- such a point is in-panel, merely rotated -- while
                 * catching every grossly wrong coordinate, which is the
                 * failure this exists for. Tightening it would mean plumbing
                 * rotation state into this task for no gain. */
                bool inj_in_panel =
                    (inj_x < ILI9488_PANEL_WIDTH && inj_y < ILI9488_PANEL_HEIGHT) ||
                    (inj_x < ILI9488_PANEL_HEIGHT && inj_y < ILI9488_PANEL_WIDTH);
                if (!inj_in_panel) {
                    ESP_LOGW(TAG, "touch: inject (%u,%u) is outside the %ux%u panel in either "
                                  "rotation -- rejected",
                             inj_x, inj_y, ILI9488_PANEL_WIDTH, ILI9488_PANEL_HEIGHT);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_TOUCH, subcmd,
                                        "out of range");
                    rejected = true;
                    break;
                }
                /* Two independent consumers of the same wire event, on purpose:
                 * screen_idle_inject_touch() only ever cared THAT a touch
                 * happened (idle-timer reset / wake), never where -- see its
                 * header comment. lvgl_port_inject_touch() is the new half:
                 * it hands x/y to LVGL's input device so the injected point
                 * actually hit-tests against the UI, which is the whole point
                 * of this command (previously the coordinates went nowhere --
                 * see lvgl_port.c's touch_read_cb). Both are cheap
                 * lock-protected variable writes, not lv_* calls, so calling
                 * both from this UART task is safe -- lvgl_port_inject_touch()
                 * never touches LVGL itself; only touch_read_cb (running on
                 * lvgl_port_task) reads what it wrote. */
                err = screen_idle_inject_touch(ctx->idle, inj_x, inj_y, inj_pressed);
                lvgl_port_inject_touch(inj_x, inj_y, inj_pressed);
                break;
            }
            case TOUCH_CMD_SET_TAP_DUMP: {
                if (!bridge_args_ok("touch", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_TOUCH, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                kiln_ui_set_auto_tap_dump(msg.payload[1] != 0);
                err = ESP_OK;
                break;
            }
            case TOUCH_CMD_LOG_TAP_TARGETS: {
                /* No arguments -- the subcommand byte alone is the whole
                 * frame, which msg.length >= 1 above has already
                 * established. Fire-and-forget, same as INJECT and
                 * SET_TAP_DUMP: the actual dump goes out as ESP_LOGI lines
                 * over uart_log_bridge, not as a reply on this task.
                 *
                 * Routed through lvgl_port_request_tap_dump() rather than
                 * calling kiln_ui_log_tap_targets() directly (bench-
                 * reproduced 2026-09-19: IllegalInstruction panic,
                 * exc_task='touch_uart_brid'). This task's stack is only
                 * TOUCH_UART_BRIDGE_STACK_BYTES (4096 B, see below); kiln_ui_log_tap_targets() recurses the live LVGL
                 * tree and calls ESP_LOGI (itself stack-hungry, formatting
                 * a line per widget) at every node, which does not fit
                 * alongside this task's other locals with the ~176 B that
                 * was free at idle. lvgl_port_request_tap_dump() instead
                 * flags lvgl_port_task -- which already owns every other
                 * LVGL access and carries an 8192 B stack -- to run the
                 * dump on ITS next loop tick. See lvgl_port.h's declaration
                 * comment for the thread-safety angle this also fixes.
                 *
                 * 2026-09-24: this task's stack was raised from 3072 B to
                 * TOUCH_UART_BRIDGE_STACK_BYTES (4096 B) after bench SK-02
                 * measured only 440 B high-water free against the 512 B
                 * absolute floor -- PSRAM stack, so the DRAM impact is 0 B. */
                lvgl_port_request_tap_dump();
                err = ESP_OK;
                break;
            }
            default:
                ESP_LOGW(TAG, "touch: unknown subcmd 0x%02X -- rejected", subcmd);
                bridge_reply_unsupported(ctx->proto, &msg, UART_TASK_ID_TOUCH, subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason */
        }
        if (err != ESP_OK) {
            /* Reported to the host, not just the log -- same fix as
             * thermo_bridge_task()/io_bridge_task()'s own "driver error"
             * reply: a silent drop here was indistinguishable from success
             * on the wire. */
            ESP_LOGW(TAG, "touch: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
            bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_TOUCH, subcmd, "driver error");
            continue;
        }
        if (reply_len > 0) {
            bridge_reply(ctx->proto, &msg, UART_TASK_ID_TOUCH, reply, reply_len);
        }
    }
}

esp_err_t uart_bridge_start_touch_task(uart_protocol_t *proto, screen_idle_t *idle)
{
    if (!proto || !idle) {
        return ESP_ERR_INVALID_ARG;
    }

    static touch_bridge_ctx_t ctx;
    ctx.proto = proto;
    ctx.idle = idle;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_TOUCH, BRIDGE_INBOX_LEN,
                                                &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    /* 2026-08-22: PSRAM stack -- touch_bridge_task only calls
     * screen_idle_get_state()/lvgl_port_inject_touch(), neither of which
     * touches hardware directly or reaches flash/NVS. */
    static TaskHandle_t s_touch_bridge_task_handle; /* lives for the program's duration, same as ctx */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(touch_bridge_task, "touch_uart_bridge",
                                                         TOUCH_UART_BRIDGE_STACK_BYTES,
                                                         &ctx, 5, &s_touch_bridge_task_handle, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_TOUCH);
        return ESP_ERR_NO_MEM;
    }
    stack_margin_register("touch_uart_bridge", &s_touch_bridge_task_handle, TOUCH_UART_BRIDGE_STACK_BYTES);
    return ESP_OK;
}
