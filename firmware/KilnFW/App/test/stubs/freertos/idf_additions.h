// Host-test stub -- see stubs/esp_err.h for why these exist. wifi_prov.c and
// uart_log_bridge.c each call xTaskCreatePinnedToCoreWithCaps() (the
// PSRAM-capable-stack variant of xTaskCreatePinnedToCore(), declared by the
// real ESP-IDF in this header) once, to start a background task neither host
// test needs running -- same "the tests call the do_*() bodies directly"
// reasoning stubs/freertos/task.h's xTaskCreatePinnedToCore() stub already
// documents for the plain variant. Returning pdFAIL here is deliberate, not
// an oversight: both real call sites already handle failure (they log a
// warning and continue without the background task), so this is exercised,
// not undefined, behavior on the host build.
#ifndef TEST_STUB_IDF_ADDITIONS_H
#define TEST_STUB_IDF_ADDITIONS_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name,
                                                          unsigned long stack_depth, void *arg,
                                                          UBaseType_t priority, TaskHandle_t *out_handle,
                                                          BaseType_t core_id, uint32_t mem_caps)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)arg;
    (void)priority;
    (void)core_id;
    (void)mem_caps;
    if (out_handle) {
        *out_handle = NULL;
    }
    return pdFAIL;
}

#endif // TEST_STUB_IDF_ADDITIONS_H
