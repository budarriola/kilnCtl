#ifndef SAFTYFW_TASK_HARNESS_TASK_H
#define SAFTYFW_TASK_HARNESS_TASK_H

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words, void *arg,
                        UBaseType_t priority, TaskHandle_t *out_handle);
void       vTaskCoreAffinitySet(TaskHandle_t task, UBaseType_t affinity_mask);
TickType_t xTaskGetTickCount(void);
void       vTaskDelayUntil(TickType_t *previous_wake_time, TickType_t time_increment);
uint32_t   ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
void       vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher_priority_task_woken);

#define taskENTER_CRITICAL() do {} while (0)
#define taskEXIT_CRITICAL()  do {} while (0)

#endif
