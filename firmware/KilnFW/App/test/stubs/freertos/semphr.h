// Host-test stub -- see stubs/esp_err.h for why these exist.
#ifndef TEST_STUB_SEMPHR_H
#define TEST_STUB_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct semaphore_s *SemaphoreHandle_t;

/* 2026-08-21: added for wifi_prov.c's host tests. post_and_wait() (never
 * called by the tests -- they call do_*() bodies directly) is the only
 * caller of these; they exist purely so the file compiles and links. */
typedef struct {
    int dummy;
} StaticSemaphore_t;

static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    return (SemaphoreHandle_t)storage;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)sem;
    (void)ticks;
    return pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t sem) { (void)sem; }

#endif // TEST_STUB_SEMPHR_H
