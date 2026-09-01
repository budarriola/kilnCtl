// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. Logging is a no-op on the host build: the
// arguments are still fully type-checked (this consumes them through a real
// variadic function) so a bad format string is still a compile error, it
// just never prints anything.
#ifndef TEST_STUB_ESP_LOG_H
#define TEST_STUB_ESP_LOG_H

#include <stdarg.h>

static inline void esp_log_noop(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

/* ESP_LOGE gets its own counter (still a no-op otherwise -- same convention
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
    (void)fmt;
    g_esp_loge_calls++;
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
