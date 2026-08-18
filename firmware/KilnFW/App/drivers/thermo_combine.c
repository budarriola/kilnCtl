#include "thermo_combine.h"

#include <math.h>

float thermo_combine(const float *channel_c, const bool *channel_ok, uint8_t channel_count,
                     uint8_t thermo_mask, bool *out_valid)
{
    float sum = 0.0f;
    uint8_t n = 0;

    for (uint8_t ci = 0; ci < channel_count && ci < 8; ci++) {
        if ((thermo_mask & (uint8_t)(1u << ci)) == 0) {
            continue;
        }
        if (!channel_ok[ci]) {
            /* Dropped, not zeroed -- a faulted channel contributing a 0.0f
             * (or worse, a stale pre-fault reading a caller forgot to mark
             * invalid) would pull the average toward garbage exactly when
             * the mean most needs the remaining good channels untouched. */
            continue;
        }
        sum += channel_c[ci];
        n++;
    }

    if (n == 0) {
        *out_valid = false;
        return NAN; /* mirrors zones_config_apply_cal()'s NaN-passthrough convention */
    }

    *out_valid = true;
    return sum / (float)n;
}
