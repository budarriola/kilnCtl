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

/* 2026-08-21: added for uart_log_bridge.c's host test. uart_log_vprintf()
 * calls this to detect (and drop) a log line produced by its own sender
 * task, by comparing against uart_log_bridge_t.sender_task -- which is
 * zero-initialized (NULL) until uart_log_bridge_start() runs, exactly the
 * state the host tests exercise (they call uart_log_vprintf() directly,
 * never uart_log_bridge_start()). Returning NULL here too would make every
 * call look like it came from the (nonexistent) sender task and get
 * silently self-filtered -- a stub artifact, not real firmware behavior
 * (on target, "no task" is never a real return value here). A fixed
 * non-NULL sentinel keeps "current task == sender_task" false, matching
 * every real caller before the sender task exists. */
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }

/* 2026-08-22: added for profile_executor.c's/autotune_engine.c's host tests,
 * which #include those .c files directly (same convention as the functions
 * above). Neither is ever actually reached by those tests -- every call site
 * sits behind the pre-start guard under test -- these exist purely so the
 * translation unit compiles and links. */
static inline TickType_t xTaskGetTickCount(void) { return 0; }
static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }

#endif // TEST_STUB_TASK_H
