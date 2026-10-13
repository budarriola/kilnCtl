// update_fetch host test: tasks are Windows threads (fake_support.c).
#ifndef UF_STUB_TASK_H
#define UF_STUB_TASK_H
#define TEST_STUB_TASK_H
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
typedef struct task_s *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define tskIDLE_PRIORITY 0u
#define tskNO_AFFINITY ((BaseType_t)-1)
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned long stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
void vTaskDelete(TaskHandle_t t);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
#endif
