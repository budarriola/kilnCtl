// update_fetch host test: real blocking semaphores (Windows) so the TLS task and the writer task can run as threads.
#ifndef UF_STUB_SEMPHR_H
#define UF_STUB_SEMPHR_H
#define TEST_STUB_SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
#endif
