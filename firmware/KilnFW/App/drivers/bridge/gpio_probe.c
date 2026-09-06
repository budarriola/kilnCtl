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

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gpio_probe_denylist.h"
#include "hal_gpio.h"
#include "profile_executor_state.h"
#include "settings.h"
#include "stack_margin.h"
#include "uart_task_ids.h"

#define BRIDGE_INBOX_LEN 4
#define BRIDGE_REPLY_MAX UART_PROTO_MAX_PAYLOAD
#define BRIDGE_REPLY_ACK_TIMEOUT_MS 200u

/* Deny-list itself -- see gpio_probe_denylist.h's own header comment for
 * the full rationale (what's on it, why SAFETY_TX_IO/SAFETY_RX_IO are
 * deliberately NOT on it) and why it lives in its own header rather than
 * inline here (host-testability: this translation unit, once
 * CONFIG_KILNCTL_ENABLE_GPIO_PROBE pulls in both profile_executor.h and
 * gpio_probe.h's real espInterfaces/uart_protocol.h, cannot itself be
 * compiled by the host-test harness -- gpio_probe_pin_is_denied() can be,
 * on its own, and test_gpio_probe.c does exactly that). */
static bool gpio_probe_is_denied(int gpio_num)
{
    return gpio_probe_pin_is_denied(gpio_num);
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

static bool gp_mode_to_hal(uint8_t mode, hal_gpio_dir_t *out_dir, hal_gpio_pull_t *out_pull)
{
    switch (mode) {
        case GPIO_PROBE_MODE_INPUT:          *out_dir = HAL_GPIO_DIR_IN;  *out_pull = HAL_GPIO_PULL_NONE; return true;
        case GPIO_PROBE_MODE_INPUT_PULLUP:   *out_dir = HAL_GPIO_DIR_IN;  *out_pull = HAL_GPIO_PULL_UP;   return true;
        case GPIO_PROBE_MODE_INPUT_PULLDOWN: *out_dir = HAL_GPIO_DIR_IN;  *out_pull = HAL_GPIO_PULL_DOWN; return true;
        case GPIO_PROBE_MODE_OUTPUT:         *out_dir = HAL_GPIO_DIR_OUT; *out_pull = HAL_GPIO_PULL_NONE; return true;
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
                hal_gpio_dir_t dir; hal_gpio_pull_t pull;
                if (!gp_mode_to_hal(mode, &dir, &pull)) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "unknown mode"); break;
                }
                if (gpio_probe_is_denied(gpio_num)) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "pin is on the deny-list"); break;
                }
                if (gpio_probe_run_blocked()) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, "refused: a profile is running or paused");
                    break;
                }
                /* Runtime-pin escape hatch -- see hal_gpio.h's contract note:
                 * hal_gpio_set_direction/set_pull exist specifically for this
                 * module's arbitrary-pin scanning, ordinary drivers must not
                 * reach for them. */
                hal_status_t st = hal_gpio_set_direction(gpio_num, dir);
                if (st == HAL_OK) {
                    st = hal_gpio_set_pull(gpio_num, pull);
                }
                if (st != HAL_OK) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, hal_status_to_name(st)); break;
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
                hal_status_t st = hal_gpio_set(gpio_num, level ? true : false);
                if (st != HAL_OK) {
                    gp_reply_fail(ctx->proto, &msg, subcmd, hal_status_to_name(st)); break;
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
                bool level = hal_gpio_get(gpio_num);
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
                    bool level = hal_gpio_get(ctx->tracked[i].gpio_num);
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

    /* 2026-08-20: this task is registered right after uart_bridge_ext.c's
     * CONTROL/PROFILES/AUTOTUNE/WIFI, i.e. in the same post-wifi_prov boot
     * window where the WiFi driver's own internal-SRAM buffer allocations
     * were observed racing plain xTaskCreatePinnedToCore() calls for the
     * same shrinking pool -- see uart_bridge_ext.c's retry_task_create_pinned()
     * comment for the full evidence. This task never had a retry loop at
     * all (a bare failed create here was silent, worse than the other
     * four), so it gets the same PSRAM-stack fix: WithCaps + MALLOC_CAP_SPIRAM
     * removes it from that internal-SRAM race entirely rather than papering
     * over it with retries. Not latency-critical -- it blocks on its inbox
     * like every other bridge task. */
    /* 2026-08-22: 3072 was not enough and overflowed on the *first* command
     * this task ever handled (coredump: "A stack overflow in task gpio_probe
     * has been detected", USED/FREE 3328/272, reached via SET_MODE's reply).
     * The frame is inherently large for a bridge task: uart_proto_message_t
     * msg (~260 B) lives across the whole loop, gp_reply() adds
     * reply[UART_PROTO_MAX_PAYLOAD] (253 B), gp_reply_fail() nests its own
     * body[98] under that, and every path ends in an ESP_LOGx whose
     * vsnprintf wants another ~1.5 kB. 6144 leaves real headroom rather
     * than trimming to the observed high-water mark. */
    static TaskHandle_t s_gpio_probe_task; /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(gpio_probe_task, "gpio_probe", 6144, &ctx, 3,
                                                         &s_gpio_probe_task, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_GPIO_PROBE);
        return ESP_ERR_NO_MEM;
    }
    /* Registration only, no size change -- only reached with a real handle
     * since the failure branch above already returned. 6144 must match the
     * xTaskCreatePinnedToCoreWithCaps() literal above. */
    stack_margin_register("gpio_probe", &s_gpio_probe_task, 6144);
    return ESP_OK;
}

#endif /* CONFIG_KILNCTL_ENABLE_GPIO_PROBE */
