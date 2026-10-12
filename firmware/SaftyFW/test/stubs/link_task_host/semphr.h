#ifndef SAFTYFW_TEST_STUB_SEMPHR_H
#define SAFTYFW_TEST_STUB_SEMPHR_H
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t h);
#endif
