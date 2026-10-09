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
 * THERMO (task 1) -- three MAX31856 channels
 * ------------------------------------------------------------------------ */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    /* Kept only because uart_bridge_start_thermo_task()'s signature (unchanged
     * by this pass) still takes the bus; every actual channel access now goes
     * through thermo_owner instead of dereferencing it here -- 2026-08-19,
     * TODO.md 10.14 Phase 2, same convention io_bridge_ctx_t::io adopted in
     * Phase 1. */
    MAX31856BusClass *bus;

    /* SET_AUTO_REPORT state. The destination is remembered from the request
     * that switched reporting on, so the pushes go to whoever asked for them
     * rather than to an assumed host task_id. */
    uint8_t auto_mask;
    uint16_t auto_period_ms;
    uart_proto_device_t auto_device;
    uint8_t auto_task;
} thermo_bridge_ctx_t;

/* One 12-byte READ record. Called for channels that failed as well as ones
 * that worked -- a missing channel is more confusing than an explicitly bad
 * one (see the READ response layout in uart_task_ids.h). */
static void thermo_put_record(uint8_t *out, uint8_t channel, const MAX31856Reading *r)
{
    out[0] = channel;
    bridge_put_f32_le(&out[1], r->tc_temperature_c);
    bridge_put_f32_le(&out[5], r->cj_temperature_c);
    out[9] = r->fault_status;
    /* bit2 is the USER-FACING staleness answer, matching /api/status's
     * "stale" and the LCD exactly: the reading is too old to trust, not
     * merely "no new conversion since the last poll" (the driver's own
     * MAX31856Reading::stale, which is true for a value a fraction of a
     * second old whenever the host polls faster than the part converts).
     * One threshold, KILN_TEMP_STALE_AGE_MS, for all three UIs. */
    out[10] = (uint8_t)((r->fault_pin_asserted ? 0x01u : 0u) | (r->spi_failed ? 0x02u : 0u) |
                        ((r->age_ms >= KILN_TEMP_STALE_AGE_MS) ? 0x04u : 0u));
    out[11] = 0;
}

/* chan_mask has bit N set for channel N. Returns the payload length.
 * 2026-08-19, TODO.md 10.14 Phase 2: goes through thermo_owner_command_read()
 * instead of MAX31856_bus_channel()+MAX31856_read() directly -- a channel
 * that never came up now comes back as ESP_ERR_NOT_FOUND with the reading
 * already filled NaN/spi_failed by thermo_owner itself (its own contract,
 * mirroring MAX31856_read()'s), so the "never came up" branch that used to
 * live here is gone; the two paths converged. */
static size_t thermo_build_read_payload(uint8_t chan_mask, uint8_t *out)
{
    size_t o = 2;
    uint8_t count = 0;

    for (uint8_t ch = 0; ch < THERMO_CHANNEL_COUNT; ++ch) {
        if ((chan_mask & (1u << ch)) == 0) {
            continue;
        }
        MAX31856Reading reading;
        (void)thermo_owner_command_read(ch, &reading);
        /* Zone i <-> channel i (zones_http.h). Firmware-applied calibration
         * now reaches every consumer, per TODO.md section 3 -- the UART
         * bridge was the one documented holdout. */
        reading.tc_temperature_c = zones_config_apply_cal(ch, reading.tc_temperature_c);
        thermo_put_record(&out[o], ch, &reading);
        o += 12;
        count++;
    }

    out[0] = THERMO_CMD_READ;
    out[1] = count;
    return o;
}

static size_t thermo_build_faults_payload(uint8_t chan_mask, uint8_t *out)
{
    size_t o = 2;
    uint8_t count = 0;

    for (uint8_t ch = 0; ch < THERMO_CHANNEL_COUNT; ++ch) {
        if ((chan_mask & (1u << ch)) == 0) {
            continue;
        }
        uint8_t sr = 0;
        uint8_t mask = 0;
        esp_err_t fault_err = thermo_owner_command_read_faults(ch, &sr, &mask);
        /* Bug fix, 2026-08-20: this used to discard fault_err and emit the
         * zeroed sr/mask unconditionally, so a channel that never came up
         * (thermo_owner_command_read_faults() -> ESP_ERR_NOT_FOUND, see
         * thermo_owner.h's BENCH NOTE) reported "no faults, SR 0x00, MASK
         * 0x00" -- indistinguishable from a live part that genuinely has no
         * faults set, and directly contradicting THERMO_CMD_READ's honest
         * "SPI read failed" for the same channel. Follow the convention
         * MAX31856_read_all() already uses for the same situation (MAX31856.c,
         * "never came up; the caller reports it as absent, not as 0 degC"):
         * omit the channel from the reply instead of fabricating a clean
         * reading for it. A channel missing from the list is not ambiguous
         * the way a wire could otherwise be misread; every caller already
         * has to handle count < requested (a channel not on the bus never
         * appears in the first place). */
        if (fault_err != ESP_OK) {
            continue;
        }
        out[o++] = ch;
        out[o++] = sr;
        out[o++] = mask;
        count++;
    }

    out[0] = THERMO_CMD_READ_FAULTS;
    out[1] = count;
    return o;
}

/* byte1 of most THERMO subcommands: a channel index, or THERMO_CHANNEL_ALL.
 * Returns 0 for anything out of range; callers treat that as a rejection
 * rather than as "select nothing", so a bad selector produces a logged refusal
 * instead of an empty reply the host would have to guess the meaning of. */
static uint8_t thermo_channel_mask(uint8_t selector)
{
    if (selector == THERMO_CHANNEL_ALL) {
        return (uint8_t)((1u << THERMO_CHANNEL_COUNT) - 1u);
    }
    if (selector < THERMO_CHANNEL_COUNT) {
        return (uint8_t)(1u << selector);
    }
    return 0;
}

static void thermo_bridge_task(void *arg)
{
    thermo_bridge_ctx_t *ctx = (thermo_bridge_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (ctx->auto_mask != 0 && ctx->auto_period_ms != 0) {
            wait = pdMS_TO_TICKS(ctx->auto_period_ms);
            if (wait == 0) {
                wait = 1;
            }
        }

        if (uart_protocol_receive(ctx->inbox, &msg, wait) != ESP_OK) {
            /* Nothing arrived within the auto-report period: that IS the
             * report tick. Same payload the READ query returns. */
            if (ctx->auto_mask != 0 && ctx->auto_period_ms != 0) {
                size_t len = thermo_build_read_payload(ctx->auto_mask, reply);
                bridge_push(ctx->proto, ctx->auto_device, ctx->auto_task, UART_TASK_ID_THERMO, reply,
                            len);
            }
            continue;
        }
        if (msg.length < 1) {
            /* A zero-length payload has no subcommand byte at all -- there is
             * nothing to dispatch on, so it is dropped rather than defaulted. */
            ESP_LOGW(TAG, "thermo: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;
        size_t reply_len = 0;
        /* Set by a guard that has already logged the precise reason, so the
         * generic "subcmd failed" line below does not bury it. */
        bool rejected = false;
        uint8_t chan_mask = 0;

        /* 2026-08-19, TODO.md 10.14 Phase 2: every case below used to call
         * MAX31856_bus_channel()+MAX31856_*() directly. All of that now lives
         * in thermo_owner.c, the single task that touches the MAX31856 SPI
         * API -- this task just posts and translates the result back onto
         * the wire, same "rejected -> no reply" / "err != ESP_OK -> logged,
         * no reply" / "reply_len > 0 -> reply" shape as before (and as
         * kiln_io_owner's Phase 1 rewrite of io_bridge_task above). A bad
         * channel index still comes back as ESP_ERR_NOT_FOUND, now from
         * thermo_owner_command_*() instead of a NULL MAX31856_bus_channel(). */
        switch (subcmd) {
            case THERMO_CMD_CONFIG_CHANNEL: {
                if (!bridge_args_ok("thermo", &msg, 6)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u) ||
                    /* CR1: TC[3:0] and AVGSEL[2:0]. MAX31856_config_channel drops
                     * these straight into the register, so an over-wide value
                     * would silently bleed into the neighbouring field and
                     * mis-linearize every reading from that channel. */
                    !bridge_range_ok("thermo", subcmd, "tc_type", msg.payload[2], 0, 0x0Fu) ||
                    !bridge_range_ok("thermo", subcmd, "avg_mode", msg.payload[3], 0, 0x07u)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = thermo_owner_command_config_channel(msg.payload[1], msg.payload[2],
                                                          msg.payload[3], msg.payload[4] != 0,
                                                          msg.payload[5] != 0);
                break;
            }
            case THERMO_CMD_SET_THRESHOLDS: {
                if (!bridge_args_ok("thermo", &msg, 12)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = thermo_owner_command_set_thresholds(msg.payload[1], bridge_f32_le(&msg.payload[2]),
                                                          bridge_f32_le(&msg.payload[6]),
                                                          (int8_t)msg.payload[10],
                                                          (int8_t)msg.payload[11]);
                break;
            }
            case THERMO_CMD_SET_CJ_OFFSET: {
                if (!bridge_args_ok("thermo", &msg, 6)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = thermo_owner_command_set_cj_offset(msg.payload[1], bridge_f32_le(&msg.payload[2]));
                break;
            }
            case THERMO_CMD_ONE_SHOT: {
                if (!bridge_args_ok("thermo", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = thermo_owner_command_trigger_one_shot(msg.payload[1]);
                break;
            }
            case THERMO_CMD_READ: {
                if (!bridge_args_ok("thermo", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                /* Selector is a channel index or 0xFF; anything else selects no
                 * channels, and answering that with an empty reply would look
                 * to the host exactly like "all three thermocouples vanished". */
                chan_mask = thermo_channel_mask(msg.payload[1]);
                if (chan_mask == 0) {
                    ESP_LOGW(TAG, "thermo: subcmd 0x%02X selector=%u is not a channel or 0xFF -- "
                                  "rejected", subcmd, msg.payload[1]);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                reply_len = thermo_build_read_payload(chan_mask, reply);
                err = ESP_OK;
                break;
            }
            case THERMO_CMD_READ_FAULTS: {
                if (!bridge_args_ok("thermo", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                chan_mask = thermo_channel_mask(msg.payload[1]);
                if (chan_mask == 0) {
                    ESP_LOGW(TAG, "thermo: subcmd 0x%02X selector=%u is not a channel or 0xFF -- "
                                  "rejected", subcmd, msg.payload[1]);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                reply_len = thermo_build_faults_payload(chan_mask, reply);
                err = ESP_OK;
                break;
            }
            case THERMO_CMD_CLEAR_FAULTS: {
                if (!bridge_args_ok("thermo", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = thermo_owner_command_clear_faults(msg.payload[1]);
                break;
            }
            case THERMO_CMD_SET_AUTO_REPORT: {
                if (!bridge_args_ok("thermo", &msg, 4)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                ctx->auto_mask = (uint8_t)(msg.payload[1] &
                                           ((1u << THERMO_CHANNEL_COUNT) - 1u));
                ctx->auto_period_ms = bridge_clamp_auto_period("thermo",
                                                               bridge_u16_le(&msg.payload[2]));
                ctx->auto_device = msg.device;
                ctx->auto_task = msg.task_id;
                ESP_LOGI(TAG, "thermo: auto-report mask 0x%02X period %ums", ctx->auto_mask,
                         ctx->auto_period_ms);
                err = ESP_OK;
                break;
            }
            case THERMO_CMD_READ_REG: {
                if (!bridge_args_ok("thermo", &msg, 4)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                uint8_t len = msg.payload[3];
                /* Bounds the burst into reply[4..] as well as the driver's own
                 * buffer; reply is BRIDGE_REPLY_MAX so 4 + 16 always fits. */
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u) ||
                    !bridge_range_ok("thermo", subcmd, "len", len, 1, MAX31856_MAX_BURST_LEN)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                /* Unlike every other thermo *query* subcommand, this one used to
                 * let the owner's failure (channel never came up -- e.g. the
                 * daughterboard unplugged, see thermo_owner.h's BENCH NOTE) fall
                 * straight into the generic "log and drop" handling below that
                 * every non-query thermo command shares. For a command that
                 * writes nothing, that means the host sees a silent timeout
                 * instead of an answer -- the one thing THERMO_CMD_READ and
                 * THERMO_CMD_READ_FAULTS both avoid, by encoding "this channel
                 * failed" in the payload instead of failing the whole request.
                 * Match that here: always reply, with a 0-length data body when
                 * the read failed, so the host gets a definite "no data" instead
                 * of guessing whether the request was ever received. Deliberately
                 * uses its own local instead of the shared `err` -- setting that
                 * would route back through the drop-the-reply path this exists
                 * to avoid. */
                esp_err_t reg_err = thermo_owner_command_read_reg(msg.payload[1], msg.payload[2],
                                                                   &reply[4], len);
                reply[0] = THERMO_CMD_READ_REG;
                reply[1] = msg.payload[1];
                reply[2] = msg.payload[2];
                reply[3] = (reg_err == ESP_OK) ? len : 0u;
                reply_len = 4u + reply[3];
                err = ESP_OK;
                if (reg_err != ESP_OK) {
                    ESP_LOGW(TAG, "thermo: subcmd 0x%02X ch%u reg 0x%02X failed: %s -- "
                                  "replying with 0 data bytes instead of dropping the reply",
                             subcmd, msg.payload[1], msg.payload[2], esp_err_to_name(reg_err));
                }
                break;
            }
            case THERMO_CMD_WRITE_REG: {
                if (!bridge_args_ok("thermo", &msg, 4)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = thermo_owner_command_write_reg(msg.payload[1], msg.payload[2], msg.payload[3]);
                break;
            }
            default:
                ESP_LOGW(TAG, "thermo: unknown subcmd 0x%02X -- rejected", subcmd);
                bridge_reply_unsupported(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason and replied */
        }
        if (err != ESP_OK) {
            /* A driver error mid-command is reported and dropped, never
             * retried here: the host resends if it cares, and a bridge task
             * that retried on its own would keep hammering a dead SPI bus
             * instead of servicing the next command. Now also told to the
             * host instead of only to the log -- see bridge_reply_reject()'s
             * doc comment for why a silent drop here was indistinguishable
             * from success. */
            ESP_LOGW(TAG, "thermo: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
            bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_THERMO, subcmd, "driver error");
            continue;
        }
        if (reply_len > 0) {
            bridge_reply(ctx->proto, &msg, UART_TASK_ID_THERMO, reply, reply_len);
        }
    }
}

esp_err_t uart_bridge_start_thermo_task(uart_protocol_t *proto, MAX31856BusClass *bus)
{
    if (!proto || !bus) {
        return ESP_ERR_INVALID_ARG;
    }

    static thermo_bridge_ctx_t ctx; /* the task lives for the program's duration */
    ctx.proto = proto;
    ctx.bus = bus;
    ctx.auto_mask = 0;
    ctx.auto_period_ms = 0;
    ctx.auto_device = UART_PROTO_DEVICE_HOST;
    ctx.auto_task = UART_TASK_ID_THERMO;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_THERMO, BRIDGE_INBOX_LEN,
                                                &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    /* 2026-08-22: PSRAM stack, same audit as safety_bridge_task/
     * link_watchdog_task above -- thermo_bridge_task reaches the MAX31856
     * bus only through thermo_owner_command_*() (thermo_owner_task keeps its
     * own internal stack for the actual SPI transactions), and never touches
     * flash/NVS. */
    static TaskHandle_t s_thermo_bridge_task_handle; /* lives for the program's duration, same as ctx */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(thermo_bridge_task, "thermo_uart_bridge", 4096,
                                                         &ctx, 5, &s_thermo_bridge_task_handle, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_THERMO);
        return ESP_ERR_NO_MEM;
    }
    stack_margin_register("thermo_uart_bridge", &s_thermo_bridge_task_handle, 4096);
    return ESP_OK;
}
