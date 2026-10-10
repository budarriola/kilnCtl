// task.h -- minimal host-test stub, see FreeRTOS.h in this same directory
// for scope/rationale. Bodies live in test_relay_owner_gpio_init_stubs.c.
#ifndef SAFTYFW_TEST_STUB_TASK_H
#define SAFTYFW_TEST_STUB_TASK_H

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words,
                        void *arg, UBaseType_t priority, TaskHandle_t *out_handle);
void       vTaskCoreAffinitySet(TaskHandle_t task, UBaseType_t affinity_mask);
TickType_t xTaskGetTickCount(void);

// current_task.c's current_task_fn() references these (vTaskDelayUntil in
// its for(;;) loop, taskENTER/EXIT_CRITICAL around the published-snapshot
// copy) -- that function is never INVOKED on host (no scheduler; only
// current_task_start()'s pre-task-creation code runs for real), but it
// still has to COMPILE and link, since its address is taken and passed to
// xTaskCreate(). vTaskDelayUntil() gets a real (unused) stub body in
// test_relay_owner_gpio_init_stubs.c; the critical-section macros are
// no-ops here, matching that they are never actually entered.
void vTaskDelayUntil(TickType_t *previous_wake_time, TickType_t time_increment);
void vTaskDelay(TickType_t ticks);
#define taskENTER_CRITICAL() do {} while (0)
#define taskEXIT_CRITICAL()  do {} while (0)

#endif // SAFTYFW_TEST_STUB_TASK_H
