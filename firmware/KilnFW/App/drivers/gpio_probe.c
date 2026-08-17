#include "gpio_probe.h"

#include "sdkconfig.h"

static const char *TAG = "gpio_probe";

#if !CONFIG_KILNCTL_ENABLE_GPIO_PROBE

/* Stub build: no task, no symbol pulls in driver/gpio.h or profile_executor.h,
 * and every subcommand in uart_task_ids.h simply goes unanswered because
 * UART_TASK_ID_GPIO_PROBE is never registered -- same as any other
 * unregistered task_id. */
esp_err_t uart_bridge_start_gpio_probe_task(uart_protocol_t *proto)
{
    (void)proto;
    return ESP_ERR_NOT_SUPPORTED;
}

#else /* CONFIG_KILNCTL_ENABLE_GPIO_PROBE */

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "profile_executor.h"
#include "settings.h"
#include "uart_task_ids.h"

#define BRIDGE_INBOX_LEN 4
#define BRIDGE_REPLY_MAX UART_PROTO_MAX_PAYLOAD
#define BRIDGE_REPLY_ACK_TIMEOUT_MS 200u

/* Pins this probe may never touch -- not "should not", *may not*. Every one
 * of these is load-bearing for something the probe itself depends on (the PC
 * link), for another peripheral (SPI/I2C/the SX1509/the display), or for the
 * safety domain. SAFETY_FAULT_IO (GPIO6) is the one the TODO calls out by
 * name: a debug tool that can drive it can silently tell the safety
 * processor "the main controller is fine" while nothing of the sort is
 * true. Checked against SET_MODE, WRITE and READ alike -- "never touch"
 * means never touch, not "never write". */
static bool gpio_probe_is_denied(int gpio_num)
{
    const int denied[] = {
        KILN_SPI_SCLK_IO, KILN_SPI_MOSI_IO, KILN_SPI_MISO_IO,
        THERMO_CS0_IO, THERMO_CS1_IO, THERMO_CS2_IO,
        I2C_MASTER_SCL_IO, I2C_MASTER_SDA_IO,
        SX1509_IRQ_IO, SX1509_RESET_IO,
        DISPLAY_CS_IO,
        UART_OWNER_TX_IO, UART_OWNER_RX_IO,
        SAFETY_TX_IO, SAFETY_RX_IO, SAFETY_FAULT_IO,
    };
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        if (denied[i] == gpio_num) {
            return true;
        }
    }
    return false;
}

/* Refused whenever a profile could be actively driving relays: this probe
 * must never become a second, ungoverned way to influence a firing. RUNNING
 * and PAUSED both count -- a paused run resumes on its own timeline and the
 * probe should not be poking pins in the meantime either. */
static bool gpio_probe_run_blocked(void)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    return st.state == PROFILE_EXEC_RUNNING || st.state == PROFILE_EXEC_PAUSED;
}

typedef struct {
    uint8_t gpio_num;
    uint8_t mode;
    bool in_use;
} tracked_pin_t;

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
    tracked_pin_t tracked[GPIO_PROBE_MAX_TRACKED];
} gpio_probe_ctx_t;

static tracked_pin_t *gpio_probe_track_find(gpio_probe_ctx_t *ctx, uint8_t gpio_num)
{
    for (size_t i = 0; i < GPIO_PROBE_MAX_TRACKED; ++i) {
        if (ctx->tracked[i].in_use && ctx->tracked[i].gpio_num == gpio_num) {
            return &ctx->tracked[i];
        }
    }
    return NULL;
}

/* Records/updates gpio_num's tracked mode. Silently drops the oldest unused
 * slot's worth of tracking if the table is full and this is a new pin --
 * READ_ALL exists for bring-up convenience, not as a guarantee, and a probe
 * session touching more than GPIO_PROBE_MAX_TRACKED pins is already unusual
 * enough that losing READ_ALL coverage of the least-recently-set one is a
 * reasonable trade against refusing SET_MODE outright. */
static void gpio_probe_track_set(gpio_probe_ctx_t *ctx, uint8_t gpio_num, uint8_t mode)
{
    tracked_pin_t *slot = gpio_probe_track_find(ctx, gpio_num);
    if (!slot) {
        for (size_t i = 0; i < GPIO_PROBE_MAX_TRACKED; ++i) {
            if (!ctx->tracked[i].in_use) {
                slot = &ctx->tracked[i];
                break;
            }
        }
    }
    if (!slot) {
        slot = &ctx->tracked[0]; /* table full: evict slot 0 */
    }
    slot->in_use = true;
    slot->gpio_num = gpio_num;
    slot->mode = mode;
}

static void gp_reply(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t subcmd,
                     const uint8_t *body, size_t body_len)
{
    uint8_t reply[BRIDGE_REPLY_MAX];
    reply[0] = subcmd;
    size_t o = 1;
    if (body_len > 0) {
        memcpy(&reply[o], body, body_len);
        o += body_len;
    }
    esp_err_t err = uart_protocol_send(proto, msg->device, msg->task_id, UART_TASK_ID_GPIO_PROBE,
                                       reply, o, BRIDGE_REPLY_ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reply (cmd 0x%02X) failed: %s", subcmd, esp_err_to_name(err));
    }
}

static void gp_reply_fail(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t subcmd,
                          const char *reason)
{
    uint8_t body[2 + 96];
    body[0] = 0; /* ok = false */
    size_t rlen = reason ? strlen(reason) : 0;
    if (rlen > 96) {
        rlen = 96;
    }
    body[1] = (uint8_t)rlen;
    if (rlen > 0) {
        memcpy(&body[2], reason, rlen);
    }
    ESP_LOGW(TAG, "refused (cmd 0x%02X): %s", subcmd, reason ? reason : "?");
    gp_reply(proto, msg, subcmd, body, 2 + rlen);
}

static bool gp_mode_to_esp(uint8_t mode, gpio_mode_t *out_dir, gpio_pull_mode_t *out_pull)
{
    switch (mode) {
        case GPIO_PROBE_MODE_INPUT:          *out_dir = GPIO_MODE_INPUT;  *out_pull = GPIO_FLOATING;     return true;
        case GPIO_PROBE_MODE_INPUT_PULLUP:   *out_dir = GPIO_MODE_INPUT;  *out_pull = GPIO_PULLUP_ONLY;  return true;
        case GPIO_PROBE_MODE_INPUT_PULLDOWN: *out_dir = GPIO_MODE_INPUT;  *out_pull = GPIO_PULLDOWN_ONLY; return true;
        case GPIO_PROBE_MODE_OUTPUT:         *out_dir = GPIO_MODE_OUTPUT; *out_pull = GPIO_FLOATING;     return true;
        default: return false;
    }
}

static void gpio_probe_task(void *arg)
{
    gpio_probe_ctx_t *ctx = (gpio_probe_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "empty payload -- rejected");
            continue;
        }
        uint8_t subcmd = msg.payload[0];

        switch (subcmd) {
            case GPIO_PROBE_CMD_SET_MODE: {
                if (msg.length < 3) { ESP_LOGW(TAG, "SET_MODE: short payload"); break; }
                uint8_t gpio_num = msg.payload[1];
                uint8_t mode = msg.payload[2];
                gpio_mode_t dir; gpio_pull_mode_t pull;
                if (!gp_mode_to_esp(mode, &dir, &pull)) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "unknown mode"); break;
                }
                if (gpio_probe_is_denied(gpio_num)) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "pin is on the deny-list"); break;
                }
                if (gpio_probe_run_blocked()) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "refused: a profile is running or paused");
                    break;
                }
                esp_err_t err = gpio_set_direction((gpio_num_t)gpio_num, dir);
                if (err == ESP_OK) {
                    err = gpio_set_pull_mode((gpio_num_t)gpio_num, pull);
                }
                if (err != ESP_OK) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, esp_err_to_name(err)); break;
                }
                gpio_probe_track_set(ctx, gpio_num, mode);
                ESP_LOGI(TAG, "gpio%u set_mode %u", gpio_num, mode);
                uint8_t ok = 1;
                gp_reply(ctx->proto, &msg, subcmd, &ok, 1);
                break;
            }
            case GPIO_PROBE_CMD_WRITE: {
                if (msg.length < 3) { ESP_LOGW(TAG, "WRITE: short payload"); break; }
                uint8_t gpio_num = msg.payload[1];
                uint8_t level = msg.payload[2];
                if (gpio_probe_is_denied(gpio_num)) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "pin is on the deny-list"); break;
                }
                if (gpio_probe_run_blocked()) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "refused: a profile is running or paused");
                    break;
                }
                tracked_pin_t *t = gpio_probe_track_find(ctx, gpio_num);
                if (!t || t->mode != GPIO_PROBE_MODE_OUTPUT) {
                    gp_reply_fail(ctx->proto, &msg, subcmd,
                                  "pin not configured OUTPUT via SET_MODE first");
                    break;
                }
                esp_err_t err = gpio_set_level((gpio_num_t)gpio_num, level ? 1 : 0);
                if (err != ESP_OK) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, esp_err_to_name(err)); break;
                }
                ESP_LOGI(TAG, "gpio%u write %u", gpio_num, level);
                uint8_t ok = 1;
                gp_reply(ctx->proto, &msg, subcmd, &ok, 1);
                break;
            }
            case GPIO_PROBE_CMD_READ: {
                if (msg.length < 2) { ESP_LOGW(TAG, "READ: short payload"); break; }
                uint8_t gpio_num = msg.payload[1];
                if (gpio_probe_is_denied(gpio_num)) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "pin is on the deny-list"); break;
                }
                int level = gpio_get_level((gpio_num_t)gpio_num);
                uint8_t body[2] = {1, (uint8_t)(level ? 1 : 0)};
                gp_reply(ctx->proto, &msg, subcmd, body, 2);
                break;
            }
            case GPIO_PROBE_CMD_READ_ALL: {
                uint8_t body[1 + GPIO_PROBE_MAX_TRACKED * 3];
                size_t o = 0;
                uint8_t count = 0;
                for (size_t i = 0; i < GPIO_PROBE_MAX_TRACKED; ++i) {
                    if (!ctx->tracked[i].in_use) {
                        continue;
                    }
                    int level = gpio_get_level((gpio_num_t)ctx->tracked[i].gpio_num);
                    body[1 + o] = ctx->tracked[i].gpio_num; o++;
                    body[1 + o] = ctx->tracked[i].mode; o++;
                    body[1 + o] = (uint8_t)(level ? 1 : 0); o++;
                    count++;
                }
                body[0] = count;
                gp_reply(ctx->proto, &msg, subcmd, body, 1 + o);
                break;
            }
            default:
                ESP_LOGW(TAG, "unknown subcmd 0x%02X -- rejected", subcmd);
                break;
        }
    }
}

esp_err_t uart_bridge_start_gpio_probe_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "GPIO probe ENABLED -- raw pin control is reachable over the PC link. "
                  "Deny-list covers SPI/I2C/SX1509/display/PC-link/safety-link pins; refused "
                  "while any profile is running or paused. Do not build this into anything "
                  "going near a real heater.");

    static gpio_probe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_GPIO_PROBE, BRIDGE_INBOX_LEN,
                                                &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreatePinnedToCore(gpio_probe_task, "gpio_probe", 3072, &ctx, 3,
                                                 NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_GPIO_PROBE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif /* CONFIG_KILNCTL_ENABLE_GPIO_PROBE */
