#include "uart_bridge.h"

#include <string.h>

#include "build_info.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "uart_task_ids.h"

static const char *TAG = "uart_bridge";

#define BRIDGE_INBOX_LEN 8

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    DcDacClass *dac;
} dac_bridge_ctx_t;

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    SSD1306Class *oled;
} oled_bridge_ctx_t;

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    AD9833Class *gen;
} ad9833_bridge_ctx_t;

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    PCF8575Class *expander;
} pcf8575_bridge_ctx_t;

static void dac_bridge_task(void *arg)
{
    dac_bridge_ctx_t *ctx = (dac_bridge_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            continue;
        }

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;

        switch (subcmd) {
            case DAC_CMD_SET_CHANNEL_PERCENT: {
                if (msg.length < 6) break;
                uint8_t channel = msg.payload[1];
                float percent;
                memcpy(&percent, &msg.payload[2], sizeof(percent));
                err = DcDac_set_channel_percent(ctx->dac, channel, percent);
                break;
            }
            case DAC_CMD_SET_ALL_PERCENT: {
                if (msg.length < 17) break;
                float percents[4];
                memcpy(percents, &msg.payload[1], sizeof(percents));
                err = DcDac_set_all_percent(ctx->dac, percents);
                break;
            }
            case DAC_CMD_POWER_DOWN: {
                if (msg.length < 3) break;
                uint8_t channel = msg.payload[1];
                uint8_t power_mode = msg.payload[2];
                err = DcDac_power_down_channel(ctx->dac, channel, power_mode);
                break;
            }
            default:
                ESP_LOGW(TAG, "dac: unknown subcmd 0x%02X", subcmd);
                break;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dac: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
        }
    }
}

static void ad9833_bridge_task(void *arg)
{
    ad9833_bridge_ctx_t *ctx = (ad9833_bridge_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            continue;
        }

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;

        switch (subcmd) {
            case AD9833_CMD_SET_FREQUENCY: {
                if (msg.length < 10) break;
                uint8_t reg = msg.payload[1];
                double freq_hz;
                memcpy(&freq_hz, &msg.payload[2], sizeof(freq_hz));
                err = AD9833_set_frequency(ctx->gen, (AD9833FreqReg)reg, freq_hz);
                break;
            }
            case AD9833_CMD_SET_PHASE: {
                if (msg.length < 10) break;
                uint8_t reg = msg.payload[1];
                double degrees;
                memcpy(&degrees, &msg.payload[2], sizeof(degrees));
                err = AD9833_set_phase(ctx->gen, (AD9833PhaseReg)reg, degrees);
                break;
            }
            case AD9833_CMD_SET_WAVEFORM: {
                if (msg.length < 2) break;
                err = AD9833_set_waveform(ctx->gen, (AD9833Waveform)msg.payload[1]);
                break;
            }
            case AD9833_CMD_SELECT_FREQ_REG: {
                if (msg.length < 2) break;
                err = AD9833_select_freq_reg(ctx->gen, (AD9833FreqReg)msg.payload[1]);
                break;
            }
            case AD9833_CMD_SELECT_PHASE_REG: {
                if (msg.length < 2) break;
                err = AD9833_select_phase_reg(ctx->gen, (AD9833PhaseReg)msg.payload[1]);
                break;
            }
            case AD9833_CMD_RESET: {
                if (msg.length < 2) break;
                err = AD9833_reset(ctx->gen, msg.payload[1] != 0);
                break;
            }
            case AD9833_CMD_SLEEP: {
                if (msg.length < 3) break;
                err = AD9833_sleep(ctx->gen, msg.payload[1] != 0, msg.payload[2] != 0);
                break;
            }
            default:
                ESP_LOGW(TAG, "ad9833: unknown subcmd 0x%02X", subcmd);
                break;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ad9833: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
        }
    }
}

static void oled_bridge_task(void *arg)
{
    oled_bridge_ctx_t *ctx = (oled_bridge_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            continue;
        }

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;

        switch (subcmd) {
            case OLED_CMD_CLEAR:
                err = SSD1306_clear(ctx->oled);
                break;
            case OLED_CMD_SET_CURSOR: {
                if (msg.length < 3) break;
                err = SSD1306_set_cursor(ctx->oled, msg.payload[1], msg.payload[2]);
                break;
            }
            case OLED_CMD_PRINT: {
                if (msg.length < 1) break;
                /* Text is msg.length-1 bytes starting at payload[1], not
                 * null-terminated on the wire; copy into a bounded local
                 * buffer (max payload is 128, so this always fits) before
                 * handing it to the null-terminated-string API. */
                char text[UART_PROTO_MAX_PAYLOAD];
                size_t text_len = msg.length - 1;
                memcpy(text, &msg.payload[1], text_len);
                text[text_len] = '\0';
                err = SSD1306_write_str(ctx->oled, text);
                break;
            }
            case OLED_CMD_DISPLAY:
                err = SSD1306_display(ctx->oled);
                break;
            case OLED_CMD_SET_CONTRAST: {
                if (msg.length < 2) break;
                err = SSD1306_set_contrast(ctx->oled, msg.payload[1]);
                break;
            }
            case OLED_CMD_SET_INVERT: {
                if (msg.length < 2) break;
                err = SSD1306_set_invert(ctx->oled, msg.payload[1] != 0);
                break;
            }
            case OLED_CMD_SET_POWER: {
                if (msg.length < 2) break;
                err = SSD1306_set_power(ctx->oled, msg.payload[1] != 0);
                break;
            }
            default:
                ESP_LOGW(TAG, "oled: unknown subcmd 0x%02X", subcmd);
                break;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "oled: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
        }
    }
}

/* Little-endian u16 out of a payload, matching every other multi-byte
 * payload field in this protocol (see the endianness note in
 * uart_task_ids.h). */
static uint16_t bridge_u16_le(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void bridge_put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void pcf8575_bridge_task(void *arg)
{
    pcf8575_bridge_ctx_t *ctx = (pcf8575_bridge_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            continue;
        }

        uint8_t subcmd = msg.payload[0];
        esp_err_t err = ESP_ERR_INVALID_ARG;
        /* Largest reply is SCAN: subcmd + count + up to 8 addresses. */
        uint8_t reply[2 + PCF8575_ADDR_COUNT];
        size_t reply_len = 0;

        switch (subcmd) {
            case PCF8575_CMD_WRITE_PORT: {
                if (msg.length < 3) break;
                err = PCF8575_write_port(ctx->expander, bridge_u16_le(&msg.payload[1]));
                break;
            }
            case PCF8575_CMD_WRITE_PIN: {
                if (msg.length < 3) break;
                err = PCF8575_write_pin(ctx->expander, msg.payload[1], msg.payload[2] != 0);
                break;
            }
            case PCF8575_CMD_SET_MASK: {
                if (msg.length < 3) break;
                err = PCF8575_set_mask(ctx->expander, bridge_u16_le(&msg.payload[1]));
                break;
            }
            case PCF8575_CMD_CLEAR_MASK: {
                if (msg.length < 3) break;
                err = PCF8575_clear_mask(ctx->expander, bridge_u16_le(&msg.payload[1]));
                break;
            }
            case PCF8575_CMD_TOGGLE_MASK: {
                if (msg.length < 3) break;
                err = PCF8575_toggle_mask(ctx->expander, bridge_u16_le(&msg.payload[1]));
                break;
            }
            case PCF8575_CMD_SET_ADDRESS: {
                if (msg.length < 2) break;
                err = PCF8575_set_address(ctx->expander, msg.payload[1]);
                break;
            }
            case PCF8575_CMD_READ_PORT: {
                uint16_t port = 0;
                err = PCF8575_read_port(ctx->expander, &port);
                if (err != ESP_OK) break;
                reply[0] = PCF8575_CMD_READ_PORT;
                bridge_put_u16_le(&reply[1], port);
                bridge_put_u16_le(&reply[3], PCF8575_get_shadow(ctx->expander));
                reply[5] = ctx->expander->addr;
                reply_len = 6;
                break;
            }
            case PCF8575_CMD_SCAN: {
                uint8_t found[PCF8575_ADDR_COUNT];
                size_t count = 0;
                err = PCF8575_scan(ctx->expander->bus, found, sizeof(found), &count);
                if (err != ESP_OK) break;
                reply[0] = PCF8575_CMD_SCAN;
                reply[1] = (uint8_t)count;
                memcpy(&reply[2], found, count);
                reply_len = 2 + count;
                break;
            }
            default:
                ESP_LOGW(TAG, "pcf8575: unknown subcmd 0x%02X", subcmd);
                break;
        }

        if (err != ESP_OK) {
            /* Query failures are deliberately silent on the wire (only
             * logged), same as every other bridge: the requester sees a
             * reply-timeout rather than a bogus port value. */
            ESP_LOGW(TAG, "pcf8575: subcmd 0x%02X failed: %s", subcmd, esp_err_to_name(err));
            continue;
        }

        if (reply_len > 0) {
            /* Answer whoever asked, carried in the inbound message's
             * device/task_id -- same arrangement as info_bridge_task. */
            esp_err_t send_err = uart_protocol_send(ctx->proto, msg.device, msg.task_id,
                                                     UART_TASK_ID_PCF8575, reply, reply_len, 1000);
            if (send_err != ESP_OK) {
                ESP_LOGW(TAG, "pcf8575 reply (cmd 0x%02X) to dev%u/task%u failed: %s", subcmd,
                         msg.device, msg.task_id, esp_err_to_name(send_err));
            }
        }
    }
}

esp_err_t uart_bridge_start_pcf8575_task(uart_protocol_t *proto, PCF8575Class *expander)
{
    if (!proto || !expander) {
        return ESP_ERR_INVALID_ARG;
    }

    static pcf8575_bridge_ctx_t ctx;
    ctx.proto = proto;
    ctx.expander = expander;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_PCF8575, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreatePinnedToCore(pcf8575_bridge_task, "pcf8575_uart_bridge", 4096,
                                                  &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_PCF8575);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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
            continue;
        }

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
            default:
                ESP_LOGW(TAG, "system: unknown subcmd 0x%02X", msg.payload[0]);
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

esp_err_t uart_bridge_start_oled_task(uart_protocol_t *proto, SSD1306Class *oled)
{
    if (!proto || !oled) {
        return ESP_ERR_INVALID_ARG;
    }

    static oled_bridge_ctx_t ctx;
    ctx.proto = proto;
    ctx.oled = oled;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_OLED, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreatePinnedToCore(oled_bridge_task, "oled_uart_bridge", 4096, &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_OLED);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t uart_bridge_start_dac_task(uart_protocol_t *proto, DcDacClass *dac)
{
    if (!proto || !dac) {
        return ESP_ERR_INVALID_ARG;
    }

    static dac_bridge_ctx_t ctx; /* task lives for the program's duration */
    ctx.proto = proto;
    ctx.dac = dac;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_DAC, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreatePinnedToCore(dac_bridge_task, "dac_uart_bridge", 4096, &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_DAC);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t uart_bridge_start_ad9833_task(uart_protocol_t *proto, AD9833Class *gen)
{
    if (!proto || !gen) {
        return ESP_ERR_INVALID_ARG;
    }

    static ad9833_bridge_ctx_t ctx;
    ctx.proto = proto;
    ctx.gen = gen;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_AD9833, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreatePinnedToCore(ad9833_bridge_task, "ad9833_uart_bridge", 4096, &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_AD9833);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

typedef struct {
    uint8_t gpio;
    uint8_t function_id;
} pin_config_entry_t;

/* Built from the same settings.h macros the drivers themselves are
 * initialized with, so this can't drift out of sync with what's actually
 * wired up in app_main. */
static const pin_config_entry_t s_pin_config[] = {
    { (uint8_t)I2C_MASTER_SDA_IO, PIN_FUNC_I2C_SDA },
    { (uint8_t)I2C_MASTER_SCL_IO, PIN_FUNC_I2C_SCL },
    { (uint8_t)UART_OWNER_TX_IO, PIN_FUNC_UART_TX },
    { (uint8_t)UART_OWNER_RX_IO, PIN_FUNC_UART_RX },
    { (uint8_t)AD9833_SCLK_IO, PIN_FUNC_SPI_SCLK },
    { (uint8_t)AD9833_MOSI_IO, PIN_FUNC_SPI_MOSI },
    { (uint8_t)AD9833_CS_IO, PIN_FUNC_SPI_CS },
    { (uint8_t)HEARTBEAT_LED_GPIO, PIN_FUNC_LED_HEARTBEAT },
};
#define PIN_CONFIG_COUNT (sizeof(s_pin_config) / sizeof(s_pin_config[0]))

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

static void info_bridge_task(void *arg)
{
    info_bridge_ctx_t *ctx = (info_bridge_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[1 + PIN_CONFIG_COUNT * 2 + 32]; /* pin table is the largest reply */

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            continue;
        }

        size_t reply_len;
        switch (msg.payload[0]) {
            case INFO_CMD_GET_PIN_CONFIG:
                reply_len = build_pin_config_reply(reply);
                break;
            case INFO_CMD_GET_FW_VERSION:
                reply_len = build_fw_version_reply(reply);
                break;
            default:
                continue;
        }

        /* Reply directly to whoever asked (carried in the inbound
         * message's device/task_id), not a hardcoded destination -- this
         * task doesn't need to know or care who's on the other end. */
        esp_err_t err = uart_protocol_send(ctx->proto, msg.device, msg.task_id, UART_TASK_ID_INFO,
                                            reply, reply_len, 1000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "info reply (cmd 0x%02X) to dev%u/task%u failed: %s", msg.payload[0],
                     msg.device, msg.task_id, esp_err_to_name(err));
        }
    }
}

static void info_boot_push_task(void *arg)
{
    info_bridge_ctx_t *ctx = (info_bridge_ctx_t *)arg;

    uint8_t reply[48]; /* version(2) + dirty(1) + commit_len(1) + commit + datetime_len(1) + datetime */
    size_t reply_len = build_fw_version_reply(reply);

    esp_err_t err = uart_protocol_send(ctx->proto, UART_PROTO_DEVICE_HOST, UART_TASK_ID_INFO,
                                        UART_TASK_ID_INFO, reply, reply_len, 300);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "boot version push not delivered (%s) -- fine if nothing was connected yet",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "boot version push delivered");
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
