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

#include "hal_adc.h"

#include "board_pins.h"
#include "config_store.h"
#include "current_sense.h"
#include "task_priorities.h"
#include "watchdog_task.h"

// Compile-time cross-check: ct_amps_cal.h's CT_AMPS_CAL_NUM_CHANNELS and
// config_store.h's CONFIG_STORE_CT_CAL_NUM_CHANNELS must agree -- both are
// meant to be "one per ADC0/1/2", but neither header includes the other
// (deliberately, see each file's own header comment), so this is the one
// place that would catch them drifting apart.
typedef char current_task_ct_cal_channel_counts_match
    [(CT_AMPS_CAL_NUM_CHANNELS == CONFIG_STORE_CT_CAL_NUM_CHANNELS) ? 1 : -1];

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

    // Phase 9 / 2026-08-24: load the FULL current-sense calibration from
    // config_store (this task's own glue -- current_sense.c itself must not
    // depend on config_store.h, the same "sampling/conversion here,
    // commissioning storage elsewhere" split every other current_sense_
    // cal_t field already respects). current_task_reload_cal() covers
    // i_present_a/zero_counts/k_ct_v_per_a/gain/mains_voltage_v AND ct_cal
    // in one call (it reads ct_cal out of the same config_store record), so
    // this replaces the old ct_cal-only reload here -- current_sense_set_
    // cal() (unlike current_sense_set_ct_cal()) replaces the WHOLE cal
    // struct, and until this call landed, current_sense_set_cal() was never
    // called anywhere in src/: k_ct_v_per_a stayed 0.0f forever, which
    // silently disabled S3/S9/S11/S6b's presence detection (fixed
    // separately, current_presence_policy.h, but calibration still needs to
    // actually reach current_sense.c for the reported amps/power figures to
    // mean anything). config_store_boot_load() has already run by the time
    // main.c calls current_task_start() (step 4 runs before step 7 -- see
    // main.c), so this reads a real record (or a safe all-uncalibrated
    // default, never uninitialised memory) even on the very first pass
    // through this loop.
    current_task_reload_cal();

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
    hal_adc_init();
    // hal_adc_gpio_enable() disables the digital functions on these pins
    // (ARCHITECTURE.md section 8) -- call it once, here, rather than letting
    // each channel's first read do it implicitly.
    hal_adc_gpio_enable(SAFTYFW_PIN_ADC0_GPIO);
    hal_adc_gpio_enable(SAFTYFW_PIN_ADC1_GPIO);
    hal_adc_gpio_enable(SAFTYFW_PIN_ADC2_GPIO);

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

void current_task_reload_ct_cal(void)
{
    config_store_ct_channel_cal_t stored[CONFIG_STORE_CT_CAL_NUM_CHANNELS];
    config_store_get_ct_cal(stored);

    ct_amps_cal_table_t table;
    for (unsigned n = 0; n < CT_AMPS_CAL_NUM_CHANNELS; n++) {
        table.channels[n].calibrated = stored[n].calibrated;
        table.channels[n].gain = stored[n].gain;
        table.channels[n].offset = stored[n].offset;
    }
    current_sense_set_ct_cal(&table);
}

void current_task_reload_cal(void)
{
    // ONE call, the whole committed record -- config_store.h deliberately
    // exposes no per-field getters for i_present_a/zero_counts/k_ct_v_per_a/
    // gain/mains_voltage_v (see config_store_get_full_record()'s own doc
    // comment: this is the "read-back must reflect the enforced record"
    // path, same contract current_task_reload_ct_cal() relies on via
    // config_store_get_ct_cal()). Before config_store_boot_load() has run,
    // this already returns config_store_default()'s all-uncalibrated
    // record (never uninitialised memory) -- same safe-default contract
    // every other config_store getter documents.
    config_store_record_t rec;
    config_store_get_full_record(&rec);

    current_sense_cal_t cal;
    cal.i_present_a = rec.i_present_a;
    cal.mains_voltage_v = rec.mains_voltage_v;
    bool k_ct_all_set = true;
    for (unsigned n = 0; n < 3u; n++) {
        cal.zero_counts[n] = rec.zero_counts[n];
        cal.k_ct_v_per_a[n] = rec.k_ct_v_per_a[n];
        cal.gain[n] = rec.gain[n];
        if (rec.k_ct_v_per_a[n] <= 0.0f) {
            k_ct_all_set = false;
        }
    }
    // `calibrated` gates current_snapshot_t.calibrated / current_sense_
    // power_t.calibrated (KILNLINK_POWER_FLAG_CALIBRATED on the wire,
    // link_task.c's link_task_send_power()) -- docs/CONFIG_REFERENCE.md
    // section 3 scopes k_ct_v_per_a/gain/mains_voltage_v as "power estimate
    // only, no guard", so this flag answers exactly that question ("is the
    // reported amps/power number real") and nothing about guard readiness:
    // S3/S9/S11/S6b's presence detection no longer depends on k_ct_v_per_a
    // at all (current_presence_policy.h) and does not read this flag.
    // i_present_a/zero_counts are deliberately NOT part of this condition --
    // CONFIG_REFERENCE.md section 3 does not list them among the no-safe-
    // default fields gated by calibration_missing (they ship with real
    // defaults, 2.0A and 0 respectively), so their absence must not be
    // reported as "uncalibrated" here.
    cal.calibrated = k_ct_all_set;
    // ct_cal converted from the same record -- current_task_reload_ct_cal()
    // above updates this sub-field IN PLACE (via current_sense_set_ct_cal())
    // whenever SAFETY_CMD_SET_CT_CAL lands on its own; converting it again
    // here from the SAME committed record keeps this call self-contained
    // (a caller does not need to also call reload_ct_cal() for ct_cal to
    // end up correct) and idempotent -- both paths read the same
    // config_store record, so they can never disagree.
    for (unsigned n = 0; n < CT_AMPS_CAL_NUM_CHANNELS; n++) {
        cal.ct_cal.channels[n].calibrated = rec.ct_cal[n].calibrated;
        cal.ct_cal.channels[n].gain = rec.ct_cal[n].gain;
        cal.ct_cal.channels[n].offset = rec.ct_cal[n].offset;
    }

    current_sense_set_cal(&cal);
}
