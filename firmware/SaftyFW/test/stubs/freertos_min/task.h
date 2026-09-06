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

#endif // SAFTYFW_TEST_STUB_TASK_H
