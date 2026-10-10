// Private shim for test_dashboard_http_relay.c: the shared host stub plus the
// heap_caps statistics getters dashboard_get_status() reads. Definitions are
// in the test file.
#ifndef DASHBOARD_HTTP_RELAY_ESP_HEAP_CAPS_H
#define DASHBOARD_HTTP_RELAY_ESP_HEAP_CAPS_H
#include "../stubs/esp_heap_caps.h"
#define MALLOC_CAP_DMA (1 << 3)
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
#endif
