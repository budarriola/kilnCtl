// Host test campaign 10 (uart_bridge helpers): uart_bridge.c's untrusted-
// payload guards (bridge_args_ok, bridge_range_ok, bridge_clamp_auto_period),
// the little-endian/lstring codecs, bridge_reply_reject's wire shape and the
// PC-link watchdog task (link_watchdog_task: relay drop on link loss, per-mask
// retry, stay-off-on-recovery).
//
// #includes uart_bridge.c directly (same convention as
// test_uart_bridge_ext_worker.c) and pre-empts the include guards of every
// header that drags in LVGL/panel/SPI types the host stubs do not cover; the
// handful of symbols the .c actually uses from them are faked below.
// vTaskDelay is replaced with a fake that longjmps out of the otherwise
// infinite watchdog loop after a scripted number of ticks.
#define UART_BRIDGE_H
#define FACTORY_RESET_H
#define WATCHDOG_CFG_H
#define ILI9488_H
#define DANGER_MODE_H
#define KILNCTL_HEAT_INTERLOCK_H
#define KILN_IO_H
#define KILN_IO_OWNER_H
#define KILN_UI_H
#define LVGL_PORT_H
#define OTA_HTTP_H
#define RELAY_AUTHORITY_H
#define SETTINGS_H
#define STACK_MARGIN_H
#define THERMO_OWNER_H
#define UART_TASK_IDS_H
#define WIFI_PROV_H
#define ZONES_CONFIG_ACCESSORS_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "../../../../hwAbstraction/esp/uart/uart_protocol.h"

int g_test_failures = 0;
int g_test_count = 0;

#define CHECK(cond) TEST_CHECK((cond), #cond)

#define UART_BRIDGE_LINK_TIMEOUT_MS 5000u
#define UART_BRIDGE_LINK_CHECK_MS 250u

typedef struct kiln_io_s { int unused; } kiln_io_t;
typedef struct SafetyLinkClass_s { int unused; } SafetyLinkClass;

/* ---- fakes for symbols uart_bridge.c uses ---- */
static unsigned g_tick = 0;
static unsigned g_delay_calls = 0;
static unsigned g_delay_limit = 0;
static jmp_buf g_loop_exit;
static bool g_danger = false;
static uint8_t g_unowned = 0x0F;
static unsigned g_drop_calls = 0;
static uint8_t g_drop_last_mask = 0xEE;
static uint8_t g_drop_last_value = 0xEE;
static esp_err_t g_drop_result = ESP_OK;
static unsigned g_send_calls = 0;
static esp_err_t g_send_result = ESP_OK;
static uint8_t g_sent[300];
static size_t g_sent_len = 0;
static uint8_t g_sent_src_task = 0;
static uint32_t g_sent_timeout = 0;

#undef xTaskGetTickCount
#define xTaskGetTickCount() ((TickType_t)g_tick)
#undef vTaskDelay
#define vTaskDelay(t) fake_vTaskDelay(t)
static void fake_vTaskDelay(TickType_t t)
{
    (void)t;
    g_tick += UART_BRIDGE_LINK_CHECK_MS; /* 1 kHz tick: ms == ticks */
    if (++g_delay_calls > g_delay_limit) {
        longjmp(g_loop_exit, 1);
    }
}

bool danger_mode_active(void) { return g_danger; }
uint8_t link_watchdog_decide_unowned_mask(bool danger)
{
    return danger ? 0u : g_unowned;
}
esp_err_t kiln_io_owner_command_set_relay_mask_authorized(uint8_t mask, uint8_t value)
{
    g_drop_calls++;
    g_drop_last_mask = mask;
    g_drop_last_value = value;
    return g_drop_result;
}
esp_err_t uart_protocol_send(uart_protocol_t *proto, uart_proto_device_t dst_device, uint8_t dst_task,
                             uint8_t src_task, const uint8_t *payload, size_t length, uint32_t ack_timeout_ms)
{
    (void)proto; (void)dst_device; (void)dst_task;
    g_send_calls++;
    g_sent_src_task = src_task;
    g_sent_timeout = ack_timeout_ms;
    g_sent_len = length < sizeof(g_sent) ? length : sizeof(g_sent);
    memcpy(g_sent, payload, g_sent_len);
    return g_send_result;
}
void stack_margin_register(const char *n, void *h, unsigned s) { (void)n; (void)h; (void)s; }
static void fake_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
/* uart_bridge.c has #if blocks inside ESP_LOGW()/ESP_LOGE() arguments, which MSVC rejects inside a
 * function-like macro call; object-like macros turn them into plain function calls. */
#undef ESP_LOGW
#undef ESP_LOGE
#define ESP_LOGW fake_log
#define ESP_LOGE fake_log

#include "../drivers/bridge/link_watchdog_decide.h"
#include "../drivers/bridge/uart_bridge.c"

/* ---------------------------------------------------------------- */

static uart_proto_message_t make_msg(uint8_t len)
{
    uart_proto_message_t m;
    memset(&m, 0xA5, sizeof(m));
    m.device = UART_PROTO_DEVICE_HOST;
    m.task_id = 7;
    m.length = len;
    m.payload[0] = 0x42;
    return m;
}

static void test_args_ok(void)
{
    TEST_SECTION("bridge_args_ok");
    uart_proto_message_t m = make_msg(3);
    CHECK(bridge_args_ok("t", &m, 3));
    CHECK(bridge_args_ok("t", &m, 2));
    CHECK(bridge_args_ok("t", &m, 0));
    CHECK(!bridge_args_ok("t", &m, 4));
    m.length = 0;
    CHECK(!bridge_args_ok("t", &m, 1));
    CHECK(bridge_args_ok("t", &m, 0));
    m.length = 255;
    CHECK(bridge_args_ok("t", &m, 255));
    CHECK(!bridge_args_ok("t", &m, 256));
    CHECK(!bridge_args_ok("t", &m, (size_t)-1));
}

static void test_range_ok(void)
{
    TEST_SECTION("bridge_range_ok");
    CHECK(bridge_range_ok("t", 1, "relay", 1, 1, 4));
    CHECK(bridge_range_ok("t", 1, "relay", 4, 1, 4));
    CHECK(!bridge_range_ok("t", 1, "relay", 0, 1, 4));
    CHECK(!bridge_range_ok("t", 1, "relay", 5, 1, 4));
    CHECK(bridge_range_ok("t", 1, "pin", 7, 7, 7));
    CHECK(!bridge_range_ok("t", 1, "pin", 6, 7, 7));
    CHECK(!bridge_range_ok("t", 1, "pin", 8, 7, 7));
    CHECK(bridge_range_ok("t", 1, "x", 0, 0, 0));
    CHECK(bridge_range_ok("t", 1, "x", 0xFFFFFFFFu, 0, 0xFFFFFFFFu));
    CHECK(!bridge_range_ok("t", 1, "x", 0xFFFFFFFFu, 0, 0xFFFFFFFEu));
    CHECK(!bridge_range_ok("t", 1, "inverted", 5, 9, 3));
}

static void test_clamp(void)
{
    TEST_SECTION("bridge_clamp_auto_period");
    CHECK(bridge_clamp_auto_period("t", 0) == 0);
    CHECK(bridge_clamp_auto_period("t", 1) == BRIDGE_AUTO_REPORT_MIN_MS);
    CHECK(bridge_clamp_auto_period("t", BRIDGE_AUTO_REPORT_MIN_MS - 1) == BRIDGE_AUTO_REPORT_MIN_MS);
    CHECK(bridge_clamp_auto_period("t", BRIDGE_AUTO_REPORT_MIN_MS) == BRIDGE_AUTO_REPORT_MIN_MS);
    CHECK(bridge_clamp_auto_period("t", BRIDGE_AUTO_REPORT_MIN_MS + 1) == BRIDGE_AUTO_REPORT_MIN_MS + 1);
    CHECK(bridge_clamp_auto_period("t", 65535) == 65535);
}

static void test_codecs(void)
{
    TEST_SECTION("little-endian and lstring codecs");
    uint8_t b[8];
    bridge_put_u16_le(b, 0x1234);
    CHECK(b[0] == 0x34 && b[1] == 0x12);
    CHECK(bridge_u16_le(b) == 0x1234);
    bridge_put_u16_le(b, 0xFFFF);
    CHECK(bridge_u16_le(b) == 0xFFFF);
    bridge_put_u32_le(b, 0xA1B2C3D4u);
    CHECK(b[0] == 0xD4 && b[1] == 0xC3 && b[2] == 0xB2 && b[3] == 0xA1);
    bridge_put_f32_le(b, 1.5f);
    CHECK(bridge_f32_le(b) == 1.5f);
    bridge_put_f32_le(b, -273.15f);
    CHECK(bridge_f32_le(b) == -273.15f);

    uint8_t out[16];
    memset(out, 0xEE, sizeof(out));
    size_t o = bridge_put_lstring(out, sizeof(out), 2, "abc");
    CHECK(o == 2 + 1 + 3 && out[2] == 3 && memcmp(&out[3], "abc", 3) == 0);
    CHECK(out[0] == 0xEE && out[1] == 0xEE && out[6] == 0xEE); /* nothing outside written */
    memset(out, 0xEE, sizeof(out));
    o = bridge_put_lstring(out, 8, 2, "abcdefghijkl"); /* room = 8-2-1 = 5 */
    CHECK(o == 8 && out[2] == 5 && memcmp(&out[3], "abcde", 5) == 0 && out[8] == 0xEE);
    memset(out, 0xEE, sizeof(out));
    o = bridge_put_lstring(out, 8, 8, "x"); /* o == cap: nothing written, o returned unchanged */
    CHECK(o == 8 && out[8] == 0xEE);
    o = bridge_put_lstring(out, 4, 6, "x"); /* o > cap */
    CHECK(o == 6 && out[6] == 0xEE);
    o = bridge_put_lstring(out, 4, 3, "x"); /* room = 0: length byte only */
    CHECK(o == 4 && out[3] == 0);
    uint8_t big[400];
    char text[301];
    memset(text, 'z', 300);
    text[300] = '\0';
    o = bridge_put_lstring(big, sizeof(big), 0, text);
    CHECK(o == 256 && big[0] == 255);
    o = bridge_put_lstring(big, sizeof(big), 0, "");
    CHECK(o == 1 && big[0] == 0);
}

static void reset_send(void)
{
    g_send_calls = 0;
    g_send_result = ESP_OK;
    g_sent_len = 0;
    memset(g_sent, 0, sizeof(g_sent));
}

static void test_reply_reject(void)
{
    TEST_SECTION("bridge_reply_reject / unsupported / push");
    uart_protocol_t proto;
    memset(&proto, 0, sizeof(proto));
    uart_proto_message_t m = make_msg(1);

    reset_send();
    bridge_reply_reject(&proto, &m, 9, 0x31, "bad arg");
    CHECK(g_send_calls == 1 && g_sent_src_task == 9);
    CHECK(g_sent_len == 2 + 1 + 7 && g_sent[0] == 0x31 && g_sent[1] == 0 && g_sent[2] == 7 &&
          memcmp(&g_sent[3], "bad arg", 7) == 0);
    CHECK(g_sent_timeout == BRIDGE_REPLY_ACK_TIMEOUT_MS);

    reset_send();
    bridge_reply_reject(&proto, &m, 9, 0x31, NULL);
    CHECK(g_sent_len == 2 && g_sent[0] == 0x31 && g_sent[1] == 0);
    reset_send();
    bridge_reply_reject(&proto, &m, 9, 0x31, "");
    CHECK(g_sent_len == 2);

    reset_send();
    bridge_reply_unsupported(&proto, &m, 9, 0x77);
    CHECK(g_sent_len == 2 + 1 + 11 && g_sent[0] == 0x77 && g_sent[1] == 0 &&
          memcmp(&g_sent[3], "unsupported", 11) == 0);
    CHECK(g_sent_len > 2); /* never the 2-byte empty-success shape */

    char longr[400];
    memset(longr, 'q', sizeof(longr));
    longr[399] = '\0';
    reset_send();
    bridge_reply_reject(&proto, &m, 9, 0x31, longr);
    CHECK(g_sent_len == BRIDGE_REPLY_MAX && g_sent[2] == BRIDGE_REPLY_MAX - 3);

    /* link activity: successful send notes it, failed send does not */
    s_link_ever_seen = false;
    reset_send();
    g_send_result = ESP_FAIL;
    bridge_reply_reject(&proto, &m, 9, 1, "x");
    CHECK(!s_link_ever_seen);
    g_send_result = ESP_OK;
    bridge_reply_reject(&proto, &m, 9, 1, "x");
    CHECK(s_link_ever_seen);

    s_link_ever_seen = false;
    g_send_result = ESP_FAIL;
    bridge_push(&proto, UART_PROTO_DEVICE_HOST, 3, 4, (const uint8_t *)"ab", 2);
    CHECK(!s_link_ever_seen && g_sent_timeout == 300);
    g_send_result = ESP_OK;
    bridge_push(&proto, UART_PROTO_DEVICE_HOST, 3, 4, (const uint8_t *)"ab", 2);
    CHECK(s_link_ever_seen);
}

/* ---- link watchdog ---- */
static link_watchdog_ctx_t g_ctx;
static kiln_io_t g_io;

static void wd_reset(void)
{
    g_tick = 100000;
    g_delay_calls = 0;
    g_danger = false;
    g_unowned = 0x0F;
    g_drop_calls = 0;
    g_drop_last_mask = 0xEE;
    g_drop_last_value = 0xEE;
    g_drop_result = ESP_OK;
    s_link_ever_seen = false;
    s_link_last_activity = 0;
    g_ctx.io = &g_io;
    g_ctx.link = NULL;
}

static void wd_run(unsigned iterations)
{
    g_delay_limit = iterations;
    g_delay_calls = 0;
    if (setjmp(g_loop_exit) == 0) {
        link_watchdog_task(&g_ctx);
    }
}

static void test_watchdog(void)
{
    TEST_SECTION("link_watchdog_task");

    /* never-seen host: link is down, unowned relays dropped once, value 0 */
    wd_reset();
    wd_run(5);
    CHECK(g_drop_calls == 1);
    CHECK(g_drop_last_mask == 0x0F && g_drop_last_value == 0);

    /* host seen but silent: still up until the timeout */
    wd_reset();
    s_link_ever_seen = true;
    s_link_last_activity = g_tick; /* activity at t0 */
    wd_run(10); /* 10 * 250 ms = 2.5 s < 5 s */
    CHECK(g_drop_calls == 0);

    /* silent past the timeout: dropped */
    wd_reset();
    s_link_ever_seen = true;
    s_link_last_activity = g_tick;
    wd_run(21); /* 5250 ms */
    CHECK(g_drop_calls == 1 && g_drop_last_mask == 0x0F);

    /* boundary: exactly timeout ticks since activity counts as lost, one tick less as up */
    wd_reset();
    s_link_ever_seen = true;
    s_link_last_activity = g_tick - (UART_BRIDGE_LINK_TIMEOUT_MS - UART_BRIDGE_LINK_CHECK_MS) + 1;
    wd_run(1); /* now - last = 5000 - 1 + ... */
    CHECK(g_drop_calls == 0);
    wd_reset();
    s_link_ever_seen = true;
    s_link_last_activity = g_tick - (UART_BRIDGE_LINK_TIMEOUT_MS - UART_BRIDGE_LINK_CHECK_MS);
    wd_run(1); /* elapsed == timeout exactly */
    CHECK(g_drop_calls == 1);

    /* failed drop is retried every tick until it succeeds, then stops */
    wd_reset();
    g_drop_result = ESP_FAIL;
    wd_run(4);
    CHECK(g_drop_calls == 4);
    /* a successful first drop is sticky: later ticks do not repeat it */
    wd_reset();
    wd_run(6);
    CHECK(g_drop_calls == 1);

    /* nothing unowned (all relays on-board owned): no write at all */
    wd_reset();
    g_unowned = 0;
    wd_run(5);
    CHECK(g_drop_calls == 0);

    /* danger mode suppresses the drop wholesale */
    wd_reset();
    g_danger = true;
    wd_run(5);
    CHECK(g_drop_calls == 0);

    /* only the unowned subset is written */
    wd_reset();
    g_unowned = 0x05;
    wd_run(3);
    CHECK(g_drop_calls == 1 && g_drop_last_mask == 0x05 && g_drop_last_value == 0);

    /* no expander handle: no write attempt */
    wd_reset();
    g_ctx.io = NULL;
    wd_run(5);
    CHECK(g_drop_calls == 0);

    /* link comes back: relays are NOT re-energized (no write with nonzero value), and a
     * later outage drops again */
    wd_reset();
    s_link_ever_seen = true;
    s_link_last_activity = g_tick - 6000; /* lost */
    wd_run(2);
    CHECK(g_drop_calls == 1);
    CHECK(g_drop_last_value == 0);
}

static void test_start_link_watchdog(void)
{
    TEST_SECTION("uart_bridge_start_link_watchdog");
    CHECK(uart_bridge_start_link_watchdog(NULL, NULL) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_args_ok();
    test_range_ok();
    test_clamp();
    test_codecs();
    test_reply_reject();
    test_watchdog();
    test_start_link_watchdog();
    printf("test_uart_bridge_core: %d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
