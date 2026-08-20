// log_task.c -- SKELETON STUB, see log_task.h. Idles only; no log queue,
// no drop counter, no USB hand-off exists yet. Filling this in follows the
// USB protocol work (docs/PLAN.md section 5, milestone M-B onward) -- see
// ../../SaftyFW/src/tasks/log_task.c for the pattern this will likely
// follow (a bounded FreeRTOS queue plus a TX-reserve watermark so log
// traffic can never starve telemetry).
#include "log_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "task_priorities.h"

#define LOG_TASK_STACK_WORDS   configMINIMAL_STACK_SIZE
#define LOG_TASK_IDLE_DELAY_MS 1000u

static TaskHandle_t s_task_handle = NULL;

static void log_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        // Deferred logging, drop-counting, never blocks producers
        // (docs/PLAN.md section 4.1). Not implemented yet -- this loop only
        // idles.
        vTaskDelay(pdMS_TO_TICKS(LOG_TASK_IDLE_DELAY_MS));
    }
}

bool log_task_start(void)
{
    BaseType_t ok = xTaskCreate(log_task_fn, "log_task", LOG_TASK_STACK_WORDS, NULL,
                                 SIMFW_PRIO_LOG_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_ELASTIC_PATH);
    return true;
}
