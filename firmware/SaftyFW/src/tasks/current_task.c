// current_task.c -- Phase 6: owns ADC0/1/2 (docs/ARCHITECTURE.md section 3,
// adc_owner). Sampling/conversion/calibration math lives in
// src/current_sense.{c,h}; this file owns the hardware init, the task
// timing, and publishing mutex-guarded copies of current_sense's output for
// any future consumer. Scope, from docs/CURRENT_SENSE.md section 0:
// load-active detection and a power estimate only -- no guard reads any of
// this yet (S3/S4/S9 need link_task's relay_recent_mask context, Phase 7).
#include "current_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "hardware/adc.h"

#include "board_pins.h"
#include "current_sense.h"
#include "task_priorities.h"
#include "watchdog_task.h"

#define CURRENT_TASK_STACK_WORDS   configMINIMAL_STACK_SIZE

static TaskHandle_t s_task_handle = NULL;

// Published copies, guarded by a short critical section on both the write
// (end of each sample pass) and every read (the getters below) -- the same
// "snapshot-and-clear under a critical section" discipline
// watchdog_task.c already uses for its checkin mask. A full mutex is not
// needed: the copy is a small fixed-size struct with no blocking call
// inside the guarded region, so a critical section is strictly lighter and
// cannot invert priority against relay_owner/safety_core on this same core.
static current_snapshot_t     s_published_snapshot;
static current_sense_power_t  s_published_power;

static void current_task_fn(void *arg)
{
    (void)arg;

    current_sense_init();

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFTYFW_PERIOD_CURRENT_TASK_MS));

        // current_sense_sample() is the only place that touches the ADC
        // hardware (docs/CURRENT_SENSE.md section 4: round-robin ADC0/1/2,
        // 16x oversample, first-after-mux-switch conversion discarded,
        // peak-envelope conversion per section 2). This task is the sole
        // caller, so no locking is needed around the call itself.
        current_sense_sample();

        current_snapshot_t     snap;
        current_sense_power_t  power;
        current_sense_get_snapshot(&snap);
        current_sense_get_power(&power);

        taskENTER_CRITICAL();
        s_published_snapshot = snap;
        s_published_power = power;
        taskEXIT_CRITICAL();

        watchdog_task_checkin(WATCHDOG_CHECKIN_CURRENT_TASK);
    }
}

bool current_task_start(void)
{
    adc_init();
    // adc_gpio_init() disables the digital functions on these pins
    // (ARCHITECTURE.md section 8) -- call it once, here, rather than letting
    // each channel's first read do it implicitly.
    adc_gpio_init(SAFTYFW_PIN_ADC0_GPIO);
    adc_gpio_init(SAFTYFW_PIN_ADC1_GPIO);
    adc_gpio_init(SAFTYFW_PIN_ADC2_GPIO);

    BaseType_t ok = xTaskCreate(current_task_fn, "current_task", CURRENT_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_CURRENT_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_TRIP_PATH);
    return true;
}

void current_task_get_snapshot(current_snapshot_t *out)
{
    taskENTER_CRITICAL();
    *out = s_published_snapshot;
    taskEXIT_CRITICAL();
}

void current_task_get_power(current_sense_power_t *out)
{
    taskENTER_CRITICAL();
    *out = s_published_power;
    taskEXIT_CRITICAL();
}
