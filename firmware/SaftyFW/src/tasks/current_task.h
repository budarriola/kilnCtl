// current_task.h -- samples ADC0/1/2 (Phase 6). Genuinely shared hardware:
// one SAR ADC round-robined across three current-sense channels
// (docs/ARCHITECTURE.md section 3, adc_owner). The actual sampling/
// conversion math lives in src/current_sense.{c,h}; this task owns the
// hardware, the timing, and publishing mutex-guarded copies of its output.
#ifndef SAFTYFW_TASKS_CURRENT_TASK_H
#define SAFTYFW_TASKS_CURRENT_TASK_H

#include <stdbool.h>

#include "current_sense.h"
#include "snapshots.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates current_task at SAFTYFW_PRIO_CURRENT_TASK, pinned to
// SAFTYFW_CORE_TRIP_PATH. Returns false if task creation failed.
bool current_task_start(void);

// Copies out the most recent published current_snapshot_t. Safe to call
// from any task. Nothing calls this yet (Phase 7's S3/S4/S9 are the first
// real consumer) -- it exists so this phase's output is actually reachable
// once that lands.
void current_task_get_snapshot(current_snapshot_t *out);

// Copies out the most recent published power-estimate quantities
// (docs/CURRENT_SENSE.md section 3b). Safe to call from any task. Intended
// consumer is Phase 8's SAFETY_CMD_POWER telemetry frame, not a guard --
// see current_sense.h's header comment for why this is a separate struct.
void current_task_get_power(current_sense_power_t *out);

// Re-reads config_store's CT calibration (config_store_get_ct_cal()) and
// pushes it into current_sense.c via current_sense_set_ct_cal(), without
// disturbing any other calibration field. Called once from current_task_fn()
// 's own boot sequence, and again by link_task.c's SAFETY_CMD_SET_CT_CAL
// handler every time a new value is written -- a live commissioning session
// must take effect immediately, not after a reboot. Safe to call from any
// task: current_sense_set_ct_cal() only ever replaces the whole ct_cal
// sub-struct atomically from current_task's own perspective (no torn read),
// same reasoning as every other current_sense.c setter.
void current_task_reload_ct_cal(void);

// Re-reads config_store's FULL record (config_store_get_full_record(), one
// call, not the individual field getters -- config_store.h exposes none for
// these fields) and pushes the PRIMARY current-sense calibration out of it
// (i_present_a, zero_counts[3], k_ct_v_per_a[3], gain[3], mains_voltage_v,
// ct_cal[3], and a derived `calibrated` fact -- see this function's .c
// comment for exactly how `calibrated` is derived) into current_sense.c via
// current_sense_set_cal(), which replaces the WHOLE current_sense_cal_t at
// once (unlike current_task_reload_ct_cal() above, which only ever touches
// the ct_cal sub-field in place). Because this reads ct_cal out of the same
// record too, it is self-contained -- callers do not need to also call
// current_task_reload_ct_cal() around it for ct_cal to end up correct.
//
// Called once from current_task_fn()'s own boot sequence (replacing the
// old ct_cal-only reload), and again by link_task.c's SAFETY_CMD_
// COMMIT_CONFIG handler every time a commissioning session commits -- same
// "a live commissioning session must take effect immediately, not after a
// reboot" reasoning as current_task_reload_ct_cal()'s own doc comment.
void current_task_reload_cal(void);

// --- CT_COMMISSIONING_PLAN.md step 2: auto idle-offset measurement --------
//
// A non-blocking alternative to calling current_sense_recalibrate_zero()
// directly from link_task: that function blocks its caller for the whole
// measurement (>=10s at CURRENT_TASK_CT_AUTO_ZERO_TARGET_SAMPLES), which
// link_task's own 30 ms watchdog deadline cannot survive (see link_frame.h's
// LINK_FRAME_CT_AUTO_ZERO_BEGIN_CMD comment). Instead, current_task_fn()'s
// own loop accumulates one raw-counts sample per its normal period while a
// request is armed -- current_task_checkin still happens every pass,
// unaffected.

typedef enum {
    CURRENT_TASK_AUTO_ZERO_IDLE = 0,
    CURRENT_TASK_AUTO_ZERO_IN_PROGRESS = 1,
    CURRENT_TASK_AUTO_ZERO_DONE = 2,
} current_task_auto_zero_state_t;

typedef struct {
    current_task_auto_zero_state_t state;
    uint8_t  channel;
    uint16_t samples_taken;
    uint16_t samples_target;
    uint16_t zero_counts; // mean raw ADC counts, valid only when state == DONE
} current_task_auto_zero_status_t;

// Arms a fresh measurement on `channel` (0..2). Refuses (returns false,
// nothing changed) if channel is out of range or a measurement is already
// IN_PROGRESS -- a fresh BEGIN while DONE is allowed (starts a new
// measurement, discarding the previous result) same as SET_CT_CAL always
// applying regardless of prior state. Safe to call from any task (link_task
// is the only caller today) -- guarded the same short-critical-section way
// current_task_get_snapshot() already is.
bool current_task_ct_auto_zero_begin(uint8_t channel);

// Copies out the current accumulator state. Safe to call from any task.
void current_task_ct_auto_zero_poll(current_task_auto_zero_status_t *out);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_CURRENT_TASK_H
