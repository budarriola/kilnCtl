// test_relay_owner_gpio_init_stubs.c -- the minimal FreeRTOS/watchdog_task
// bodies relay_owner.c's host build needs to link (see
// stubs/freertos_min/FreeRTOS.h for scope/rationale). relay_owner_task()
// itself (the for(;;) loop these back) is never invoked by
// test_relay_owner_gpio_init.c -- there is no scheduler on host -- these
// exist purely so the translation unit links; relay_owner_start()'s
// pre-task-creation code (the part under test: hal_gpio_init_out(), then
// xQueueCreate()) runs for real against these stubs.
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "../src/config_store.h"
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

// current_task.c's current_task_fn() references this (never invoked on
// host, see task.h's stub comment) -- body exists purely to link.
void vTaskDelayUntil(TickType_t *previous_wake_time, TickType_t time_increment)
{
    (void)previous_wake_time;
    (void)time_increment;
}

// Link-only stubs for current_task.c's current_task_reload_cal()/
// current_task_reload_ct_cal() (called only from current_task_fn(), which
// current_task_start()'s host test never invokes -- see above). The real
// implementations live in config_store_flash.c, which is deliberately NOT
// in this host build (it needs a real/fake flash backend this test suite
// doesn't otherwise pull in for current_task.c's sake). These bodies are
// never exercised; they exist only so the linker resolves the reference.
void config_store_get_ct_cal(config_store_ct_channel_cal_t out[CONFIG_STORE_CT_CAL_NUM_CHANNELS])
{
    for (unsigned n = 0; n < CONFIG_STORE_CT_CAL_NUM_CHANNELS; n++) {
        out[n].calibrated = false;
        out[n].gain = 0.0f;
        out[n].offset = 0.0f;
    }
}

// Return type changed void -> bool 2026-09-14 (config_store.h/config_store_
// flash.c, review Finding C) -- this stub always reports "no real snapshot"
// (false), matching its own always-zeroed body, which is honest: it never
// actually reads anything.
bool config_store_get_full_record(config_store_record_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

TickType_t xTaskGetTickCount(void)
{
    return 0;
}

void watchdog_task_checkin(watchdog_checkin_id_t id)
{
    (void)id; // never called: relay_owner_task()'s loop never runs on host
}
