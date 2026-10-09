// ct_amps_cal.c -- see ct_amps_cal.h.
#include "ct_amps_cal.h"

#include <stddef.h> // NULL

ct_amps_cal_table_t ct_amps_cal_uncalibrated_table(void)
{
    ct_amps_cal_table_t table;
    for (unsigned ch = 0; ch < CT_AMPS_CAL_NUM_CHANNELS; ch++) {
        table.channels[ch].calibrated = false;
        table.channels[ch].gain = 0.0f;
        table.channels[ch].offset = 0.0f;
    }
    return table;
}

float ct_amps_cal_apply(const ct_amps_cal_table_t *table, uint8_t channel, float raw_amps)
{
    if (table == NULL || channel >= CT_AMPS_CAL_NUM_CHANNELS) {
        return raw_amps; // identity -- see this file's header comment
    }

    const ct_amps_cal_channel_t *ch = &table->channels[channel];
    if (!ch->calibrated) {
        return raw_amps; // uncommissioned -- compiled-in identity, never ch->gain/offset
    }

    float corrected = ch->gain * raw_amps + ch->offset;
    if (corrected < 0.0f) {
        corrected = 0.0f; // a current reading is never negative -- same
                           // convention as current_sense.c's cs_counts_to_amps()
    }
    return corrected;
}

bool ct_amps_cal_is_calibrated(const ct_amps_cal_table_t *table, uint8_t channel)
{
    if (table == NULL || channel >= CT_AMPS_CAL_NUM_CHANNELS) {
        return false;
    }
    return table->channels[channel].calibrated;
}
