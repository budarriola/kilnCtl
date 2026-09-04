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
#include "zones_http.h"
#include "uart_bridge_internal.h"

static const char *TAG = "uart_bridge";

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

/* byte0=count-this-page(N) + byte1=truncated(0/1) + byte2=next_start_index
 * (only meaningful when truncated=1) + N * {name_len(1) +
 * name(<=STACK_MARGIN_NAME_MAX-1) + configured_stack_bytes(4) + hwm_bytes(4)
 * + flags(1)}.
 *
 * DRAM_PSRAM_PLAN.md section 7 (2026-09-02, cap-raise pass): this used to
 * report only byte0=count with no truncation signal, sized against a stale
 * "today's <=6 registered tasks" assumption. The real count was already 28
 * at STACK_MARGIN_MAX_TASKS=28 (now 40) -- worst case
 * 1 + 40 * (1 + 19 + 4 + 4 + 1) = 1 + 40*29 = 1161, nowhere close to fitting
 * BRIDGE_REPLY_MAX (253) in one reply, and even the REAL (short) names
 * registered before this pass only left room for roughly the first 10 of 28
 * entries -- most of the registry was invisible over this wire path with no
 * indication anything was missing beyond an ESP_LOGW nobody was watching.
 * `start_index` (from the request's optional byte1, 0 if absent -- see
 * uart_task_ids.h) lets the caller page through the whole registry; the
 * `truncated`/`next_start_index` bytes make that non-silent instead of
 * requiring the caller to notice the reply looks short. */
static size_t build_stack_margin_reply(uint8_t *out, size_t start_index)
{
    size_t o = 3; /* [0]=count-this-page, [1]=truncated, [2]=next_start_index -- filled in below */
    size_t n = 0;
    size_t total = stack_margin_count();
    size_t i = start_index;
    bool truncated = false;

    for (; i < total; ++i) {
        const char *name = NULL;
        uint32_t configured_bytes = 0, hwm_bytes = 0;
        stack_margin_level_t level = STACK_MARGIN_LEVEL_OK;
        bool alive = false;
        if (!stack_margin_read(i, &name, &configured_bytes, &hwm_bytes, &level, &alive)) {
            continue; /* index vanished mid-loop -- can't happen (count() is stable), but never fabricate an entry */
        }

        size_t name_len = strlen(name);
        if (name_len > STACK_MARGIN_NAME_MAX - 1) {
            name_len = STACK_MARGIN_NAME_MAX - 1; /* stack_margin_register() already truncates at registration; belt-and-braces here too */
        }
        size_t entry_len = 1 + name_len + 4 + 4 + 1;
        if (o + entry_len > BRIDGE_REPLY_MAX) {
            /* Would overflow the shared reply buffer -- stop rather than
             * write past it, and say so on the wire (truncated=1,
             * next_start_index=i) rather than just logging: a caller that
             * never looks at the device log has no other way to know this
             * page wasn't the whole registry. */
            ESP_LOGW(TAG, "info: GET_STACK_MARGIN page truncated at %u/%u entries from "
                          "start_index=%u -- reply buffer full, next_start_index=%u",
                     (unsigned)n, (unsigned)total, (unsigned)start_index, (unsigned)i);
            truncated = true;
            break;
        }

        out[o++] = (uint8_t)name_len;
        memcpy(&out[o], name, name_len);
        o += name_len;
        out[o++] = (uint8_t)(configured_bytes & 0xFF);
        out[o++] = (uint8_t)((configured_bytes >> 8) & 0xFF);
        out[o++] = (uint8_t)((configured_bytes >> 16) & 0xFF);
        out[o++] = (uint8_t)((configured_bytes >> 24) & 0xFF);
        out[o++] = (uint8_t)(hwm_bytes & 0xFF);
        out[o++] = (uint8_t)((hwm_bytes >> 8) & 0xFF);
        out[o++] = (uint8_t)((hwm_bytes >> 16) & 0xFF);
        out[o++] = (uint8_t)((hwm_bytes >> 24) & 0xFF);
        out[o++] = (uint8_t)((alive ? 0x01u : 0x00u) | (((uint8_t)level & 0x03u) << 1));
        n++;
    }

    out[0] = (uint8_t)n;
    out[1] = truncated ? 1u : 0u;
    /* i stops at UINT8_MAX worst case (STACK_MARGIN_MAX_TASKS is nowhere
     * near 256), so this cast never wraps a real next-page index. */
    out[2] = truncated ? (uint8_t)i : 0u;
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
            case INFO_CMD_GET_STACK_MARGIN: {
                /* byte1 = start_index, optional -- a pre-paging PC build
                 * sends only byte0 (msg.length==1), which reads as
                 * start_index=0, same as always. See uart_task_ids.h. */
                size_t start_index = (msg.length >= 2) ? (size_t)msg.payload[1] : 0;
                reply_len = build_stack_margin_reply(reply, start_index);
                break;
            }
            default:
                ESP_LOGW(TAG, "info: unknown subcmd 0x%02X -- rejected", msg.payload[0]);
                bridge_reply_unsupported(ctx->proto, &msg, UART_TASK_ID_INFO, msg.payload[0]);
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

    /* 2026-08-22: PSRAM stack -- info_bridge_task only reports version/
     * uptime/reset-reason state, no flash/NVS access, no hardware ownership.
     *
     * 2026-09-04 (stack-headroom follow-up to commit ead4123): measured worst
     * case is GET_STACK_MARGIN itself -- its reply is always truncated (25
     * registered tasks don't fit BRIDGE_REPLY_MAX=253), so every call takes
     * the ESP_LOGW formatting path, and that path measured deeper again
     * (476 B free of 3072, 15.5%) under concurrent HTTP load, where
     * uart_protocol_send()'s retry/backoff under link contention adds its
     * own frames on top. Non-progressive across repeated adversarial bursts
     * (confirmed by hammering it), but only ~34 B above the 15% CRITICAL
     * cutoff -- thin enough, and cheap enough given this stack is PSRAM (not
     * the scarce internal-DRAM budget the ~11.9 kB cliff applies to), to
     * warrant the extra 512 B rather than leaving it this close to the line.
     * See ROADMAP.md's ead4123 entry for the before/after re-measurement. */
    static TaskHandle_t s_info_bridge_task; /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(info_bridge_task, "info_uart_bridge", 3584, &ctx, 5,
                                                         &s_info_bridge_task, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_INFO);
        return ESP_ERR_NO_MEM;
    }
    /* Registration only, no size change -- only reached with a real handle
     * since the failure branch above already returned. 3584 must match the
     * xTaskCreatePinnedToCoreWithCaps() literal above. */
    stack_margin_register("info_uart_bridge", &s_info_bridge_task, 3584);

    /* Best-effort unsolicited push so a GUI already connected at boot shows
     * the version immediately, without polling. If nothing is listening
     * (typical case -- the PC usually isn't already connected the instant
     * the board powers up) this just fails after uart_protocol_send's own
     * retries/timeout and is logged, not treated as an error: a GUI that
     * connects later independently pulls the version via
     * INFO_CMD_GET_FW_VERSION. Runs in its own one-shot task so app_main
     * isn't blocked for the ~seconds this can take to give up. */
    /* 2026-08-22: PSRAM stack, same reasoning as info_bridge_task above --
     * one-shot version push, no flash/NVS, no hardware ownership. */
    BaseType_t boot_push_created = xTaskCreatePinnedToCoreWithCaps(info_boot_push_task, "info_boot_push", 3072,
                                                                   &ctx, 5, NULL, tskNO_AFFINITY,
                                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (boot_push_created != pdPASS) {
        ESP_LOGW(TAG, "failed to start boot version-push task (non-fatal)");
    }

    return ESP_OK;
}
