// FreeRTOS.h -- minimal host-test stub. Not a general FreeRTOS replacement:
// only the types/macros actually referenced by the TUs below are provided.
//
// GROWTH RULE: build_host_tests.ps1 puts this directory on the ONE global
// /I list shared by every TU in the single `cl` invocation (all sources
// compile together, not per-file) -- so any future src file that includes
// FreeRTOS.h will silently compile against this minimal stub instead of
// failing loudly, whether or not that's intended. Today's known clients,
// both HAL Phase 1b:
//   - tasks/relay_owner.c (test_relay_owner_gpio_init.c) -- needs the
//     task-creation surface (xTaskCreate/xQueueCreate/TickType_t) to let
//     relay_owner_start() run its pre-task-creation code for real;
//     relay_owner_task() itself is never invoked (no scheduler on host).
//   - current_sense.c (test_current_sense_hal_adc.c) -- needs only
//     xTaskGetTickCount()/portTICK_PERIOD_MS.
// Before adding a third client, either confirm this stub's minimal surface
// still covers it (extend deliberately, don't silently widen scope further)
// or split build_host_tests.ps1 into multiple `cl` invocations so /I can be
// scoped per-TU -- not attempted here to avoid restructuring a working
// build script for this pass.
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
