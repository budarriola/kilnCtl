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
    /* st.data's low 4 bits are the SX1509's raw, PHYSICAL pin order (relay 2
     * and relay 4 swapped on this board, see kiln_io.c's
     * kiln_relay_logical_to_pin_bit comment) -- it has to be, it also
     * carries the other 12 non-relay pins as real hardware bits. out[5]
     * below (relay_shadow) is the LOGICAL relay-N-means-bit-(N-1) order
     * every other consumer (dashboard, danger_mode, MCP tools) expects.
     * A PC-side tool reading both should trust relay_shadow for relay
     * state, not re-derive it from data's low 4 bits. */
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
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("io", subcmd, "relay", msg.payload[1], 1,
                                     KILN_IO_RELAY_COUNT)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                uint32_t sources = 0;
                kiln_io_owner_relay_result_t rr =
                    kiln_io_owner_command_set_relay(msg.payload[1], msg.payload[2] != 0, &sources);
                if (rr == KILN_IO_OWNER_RELAY_ERR_OWNED) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- relay %u owned by a running profile",
                             subcmd, msg.payload[1]);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "owned");
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_SAFETY) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- relay %u ON while safety fault "
                                  "sources 0x%02X asserted (safety wins, see "
                                  "docs/SAFETY_MODEL.md)", subcmd, msg.payload[1],
                             (unsigned)sources);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "safety");
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_UPDATING) {
                    /* 2026-08-21: distinct from ERR_SAFETY above -- nothing
                     * is faulted, an ESP/Pico firmware update is in progress
                     * (kiln_io_owner.h's KILN_IO_OWNER_RELAY_ERR_UPDATING
                     * comment). Re-derive the exact reason
                     * relay_on_blocked() already logged once, same as
                     * dashboard_http.c's dashboard_set_relay() does for this
                     * same case. */
                    char reason[HEAT_INTERLOCK_REASON_MAX];
                    if (ota_http_heat_blocked_by_update(reason, sizeof(reason))) {
                        ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- relay %u ON: %s", subcmd,
                                 msg.payload[1], reason);
                    } else {
                        ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- relay %u ON: firmware update "
                                      "in progress", subcmd, msg.payload[1]);
                    }
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "updating");
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK) {
                    /* 2026-09-15: distinct from ERR_SAFETY above -- an
                     * unacknowledged crash report is stored, not a live
                     * safety fault (kiln_io_owner.h's
                     * KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK comment). */
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- relay %u ON: unacknowledged crash "
                                  "report", subcmd, msg.payload[1]);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "crash_unacked");
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_RUNNING) {
                    /* docs/SYSTEM_MODE_GATE_PLAN.md, owner decision 2026-09-25
                     * (Q1): distinct from ERR_SAFETY above -- a firing or
                     * autotune run is active, not a live safety fault. */
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- relay %u ON: a firing or autotune "
                                  "run is active", subcmd, msg.payload[1]);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "running");
                    rejected = true;
                    break;
                }
                err = (rr == KILN_IO_OWNER_RELAY_OK) ? ESP_OK : ESP_FAIL;
                break;
            }
            case IO_CMD_SET_RELAY_MASK: {
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
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
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                uint32_t sources = 0;
                kiln_io_owner_relay_result_t rr =
                    kiln_io_owner_command_set_relay_mask(msg.payload[1], msg.payload[2], &sources);
                if (rr == KILN_IO_OWNER_RELAY_ERR_OWNED) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- mask 0x%02X selects a relay owned "
                                  "by a running profile", subcmd, msg.payload[1]);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "owned");
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_SAFETY) {
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- mask 0x%02X/value 0x%02X turns a "
                                  "relay ON while safety fault sources 0x%02X asserted (safety "
                                  "wins, see docs/SAFETY_MODEL.md)", subcmd, msg.payload[1],
                             msg.payload[2], (unsigned)sources);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "safety");
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_UPDATING) {
                    /* Same distinction as IO_CMD_SET_RELAY above. */
                    char reason[HEAT_INTERLOCK_REASON_MAX];
                    if (ota_http_heat_blocked_by_update(reason, sizeof(reason))) {
                        ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- mask 0x%02X/value 0x%02X: %s",
                                 subcmd, msg.payload[1], msg.payload[2], reason);
                    } else {
                        ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- mask 0x%02X/value 0x%02X: "
                                      "firmware update in progress", subcmd, msg.payload[1],
                                 msg.payload[2]);
                    }
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "updating");
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK) {
                    /* Same distinction as IO_CMD_SET_RELAY above. */
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- mask 0x%02X/value 0x%02X: "
                                  "unacknowledged crash report", subcmd, msg.payload[1], msg.payload[2]);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "crash_unacked");
                    rejected = true;
                    break;
                }
                if (rr == KILN_IO_OWNER_RELAY_ERR_RUNNING) {
                    /* Same distinction as IO_CMD_SET_RELAY above. */
                    ESP_LOGW(TAG, "io: subcmd 0x%02X refused -- mask 0x%02X/value 0x%02X: a firing "
                                  "or autotune run is active", subcmd, msg.payload[1], msg.payload[2]);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "running");
                    rejected = true;
                    break;
                }
                err = (rr == KILN_IO_OWNER_RELAY_OK) ? ESP_OK : ESP_FAIL;
                break;
            }
            case IO_CMD_SET_IO: {
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("io", subcmd, "io", msg.payload[1], 1,
                                     KILN_IO_DIGITAL_COUNT)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = kiln_io_owner_command_set_io(msg.payload[1], msg.payload[2] != 0);
                break;
            }
            case IO_CMD_SET_IO_DIR: {
                if (!bridge_args_ok("io", &msg, 4)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("io", subcmd, "io", msg.payload[1], 1,
                                     KILN_IO_DIGITAL_COUNT)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
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
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
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
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                uint32_t sources = 0;
                kiln_io_owner_sx_result_t sr =
                    kiln_io_owner_command_sx_write_reg(msg.payload[1], msg.payload[2], &sources);
                if (sr == KILN_IO_OWNER_SX_REFUSED_RELAY) {
                    ESP_LOGW(TAG, "io: SX_WRITE_REG reg 0x%02X val 0x%02X refused -- would energize a "
                                  "relay pin while safety fault sources 0x%02X asserted (safety wins, "
                                  "see docs/SAFETY_MODEL.md)", msg.payload[1], msg.payload[2],
                             (unsigned)sources);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "safety");
                    rejected = true;
                    break;
                }
                err = (sr == KILN_IO_OWNER_SX_OK) ? ESP_OK : ESP_FAIL;
                break;
            }
            case IO_CMD_SX_READ_REG: {
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                uint8_t len = msg.payload[2];
                /* Bounds the burst into reply[3..]; 3 + 16 is well inside
                 * BRIDGE_REPLY_MAX, so no reply can be built past the buffer. */
                if (!bridge_range_ok("io", subcmd, "len", len, 1, 16)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = kiln_io_owner_command_sx_read_reg(msg.payload[1], &reply[3], len);
                if (err != ESP_OK) break;
                reply[0] = IO_CMD_SX_READ_REG;
                reply[1] = msg.payload[1];
                reply[2] = len;
                reply_len = 3u + len;
                break;
            }
            case IO_CMD_SX_SET_DIR: {
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                uint16_t dir_mask = bridge_u16_le(&msg.payload[1]);
                kiln_io_owner_sx_result_t sr = kiln_io_owner_command_sx_set_dir(dir_mask);
                if (sr == KILN_IO_OWNER_SX_REFUSED_RELAY) {
                    ESP_LOGW(TAG, "io: SX_SET_DIR mask 0x%04X refused -- would retarget a relay pin's "
                                  "direction (relay pins are always outputs, see kiln_io.h)", dir_mask);
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "safety");
                    rejected = true;
                    break;
                }
                err = (sr == KILN_IO_OWNER_SX_OK) ? ESP_OK : ESP_FAIL;
                break;
            }
            case IO_CMD_SX_SET_PULLUP: {
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                {
                    uint16_t mask = bridge_u16_le(&msg.payload[1]);
                    kiln_io_owner_sx_result_t sr = kiln_io_owner_command_sx_set_pullup(mask);
                    if (sr == KILN_IO_OWNER_SX_REFUSED_RELAY) {
                        ESP_LOGW(TAG, "io: SX_SET_PULLUP mask 0x%04X refused -- would reconfigure a "
                                      "relay pin's pull-up (relay pins are always plain push-pull "
                                      "outputs, see kiln_io.h)", mask);
                        bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "safety");
                        rejected = true;
                        break;
                    }
                    err = (sr == KILN_IO_OWNER_SX_OK) ? ESP_OK : ESP_FAIL;
                }
                break;
            }
            case IO_CMD_SX_SET_OPENDRAIN: {
                if (!bridge_args_ok("io", &msg, 3)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                {
                    uint16_t mask = bridge_u16_le(&msg.payload[1]);
                    kiln_io_owner_sx_result_t sr = kiln_io_owner_command_sx_set_opendrain(mask);
                    if (sr == KILN_IO_OWNER_SX_REFUSED_RELAY) {
                        ESP_LOGW(TAG, "io: SX_SET_OPENDRAIN mask 0x%04X refused -- would let a relay "
                                      "pin's commanded HIGH float instead of drive (relay pins are "
                                      "always push-pull, see kiln_io.h)", mask);
                        bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "safety");
                        rejected = true;
                        break;
                    }
                    err = (sr == KILN_IO_OWNER_SX_OK) ? ESP_OK : ESP_FAIL;
                }
                break;
            }
            case IO_CMD_SX_SET_DEBOUNCE: {
                if (!bridge_args_ok("io", &msg, 4)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("io", subcmd, "debounce config", msg.payload[3], 0, 7)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                err = kiln_io_owner_command_sx_set_debounce(bridge_u16_le(&msg.payload[1]),
                                                            msg.payload[3]);
                break;
            }
            case IO_CMD_SX_SET_INT_MASK: {
                if (!bridge_args_ok("io", &msg, 5)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                {
                    uint16_t mask = bridge_u16_le(&msg.payload[1]);
                    uint32_t sense = io_expand_sense(bridge_u16_le(&msg.payload[3]));
                    kiln_io_owner_sx_result_t sr = kiln_io_owner_command_sx_set_int_mask(mask, sense);
                    if (sr == KILN_IO_OWNER_SX_REFUSED_RELAY) {
                        ESP_LOGW(TAG, "io: SX_SET_INT_MASK mask 0x%04X refused -- would enable "
                                      "interrupts on a relay pin (relay pins are always outputs, "
                                      "see kiln_io.h)", mask);
                        bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "safety");
                        rejected = true;
                        break;
                    }
                    err = (sr == KILN_IO_OWNER_SX_OK) ? ESP_OK : ESP_FAIL;
                }
                break;
            }
            case IO_CMD_SX_LED_DRIVER: {
                if (!bridge_args_ok("io", &msg, 4)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
                if (!bridge_range_ok("io", subcmd, "pin", msg.payload[1], 0,
                                     SX1509_PIN_COUNT - 1u)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "out of range");
                    rejected = true;
                    break;
                }
                {
                    uint8_t pin = msg.payload[1];
                    kiln_io_owner_sx_result_t sr = kiln_io_owner_command_sx_led_driver(
                        pin, msg.payload[2] != 0, msg.payload[3]);
                    if (sr == KILN_IO_OWNER_SX_REFUSED_RELAY) {
                        /* Audit item, TODO.md "Audit 2026-08-27 -- open items": this used to reach
                         * kiln_io_owner_command_sx_led_driver() unconditionally -- no safety gate, no
                         * ownership gate, no Relay2<->Relay4 remap, and no relay_shadow update, unlike
                         * its two neighbours (SX_WRITE_REG/SX_SET_DIR just above, both correct). Fixed
                         * by refusing entirely on a relay pin (see kiln_io_owner.c's
                         * sx_led_driver_touches_relay() doc comment for why total refusal, not gating,
                         * is the right call here) rather than trying to retrofit all four. */
                        ESP_LOGW(TAG, "io: SX_LED_DRIVER pin %u refused -- would PWM a relay pin "
                                      "(mechanical coils are on/off only, see kiln_io.h)", pin);
                        bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "safety");
                        rejected = true;
                        break;
                    }
                    err = (sr == KILN_IO_OWNER_SX_OK) ? ESP_OK : ESP_FAIL;
                }
                break;
            }
            case IO_CMD_SX_RESET: {
                if (!bridge_args_ok("io", &msg, 2)) {
                    bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "truncated");
                    rejected = true;
                    break;
                }
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
                bridge_reply_unsupported(ctx->proto, &msg, UART_TASK_ID_IO, subcmd);
                rejected = true;
                break;
        }

        if (rejected) {
            continue; /* the guard above logged the specific reason and replied */
        }
        if (err != ESP_OK) {
            /* Reported, not retried: an expander that stopped answering will
             * fail the next command too, and a retry loop here would keep the
             * task off its ~INT and auto-report duties for as long as the I2C
             * bus stays broken. Now also told to the host instead of only the
             * log -- see bridge_reply_reject()'s doc comment. */
            ESP_LOGW(TAG, "io: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
            bridge_reply_reject(ctx->proto, &msg, UART_TASK_ID_IO, subcmd, "driver error");
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
     * cleared (which kiln_io_read does), so a falling edge is the event.
     *
     * This arm site has the SAME shape as the safety MAX31856 ~DRDY stall
     * (docs/audits/safety_tc_drdy_stall_2026-09-08.md): this task starts, and
     * therefore arms the edge IRQ, AFTER kiln_io_init() has already enabled
     * the expander's interrupt sources, so a source can assert before the
     * handler is armed and the edge is missed.
     *
     * Audited 2026-09-08 (docs/audits/irq_arm_site_inventory_2026-09-08.md)
     * and DELIBERATELY LEFT AS SELF-HEALING rather than given DRDY's
     * arm-time level-check fix: unlike DRDY, kiln_io_read() is called
     * continuously from independent paths (dashboard/status polling), and
     * every call re-reads current input state AND clears the expander's
     * interrupt latch as a side effect. So a missed edge here never leaves
     * stale/incorrect state cached -- it can only delay or drop one
     * ISR-driven *auto-report push* (a proactive notification), not corrupt
     * a reading the way DRDY did, where the notification was the only
     * reader. Do not "fix" this without hardware verification: doing so
     * means restructuring uart_bridge_start_io_task()/kiln_io_init() boot
     * ordering, which touches relay-safety-adjacent boot sequencing on the
     * expander used for relay control feedback. */
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

    /* 2026-08-22: PSRAM stack -- io_bridge_task reaches the SX1509 expander
     * only through kiln_io_owner_command_*() (kiln_io_owner_task keeps its
     * own internal stack for the actual I2C transactions), and never touches
     * flash/NVS. */
    static TaskHandle_t s_io_bridge_task_handle; /* lives for the program's duration, same as ctx */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(io_bridge_task, "io_uart_bridge", 4096, &ctx, 5,
                                                         &s_io_bridge_task_handle, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_IO);
        return ESP_ERR_NO_MEM;
    }
    stack_margin_register("io_uart_bridge", &s_io_bridge_task_handle, 4096);
    return ESP_OK;
}
