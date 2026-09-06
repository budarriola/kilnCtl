// test_relay_owner_gpio_init_stubs.c -- the minimal FreeRTOS/watchdog_task
// bodies relay_owner.c's host build needs to link (see
// stubs/freertos_min/FreeRTOS.h for scope/rationale). relay_owner_task()
// itself (the for(;;) loop these back) is never invoked by
// test_relay_owner_gpio_init.c -- there is no scheduler on host -- these
// exist purely so the translation unit links; relay_owner_start()'s
// pre-task-creation code (the part under test: hal_gpio_init_out(), then
// xQueueCreate()) runs for real against these stubs.
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "../src/tasks/watchdog_task.h"

static int s_dummy_queue;
static int s_dummy_task;

QueueHandle_t xQueueCreate(UBaseType_t num_items, UBaseType_t item_size)
{
    (void)num_items;
    (void)item_size;
    return &s_dummy_queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait)
{
    (void)queue;
    (void)item;
    (void)ticks_to_wait;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *out_item, TickType_t ticks_to_wait)
{
    (void)queue;
    (void)out_item;
    (void)ticks_to_wait;
    return pdFALSE; // never called: relay_owner_task()'s loop never runs on host
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words,
                        void *arg, UBaseType_t priority, TaskHandle_t *out_handle)
{
    (void)fn;
    (void)name;
    (void)stack_words;
    (void)arg;
    (void)priority;
    if (out_handle) {
        *out_handle = &s_dummy_task;
    }
    return pdPASS;
}

void vTaskCoreAffinitySet(TaskHandle_t task, UBaseType_t affinity_mask)
{
    (void)task;
    (void)affinity_mask;
}

TickType_t xTaskGetTickCount(void)
{
    return 0;
}

void watchdog_task_checkin(watchdog_checkin_id_t id)
{
    (void)id; // never called: relay_owner_task()'s loop never runs on host
}
