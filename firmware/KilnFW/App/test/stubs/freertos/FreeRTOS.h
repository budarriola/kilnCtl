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

/* 2026-08-22: added for profile_executor.c's/autotune_engine.c's host tests
 * (test_profile_executor_prestart.c, test_autotune_engine_prestart.c), which
 * #include those .c files directly and so need ticks_to_s()/ticks_to_ms()'s
 * configTICK_RATE_HZ to resolve. Value is arbitrary (never actually reached
 * by those tests -- every call site sits behind the pre-start guard under
 * test), 1000 just matches this codebase's actual sdkconfig (1ms ticks). */
#define configTICK_RATE_HZ 1000u

/* 2026-08-22: added for boot_button.c's host tests (test_boot_button.c) --
 * its now_ms() helper multiplies xTaskGetTickCount() by this, mirroring
 * ota_http.c's identical now_ms(). 1, matching this codebase's actual
 * sdkconfig (1ms ticks, same value configTICK_RATE_HZ above already
 * encodes) -- never actually reached with a non-zero xTaskGetTickCount()
 * by any host test (that stub always returns 0), so the value only matters
 * for the code to compile. */
#define portTICK_PERIOD_MS 1u

#endif // TEST_STUB_FREERTOS_H
