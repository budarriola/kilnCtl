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

// 2026-09-09: configMINIMAL_STACK_SIZE overflowed on core1 (confirmed via
// SWD -- core1 halted forever inside vApplicationStackOverflowHook, core0
// then deadlocked forever inside xQueueGenericSend's spin_lock_unsafe_
// blocking, since the two-struct-copy critical section here never
// completed) after CT_COMMISSIONING_PLAN.md step 2's auto idle-offset
// calibration state and the current_task_reload_cal()/current_sense_
// sample()/get_snapshot()/get_power() call chain grew past the minimal
// stack's margin. Sized like link_task.c/safety_core.c's own *6, which
// carry the same "a bare configMINIMAL_STACK_SIZE looked fine until this
// task's call depth grew" lesson -- see safety_core.c's comment above
// SAFETY_CORE_STACK_WORDS.
#define CURRENT_TASK_STACK_WORDS   (configMINIMAL_STACK_SIZE * 6)

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

// 2026-09-18 (owner-directed fix, summed-CT-topology support): the two
// inputs config_store_mask_current_present_to_fitted()/config_store_ct_
// channel_fitted() (config_store.h -- THE single place those predicates
// live) need, cached rather than read fresh via config_store_get_full_
// record() inside current_task_any_current_present() itself. That function
// was deliberately added (2026-09-15, Opus review F1) to avoid a whole
// current_snapshot_t landing on link_task's own tight stack budget via its
// SET_CONFIG/COMMIT_CONFIG call chain -- a config_store_record_t is far
// larger still, so this caches just the two resolved scalars the mask
// actually needs, not the whole record. Refreshed only inside current_
// task_reload_cal() (called at boot and after every COMMIT_CONFIG,
// current_task.h's own doc comment on that function). Same
// taskENTER_CRITICAL()/EXIT discipline as s_published_snapshot above when
// read from current_task_any_current_present(): small fixed fields, no
// blocking call inside the guarded region.
static bool    s_ct_installed_effective = true;
static uint8_t s_ct_topology = CONFIG_STORE_CT_TOPOLOGY_PER_ZONE;

// CT_COMMISSIONING_PLAN.md step 2 -- see current_task.h's header comment for
// why this accumulates incrementally here rather than calling current_
// sense_recalibrate_zero() (which blocks its caller) from link_task.
// Guarded by the same taskENTER_CRITICAL()/taskEXIT_CRITICAL() discipline as
// s_published_snapshot/s_published_power just above -- small fixed fields,
// no blocking call inside the guarded region.
#define CURRENT_TASK_CT_AUTO_ZERO_TARGET_SAMPLES 200u // >=10s at SAFTYFW_PERIOD_CURRENT_TASK_MS (50ms), CT_COMMISSIONING_PLAN.md step 2's ">= 200 samples at 20 Hz"
static current_task_auto_zero_state_t s_auto_zero_state = CURRENT_TASK_AUTO_ZERO_IDLE;
static uint8_t  s_auto_zero_channel = 0;
static uint32_t s_auto_zero_sum = 0;
static uint16_t s_auto_zero_count = 0;
static uint16_t s_auto_zero_target = CURRENT_TASK_CT_AUTO_ZERO_TARGET_SAMPLES;
static uint16_t s_auto_zero_result_counts = 0;

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
        // CT_COMMISSIONING_PLAN.md step 2 -- one accumulator step per pass,
        // never a blocking wait. snap.counts_avg[] is this SAME pass's raw,
        // pre-conversion ADC mean (snapshots.h), the identical source
        // SAFETY_CMD_POWER's counts_avg field already publishes.
        if (s_auto_zero_state == CURRENT_TASK_AUTO_ZERO_IN_PROGRESS) {
            s_auto_zero_sum += snap.counts_avg[s_auto_zero_channel];
            s_auto_zero_count++;
            if (s_auto_zero_count >= s_auto_zero_target) {
                s_auto_zero_result_counts = (uint16_t)(s_auto_zero_sum / s_auto_zero_count);
                s_auto_zero_state = CURRENT_TASK_AUTO_ZERO_DONE;
            }
        }
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

bool current_task_any_current_present(void)
{
    taskENTER_CRITICAL();
    // Masked to fitted channels only, through config_store_mask_current_
    // present_to_fitted() (config_store.h -- THE single place this masking
    // lives, same function safety_core.c's own any_current_present
    // derivation calls). An unfitted channel carries only an uncalibrated
    // op-amp DC offset floor, never real current information, so it must
    // not be able to contribute to this fact -- see that function's doc
    // comment for the full fail-safe-direction argument.
    bool any = config_store_mask_current_present_to_fitted(
        s_published_snapshot.present, s_ct_installed_effective, s_ct_topology);
    taskEXIT_CRITICAL();
    return any;
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

    // Refresh the two scalars current_task_any_current_present() hands to
    // config_store_mask_current_present_to_fitted() -- same "unanswered
    // ct_installed means installed" effective-installed resolution
    // safety_core.c's own cts_disabled local applies. taskENTER_CRITICAL()
    // here matches the critical section current_task_any_current_present()
    // reads these two fields under; s_published_snapshot's own critical
    // section above this function's call site is a separate, already-
    // existing guarded region.
    bool ct_installed_effective =
        ((rec.fields_set & CONFIG_STORE_SET_CT_INSTALLED) == 0u) || (rec.ct_installed != 0u);
    taskENTER_CRITICAL();
    s_ct_installed_effective = ct_installed_effective;
    s_ct_topology = rec.ct_topology;
    taskEXIT_CRITICAL();
}

// CT_COMMISSIONING_PLAN.md step 2 -- see current_task.h's header comment.
bool current_task_ct_auto_zero_begin(uint8_t channel)
{
    if (channel >= 3u) {
        return false;
    }
    bool ok = false;
    taskENTER_CRITICAL();
    if (s_auto_zero_state != CURRENT_TASK_AUTO_ZERO_IN_PROGRESS) {
        s_auto_zero_channel = channel;
        s_auto_zero_sum = 0;
        s_auto_zero_count = 0;
        s_auto_zero_target = CURRENT_TASK_CT_AUTO_ZERO_TARGET_SAMPLES;
        s_auto_zero_result_counts = 0;
        s_auto_zero_state = CURRENT_TASK_AUTO_ZERO_IN_PROGRESS;
        ok = true;
    }
    taskEXIT_CRITICAL();
    return ok;
}

void current_task_ct_auto_zero_poll(current_task_auto_zero_status_t *out)
{
    if (!out) {
        return;
    }
    taskENTER_CRITICAL();
    out->state = s_auto_zero_state;
    out->channel = s_auto_zero_channel;
    out->samples_taken = s_auto_zero_count;
    out->samples_target = s_auto_zero_target;
    out->zero_counts = s_auto_zero_result_counts;
    taskEXIT_CRITICAL();
}
