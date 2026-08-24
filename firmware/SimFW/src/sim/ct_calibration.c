// ct_calibration.c -- see ct_calibration.h for the design summary, the
// "uncalibrated is an explicit state" rationale, and the regeneration
// command for the compiled-in defaults.
#include "ct_calibration.h"

#include "ct_calibration_defaults.h"

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

float ct_cal_apply(const ct_cal_table_t *table, uint8_t channel, float amps)
{
    if (channel >= CT_CAL_NUM_CHANNELS) {
        return 0.0f; // caller bug -- output silence, never a guessed drive level
    }
    // No table at all, or this specific channel was never calibrated: fall
    // back to the historical IDENTITY behavior. Deliberately NOT "use
    // channel 0's constants" or "use gain 1 / offset 0 from the struct" --
    // an uncalibrated channel must behave exactly as it did before this
    // module existed.
    if (table == NULL || !table->channels[channel].calibrated) {
        return clamp01(amps);
    }
    const ct_cal_channel_t *cal = &table->channels[channel];
    return clamp01(cal->gain * amps + cal->offset);
}

bool ct_cal_is_calibrated(const ct_cal_table_t *table, uint8_t channel)
{
    if (table == NULL || channel >= CT_CAL_NUM_CHANNELS) {
        return false;
    }
    return table->channels[channel].calibrated;
}

const char *ct_cal_id(const ct_cal_table_t *table)
{
    if (table == NULL || !table->ct_id_known) {
        return "";
    }
    return table->ct_id;
}

bool ct_cal_id_known(const ct_cal_table_t *table)
{
    return table != NULL && table->ct_id_known;
}

const ct_cal_table_t *ct_cal_default_table(void)
{
    return &CT_CAL_DEFAULT_TABLE;
}
