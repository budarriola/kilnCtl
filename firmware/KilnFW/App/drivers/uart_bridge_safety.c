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
#include "ILI9488.h" /* ILI9488_PANEL_WIDTH/HEIGHT -- TOUCH_CMD_INJECT bounds check */
#include "SX1509.h"
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
#include "zones_http.h"
#include "uart_bridge_internal.h"

static const char *TAG = "uart_bridge";

/* --------------------------------------------------------------------------
 * SAFETY (task 7) -- the isolated link to the RP2040
 * ------------------------------------------------------------------------ */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    SafetyLinkClass *link;
} safety_bridge_ctx_t;

static void safety_bridge_task(void *arg)
{
    safety_bridge_ctx_t *ctx = (safety_bridge_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "safety: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;
        size_t reply_len = 0;
        bool rejected = false;

        switch (subcmd) {
            case SAFETY_CMD_GET_STATUS: {
                /* Answered from the cache, never by talking to the far side --
                 * a dead Pico is stale data, not a hung request. */
                reply_len = safety_link_build_status_payload(ctx->link, reply);
                err = (reply_len > 0) ? ESP_OK : ESP_FAIL;
                break;
            }
            case SAFETY_CMD_REQUEST_ENABLE: {
                if (!bridge_args_ok("safety", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_SAFETY, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                err = safety_link_request_enable(ctx->link, msg.payload[1] != 0);
                break;
            }
            case SAFETY_CMD_PING: {
                err = safety_link_ping(ctx->link);
                break;
            }
            case SAFETY_CMD_GET_LINK_STATS: {
                reply_len = safety_link_build_stats_payload(ctx->link, reply);
                err = (reply_len > 0) ? ESP_OK : ESP_FAIL;
                break;
            }
            case SAFETY_CMD_GET_DIAG: {
                /* LINK_PROTOCOL.md sec 7: mirror DIAG onto the PC link too --
                 * answered from the cache, never by talking to the Pico. */
                reply_len = safety_link_build_diag_payload(ctx->link, reply);
                err = (reply_len > 0) ? ESP_OK : ESP_FAIL;
                break;
            }
            case SAFETY_CMD_GET_TRIP_EVENT: {
                reply_len = safety_link_build_trip_event_payload(ctx->link, reply);
                err = (reply_len > 0) ? ESP_OK : ESP_FAIL;
                break;
            }
            case SAFETY_CMD_FW_VERSION: {
                /* LINK_PROTOCOL.md sec 7's last unmirrored frame: the Pico's
                 * own build identity + config CRC, answered from the cache
                 * exactly like GET_DIAG/GET_TRIP_EVENT above -- never by
                 * asking the Pico here (safety_poll_task() already requests
                 * 0x0B on its own cadence until the answer is known).
                 * Without this case the PC-side reader in pc_tools fell
                 * through to bridge_reply_unsupported(), so "which RP2040
                 * firmware is running" was visible over Wi-Fi but not over
                 * the wired link -- the one that still works when Wi-Fi
                 * does not, which is exactly when you are most likely to be
                 * asking. Variable-length reply, unlike the fixed-size two
                 * above.
                 *
                 * Id note: 0x0B is SAFETY_CMD_FW_VERSION, already enumerated
                 * in uart_task_ids.h as the Pico->ESP frame id. It doubles
                 * here as the PC->ESP query on UART_TASK_ID_SAFETY -- the
                 * same request-and-reply-share-one-id convention
                 * SAFETY_CMD_GET_CT_CAL/SAFETY_CMD_CT_CAL already use, and
                 * the value pc_tools' protocol.py sends. Directions are
                 * distinguished by who is talking, not by a second id. */
                reply_len = safety_link_build_fw_version_payload(ctx->link, reply);
                err = (reply_len > 0) ? ESP_OK : ESP_FAIL;
                break;
            }
            case SAFETY_CMD_SET_POLL_PERIOD: {
                if (!bridge_args_ok("safety", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_SAFETY, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                err = safety_link_set_poll_period(ctx->link, bridge_u16_le(&msg.payload[1]));
                break;
            }
            case SAFETY_CMD_CLEAR_TRIP: {
                /* No args: the ESP derives trip_mask itself from its own
                 * cached Pico DIAG state rather than trusting one supplied
                 * over the PC link -- see safety_link_send_clear_trip()'s
                 * doc comment. Fire-and-forget broadcast to the Pico, same
                 * as PING; the PC observes the outcome via the next
                 * GET_STATUS/GET_LINK_STATS poll, not an ACK from here. */
                err = safety_link_send_clear_trip(ctx->link);
                break;
            }
            case SAFETY_CMD_SET_CONFIG: {
                /* 1 byte: tc_type. Fire-and-forget broadcast to the Pico,
                 * same shape as CLEAR_TRIP above -- the PC observes the
                 * outcome via the next GET_DIAG/GET_FW_VERSION poll, not an
                 * ACK from here. safety_link_send_set_config() does the
                 * wire-level range check; a truncated frame is caught by
                 * bridge_args_ok() first, same "never guess at a missing
                 * byte" discipline every other subcommand in this file
                 * follows. */
                if (!bridge_args_ok("safety", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_SAFETY, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                err = safety_link_send_set_config(ctx->link, msg.payload[1]);
                break;
            }
            case SAFETY_CMD_ROLLBACK: {
                /* No args: tools/PcTools/TODO.md's `ota_rollback(processor)`
                 * line, Pico half. Fire-and-forget broadcast to the Pico,
                 * same shape as CLEAR_TRIP above -- the PC observes the
                 * outcome via the link dropping and recovering with a new
                 * boot_id on the next GET_STATUS poll (success), or nothing
                 * changing at all (refused -- ARMED, or no valid slot to
                 * fall back to, both entirely SaftyFW's decision). */
                err = safety_link_send_rollback(ctx->link);
                break;
            }
            case SAFETY_CMD_SET_FAULT_OUT: {
                /* A truncated SET_FAULT_OUT must never be guessed at: byte1
                 * decides whether the isolated fault line into the safety
                 * processor is asserted, and reading a stale buffer byte here
                 * could de-assert a fault that is still real. */
                if (!bridge_args_ok("safety", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_SAFETY, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                err = safety_link_set_fault(ctx->link, msg.payload[1] != 0);
                break;
            }
            case SAFETY_CMD_SET_CT_CAL: {
                /* Same 11-byte payload PC->ESP as ESP->Pico (uart_task_ids.h's
                 * SAFETY_CMD_SET_CT_CAL doc comment) -- byte1 channel,
                 * byte2 calibrated, bytes3..6 gain f32 LE, bytes7..10 offset
                 * f32 LE. Fire-and-forget broadcast to the Pico, same shape
                 * as SAFETY_CMD_SET_CONFIG above -- the PC observes the
                 * outcome via the next GET_CT_CAL readback, not an ACK from
                 * here. safety_link_send_set_ct_cal() does the wire-level
                 * channel range check; a truncated frame is caught by
                 * bridge_args_ok() first, same discipline every other
                 * subcommand in this file follows. */
                if (!bridge_args_ok("safety", &msg, KILNLINK_SET_CT_CAL_LEN)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_SAFETY, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("safety", subcmd, "ct_cal channel", msg.payload[1], 0,
                                     KILNLINK_SET_CT_CAL_NUM_CHANNELS - 1u)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_SAFETY, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = safety_link_send_set_ct_cal(ctx->link, msg.payload[1], msg.payload[2] != 0,
                                                  bridge_f32_le(&msg.payload[3]),
                                                  bridge_f32_le(&msg.payload[7]));
                break;
            }
            case SAFETY_CMD_GET_CT_CAL: {
                /* Own id (0x22) since KILNLINK_PROTOCOL_VERSION 7 -- no
                 * longer shared with the reply (SAFETY_CMD_CT_CAL, 0x1A).
                 * That split is what lets the "err != ESP_OK" bottom block
                 * below send a driver-error refusal for this command now:
                 * a refusal frame ({0x22, ok=0, reason}) can never again be
                 * misread as a truncated/malformed CT_CAL reply, since a
                 * real success reply carries the DIFFERENT id 0x1A. Unlike
                 * GET_STATUS/GET_DIAG this is never answered from a cache:
                 * safety_link_get_ct_cal() does a live, blocking round trip
                 * to the Pico and can genuinely time out (ESP_ERR_TIMEOUT)
                 * if it never answers -- that falls through to the generic
                 * "err != ESP_OK" handling below, same as every other query
                 * in this file whose driver call can fail. */
                size_t got_len = 0;
                err = safety_link_get_ct_cal(ctx->link, reply, sizeof(reply), &got_len);
                reply_len = (err == ESP_OK) ? got_len : 0;
                break;
            }
            default:
                ESP_LOGW(TAG, "safety: unknown subcmd 0x%02X -- rejected", subcmd);
                bridge_reply_unsupported(ctx->proto, &msg, UART_TASK_ID_SAFETY, subcmd);
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
             * on the wire. This is also the fix TODO.md section 11 was
             * blocked on for SAFETY_CMD_GET_CT_CAL specifically: that
             * command's request and reply used to share id 0x1A
             * (distinguished only by length), so a refusal frame here --
             * neither 1 byte nor KILNLINK_CT_CAL_LEN (28) -- could never be
             * told apart from a truncated/malformed CT_CAL reply. Splitting
             * the request onto its own id (0x22, KILNLINK_PROTOCOL_VERSION
             * 7 -- see uart_task_ids.h's SAFETY_CMD_GET_CT_CAL) removed that
             * structural block, so this refusal can now go out safely for
             * every subcommand in this switch, GET_CT_CAL included. */
            ESP_LOGW(TAG, "safety: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
            bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_SAFETY, subcmd, "driver error");
            continue;
        }
        if (reply_len > 0) {
            bridge_reply(ctx->proto, &msg, UART_TASK_ID_SAFETY, reply, reply_len);
        }
    }
}

esp_err_t uart_bridge_start_safety_task(uart_protocol_t *proto, SafetyLinkClass *link)
{
    if (!proto || !link) {
        return ESP_ERR_INVALID_ARG;
    }

    static safety_bridge_ctx_t ctx;
    ctx.proto = proto;
    ctx.link = link;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_SAFETY, BRIDGE_INBOX_LEN,
                                                &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    /* PSRAM stack -- same 2026-08-20 internal-fragmentation fix as the link
     * watchdog below and the bridge tasks in uart_bridge_ext.c. This one is
     * a PC-link surface (it relays safety STATUS to the host); the actual
     * isolated link to the RP2040 lives in safety_link.c and is unaffected by
     * whether this task exists, so a failure here costs visibility, not
     * safety. It still should not fail for want of a contiguous 4KB. */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(safety_bridge_task, "safety_uart_bridge",
                                                         4096, &ctx, 5, NULL, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_SAFETY);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
