// log_task.c -- Phase 2 skeleton only. The real log ring, the TX-reserve
// watermark that protects telemetry (docs/ARCHITECTURE.md section 1, "a log
// frame must never be able to displace a telemetry frame") and the
// dropped-frame counter are Phase 8's job.
#include "log_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "task_priorities.h"
#include "watchdog_task.h"

#define LOG_TASK_STACK_WORDS   configMINIMAL_STACK_SIZE
// No real log ring / event source yet (Phase 8), so this is a bounded wait
// rather than an indefinite block on an event that cannot yet occur.
#define LOG_TASK_POLL_MS       500

static TaskHandle_t s_task_handle = NULL;

static void log_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        // TODO (Phase 8): block on the log ring (event-driven, not polled),
        // drain best-effort into link_task's TX path, respecting the
        // telemetry reservation watermark -- log frames only ever use space
        // that remains after the telemetry budget, and are dropped at
        // enqueue above it. Count drops for the diagnostic frame.
        vTaskDelay(pdMS_TO_TICKS(LOG_TASK_POLL_MS));

        watchdog_task_checkin(WATCHDOG_CHECKIN_LOG_TASK);
    }
}

bool log_task_start(void)
{
    BaseType_t ok = xTaskCreate(log_task_fn, "log_task", LOG_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_LOG_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_LINK_PATH);
    return true;
}
