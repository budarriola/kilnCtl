// ct_calibration_defaults.h -- GENERATED FILE, DO NOT EDIT BY HAND.
//
// Regenerate with:
//     python firmware/SimFW/tools/gen_ct_cal_table.py [--json <table.json>]
//
// Source: none -- no calibration run has been performed (all channels UNCALIBRATED / identity)
//
// See src/sim/ct_calibration.h for what these constants mean and how the
// PC-side fit (measured = fit_gain * commanded + fit_offset) is inverted
// into the (gain, offset) pair stored here.
#ifndef SIMFW_SIM_CT_CALIBRATION_DEFAULTS_H
#define SIMFW_SIM_CT_CALIBRATION_DEFAULTS_H

#include "ct_calibration.h"

// NO CALIBRATION DATA EXISTS. No CT hardware is on the bench and no
// calibration run has ever been performed, so every channel below is
// marked UNCALIBRATED: ct_cal_apply() falls back to exact identity,
// pwm_scale = clamp(amps, 0, 1) -- byte-for-byte the behavior SimFW had
// before the calibration path existed. Milestone M-D is NOT closed by
// this file; only the ability to apply a calibration is.

// NO CT IDENTIFIER RECORDED. docs/PLAN.md section 11 item 12: this table
// carries no record of which physical CT it was fitted against, so
// ct_cal_id_known() below is false and ct_cal_id() returns "". Either no
// calibration run has ever been performed (see above), or the JSON table
// this was generated from predates the ct_id field / left it blank.

static const ct_cal_table_t CT_CAL_DEFAULT_TABLE = {
    .ct_id_known = false,
    .ct_id = "",
    .channels = {
        [0] = { .calibrated = false, .gain = 0.0f, .offset = 0.0f },  // UNCALIBRATED -- identity; gain/offset are ignored
        [1] = { .calibrated = false, .gain = 0.0f, .offset = 0.0f },  // UNCALIBRATED -- identity; gain/offset are ignored
        [2] = { .calibrated = false, .gain = 0.0f, .offset = 0.0f },  // UNCALIBRATED -- identity; gain/offset are ignored
    },
};

#endif // SIMFW_SIM_CT_CALIBRATION_DEFAULTS_H
