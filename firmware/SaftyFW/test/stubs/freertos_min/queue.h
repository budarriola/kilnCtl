// queue.h -- minimal host-test stub, see FreeRTOS.h in this same directory
// for scope/rationale. Bodies live in test_relay_owner_gpio_init_stubs.c.
#ifndef SAFTYFW_TEST_STUB_QUEUE_H
#define SAFTYFW_TEST_STUB_QUEUE_H

#include "FreeRTOS.h"

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t num_items, UBaseType_t item_size);
BaseType_t    xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait);
BaseType_t    xQueueReceive(QueueHandle_t queue, void *out_item, TickType_t ticks_to_wait);

#endif // SAFTYFW_TEST_STUB_QUEUE_H
