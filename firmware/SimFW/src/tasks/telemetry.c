// telemetry.c -- SKELETON STUB, see telemetry.h. Idles only; no telemetry
// frame, no event-ring drain exists yet. Filling this in follows the USB
// protocol work (docs/PLAN.md section 5.3, milestone M-B onward).
#include "telemetry.h"

#include "FreeRTOS.h"
#include "task.h"

#include "task_priorities.h"

#define TELEMETRY_STACK_WORDS   configMINIMAL_STACK_SIZE
#define TELEMETRY_IDLE_DELAY_MS 1000u

static TaskHandle_t s_task_handle = NULL;

static void telemetry_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        // Periodic state frames to USB (temps, relay states, active
        // faults, sim clock) (docs/PLAN.md section 4.1). Not implemented
        // yet -- this loop only idles.
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_IDLE_DELAY_MS));
    }
}

bool telemetry_start(void)
{
    BaseType_t ok = xTaskCreate(telemetry_task_fn, "telemetry", TELEMETRY_STACK_WORDS, NULL,
                                 SIMFW_PRIO_TELEMETRY, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_ELASTIC_PATH);
    return true;
}
