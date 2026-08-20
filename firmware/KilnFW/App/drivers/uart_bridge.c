#include "uart_bridge.h"

#include <math.h>
#include <string.h>

#include "build_info.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/idf_additions.h"
#include "factory_reset.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "SX1509.h"
#include "kiln_io.h"
#include "kiln_io_owner.h"
#include "lvgl_port.h"
#include "relay_authority.h"
#include "settings.h"
#include "thermo_owner.h"
#include "uart_task_ids.h"
#include "wifi_prov.h"
#include "zones_http.h"

static const char *TAG = "uart_bridge";

#define BRIDGE_INBOX_LEN 8

/* Every reply this file builds fits one protocol payload; the biggest is the
 * THERMO READ for three channels (2 + 3*12 = 38) and the SAFETY status (25). */
#define BRIDGE_REPLY_MAX UART_PROTO_MAX_PAYLOAD

/* ACK timeout for a reply to a query. uart_protocol_send retries internally up
 * to UART_PROTO_MAX_RETRIES (10) times, so the worst case a bridge task spends
 * inside one send is this value times ten. At the old 1000 ms that was ten
 * seconds during which the task answered nothing else -- no relay command, no
 * ~INT edge, no auto-report tick -- purely because the host stopped listening.
 * A host that has gone away must not be able to stall the tasks that switch
 * mains relays, so the ceiling is 2 s instead. The host sees a reply timeout
 * either way; the difference is only how long we wait to notice. */
#define BRIDGE_REPLY_ACK_TIMEOUT_MS 200u

/* Floor on any auto-report period. The wire lets a host ask for 1 ms, which at
 * the FreeRTOS tick rate becomes "wake immediately, every time" -- a bridge
 * task spinning at full priority hammering the I2C/SPI bus, starving the relay
 * and safety paths that share it. 0 still means "off" (that is the frozen wire
 * semantic); anything non-zero below this is quietly raised to it and logged,
 * which is more useful than rejecting a request that is merely optimistic. */
#define BRIDGE_AUTO_REPORT_MIN_MS 20u

/* --------------------------------------------------------------------------
 * Little-endian payload helpers. Every multi-byte field in this protocol is
 * little-endian on the wire (see uart_task_ids.h); the SX1509's own registers
 * are big-endian pairs, but that conversion lives inside SX1509.c and never
 * leaks up here.
 * ------------------------------------------------------------------------ */

static uint16_t bridge_u16_le(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void bridge_put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static float bridge_f32_le(const uint8_t *bytes)
{
    float value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void bridge_put_f32_le(uint8_t *out, float value)
{
    memcpy(out, &value, sizeof(value));
}

/* --------------------------------------------------------------------------
 * Untrusted-payload guards.
 *
 * Everything arriving on the PC link is attacker-or-bug-shaped until proven
 * otherwise: a frame can be truncated mid-argument, carry an index no pin or
 * channel has, or name a subcommand this firmware has never heard of. The two
 * helpers below are the only places that judgement is made, so that all ~40
 * subcommand handlers reject the same way -- with a log line naming the exact
 * reason, and *before* any driver call, so a rejected frame is never
 * half-applied.
 * ------------------------------------------------------------------------ */

/* `need` is the total payload length the subcommand requires, counting the
 * subcommand byte itself.
 *
 * uart_proto_message_t::payload is a fixed 128-byte buffer that the protocol
 * layer only fills to ::length; the bytes past that are whatever the previous
 * frame left in this task's copy. Parsing them would not read out of bounds --
 * it would read stale, attacker-chosen-last-time data and act on it. On this
 * board that is a live hazard rather than a cosmetic one: a SET_RELAY truncated
 * to two bytes would take its on/off flag from the previous frame and could
 * energize a heating element nobody asked for. */
static bool bridge_args_ok(const char *who, const uart_proto_message_t *msg, size_t need)
{
    if ((size_t)msg->length >= need) {
        return true;
    }
    ESP_LOGW(TAG, "%s: subcmd 0x%02X truncated (%u byte payload, needs %u) -- rejected", who,
             msg->payload[0], (unsigned)msg->length, (unsigned)need);
    return false;
}

/* Inclusive range gate for the index arguments -- channel 0-2, relay 1-4,
 * io 1-7, expander pin 0-15 and friends.
 *
 * The drivers underneath all range-check their own arguments too, and that
 * stays the last line of defence. This one exists in front of them because a
 * rejection here names the field and the value in the log, where the driver's
 * bare ESP_ERR_INVALID_ARG cannot distinguish "the host asked for relay 9"
 * from "the expander is not responding" -- and on a kiln those two want very
 * different reactions from whoever is reading the log. */
static bool bridge_range_ok(const char *who, uint8_t subcmd, const char *what, uint32_t value,
                            uint32_t lo, uint32_t hi)
{
    if (value >= lo && value <= hi) {
        return true;
    }
    ESP_LOGW(TAG, "%s: subcmd 0x%02X %s=%u out of range %u-%u -- rejected", who, subcmd, what,
             (unsigned)value, (unsigned)lo, (unsigned)hi);
    return false;
}

/* Clamp an auto-report period to something a bridge task can actually serve.
 * See BRIDGE_AUTO_REPORT_MIN_MS -- 0 is preserved exactly, because 0 is "off"
 * on the wire and must not become "as fast as possible". */
static uint16_t bridge_clamp_auto_period(const char *who, uint16_t period_ms)
{
    if (period_ms != 0 && period_ms < BRIDGE_AUTO_REPORT_MIN_MS) {
        ESP_LOGW(TAG, "%s: auto-report period %ums raised to the %ums floor", who, period_ms,
                 (unsigned)BRIDGE_AUTO_REPORT_MIN_MS);
        return (uint16_t)BRIDGE_AUTO_REPORT_MIN_MS;
    }
    return period_ms;
}

/* --------------------------------------------------------------------------
 * PC link liveness.
 *
 * "The link is up" here means: a frame from the host was accepted, or a frame
 * to the host was ACKed by it, within the last UART_BRIDGE_LINK_TIMEOUT_MS.
 * Both halves matter -- a host that only listens to auto-reports sends no
 * commands, but its ACKs still prove it is there. Recorded from every bridge
 * task; acted on by the watchdog at the bottom of this file.
 *
 * Written from several tasks and read from one; a TickType_t store is atomic
 * on this target and the value is a timestamp whose worst case under a race is
 * being one check period stale, so no lock is taken. Taking one would put a
 * mutex in the path of every frame for no benefit. */
static volatile TickType_t s_link_last_activity;
static volatile bool s_link_ever_seen;

static void bridge_note_link_activity(void)
{
    s_link_last_activity = xTaskGetTickCount();
    s_link_ever_seen = true;
}

/* Answers whoever asked, rather than a hardcoded destination: the requester's
 * address is carried in the inbound message, so a bridge never needs to know
 * who is on the other end of the link. */
static void bridge_reply(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                         const uint8_t *reply, size_t reply_len)
{
    esp_err_t err = uart_protocol_send(proto, msg->device, msg->task_id, src_task, reply, reply_len,
                                       BRIDGE_REPLY_ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task%u reply (cmd 0x%02X) to dev%u/task%u failed: %s", src_task, reply[0],
                 msg->device, msg->task_id, esp_err_to_name(err));
        return;
    }
    /* An ACK is proof the host is still listening, which is exactly what the
     * link watchdog wants to know -- see bridge_note_link_activity(). */
    bridge_note_link_activity();
}

/* An unsolicited push (an auto-report tick). Shorter ACK timeout than a reply:
 * a report that can't be delivered is stale by the time the retries run out,
 * and the next tick carries newer data anyway. */
static void bridge_push(uart_protocol_t *proto, uart_proto_device_t dst_device, uint8_t dst_task,
                        uint8_t src_task, const uint8_t *payload, size_t len)
{
    esp_err_t err = uart_protocol_send(proto, dst_device, dst_task, src_task, payload, len, 300);
    if (err != ESP_OK) {
        /* Deliberately not an escalation: the watchdog decides when a run of
         * these adds up to a lost link, and it decides from the absence of
         * successes, not from any single failure. */
        ESP_LOGD(TAG, "task%u auto-report push failed: %s", src_task, esp_err_to_name(err));
        return;
    }
    bridge_note_link_activity();
}

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
    out[10] = (uint8_t)((r->fault_pin_asserted ? 0x01u : 0u) | (r->spi_failed ? 0x02u : 0u) |
                        (r->stale ? 0x04u : 0u));
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
                if (!bridge_args_ok("thermo", &msg, 6)) { rejected = true; break; }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u) ||
                    /* CR1: TC[3:0] and AVGSEL[2:0]. MAX31856_config_channel drops
                     * these straight into the register, so an over-wide value
                     * would silently bleed into the neighbouring field and
                     * mis-linearize every reading from that channel. */
                    !bridge_range_ok("thermo", subcmd, "tc_type", msg.payload[2], 0, 0x0Fu) ||
                    !bridge_range_ok("thermo", subcmd, "avg_mode", msg.payload[3], 0, 0x07u)) {
                    rejected = true;
                    break;
                }
                err = thermo_owner_command_config_channel(msg.payload[1], msg.payload[2],
                                                          msg.payload[3], msg.payload[4] != 0,
                                                          msg.payload[5] != 0);
                break;
            }
            case THERMO_CMD_SET_THRESHOLDS: {
                if (!bridge_args_ok("thermo", &msg, 12)) { rejected = true; break; }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) { rejected = true; break; }
                err = thermo_owner_command_set_thresholds(msg.payload[1], bridge_f32_le(&msg.payload[2]),
                                                          bridge_f32_le(&msg.payload[6]),
                                                          (int8_t)msg.payload[10],
                                                          (int8_t)msg.payload[11]);
                break;
            }
            case THERMO_CMD_SET_CJ_OFFSET: {
                if (!bridge_args_ok("thermo", &msg, 6)) { rejected = true; break; }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) { rejected = true; break; }
                err = thermo_owner_command_set_cj_offset(msg.payload[1], bridge_f32_le(&msg.payload[2]));
                break;
            }
            case THERMO_CMD_ONE_SHOT: {
                if (!bridge_args_ok("thermo", &msg, 2)) { rejected = true; break; }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) { rejected = true; break; }
                err = thermo_owner_command_trigger_one_shot(msg.payload[1]);
                break;
            }
            case THERMO_CMD_READ: {
                if (!bridge_args_ok("thermo", &msg, 2)) { rejected = true; break; }
                /* Selector is a channel index or 0xFF; anything else selects no
                 * channels, and answering that with an empty reply would look
                 * to the host exactly like "all three thermocouples vanished". */
                chan_mask = thermo_channel_mask(msg.payload[1]);
                if (chan_mask == 0) {
                    ESP_LOGW(TAG, "thermo: subcmd 0x%02X selector=%u is not a channel or 0xFF -- "
                                  "rejected", subcmd, msg.payload[1]);
                    rejected = true;
                    break;
                }
                reply_len = thermo_build_read_payload(chan_mask, reply);
                err = ESP_OK;
                break;
            }
            case THERMO_CMD_READ_FAULTS: {
                if (!bridge_args_ok("thermo", &msg, 2)) { rejected = true; break; }
                chan_mask = thermo_channel_mask(msg.payload[1]);
                if (chan_mask == 0) {
                    ESP_LOGW(TAG, "thermo: subcmd 0x%02X selector=%u is not a channel or 0xFF -- "
                                  "rejected", subcmd, msg.payload[1]);
                    rejected = true;
                    break;
                }
                reply_len = thermo_build_faults_payload(chan_mask, reply);
                err = ESP_OK;
                break;
            }
            case THERMO_CMD_CLEAR_FAULTS: {
                if (!bridge_args_ok("thermo", &msg, 2)) { rejected = true; break; }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) { rejected = true; break; }
                err = thermo_owner_command_clear_faults(msg.payload[1]);
                break;
            }
            case THERMO_CMD_SET_AUTO_REPORT: {
                if (!bridge_args_ok("thermo", &msg, 4)) { rejected = true; break; }
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
                if (!bridge_args_ok("thermo", &msg, 4)) { rejected = true; break; }
                uint8_t len = msg.payload[3];
                /* Bounds the burst into reply[4..] as well as the driver's own
                 * buffer; reply is BRIDGE_REPLY_MAX so 4 + 16 always fits. */
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u) ||
                    !bridge_range_ok("thermo", subcmd, "len", len, 1, MAX31856_MAX_BURST_LEN)) {
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
                if (!bridge_args_ok("thermo", &msg, 4)) { rejected = true; break; }
                if (!bridge_range_ok("thermo", subcmd, "channel", msg.payload[1], 0,
                                     THERMO_CHANNEL_COUNT - 1u)) { rejected = true; break; }
                err = thermo_owner_command_write_reg(msg.payload[1], msg.payload[2], msg.payload[3]);
                break;
            }
            default:
                ESP_LOGW(TAG, "thermo: unknown subcmd 0x%02X -- rejected", subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason */
        }
        if (err != ESP_OK) {
            /* A driver error mid-command is reported and dropped, never
             * retried here: the host resends if it cares, and a bridge task
             * that retried on its own would keep hammering a dead SPI bus
             * instead of servicing the next command. */
            ESP_LOGW(TAG, "thermo: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
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

    BaseType_t created = xTaskCreatePinnedToCore(thermo_bridge_task, "thermo_uart_bridge", 4096,
                                                 &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_THERMO);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * IO (task 2) -- SX1509 expander through the kiln_io board layer
 * ------------------------------------------------------------------------ */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    kiln_io_t *io; /* used only for kiln_io_irq_gpio() at startup below --
                    * every actual expander access goes through
                    * kiln_io_owner now (2026-08-19, TODO.md 10.14 Phase 1) */

    /* The expander's ~INT arrives as a GPIO edge; the task has to wait on that
     * *and* on its inbox *and* on the auto-report period, so the two objects go
     * into a queue set and the period becomes the set's timeout. */
    SemaphoreHandle_t irq_sem;
    QueueSetHandle_t queue_set;

    uint16_t auto_period_ms;
    bool auto_enabled;
    uart_proto_device_t auto_device;
    uint8_t auto_task;
} io_bridge_ctx_t;

/* ~INT handler. Deliberately nothing but a semaphore give: the expander lives
 * on I2C, and reading it -- or logging, or anything else that can block -- is
 * illegal from interrupt context and would take the whole board down with a
 * "Guru Meditation" the first time an input twitched. Everything that has to
 * touch the part happens in io_bridge_task, which this only wakes.
 *
 * IRAM_ATTR so the handler stays callable with the flash cache disabled. */
static void IRAM_ATTR io_bridge_isr(void *arg)
{
    io_bridge_ctx_t *ctx = (io_bridge_ctx_t *)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->irq_sem, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* The 9-byte READ payload, which is also exactly what an auto-report push
 * carries. kiln_io_read (via kiln_io_owner_command_read()) also clears the
 * expander's interrupt source, which is what releases ~INT -- so this is
 * the call that re-arms the edge. */
static size_t io_build_read_payload(uint8_t *out)
{
    kiln_io_state_t st;
    memset(&st, 0, sizeof(st));
    esp_err_t err = kiln_io_owner_command_read(&st);
    if (err != ESP_OK) {
        st.flags |= KILN_IO_FLAG_I2C_FAILED;
    }

    out[0] = IO_CMD_READ;
    bridge_put_u16_le(&out[1], st.data);
    bridge_put_u16_le(&out[3], st.dir);
    out[5] = st.relay_shadow;
    out[6] = st.io_levels;
    out[7] = st.drdy_bits;
    out[8] = st.flags;
    return 9;
}

/* The wire's SX_SET_INT_MASK carries sense as a u16: two bits per *pin pair*
 * (uart_task_ids.h), while the part -- and SX1509_set_interrupt -- take two
 * bits per pin, i.e. 32 bits. Expand each pair's mode onto both of its pins;
 * that is the only reading of the frozen 16-bit field that is well defined. */
static uint32_t io_expand_sense(uint16_t packed)
{
    uint32_t sense = 0;
    for (uint8_t pair = 0; pair < SX1509_PIN_COUNT / 2u; ++pair) {
        uint32_t mode = (uint32_t)((packed >> (2u * pair)) & 0x3u);
        sense |= SX1509_SENSE_FOR_PIN(2u * pair, mode);
        sense |= SX1509_SENSE_FOR_PIN(2u * pair + 1u, mode);
    }
    return sense;
}

static void io_bridge_task(void *arg)
{
    io_bridge_ctx_t *ctx = (io_bridge_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (ctx->auto_enabled && ctx->auto_period_ms != 0) {
            wait = pdMS_TO_TICKS(ctx->auto_period_ms);
            if (wait == 0) {
                wait = 1;
            }
        }

        QueueSetMemberHandle_t active = xQueueSelectFromSet(ctx->queue_set, wait);

        if (active == NULL) {
            /* Period expired: the periodic half of the auto report. */
            if (ctx->auto_enabled && ctx->auto_period_ms != 0) {
                size_t len = io_build_read_payload(reply);
                bridge_push(ctx->proto, ctx->auto_device, ctx->auto_task, UART_TASK_ID_IO, reply,
                            len);
            }
            continue;
        }

        if (active == (QueueSetMemberHandle_t)ctx->irq_sem) {
            xSemaphoreTake(ctx->irq_sem, 0);
            /* An edge on ~INT. Read (which also clears the interrupt source
             * and releases the line) and push, whether or not the periodic
             * half is switched on -- but only if reporting was asked for. */
            size_t len = io_build_read_payload(reply);
            if (ctx->auto_enabled) {
                bridge_push(ctx->proto, ctx->auto_device, ctx->auto_task, UART_TASK_ID_IO, reply,
                            len);
            }
            continue;
        }

        if (uart_protocol_receive(ctx->inbox, &msg, 0) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "io: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;
        size_t reply_len = 0;
        bool rejected = false;

        switch (subcmd) {
            /* 2026-08-19, TODO.md 10.14 Phase 1: every case below used to call
             * kiln_io_*()/SX1509_*() (and, for SET_RELAY[/_MASK] and
             * SX_WRITE_REG/SX_SET_DIR, its own copy of the ownership/safety
             * gate) directly. All of that now lives in kiln_io_owner.c, the
             * single task that touches the expander -- this task just posts
             * and translates the result back onto the wire, the same
             * "rejected -> no reply" / "err != ESP_OK -> logged, no reply" /
             * "reply_len > 0 -> reply" shape as before. */
            case IO_CMD_SET_RELAY: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                if (!bridge_range_ok("io", subcmd, "relay", msg.payload[1], 1,
                                     KILN_IO_RELAY_COUNT)) { rejected = true; break; }
                uint32_t sources = 0;
                kiln_io_owner_relay_result_t rr =
                    kiln_io_owner_command_set_relay(msg.payload[1], msg.payload[2] != 0, &sources);
                if (rr == KILN_IO_OWNER_RELAY_ERR_OWNED) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- relay %u owned by a running profile",
                             subcmd, msg.payload[1]);
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_SAFETY) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- relay %u ON while safety fault "
                                  "sources 0x%02X asserted (safety wins, see "
                                  "docs/SAFETY_MODEL.md)", subcmd, msg.payload[1],
                             (unsigned)sources);
                    rejected = true;
                    break;
                }
                err = (rr == KILN_IO_OWNER_RELAY_OK) ? ESP_OK : ESP_FAIL;
                break;
            }
            case IO_CMD_SET_RELAY_MASK: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                /* kiln_io_set_relay_mask silently trims bits above 3 and then
                 * returns ESP_OK for an all-zero mask, so a host that sent a
                 * garbage mask would be told its relay command succeeded when
                 * nothing moved. Refuse it here instead -- this stays a
                 * uart_bridge-only wire-format check, not a kiln_io_owner
                 * concern (a bad mask isn't an ownership or safety question). */
                const uint8_t relay_bits = (uint8_t)((1u << KILN_IO_RELAY_COUNT) - 1u);
                if ((msg.payload[1] & (uint8_t)~relay_bits) != 0 || msg.payload[1] == 0) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X relay mask 0x%02X selects no valid relay "
                                  "(valid bits 0x%02X) -- rejected", subcmd, msg.payload[1],
                             relay_bits);
                    rejected = true;
                    break;
                }
                uint32_t sources = 0;
                kiln_io_owner_relay_result_t rr =
                    kiln_io_owner_command_set_relay_mask(msg.payload[1], msg.payload[2], &sources);
                if (rr == KILN_IO_OWNER_RELAY_ERR_OWNED) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- mask 0x%02X selects a relay owned "
                                  "by a running profile", subcmd, msg.payload[1]);
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_SAFETY) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- mask 0x%02X/value 0x%02X turns a "
                                  "relay ON while safety fault sources 0x%02X asserted (safety "
                                  "wins, see docs/SAFETY_MODEL.md)", subcmd, msg.payload[1],
                             msg.payload[2], (unsigned)sources);
                    rejected = true;
                    break;
                }
                err = (rr == KILN_IO_OWNER_RELAY_OK) ? ESP_OK : ESP_FAIL;
                break;
            }
            case IO_CMD_SET_IO: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                if (!bridge_range_ok("io", subcmd, "io", msg.payload[1], 1,
                                     KILN_IO_DIGITAL_COUNT)) { rejected = true; break; }
                err = kiln_io_owner_command_set_io(msg.payload[1], msg.payload[2] != 0);
                break;
            }
            case IO_CMD_SET_IO_DIR: {
                if (!bridge_args_ok("io", &msg, 4)) { rejected = true; break; }
                if (!bridge_range_ok("io", subcmd, "io", msg.payload[1], 1,
                                     KILN_IO_DIGITAL_COUNT)) { rejected = true; break; }
                err = kiln_io_owner_command_set_io_dir(msg.payload[1], msg.payload[2] != 0,
                                                       msg.payload[3] != 0);
                break;
            }
            case IO_CMD_READ: {
                /* No arguments -- the subcommand byte alone is the whole frame,
                 * which msg.length >= 1 above has already established. */
                reply_len = io_build_read_payload(reply);
                err = ESP_OK;
                break;
            }
            case IO_CMD_SET_AUTO_REPORT: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                ctx->auto_period_ms = bridge_clamp_auto_period("io",
                                                               bridge_u16_le(&msg.payload[1]));
                ctx->auto_enabled = (ctx->auto_period_ms != 0);
                ctx->auto_device = msg.device;
                ctx->auto_task = msg.task_id;
                ESP_LOGI(TAG, "io: auto-report period %ums", ctx->auto_period_ms);
                err = ESP_OK;
                break;
            }
            case IO_CMD_ALL_RELAYS_OFF: {
                err = kiln_io_owner_command_all_relays_off();
                break;
            }
            case IO_CMD_SX_WRITE_REG: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                uint32_t sources = 0;
                kiln_io_owner_sx_result_t sr =
                    kiln_io_owner_command_sx_write_reg(msg.payload[1], msg.payload[2], &sources);
                if (sr == KILN_IO_OWNER_SX_REFUSED_RELAY) {
                    ESP_LOGW(TAG, "io: SX_WRITE_REG reg 0x%02X val 0x%02X refused -- would energize a "
                                  "relay pin while safety fault sources 0x%02X asserted (safety wins, "
                                  "see docs/SAFETY_MODEL.md)", msg.payload[1], msg.payload[2],
                             (unsigned)sources);
                    rejected = true;
                    break;
                }
                err = (sr == KILN_IO_OWNER_SX_OK) ? ESP_OK : ESP_FAIL;
                break;
            }
            case IO_CMD_SX_READ_REG: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                uint8_t len = msg.payload[2];
                /* Bounds the burst into reply[3..]; 3 + 16 is well inside
                 * BRIDGE_REPLY_MAX, so no reply can be built past the buffer. */
                if (!bridge_range_ok("io", subcmd, "len", len, 1, 16)) { rejected = true; break; }
                err = kiln_io_owner_command_sx_read_reg(msg.payload[1], &reply[3], len);
                if (err != ESP_OK) break;
                reply[0] = IO_CMD_SX_READ_REG;
                reply[1] = msg.payload[1];
                reply[2] = len;
                reply_len = 3u + len;
                break;
            }
            case IO_CMD_SX_SET_DIR: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                uint16_t dir_mask = bridge_u16_le(&msg.payload[1]);
                kiln_io_owner_sx_result_t sr = kiln_io_owner_command_sx_set_dir(dir_mask);
                if (sr == KILN_IO_OWNER_SX_REFUSED_RELAY) {
                    ESP_LOGW(TAG, "io: SX_SET_DIR mask 0x%04X refused -- would retarget a relay pin's "
                                  "direction (relay pins are always outputs, see kiln_io.h)", dir_mask);
                    rejected = true;
                    break;
                }
                err = (sr == KILN_IO_OWNER_SX_OK) ? ESP_OK : ESP_FAIL;
                break;
            }
            case IO_CMD_SX_SET_PULLUP: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                err = kiln_io_owner_command_sx_set_pullup(bridge_u16_le(&msg.payload[1]));
                break;
            }
            case IO_CMD_SX_SET_OPENDRAIN: {
                if (!bridge_args_ok("io", &msg, 3)) { rejected = true; break; }
                err = kiln_io_owner_command_sx_set_opendrain(bridge_u16_le(&msg.payload[1]));
                break;
            }
            case IO_CMD_SX_SET_DEBOUNCE: {
                if (!bridge_args_ok("io", &msg, 4)) { rejected = true; break; }
                if (!bridge_range_ok("io", subcmd, "debounce config", msg.payload[3], 0, 7)) {
                    rejected = true;
                    break;
                }
                err = kiln_io_owner_command_sx_set_debounce(bridge_u16_le(&msg.payload[1]),
                                                            msg.payload[3]);
                break;
            }
            case IO_CMD_SX_SET_INT_MASK: {
                if (!bridge_args_ok("io", &msg, 5)) { rejected = true; break; }
                err = kiln_io_owner_command_sx_set_int_mask(
                    bridge_u16_le(&msg.payload[1]), io_expand_sense(bridge_u16_le(&msg.payload[3])));
                break;
            }
            case IO_CMD_SX_LED_DRIVER: {
                if (!bridge_args_ok("io", &msg, 4)) { rejected = true; break; }
                if (!bridge_range_ok("io", subcmd, "pin", msg.payload[1], 0,
                                     SX1509_PIN_COUNT - 1u)) { rejected = true; break; }
                err = kiln_io_owner_command_sx_led_driver(msg.payload[1], msg.payload[2] != 0,
                                                          msg.payload[3]);
                break;
            }
            case IO_CMD_SX_RESET: {
                if (!bridge_args_ok("io", &msg, 2)) { rejected = true; break; }
                err = kiln_io_owner_command_sx_reset(msg.payload[1] != 0);
                break;
            }
            case IO_CMD_SX_SCAN: {
                size_t count = 0;
                /* reply[2..] holds the found-address list; kiln_io_owner_command_sx_scan()
                 * itself clamps to its own internal buffer (SX1509_ADDR_COUNT,
                 * far smaller than BRIDGE_REPLY_MAX - 2), so no reply can be
                 * built past this buffer. */
                err = kiln_io_owner_command_sx_scan(&reply[2], BRIDGE_REPLY_MAX - 2, &count);
                if (err != ESP_OK) break;
                reply[0] = IO_CMD_SX_SCAN;
                reply[1] = (uint8_t)count;
                reply_len = 2u + count;
                break;
            }
            default:
                ESP_LOGW(TAG, "io: unknown subcmd 0x%02X -- rejected", subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason */
        }
        if (err != ESP_OK) {
            /* Reported, not retried: an expander that stopped answering will
             * fail the next command too, and a retry loop here would keep the
             * task off its ~INT and auto-report duties for as long as the I2C
             * bus stays broken. The host sees no reply and can decide. */
            ESP_LOGW(TAG, "io: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
            continue;
        }
        if (reply_len > 0) {
            bridge_reply(ctx->proto, &msg, UART_TASK_ID_IO, reply, reply_len);
        }
    }
}

esp_err_t uart_bridge_start_io_task(uart_protocol_t *proto, kiln_io_t *io)
{
    if (!proto || !io || !io->exp) {
        return ESP_ERR_INVALID_ARG;
    }
    /* No safety param here any more -- kiln_io_owner_start() (main.c) is
     * where the SafetyLinkClass is wired in now, and its own doc comment
     * carries the same "starting with no safety link" warning this used to
     * log. That call must happen before this one; kiln_io_owner's producers
     * fail closed on their own if it hasn't. */

    static io_bridge_ctx_t ctx;
    ctx.proto = proto;
    ctx.io = io;
    ctx.auto_period_ms = 0;
    ctx.auto_enabled = false;
    ctx.auto_device = UART_PROTO_DEVICE_HOST;
    ctx.auto_task = UART_TASK_ID_IO;

    ctx.irq_sem = xSemaphoreCreateBinary();
    if (!ctx.irq_sem) {
        return ESP_ERR_NO_MEM;
    }

    /* The set must be able to hold every queued item across all members at
     * once: the inbox's depth plus the one binary semaphore. */
    ctx.queue_set = xQueueCreateSet(BRIDGE_INBOX_LEN + 1);
    if (!ctx.queue_set) {
        vSemaphoreDelete(ctx.irq_sem);
        ctx.irq_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_IO, BRIDGE_INBOX_LEN,
                                                &ctx.inbox);
    if (err != ESP_OK) {
        vQueueDelete(ctx.queue_set);
        vSemaphoreDelete(ctx.irq_sem);
        return err;
    }

    if (xQueueAddToSet(ctx.inbox, ctx.queue_set) != pdPASS ||
        xQueueAddToSet(ctx.irq_sem, ctx.queue_set) != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_IO);
        vQueueDelete(ctx.queue_set);
        vSemaphoreDelete(ctx.irq_sem);
        return ESP_ERR_INVALID_STATE;
    }

    /* kiln_io deliberately installs no ISR on ~INT -- it hands the GPIO out and
     * lets whoever owns the board decide. That is this bridge. ~INT is
     * open-drain and active low, and stays low until the interrupt source is
     * cleared (which kiln_io_read does), so a falling edge is the event. */
    int irq_gpio = kiln_io_irq_gpio(io);
    if (irq_gpio >= 0) {
        esp_err_t isr_err = gpio_install_isr_service(0);
        if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "io: gpio_install_isr_service failed: %s -- ~INT edges will not be "
                          "reported, only the periodic tick", esp_err_to_name(isr_err));
        } else {
            gpio_set_intr_type((gpio_num_t)irq_gpio, GPIO_INTR_NEGEDGE);
            isr_err = gpio_isr_handler_add((gpio_num_t)irq_gpio, io_bridge_isr, &ctx);
            if (isr_err != ESP_OK) {
                ESP_LOGW(TAG, "io: gpio_isr_handler_add failed: %s", esp_err_to_name(isr_err));
            } else {
                gpio_intr_enable((gpio_num_t)irq_gpio);
            }
        }
    }

    BaseType_t created = xTaskCreatePinnedToCore(io_bridge_task, "io_uart_bridge", 4096, &ctx, 5,
                                                 NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_IO);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * DISPLAY (task 4) -- ILI9488
 * ------------------------------------------------------------------------ */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    ILI9488Class *disp;
} display_bridge_ctx_t;

static void display_bridge_task(void *arg)
{
    display_bridge_ctx_t *ctx = (display_bridge_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "display: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;
        size_t reply_len = 0;
        bool rejected = false;

        switch (subcmd) {
            case DISPLAY_CMD_RESET: {
                if (!bridge_args_ok("display", &msg, 2)) { rejected = true; break; }
                err = ILI9488_reset(ctx->disp, msg.payload[1] != 0);
                break;
            }
            case DISPLAY_CMD_SET_POWER: {
                if (!bridge_args_ok("display", &msg, 2)) { rejected = true; break; }
                err = ILI9488_set_power(ctx->disp, msg.payload[1] != 0);
                break;
            }
            case DISPLAY_CMD_SET_ROTATION: {
                if (!bridge_args_ok("display", &msg, 2)) { rejected = true; break; }
                if (!bridge_range_ok("display", subcmd, "rotation", msg.payload[1], 0, 3)) {
                    rejected = true;
                    break;
                }
                err = ILI9488_set_rotation(ctx->disp, msg.payload[1]);
                break;
            }
            case DISPLAY_CMD_SET_INVERT: {
                if (!bridge_args_ok("display", &msg, 2)) { rejected = true; break; }
                err = ILI9488_set_invert(ctx->disp, msg.payload[1] != 0);
                break;
            }
            case DISPLAY_CMD_CLEAR: {
                if (!bridge_args_ok("display", &msg, 3)) { rejected = true; break; }
                err = ILI9488_clear(ctx->disp, bridge_u16_le(&msg.payload[1]));
                break;
            }
            case DISPLAY_CMD_FILL_RECT: {
                if (!bridge_args_ok("display", &msg, 11)) { rejected = true; break; }
                /* Coordinates and extents are checked against the panel's
                 * current rotation inside ILI9488 (ili9488_rect_in_bounds),
                 * which is the only place that knows them -- an off-screen rect
                 * comes back ESP_ERR_INVALID_ARG rather than being drawn. */
                err = ILI9488_fill_rect(ctx->disp, bridge_u16_le(&msg.payload[1]),
                                        bridge_u16_le(&msg.payload[3]),
                                        bridge_u16_le(&msg.payload[5]),
                                        bridge_u16_le(&msg.payload[7]),
                                        bridge_u16_le(&msg.payload[9]));
                break;
            }
            case DISPLAY_CMD_DRAW_RECT: {
                if (!bridge_args_ok("display", &msg, 11)) { rejected = true; break; }
                err = ILI9488_draw_rect(ctx->disp, bridge_u16_le(&msg.payload[1]),
                                        bridge_u16_le(&msg.payload[3]),
                                        bridge_u16_le(&msg.payload[5]),
                                        bridge_u16_le(&msg.payload[7]),
                                        bridge_u16_le(&msg.payload[9]));
                break;
            }
            case DISPLAY_CMD_DRAW_LINE: {
                if (!bridge_args_ok("display", &msg, 11)) { rejected = true; break; }
                err = ILI9488_draw_line(ctx->disp, bridge_u16_le(&msg.payload[1]),
                                        bridge_u16_le(&msg.payload[3]),
                                        bridge_u16_le(&msg.payload[5]),
                                        bridge_u16_le(&msg.payload[7]),
                                        bridge_u16_le(&msg.payload[9]));
                break;
            }
            case DISPLAY_CMD_SET_TEXT_CURSOR: {
                if (!bridge_args_ok("display", &msg, 5)) { rejected = true; break; }
                err = ILI9488_set_text_cursor(ctx->disp, bridge_u16_le(&msg.payload[1]),
                                              bridge_u16_le(&msg.payload[3]));
                break;
            }
            case DISPLAY_CMD_SET_TEXT_STYLE: {
                if (!bridge_args_ok("display", &msg, 7)) { rejected = true; break; }
                if (!bridge_range_ok("display", subcmd, "text size", msg.payload[5], 1,
                                     ILI9488_TEXT_SIZE_MAX)) { rejected = true; break; }
                err = ILI9488_set_text_style(ctx->disp, bridge_u16_le(&msg.payload[1]),
                                             bridge_u16_le(&msg.payload[3]), msg.payload[5],
                                             msg.payload[6] != 0);
                break;
            }
            case DISPLAY_CMD_PRINT: {
                /* ASCII, not null-terminated; ILI9488_write takes a length, so
                 * unlike the old OLED bridge nothing has to be copied. The
                 * length is derived from msg.length rather than assumed, so a
                 * bare PRINT with no text is a zero-length write, not a read of
                 * whatever the previous frame left in the payload buffer. */
                if (!bridge_args_ok("display", &msg, 2)) { rejected = true; break; }
                err = ILI9488_write(ctx->disp, (const char *)&msg.payload[1],
                                    (size_t)(msg.length - 1));
                break;
            }
            case DISPLAY_CMD_BLIT_BEGIN: {
                if (!bridge_args_ok("display", &msg, 9)) { rejected = true; break; }
                /* Window bounds and the w*h pixel budget are validated by
                 * ILI9488_blit_begin; every later BLIT_DATA chunk is checked
                 * against what remains of that budget, so no run of frames can
                 * stream past the window. */
                err = ILI9488_blit_begin(ctx->disp, bridge_u16_le(&msg.payload[1]),
                                         bridge_u16_le(&msg.payload[3]),
                                         bridge_u16_le(&msg.payload[5]),
                                         bridge_u16_le(&msg.payload[7]));
                break;
            }
            case DISPLAY_CMD_BLIT_DATA: {
                if (!bridge_args_ok("display", &msg, 3)) { rejected = true; break; }
                /* RGB565 pixels, so the pixel bytes must come in pairs. The
                 * driver aborts the whole blit on an odd length (a half pixel
                 * shifts every pixel after it); catching it here means the open
                 * window survives a host that miscounted one frame. */
                if (((msg.length - 1u) & 1u) != 0) {
                    ESP_LOGW(TAG, "display: subcmd 0x%02X carries %u pixel bytes (odd, half an "
                                  "RGB565 pixel) -- rejected", subcmd, (unsigned)(msg.length - 1u));
                    rejected = true;
                    break;
                }
                err = ILI9488_blit_data(ctx->disp, &msg.payload[1], (size_t)(msg.length - 1));
                break;
            }
            case DISPLAY_CMD_BLIT_END: {
                err = ILI9488_blit_end(ctx->disp);
                break;
            }
            case DISPLAY_CMD_READ_ID: {
                uint8_t id[3] = {0, 0, 0};
                uint16_t w = 0, h = 0;
                esp_err_t id_err = ILI9488_read_id(ctx->disp, id);
                (void)ILI9488_get_dimensions(ctx->disp, &w, &h);
                reply[0] = DISPLAY_CMD_READ_ID;
                reply[1] = (id_err == ESP_OK) ? 1u : 0u;
                reply[2] = id[0];
                reply[3] = id[1];
                reply[4] = id[2];
                bridge_put_u16_le(&reply[5], w);
                bridge_put_u16_le(&reply[7], h);
                reply_len = 9;
                /* A panel that won't answer is still a valid reply (ok = 0),
                 * so this query never times out on the PC side. */
                err = ESP_OK;
                break;
            }
            default:
                ESP_LOGW(TAG, "display: unknown subcmd 0x%02X -- rejected", subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason */
        }
        if (err != ESP_OK) {
            /* Logged and dropped. Note the driver has already torn down any
             * open blit window that the failure invalidated, so the task's
             * next command starts from a defined panel state rather than half
             * way through an image. */
            ESP_LOGW(TAG, "display: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
            continue;
        }
        if (reply_len > 0) {
            bridge_reply(ctx->proto, &msg, UART_TASK_ID_DISPLAY, reply, reply_len);
        }
    }
}

esp_err_t uart_bridge_start_display_task(uart_protocol_t *proto, ILI9488Class *disp)
{
    if (!proto || !disp) {
        return ESP_ERR_INVALID_ARG;
    }

    static display_bridge_ctx_t ctx;
    ctx.proto = proto;
    ctx.disp = disp;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_DISPLAY, BRIDGE_INBOX_LEN,
                                                &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreatePinnedToCore(display_bridge_task, "display_uart_bridge", 4096,
                                                 &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_DISPLAY);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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
                }
                break;
            }
            case TOUCH_CMD_INJECT: {
                if (!bridge_args_ok("touch", &msg, 6)) { rejected = true; break; }
                uint16_t inj_x = bridge_u16_le(&msg.payload[1]);
                uint16_t inj_y = bridge_u16_le(&msg.payload[3]);
                bool inj_pressed = msg.payload[5] != 0;
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
            default:
                ESP_LOGW(TAG, "touch: unknown subcmd 0x%02X -- rejected", subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
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

    BaseType_t created = xTaskCreatePinnedToCore(touch_bridge_task, "touch_uart_bridge", 3072,
                                                 &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_TOUCH);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * SAFETY (task 7) -- the opto-isolated link to the RP2040
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
                if (!bridge_args_ok("safety", &msg, 2)) { rejected = true; break; }
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
            case SAFETY_CMD_SET_POLL_PERIOD: {
                if (!bridge_args_ok("safety", &msg, 3)) { rejected = true; break; }
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
                if (!bridge_args_ok("safety", &msg, 2)) { rejected = true; break; }
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
                if (!bridge_args_ok("safety", &msg, 2)) { rejected = true; break; }
                err = safety_link_set_fault(ctx->link, msg.payload[1] != 0);
                break;
            }
            default:
                ESP_LOGW(TAG, "safety: unknown subcmd 0x%02X -- rejected", subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "safety: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
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
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "system: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        switch (msg.payload[0]) {
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
                if (!bridge_args_ok("system", &msg, 2)) {
                    break;
                }
                /* No reply either way -- see uart_task_ids.h's doc comment:
                 * the reboot itself (a fresh unsolicited GET_FW_VERSION push
                 * from INFO) is the real confirmation, and this command's own
                 * ACK is already the delivery confirmation. */
                esp_err_t err = factory_reset_execute((factory_reset_scope_t)msg.payload[1]);
                if (err == ESP_ERR_INVALID_ARG) {
                    ESP_LOGW(TAG, "system: FACTORY_RESET scope %u out of range -- rejected, nothing erased",
                             msg.payload[1]);
                } else if (err != ESP_OK) {
                    ESP_LOGE(TAG, "system: FACTORY_RESET scope %u erase failed: %s -- rebooting anyway",
                             msg.payload[1], esp_err_to_name(err));
                } else {
                    ESP_LOGW(TAG, "system: FACTORY_RESET scope %u requested by host -- erasing and rebooting",
                             msg.payload[1]);
                }
                break;
            }
            default:
                ESP_LOGW(TAG, "system: unknown subcmd 0x%02X -- rejected", msg.payload[0]);
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

    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, "system_uart_bridge", 3072, &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_SYSTEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * INFO (task 3) -- pin map and firmware version
 * ------------------------------------------------------------------------ */

typedef struct {
    uint8_t gpio;
    uint8_t function_id;
} pin_config_entry_t;

/* Built from the same settings.h macros the drivers themselves are initialized
 * with, so this can't drift out of sync with what app_main actually wires up.
 *
 * Only real ESP32-S3 GPIOs appear here (uart_task_ids.h): the relay drives, the
 * three ~DRDY inputs and the display's D/C and ~RESET live on the SX1509 and
 * are reported through the IO task's READ instead. */
static const pin_config_entry_t s_pin_config[] = {
    { (uint8_t)I2C_MASTER_SDA_IO, PIN_FUNC_I2C_SDA },
    { (uint8_t)I2C_MASTER_SCL_IO, PIN_FUNC_I2C_SCL },
    { (uint8_t)UART_OWNER_TX_IO, PIN_FUNC_UART_TX },
    { (uint8_t)UART_OWNER_RX_IO, PIN_FUNC_UART_RX },
    { (uint8_t)KILN_SPI_SCLK_IO, PIN_FUNC_SPI_SCLK },
    { (uint8_t)KILN_SPI_MOSI_IO, PIN_FUNC_SPI_MOSI },
    { (uint8_t)KILN_SPI_MISO_IO, PIN_FUNC_SPI_MISO },
    { (uint8_t)THERMO_CS0_IO, PIN_FUNC_SPI_CS },
    { (uint8_t)THERMO_CS1_IO, PIN_FUNC_SPI_CS },
    { (uint8_t)THERMO_CS2_IO, PIN_FUNC_SPI_CS },
    { (uint8_t)DISPLAY_CS_IO, PIN_FUNC_SPI_CS },
    { (uint8_t)THERMO_FAULT0_IO, PIN_FUNC_THERMO_FAULT },
    { (uint8_t)THERMO_FAULT1_IO, PIN_FUNC_THERMO_FAULT },
    { (uint8_t)THERMO_FAULT2_IO, PIN_FUNC_THERMO_FAULT },
    { (uint8_t)SX1509_IRQ_IO, PIN_FUNC_EXPANDER_IRQ },
    { (uint8_t)SX1509_RESET_IO, PIN_FUNC_EXPANDER_RST },
    { (uint8_t)SAFETY_TX_IO, PIN_FUNC_SAFETY_TX },
    { (uint8_t)SAFETY_RX_IO, PIN_FUNC_SAFETY_RX },
    { (uint8_t)SAFETY_FAULT_IO, PIN_FUNC_SAFETY_FAULT },
#if HEARTBEAT_LED_GPIO >= 0
    /* Absent by default on this board -- there is no MCU-driven LED (see
     * docs/HARDWARE.md, "Power"), and a gpio_num of -1 has no honest
     * single-byte representation on the wire. */
    { (uint8_t)HEARTBEAT_LED_GPIO, PIN_FUNC_LED_HEARTBEAT },
#endif
};
#define PIN_CONFIG_COUNT (sizeof(s_pin_config) / sizeof(s_pin_config[0]))

/* Both INFO replies are built from compile-time data into a BRIDGE_REPLY_MAX
 * buffer with no runtime length check, which is only safe as long as they
 * cannot grow past it. These catch the day someone adds a GPIO to the table
 * above, or a longer build stamp, at compile time rather than as a stack
 * smash in the info task. */
_Static_assert(1u + PIN_CONFIG_COUNT * 2u <= BRIDGE_REPLY_MAX,
               "GET_PIN_CONFIG reply no longer fits one protocol payload");
/* version(2) + dirty(1) + commit_len(1) + commit + datetime_len(1) + datetime.
 * 64 is also the size of info_boot_push_task's own stack buffer. */
_Static_assert(5u + sizeof(FW_GIT_COMMIT) + sizeof(FW_BUILD_DATE " " FW_BUILD_TIME) <= 64u,
               "GET_FW_VERSION reply no longer fits the boot-push buffer");

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} info_bridge_ctx_t;

static size_t build_pin_config_reply(uint8_t *out)
{
    out[0] = (uint8_t)PIN_CONFIG_COUNT;
    for (size_t i = 0; i < PIN_CONFIG_COUNT; ++i) {
        out[1 + i * 2] = s_pin_config[i].gpio;
        out[1 + i * 2 + 1] = s_pin_config[i].function_id;
    }
    return 1 + PIN_CONFIG_COUNT * 2;
}

/* FW_GIT_COMMIT/FW_GIT_DIRTY/FW_BUILD_DATE/FW_BUILD_TIME come from
 * build_info.h, regenerated on every build (see gen_build_info.cmake) so
 * these can never be stale relative to what's actually flashed. */
static size_t build_fw_version_reply(uint8_t *out)
{
    static const char commit[] = FW_GIT_COMMIT;
    static const char datetime[] = FW_BUILD_DATE " " FW_BUILD_TIME;
    size_t commit_len = sizeof(commit) - 1;   /* drop the implicit '\0' */
    size_t datetime_len = sizeof(datetime) - 1;

    size_t o = 0;
    /* Fixed-offset version field, always first -- see UART_PROTOCOL_VERSION
     * in uart_task_ids.h for why this position must never move. */
    out[o++] = (uint8_t)(UART_PROTOCOL_VERSION & 0xFF);
    out[o++] = (uint8_t)((UART_PROTOCOL_VERSION >> 8) & 0xFF);
    out[o++] = FW_GIT_DIRTY ? 1 : 0;
    out[o++] = (uint8_t)commit_len;
    memcpy(&out[o], commit, commit_len);
    o += commit_len;
    out[o++] = (uint8_t)datetime_len;
    memcpy(&out[o], datetime, datetime_len);
    o += datetime_len;
    return o;
}

/* byte0 connected(0/1) + byte1 ip_len(0-15) + up to 15 IP chars, well under
 * BRIDGE_REPLY_MAX -- no static assert needed given the other two INFO
 * replies already cover that bound (see the assert above). */
static size_t build_wifi_status_reply(uint8_t *out)
{
    char ip[16] = {0};
    bool connected = wifi_prov_is_sta_connected();
    if (connected) {
        wifi_prov_get_sta_ip(ip, sizeof(ip));
    }
    size_t ip_len = strlen(ip);

    size_t o = 0;
    out[o++] = connected ? 1 : 0;
    out[o++] = (uint8_t)ip_len;
    memcpy(&out[o], ip, ip_len);
    o += ip_len;
    return o;
}

static void info_bridge_task(void *arg)
{
    info_bridge_ctx_t *ctx = (info_bridge_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "info: empty payload -- rejected");
            continue;
        }
        bridge_note_link_activity();

        size_t reply_len;
        switch (msg.payload[0]) {
            case INFO_CMD_GET_PIN_CONFIG:
                reply_len = build_pin_config_reply(reply);
                break;
            case INFO_CMD_GET_FW_VERSION:
                reply_len = build_fw_version_reply(reply);
                break;
            case INFO_CMD_GET_WIFI_STATUS:
                reply_len = build_wifi_status_reply(reply);
                break;
            default:
                ESP_LOGW(TAG, "info: unknown subcmd 0x%02X -- rejected", msg.payload[0]);
                continue;
        }

        /* Reply directly to whoever asked (carried in the inbound
         * message's device/task_id), not a hardcoded destination -- this
         * task doesn't need to know or care who's on the other end. */
        esp_err_t err = uart_protocol_send(ctx->proto, msg.device, msg.task_id, UART_TASK_ID_INFO,
                                            reply, reply_len, BRIDGE_REPLY_ACK_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "info reply (cmd 0x%02X) to dev%u/task%u failed: %s", msg.payload[0],
                     msg.device, msg.task_id, esp_err_to_name(err));
        } else {
            bridge_note_link_activity();
        }
    }
}

static void info_boot_push_task(void *arg)
{
    info_bridge_ctx_t *ctx = (info_bridge_ctx_t *)arg;

    uint8_t reply[64]; /* version(2) + dirty(1) + commit_len(1) + commit + datetime_len(1) + datetime */
    size_t reply_len = build_fw_version_reply(reply);

    esp_err_t err = uart_protocol_send(ctx->proto, UART_PROTO_DEVICE_HOST, UART_TASK_ID_INFO,
                                        UART_TASK_ID_INFO, reply, reply_len, 300);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "boot version push not delivered (%s) -- fine if nothing was connected yet",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "boot version push delivered");
        /* An ACK on this one means a host was already attached at boot, which
         * is the earliest the link watchdog can honestly call the link up. */
        bridge_note_link_activity();
    }

    vTaskDelete(NULL);
}

esp_err_t uart_bridge_start_info_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }

    static info_bridge_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_INFO, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreatePinnedToCore(info_bridge_task, "info_uart_bridge", 3072, &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_INFO);
        return ESP_ERR_NO_MEM;
    }

    /* Best-effort unsolicited push so a GUI already connected at boot shows
     * the version immediately, without polling. If nothing is listening
     * (typical case -- the PC usually isn't already connected the instant
     * the board powers up) this just fails after uart_protocol_send's own
     * retries/timeout and is logged, not treated as an error: a GUI that
     * connects later independently pulls the version via
     * INFO_CMD_GET_FW_VERSION. Runs in its own one-shot task so app_main
     * isn't blocked for the ~seconds this can take to give up. */
    BaseType_t boot_push_created = xTaskCreatePinnedToCore(info_boot_push_task, "info_boot_push", 3072,
                                                            &ctx, 5, NULL, tskNO_AFFINITY);
    if (boot_push_created != pdPASS) {
        ESP_LOGW(TAG, "failed to start boot version-push task (non-fatal)");
    }

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * PC link watchdog -- CONFIG_KILNCTL_SX1509_RELAYS_OFF_ON_LINK_LOSS
 *
 * See the long comment in uart_bridge.h for exactly what "the link went away"
 * means in wall-clock terms and what this does about it. Everything below is
 * the mechanism.
 * ------------------------------------------------------------------------ */

typedef struct {
    kiln_io_t *io;             /* NULL if the expander never came up */
    SafetyLinkClass *link;     /* NULL if the safety link failed to start */
} link_watchdog_ctx_t;

static void link_watchdog_task(void *arg)
{
    link_watchdog_ctx_t *ctx = (link_watchdog_ctx_t *)arg;

    /* Starts "up" only so that the first pass through the loop takes the
     * down transition and logs it: at boot no host has spoken, so the honest
     * state is down and the board should already be sitting in it. */
    bool was_up = true;
    /* Sticky until a drop actually succeeds, so a failed I2C transfer is
     * retried on the next tick instead of being assumed done. */
    bool relays_confirmed_off = false;

    const TickType_t timeout_ticks = pdMS_TO_TICKS(UART_BRIDGE_LINK_TIMEOUT_MS);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(UART_BRIDGE_LINK_CHECK_MS));

        /* Unsigned tick subtraction, so this stays correct across the tick
         * counter's wrap (~49 days at 1 kHz) without a special case. */
        const bool up = s_link_ever_seen &&
                        ((TickType_t)(xTaskGetTickCount() - s_link_last_activity) < timeout_ticks);

        if (up) {
            if (!was_up) {
                ESP_LOGW(TAG, "PC link back after %ums of silence -- clearing the link fault "
                              "source; relays stay off until the host commands them",
                         (unsigned)UART_BRIDGE_LINK_TIMEOUT_MS);
                if (ctx->link) {
                    esp_err_t err = safety_link_set_fault_source(ctx->link,
                                                                 SAFETY_FAULT_SRC_PC_LINK, false);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "could not clear the PC-link fault source: %s",
                                 esp_err_to_name(err));
                    }
                }
                was_up = true;
                relays_confirmed_off = false;
            }
            continue;
        }

        if (was_up) {
            ESP_LOGE(TAG, "PC link lost (no frame or ACK for %ums) -- dropping all relays and "
                          "asserting the isolated fault line",
                     (unsigned)UART_BRIDGE_LINK_TIMEOUT_MS);
            was_up = false;
            relays_confirmed_off = false;
        }

        /* Relays first, fault line second. The relays are the thing actually
         * carrying mains to the elements; the fault line is a request to a
         * processor that may or may not be listening. Do the one we control. */
        if (ctx->io && !relays_confirmed_off) {
            esp_err_t err = kiln_io_all_relays_off(ctx->io);
            if (err == ESP_OK) {
                relays_confirmed_off = true;
            } else {
                /* Deliberately left un-sticky: this repeats every check tick
                 * for as long as the link is down and the write keeps failing.
                 * A kiln with an element stuck on and a dead I2C bus is worth
                 * a log line every 250 ms. */
                ESP_LOGE(TAG, "link-loss relay drop failed: %s -- retrying",
                         esp_err_to_name(err));
            }
        } else if (!ctx->io && !relays_confirmed_off) {
            /* No expander handle at all: the relays cannot be commanded from
             * here and their state is unknown. Say so once, and leave the
             * fault line asserted below as the only remaining lever. */
            ESP_LOGE(TAG, "link lost and no expander handle -- relay state is UNKNOWN and cannot "
                          "be forced off from this firmware");
            relays_confirmed_off = true; /* nothing more to try; stop repeating */
        }

        /* Re-asserted every tick rather than once on the transition: it is a
         * single GPIO write, and doing it unconditionally means the line is
         * still right even if something else cleared the source in between. */
        if (ctx->link) {
            esp_err_t err = safety_link_set_fault_source(ctx->link, SAFETY_FAULT_SRC_PC_LINK, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "could not assert the PC-link fault source: %s",
                         esp_err_to_name(err));
            }
        }
    }
}

esp_err_t uart_bridge_start_link_watchdog(kiln_io_t *io, SafetyLinkClass *link)
{
    if (!io && !link) {
        /* Nothing to act on: neither the relays nor the fault line is
         * reachable, so a watchdog would only burn a task. */
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_KILNCTL_SX1509_RELAYS_OFF_ON_LINK_LOSS
    /* Bench builds only -- see the Kconfig help. The fault line still follows
     * the link, because telling the safety processor the truth about the
     * control link costs nothing and is never the thing you want switched off.
     */
    ESP_LOGW(TAG, "relays-off-on-link-loss is DISABLED in this build: an unattended board can "
                  "hold relays energized after the PC link drops");
    io = NULL;
#endif

    static link_watchdog_ctx_t ctx;
    ctx.io = io;
    ctx.link = link;

    /* Priority 6: above the bridge tasks (5), because this must still run when
     * every one of them is blocked inside a uart_protocol_send that a departed
     * host will never ACK -- which is precisely the situation it exists for. */
    /* PSRAM stack, 2026-08-20, same reason and same API as lvgl_port.c's and
     * uart_bridge_ext.c's: this 3072-byte stack could not be satisfied from
     * an internal heap fragmented to a sub-1KB largest free block, and the
     * failure mode was the worst one on this board -- app_main logged "PC
     * link watchdog did not start (ESP_ERR_NO_MEM) -- RELAYS WILL NOT DROP ON
     * LINK LOSS" and dropped into safe state. A safety watchdog that cannot
     * be created because of heap fragmentation is not an acceptable failure,
     * and PSRAM is sitting 8MB empty.
     *
     * Safe in PSRAM: this task polls timestamps and, on timeout, calls
     * kiln_io/safety_link to drop relays and assert the fault line. It holds
     * no DMA buffers and runs from no ISR. Note the fault line is also
     * asserted by hardware-independent paths, so the safe direction does not
     * depend solely on this task's stack being reachable. */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(link_watchdog_task, "link_watchdog", 3072,
                                                         &ctx, 6, NULL, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PC link watchdog up: relays drop and the fault line asserts after %ums with no "
                  "frame or ACK", (unsigned)UART_BRIDGE_LINK_TIMEOUT_MS);
    return ESP_OK;
}
