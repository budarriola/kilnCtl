// Host-test stub for the update_fetch test: heap_caps_* over the host malloc, with a settable
// "internal free / largest block" the admission checks read, and a settable allocation failure.
#ifndef UPDATE_FETCH_STUB_ESP_HEAP_CAPS_H
#define UPDATE_FETCH_STUB_ESP_HEAP_CAPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM (1 << 0)
#define MALLOC_CAP_8BIT (1 << 1)
#define MALLOC_CAP_INTERNAL (1 << 2)
#define MALLOC_CAP_DEFAULT (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

extern size_t g_fake_heap_free_internal;
extern size_t g_fake_heap_largest_internal;

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
bool esp_ptr_external_ram(const void *p);

#endif
