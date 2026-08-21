// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. Logging is a no-op on the host build: the
// arguments are still fully type-checked (this consumes them through a real
// variadic function) so a bad format string is still a compile error, it
// just never prints anything.
#ifndef TEST_STUB_ESP_LOG_H
#define TEST_STUB_ESP_LOG_H

static inline void esp_log_noop(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

#define ESP_LOGE(tag, fmt, ...) esp_log_noop((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_noop((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_noop((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) esp_log_noop((tag), (fmt), ##__VA_ARGS__)

#endif // TEST_STUB_ESP_LOG_H
