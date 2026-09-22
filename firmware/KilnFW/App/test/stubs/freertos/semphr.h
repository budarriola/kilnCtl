// Host-test stub -- see stubs/esp_err.h for why these exist.
#ifndef TEST_STUB_SEMPHR_H
#define TEST_STUB_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct semaphore_s *SemaphoreHandle_t;

/* 2026-08-21: added for wifi_prov.c's host tests. wifi_prov_post_and_wait() (never
 * called by the tests -- they call do_*() bodies directly) is the only
 * caller of these; they exist purely so the file compiles and links. */
typedef struct {
    int dummy;
} StaticSemaphore_t;

static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    return (SemaphoreHandle_t)storage;
}

/* Added 2026-09-22 for safety_ceiling_sync.c's move off lazy TOCTOU mutex
 * creation to statically-allocated mutexes created once at init -- same
 * "identity doesn't matter on a single-threaded host test" reasoning as
 * xSemaphoreCreateBinaryStatic() above: the storage pointer itself is a
 * perfectly good non-NULL handle here. */
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return (SemaphoreHandle_t)storage;
}

/* Added 2026-09-01 for esp_spi_owner.c's host test (test_esp_spi_owner.c),
 * which #includes esp_spi_owner.c directly -- spi_owner_init() calls this
 * for its shutdown_done semaphore. A single process-wide dummy is enough,
 * same reasoning as xSemaphoreCreateMutex() below: host tests are single-
 * threaded and nothing here distinguishes semaphore identities. */
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    static int dummy;
    return (SemaphoreHandle_t)&dummy;
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
/* Fixed 2026-09-06 for MAX31856.c's HAL Phase 1b host test
 * (test_max31856_hal_spi.c), the first host test to call into business logic
 * that takes a REAL (non-NULL) mutex and expects to actually get it --
 * test_esp_spi_owner.c (below) deliberately relies on the OPPOSITE: a
 * non-NULL completion semaphore that never gets given, so xSemaphoreTake()
 * on it must still time out to simulate "the owner never answers". A single
 * unconditional return can't serve both, so this is an opt-in global default
 * (kept at pdFALSE, unchanged from before, so every existing host test that
 * takes a non-NULL handle expecting a timeout keeps seeing one) that a test
 * needing the opposite sets once at the top of main() -- see
 * test_max31856_hal_spi.c's g_test_stub_semaphore_take_default = pdTRUE. */
/* __declspec(selectany): this header is included by multiple .c files linked
 * into the same test executable (MAX31856.c AND test_max31856_hal_spi.c,
 * for instance) -- a plain global definition here would be a duplicate-
 * symbol link error. selectany (MSVC; every host test in this tree already
 * builds with cl.exe) tells the linker to keep exactly one of the identical
 * definitions and merge every reference onto it, so setting the variable
 * from the test file's TU is visible to MAX31856.c's TU too -- a plain
 * `extern` declared here with the definition in one .c file would work too,
 * but would need a home in some file every test executable already links,
 * which does not exist. */
__declspec(selectany) BaseType_t g_test_stub_semaphore_take_default = 0; /* pdFALSE */

#include <assert.h>
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem != NULL && "xSemaphoreTake on a NULL handle -- would assert/panic on real FreeRTOS");
    (void)ticks;
    return g_test_stub_semaphore_take_default;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t sem) { (void)sem; }

#endif // TEST_STUB_SEMPHR_H
