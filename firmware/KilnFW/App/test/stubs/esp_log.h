// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. Logging is a no-op on the host build: the
// arguments are still fully type-checked (this consumes them through a real
// variadic function) so a bad format string is still a compile error, it
// just never prints anything.
#ifndef TEST_STUB_ESP_LOG_H
#define TEST_STUB_ESP_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Ring buffer of the last few formatted log lines (any level), added
 * 2026-09-08 for the on/off zone UART-trace host test
 * (test_profile_executor_prestart.c's on/off logging tests): the owner's
 * decision on that feature is "trust the GPIO, but prove it from UART log
 * output" -- which makes the log text itself the evidence, and a silent
 * regression in a message's wording or a dropped line would silently
 * remove that coverage with every OTHER host test staying green (they only
 * cared that logging didn't crash, never what it said). This buffer lets a
 * test call esp_log_test_capture_reset() before an action and esp_log_
 * test_capture_contains(needle) after, instead of trusting the no-op back
 * to print nothing. Every pre-existing host test that never calls those two
 * functions is unaffected -- capture still runs (cheap: one vsnprintf per
 * call) but nothing reads it. */
#define ESP_LOG_TEST_CAPTURE_MAX 16
static char g_esp_log_capture[ESP_LOG_TEST_CAPTURE_MAX][256];
static int g_esp_log_capture_count = 0;

static inline void esp_log_test_capture_reset(void)
{
    g_esp_log_capture_count = 0;
}

static inline bool esp_log_test_capture_contains(const char *needle)
{
    for (int i = 0; i < g_esp_log_capture_count; i++) {
        if (strstr(g_esp_log_capture[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static inline void esp_log_test_capture_add(const char *fmt, va_list ap)
{
    if (g_esp_log_capture_count >= ESP_LOG_TEST_CAPTURE_MAX) {
        return; /* budget exhausted for this test action -- see the struct's own comment */
    }
    vsnprintf(g_esp_log_capture[g_esp_log_capture_count], sizeof(g_esp_log_capture[0]), fmt, ap);
    g_esp_log_capture_count++;
}

static inline void esp_log_noop(const char *tag, const char *fmt, ...)
{
    (void)tag;
    va_list ap;
    va_start(ap, fmt);
    esp_log_test_capture_add(fmt, ap);
    va_end(ap);
}

/* ESP_LOGE gets its own counter (still captured otherwise -- same convention
 * as esp_log_noop() above) so a host test can assert an error was actually
 * logged, not just that the code path that would log didn't crash. Added
 * for dashboard_json.c's truncation test (firmware cleanup pass, item 2):
 * proving append_zone_status_json() signals a truncation, not just that it
 * still produces balanced JSON. Every OTHER host test that never reads this
 * counter is unaffected -- it starts at 0 and nothing but ESP_LOGE touches it. */
static unsigned g_esp_loge_calls = 0;
static inline void esp_loge_noop(const char *tag, const char *fmt, ...)
{
    (void)tag;
    g_esp_loge_calls++;
    va_list ap;
    va_start(ap, fmt);
    esp_log_test_capture_add(fmt, ap);
    va_end(ap);
}

#define ESP_LOGE(tag, fmt, ...) esp_loge_noop((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_noop((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_noop((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) esp_log_noop((tag), (fmt), ##__VA_ARGS__)

/* 2026-08-21: added for uart_log_bridge.c's host test (test_uart_log_bridge.c),
 * which #includes uart_log_bridge.c directly to reach its static
 * uart_log_vprintf()/uart_log_parse_level() and calls esp_log_set_vprintf()
 * only to satisfy the linker -- the test calls uart_log_vprintf() itself
 * directly, never through a real ESP_LOGx call, so this hook is never
 * actually invoked by anything in these tests. */
typedef int (*vprintf_like_t)(const char *, va_list);

static inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    (void)func;
    return NULL;
}

#endif // TEST_STUB_ESP_LOG_H
