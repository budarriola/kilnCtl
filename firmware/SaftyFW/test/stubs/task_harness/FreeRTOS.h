// FreeRTOS.h -- host-test stub for the two task-loop harness executables
// (test_thermo_task_faults, test_watchdog_task_loop). Unlike
// stubs\freertos_min\ (which only lets a task's start() code run), this
// harness set lets the REAL task body (thermo_task_fn, watchdog_task_fn)
// run on the host: see task_harness.h for how the infinite loops are driven
// and left again. Only the surface those two tasks reference is provided.
#ifndef SAFTYFW_TASK_HARNESS_FREERTOS_H
#define SAFTYFW_TASK_HARNESS_FREERTOS_H

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
#define portTICK_PERIOD_MS ((TickType_t)1)
#define portYIELD_FROM_ISR(x) ((void)(x))

#define configMINIMAL_STACK_SIZE 128u
#define configUSE_CORE_AFFINITY 1
#define configNUMBER_OF_CORES   2

#endif
