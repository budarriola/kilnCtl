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

/* Added 2026-08-22 for boot_guard.c's host tests (test_boot_guard.c), which
 * #includes boot_guard.c directly (same convention as test_kiln_cfg_store.c
 * etc.) and so needs its xSemaphoreCreateMutex()/ensure_lock() call to link
 * and return a non-NULL handle -- a single process-wide dummy is enough:
 * xSemaphoreTake()/_Give() above are already no-ops on this stub, host tests
 * are single-threaded, and no other file needs a SECOND distinguishable
 * mutex identity. */
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int dummy;
    return (SemaphoreHandle_t)&dummy;
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
