// ct_amps_cal.h -- pure, host-testable per-channel CT amps correction for
// SaftyFW's current-sense readings. Closes the gap firmware/SimFW/tools/
// ct_calibration/README.md's "Readback path" section documented: a bench
// calibration run against a real SaftyFW could compute per-channel
// gain/offset constants, but "no MCP tool exists to push calibration
// constants ... to the RP2040's own flash" -- this module is the arithmetic
// half of closing that gap; config_store.{c,h} is the storage half and
// src/tasks/link_task.c's SAFETY_CMD_SET_CT_CAL/GET_CT_CAL handlers are the
// wire half.
//
// DELIBERATELY MIRRORS firmware/SimFW/src/sim/ct_calibration.h
// ------------------------------------------------------------
// That module solved the identical problem for SimFW (a per-channel linear
// calibration that must default to exact identity when uncommissioned) and
// its header comment explains, at length, why `calibrated` must be an
// explicit flag rather than a gain==1/offset==0 convention: a zero-
// initialized struct must never be silently mistaken for "this channel
// genuinely needs gain 0" (SimFW's case) or "gain 0" muting a current
// reading outright (this module's case -- worse here, since S3/S4/S9 read
// current_a to decide whether current is flowing where it should not be).
// This module applies the identical discipline for the identical reason.
//
// This is a SEPARATE, later correction stage from current_sense.c's own
// physics-based conversion (zero_counts/k_ct_v_per_a/gain, docs/
// CURRENT_SENSE.md section 5's ADC-counts-to-amps formula): that stage
// models the schematic/datasheet-derived ADC->volts->amps relationship,
// while this stage corrects the RESULT of that conversion against a bench
// measurement of one specific unit's actual reported current_a. See
// config_store.h's header comment on config_store_ct_channel_cal_t for the
// exact inversion convention the gain/offset stored here must already
// carry.
//
// Pure C11, no RTOS/SDK dependency (unlike current_sense.c, which touches
// hardware/adc.h directly and is therefore NOT host-tested) -- so this
// module, and only this module, is what test/test_ct_amps_cal.c exercises
// for "an uncalibrated channel applies the compiled-in default exactly."
#ifndef SAFTYFW_CT_AMPS_CAL_H
#define SAFTYFW_CT_AMPS_CAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Must equal current_task.c's channel count (one per ADC0/1/2) and
// config_store.h's CONFIG_STORE_CT_CAL_NUM_CHANNELS. Not #included from
// config_store.h to keep this module independent of it (current_sense.c
// depends on THIS header, not on config_store.h -- see current_sense.h's
// header comment on why current_sense.c must not know about config_store);
// current_task.c, which depends on both, carries a compile-time assert that
// the two agree.
#define CT_AMPS_CAL_NUM_CHANNELS 3u

// One channel's calibration. `calibrated == false` means "no bench
// calibration exists for this channel" and gain/offset are then ignored
// entirely -- see this file's header comment for why this is a flag and not
// a gain==1/offset==0 convention.
typedef struct {
    bool  calibrated;
    float gain;
    float offset;
} ct_amps_cal_channel_t;

typedef struct {
    ct_amps_cal_channel_t channels[CT_AMPS_CAL_NUM_CHANNELS];
} ct_amps_cal_table_t;

// The all-uncalibrated table: every channel's `calibrated` false, gain/
// offset 0 (irrelevant, since calibrated is false) -- config_store_default()
// 's ct_cal shape, and current_sense_init()'s own zero-initialized default.
// Provided so callers building a fresh table (current_task.c at boot,
// link_task.c's read-modify-write) have one obvious, named starting point
// rather than each hand-rolling a `{0}` and hoping the zero value keeps
// meaning "uncalibrated" if this struct ever grows a field that zero does
// NOT make safe.
ct_amps_cal_table_t ct_amps_cal_uncalibrated_table(void);

// Applies `table`'s calibration for `channel` to `raw_amps`, a current-
// sense reading already converted from ADC counts to amps by current_sense.c
// (docs/CURRENT_SENSE.md section 5's formula) but not yet corrected against
// a bench measurement.
//
//   calibrated channel   -> gain * raw_amps + offset, clamped to >= 0
//                           (current_sense.c's own "max(0, ...)" convention
//                           for cs_counts_to_amps() -- a current reading is
//                           never negative)
//   uncalibrated channel  -> raw_amps unchanged                (IDENTITY)
//   table == NULL         -> raw_amps unchanged                (IDENTITY)
//   channel out of range  -> raw_amps unchanged                (IDENTITY)
//
// Unlike firmware/SimFW/src/sim/ct_calibration.h's ct_cal_apply() (which
// answers 0.0f, not amps, for an out-of-range channel), this function
// always falls back to the identity for a bad index: current_sense.c always
// range-checks its own channel loop (0..2, the fixed ADC0/1/2 set) before
// calling this, so an out-of-range channel here would be this module's own
// bug, not untrusted input -- and "return the number unmodified" is a safer
// failure for a bug in the SAFETY-relevant read path than fabricating a
// silent 0 A that could mask real current flowing.
float ct_amps_cal_apply(const ct_amps_cal_table_t *table, uint8_t channel, float raw_amps);

// True only if `table` is non-NULL, `channel` is in range, and that channel
// carries a real bench calibration. Exists so a status/report path can say
// "uncalibrated" out loud instead of inferring it from constants.
bool ct_amps_cal_is_calibrated(const ct_amps_cal_table_t *table, uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_CT_AMPS_CAL_H
