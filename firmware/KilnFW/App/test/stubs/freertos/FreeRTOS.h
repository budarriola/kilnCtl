// Host-test stub -- see stubs/esp_err.h for why these exist.
#ifndef TEST_STUB_FREERTOS_H
#define TEST_STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif // TEST_STUB_FREERTOS_H
