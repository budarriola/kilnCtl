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

/* 2026-08-22: aborts on a NULL handle, matching real FreeRTOS's
 * xQueueSemaphoreTake() assert(( pxQueue )) -- the exact panic recovery
 * mode exposed in profile_executor.c/autotune_engine.c (a public function
 * called before *_start() has run, taking a mutex that doesn't exist yet).
 * Host tests proving those modules' pre-start guards
 * (test_profile_executor_prestart.c, test_autotune_engine_prestart.c) rely
 * on this to actually fail loudly if a guard is ever removed, rather than
 * silently no-op'ing through a NULL handle the way this stub used to --
 * which would have let a missing guard pass host tests while still
 * panicking real hardware. */
#include <assert.h>
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem != NULL && "xSemaphoreTake on a NULL handle -- would assert/panic on real FreeRTOS");
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
