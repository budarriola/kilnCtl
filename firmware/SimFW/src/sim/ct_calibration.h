// ct_calibration -- pure, host-testable per-channel CT amplitude calibration
// for SimFW. docs/PLAN.md section 3.3 ("amplitude (in simulated amps, fixture
// converts via calibration table)") is the requirement; this module is the
// arithmetic half of it.
//
// WHAT THIS IS AND IS NOT
// -----------------------
// This module gives SimFW the *ability to apply* a per-channel calibration:
//
//     pwm_scale = clamp(gain[ch] * amps + offset[ch], 0, 1)
//
// It does NOT contain a real calibration. No CT hardware exists on this bench
// and no calibration run has ever been performed, so the compiled-in default
// table (src/sim/ct_calibration_defaults.h, generated -- see below) is
// all-UNCALIBRATED, which means every channel behaves exactly as the old
// IDENTITY placeholder did: pwm_scale = clamp(amps, 0, 1). Milestone M-D is
// NOT closed by this module; what is closed is the firmware plumbing M-D
// needs. The actual constants remain hardware-gated.
//
// UNCALIBRATED IS AN EXPLICIT STATE, NOT A SET OF NEUTRAL NUMBERS
// ---------------------------------------------------------------
// ct_cal_channel_t carries a `calibrated` flag rather than relying on
// gain==1/offset==0 to mean "no calibration". That is deliberate: a
// zero-initialized struct (gain 0, offset 0) would silently mute a channel,
// and a "neutral constants" convention makes "nobody calibrated this" and
// "this channel genuinely fits y=x" indistinguishable. With the flag, an
// uncalibrated channel is visibly uncalibrated to code (ct_cal_is_calibrated)
// and to a human reading the generated header, and it can never accidentally
// inherit another channel's constants.
//
// WHERE THE CONSTANTS COME FROM
// -----------------------------
// firmware/SimFW/tools/ct_calibration/ (the PC-side runner) sweeps each
// channel and fits `measured_a = fit_gain * commanded + fit_offset`, writing
// a versioned JSON table. That fit maps command -> measured; this module
// needs the inverse (amps -> command), so the generator inverts it:
//
//     gain   =  1 / fit_gain
//     offset = -fit_offset / fit_gain
//
// which is algebraically identical to that package's
// ChannelCalibration.to_command() (`(target_amps - offset) / fit_gain`).
// The inversion is done once, at generation time, so the per-sample path
// here is a single multiply-add.
//
// Regenerate the compiled-in defaults after a bench calibration run with:
//
//     python firmware/SimFW/tools/gen_ct_cal_table.py
//         --json firmware/SimFW/tools/ct_calibration/ct_calibration_table.json
//
// (run with no --json to regenerate the all-uncalibrated identity default).
// A per-unit flashable table is the follow-up once SimFW gains its own
// config_store; src/ has no flash-persistence subsystem today, so a
// compiled-in, regenerated-and-checked-in table is the whole story for now --
// see tools/ct_calibration/README.md, "Persistence decision".
#ifndef SIMFW_SIM_CT_CALIBRATION_H
#define SIMFW_SIM_CT_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Must equal tasks/wave_owner.h's CT_WAVE_NUM_CHANNELS. Not #included from
// there: that header pulls in the RTOS-facing task API, and everything under
// src/sim/ must stay pure (tools/check_sim_purity.ps1). wave_owner.c carries
// a compile-time assert that the two agree.
#define CT_CAL_NUM_CHANNELS 3u

// One channel's calibration. `calibrated == false` means "no bench
// calibration exists for this channel" and gain/offset are then ignored
// entirely -- see the header comment above for why this is a flag and not a
// gain==1/offset==0 convention.
typedef struct {
    bool  calibrated;
    float gain;    // PWM-scale units per simulated amp
    float offset;  // PWM-scale units at 0 A
} ct_cal_channel_t;

typedef struct {
    ct_cal_channel_t channels[CT_CAL_NUM_CHANNELS];
} ct_cal_table_t;

// Converts a target current in simulated amps into a PWM full-scale fraction,
// clamped to [0, 1].
//
//   calibrated channel   -> clamp(gain * amps + offset, 0, 1)
//   uncalibrated channel -> clamp(amps, 0, 1)              (IDENTITY)
//   table == NULL        -> clamp(amps, 0, 1)              (IDENTITY)
//   channel out of range -> 0.0f                           (silent, not loud)
//
// The out-of-range answer is 0 (silent channel) rather than a clamp of
// `amps`: a bad channel index is a caller bug, and driving a CT output from a
// bug is worse than driving nothing. Callers in this tree range-check first.
float ct_cal_apply(const ct_cal_table_t *table, uint8_t channel, float amps);

// True only if `table` is non-NULL, `channel` is in range, and that channel
// carries a real bench calibration. Exists so a status/report path can say
// "uncalibrated" out loud instead of inferring it from constants.
bool ct_cal_is_calibrated(const ct_cal_table_t *table, uint8_t channel);

// The compiled-in default table (src/sim/ct_calibration_defaults.h). Today
// this is all-uncalibrated -- identity behavior on every channel. Never
// NULL.
const ct_cal_table_t *ct_cal_default_table(void);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_CT_CALIBRATION_H
