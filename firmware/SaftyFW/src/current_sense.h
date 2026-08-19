// current_sense.h -- the ADC0/1/2 current-sense front end. This is the
// `adc_owner` module named in docs/ARCHITECTURE.md section 3's module table
// ("Genuinely shared -- one SAR muxed across three inputs. Round-robin, 16x
// oversample. No DMA"). A separate file rather than folding into
// current_task.c because the sampling/conversion/calibration math here is
// substantial enough to want its own header and to be host-testable in
// spirit later, even though it is not host-testable *today* because it
// calls hardware/adc.h directly (a driver, not policy -- unlike
// safety_guards.c it is not meant to be pure).
//
// Scope, from docs/CURRENT_SENSE.md section 0: load-active detection and a
// power ESTIMATE for the GUI. This is explicitly NOT an over-current
// protection device, and NOTHING in Phase 6 (this module) may be consumed by
// a guard yet -- S3/S4/S9 need link_task's relay_recent_mask context, which
// is Phase 7. current_task.c publishes a current_snapshot_t (src/snapshots.h)
// for that future consumer; nothing calls the getter yet.
#ifndef SAFTYFW_CURRENT_SENSE_H
#define SAFTYFW_CURRENT_SENSE_H

#include <stdbool.h>
#include <stdint.h>

#include "snapshots.h"

#ifdef __cplusplus
extern "C" {
#endif

// Per-channel calibration, docs/CURRENT_SENSE.md section 5 -- "measured
// configuration, stored in flash, never compiled-in constants". Real values
// come from config_store.c, which does not exist yet (TODO.md Phase 9), so
// this struct is a zero-initialized placeholder today, exactly like
// safety_core.c's s_guard_cfg. Do NOT put a plausible-looking number in any
// field's default here -- see current_sense.c's s_cal definition for the
// exact honesty/safety-direction reasoning per field.
typedef struct {
    // ADC counts at zero primary current, per channel. Doc: "not zero and
    // must not be assumed to be" -- but the compiled DEFAULT is 0 anyway,
    // because there is no safe non-zero guess and 0 is at least an obviously
    // wrong number rather than a plausible-looking wrong one.
    uint16_t zero_counts[3];

    // Load-active threshold, amps. Shared across channels (docs/CURRENT_
    // SENSE.md section 5 table lists it "per channel" for storage
    // uniformity, but it is fundamentally one commissioning decision --
    // "between the noise floor and a conducting element" -- not a per-
    // channel physical property like zero_counts or k_ct_v_per_a. 0 = "not
    // commissioned"; see the calibrated-gate discussion in current_sense.c.
    float i_present_a;

    // CT volts-RMS-per-amp, per channel. No safe default -- depends on
    // which CT model is fitted. 0 = "not commissioned"; current_sense.c
    // refuses to report amps for a channel with k_ct_v_per_a <= 0 rather
    // than dividing by zero or guessing.
    float k_ct_v_per_a[3];

    // Rectifier gain, per channel, docs/CURRENT_SENSE.md section 2:
    // "R46/R43 = 7.15k / 10k = 0.715 ... refined if the resistors are not
    // 1%". Unlike zero_counts/i_present_a/k_ct_v_per_a, 0.715 IS a safe
    // compiled default -- it is a property of the populated resistor
    // values, not an install-specific measurement, so current_sense.c
    // substitutes 0.715 whenever this field is 0 (mirrors safety_core.c's
    // documented "substitutes a real default whenever the cfg field is 0"
    // convention for S5/S11/S12's thresholds).
    float gain[3];

    // Installation's nominal supply voltage, docs/CURRENT_SENSE.md section
    // 3b/§5. 0 = "not configured" -> p_avg_w is reported absent (NAN), never
    // a number computed against an invented default voltage.
    float mains_voltage_v;

    // True only once real commissioned values have been loaded (Phase 9).
    // Gates current_snapshot_t.calibrated. Deliberately NOT inferred from
    // "are the numeric fields non-zero", because 0 is sometimes a genuine
    // reading path (e.g. i_present_a could theoretically commission to a
    // very small number) -- an explicit flag is the only unambiguous
    // signal, same reasoning as thermo_snapshot_t.valid.
    bool calibrated;
} current_sense_cal_t;

// Reporting-only power-estimate quantities, docs/CURRENT_SENSE.md section
// 3b. Deliberately a SEPARATE struct from current_snapshot_t (src/
// snapshots.h) -- these are derived from the tau=0.5s filtered reading, and
// no guard may ever be handed a filtered value (docs/CURRENT_SENSE.md
// section 4). Keeping them in a different struct makes that a type-level
// fact, not a comment a future author has to notice and honour.
typedef struct {
    uint32_t timestamp_ms;
    float    i_conducting_a[3];       // peak-hold amps while conducting
    float    conduction_fraction[3];  // 0..1 over the last power_window_s
    float    p_avg_w[3];              // NAN when mains_voltage_v is unconfigured
    // Added for SAFETY_CMD_POWER (Frame E, kilnlink_power.h) -- link_task.c's
    // send path needs these alongside the per-channel arrays above rather
    // than re-deriving them from current_sense_cal_t (which it has no getter
    // for, deliberately -- see current_sense_set_cal()'s doc comment: only
    // config_store, Phase 9, is meant to hold calibration).
    float    mains_voltage_v; // NAN if unconfigured (cal.mains_voltage_v <= 0)
    bool     calibrated;      // mirrors current_snapshot_t.calibrated (cal.calibrated)
    bool     any_clipped;     // OR of current_snapshot_t.clipped[0..2] from the same sample pass
    // p_total_w: sum of the three p_avg_w[], NAN if ANY contributing channel
    // is NAN (unconfigured mains or -- not modeled yet -- a clipped channel;
    // see current_sense.c). energy_wh: trapezoidal-ish accumulation of
    // p_total_w over time since current_sense_init(), i.e. since last boot
    // (kilnlink_power.h's documented "resets on reboot" contract). Frozen
    // (does not advance) on any sample pass where p_total_w is NAN, so a
    // temporarily-unconfigured mains voltage never silently loses energy
    // rather than merely pausing accounting for it.
    float    p_total_w;
    double   energy_wh;
} current_sense_power_t;

// Sets calibration state. Not called by anything yet (Phase 9's
// config_store will call this once it exists); current_task_start() leaves
// the module on its zero-initialized "not commissioned" defaults.
void current_sense_set_cal(const current_sense_cal_t *cal);

// Must be called once, after adc_init()/adc_gpio_init() for ADC0/1/2 (see
// current_task.c -- current_task_start() already does both).
void current_sense_init(void);

// Runs one full round-robin pass across ADC0/1/2 (16x oversample each,
// first-after-mux-switch conversion discarded) and updates internal state.
// Not reentrant; call only from current_task's own loop, at
// SAFTYFW_PERIOD_CURRENT_TASK_MS. Touches real hardware -- see
// current_sense.c's header comment for why this is manual adc_select_input()
// polling rather than adc_run()/round-robin FIFO capture.
void current_sense_sample(void);

// Copies out the most recent unfiltered snapshot. Safe to call from any
// task; current_sense.c itself does no locking, so current_task.c wraps
// this in a short critical section around both the write (in
// current_sense_sample()'s caller) and this read -- see current_task.c.
void current_sense_get_snapshot(current_snapshot_t *out);

// Copies out the current filtered/derived reporting quantities. Same
// caller-locking contract as current_sense_get_snapshot().
void current_sense_get_power(current_sense_power_t *out);

// Mechanism for docs/CURRENT_SENSE.md section 5's "re-measure zero_counts
// whenever idle > 5 minutes with no relay commanded on" diagnostic. Blocks
// the calling task for approximately n_samples * SAFTYFW_PERIOD_CURRENT_
// TASK_MS-equivalent round-robin passes while it re-samples all three
// channels and overwrites cal_inout->zero_counts in place; does NOT set
// cal_inout->calibrated (that is a config_store-level decision the caller
// makes, since this function has no way to know whether i_present_a etc.
// have also ever been commissioned).
//
// This function has no idea whether the kiln has actually been idle for
// >=5 minutes or whether any relay is commanded on -- current_sense.c must
// not gain visibility into relay state (module isolation, docs/
// ARCHITECTURE.md section 2's "every hardware interface is owned by exactly
// one task"). The caller (a future commissioning flow / config_store.c,
// Phase 9) is responsible for the precondition. MECHANISM ONLY: this has
// never been run against real hardware -- no RP2040 attached to the machine
// this was built on.
void current_sense_recalibrate_zero(current_sense_cal_t *cal_inout, uint16_t n_samples);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_CURRENT_SENSE_H
