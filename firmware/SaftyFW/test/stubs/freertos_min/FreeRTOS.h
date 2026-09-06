// FreeRTOS.h -- minimal host-test stub, scoped to relay_owner.c's host build
// (test_relay_owner_gpio_init.c, HAL Phase 1b). Not a general FreeRTOS
// replacement: only the types/macros relay_owner.c and its own includes
// (task_priorities.h's configUSE_CORE_AFFINITY/configNUMBER_OF_CORES guard)
// actually reference. relay_owner_task() itself is never invoked by the host
// test (no scheduler runs on host) -- this only has to let relay_owner.c
// compile and let relay_owner_start() run its pre-task-creation code
// (hal_gpio_init_out(), xQueueCreate()) for real.
#ifndef SAFTYFW_TEST_STUB_FREERTOS_H
#define SAFTYFW_TEST_STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
// current_sense.c (HAL Phase 1b hal_adc host build) uses this to convert
// xTaskGetTickCount() into a millisecond timestamp; 1 here since this stub's
// xTaskGetTickCount() is a fixed fake value, not a real tick rate.
#define portTICK_PERIOD_MS ((TickType_t)1)

#define configMINIMAL_STACK_SIZE 128u

// task_priorities.h's compile-time guards require these to hold; relay_owner.c
// is a core-1/priority-7 pico-sdk task on target, but neither macro is
// exercised for real on host (vTaskCoreAffinitySet() is a no-op stub below).
#define configUSE_CORE_AFFINITY 1
#define configNUMBER_OF_CORES   2

#endif // SAFTYFW_TEST_STUB_FREERTOS_H
