// Host-test stub -- see stubs/esp_err.h for why these exist.
#ifndef TEST_STUB_FREERTOS_H
#define TEST_STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

/* 2026-08-21: added for wifi_prov.c's host tests (test_wifi_prov.c), which
 * pull in queue/task/semaphore stubs that need the real pdTRUE/pdFALSE/pdPASS
 * vocabulary. */
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0

#endif // TEST_STUB_FREERTOS_H
