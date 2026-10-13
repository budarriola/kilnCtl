#ifndef SAFTYFW_TASK_HARNESS_SEMPHR_H
#define SAFTYFW_TASK_HARNESS_SEMPHR_H

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks_to_wait);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t sem);

#endif
