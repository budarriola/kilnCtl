// Host-test stub -- see stubs/esp_err.h for why these exist.
#ifndef TEST_STUB_TASK_H
#define TEST_STUB_TASK_H

#include <stddef.h>

#include "freertos/FreeRTOS.h"

typedef struct task_s *TaskHandle_t;

/* 2026-08-21: added for wifi_prov.c's host tests. xTaskCreatePinnedToCore()
 * deliberately never invokes pxTaskCode -- the tests call wifi_prov.c's
 * do_*() bodies directly rather than through owner_task()/dns_hijack_task(),
 * so nothing here needs those tasks to actually run. */
typedef void (*TaskFunction_t)(void *);
#define tskNO_AFFINITY ((BaseType_t)-1)

static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned long stack_depth,
                                                  void *arg, UBaseType_t priority, TaskHandle_t *out_handle,
                                                  BaseType_t core_id)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)arg;
    (void)priority;
    (void)core_id;
    if (out_handle) {
        *out_handle = NULL;
    }
    return pdPASS;
}

static inline void vTaskDelete(TaskHandle_t task) { (void)task; }

#endif // TEST_STUB_TASK_H
