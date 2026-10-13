#ifndef UF_STUB_IDF_ADDITIONS_H
#define UF_STUB_IDF_ADDITIONS_H
#define TEST_STUB_IDF_ADDITIONS_H
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name, unsigned long stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, uint32_t caps);
void vTaskDeleteWithCaps(TaskHandle_t t);
#endif
