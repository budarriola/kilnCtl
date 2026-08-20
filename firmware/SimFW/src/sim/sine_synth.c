// sine_synth.c -- see sine_synth.h for the table/sample/distortion/zero-
// crossing contract (PLAN.md section 3.3).
#include "sine_synth.h"

#include <math.h>

#define SINE_SYNTH_PI_F 3.14159265358979323846f

void sine_synth_init_table(float table[SINE_SYNTH_TABLE_LEN])
{
    for (uint32_t i = 0; i < SINE_SYNTH_TABLE_LEN; i++) {
        table[i] = sinf(2.0f * SINE_SYNTH_PI_F * (float)i / (float)SINE_SYNTH_TABLE_LEN);
    }
}

/* Wraps x into [0,1) -- fmodf alone can return a negative result for
 * negative x, which a plain array index must never see. */
static float frac01(float x)
{
    float f = fmodf(x, 1.0f);
    if (f < 0.0f) {
        f += 1.0f;
    }
    return f;
}

float sine_synth_raw(const float table[SINE_SYNTH_TABLE_LEN], float freq_hz, float phase_deg, float time_s)
{
    float phase_rad = phase_deg * (SINE_SYNTH_PI_F / 180.0f);
    float angle = 2.0f * SINE_SYNTH_PI_F * freq_hz * time_s + phase_rad;
    float cycle_frac = frac01(angle / (2.0f * SINE_SYNTH_PI_F));

    float pos = cycle_frac * (float)SINE_SYNTH_TABLE_LEN;
    uint32_t idx0 = (uint32_t)pos % SINE_SYNTH_TABLE_LEN;
    uint32_t idx1 = (idx0 + 1u) % SINE_SYNTH_TABLE_LEN;
    float frac = pos - (float)(uint32_t)pos;

    return table[idx0] * (1.0f - frac) + table[idx1] * frac;
}

float sine_synth_sample(const sine_channel_cfg_t *cfg, const float table[SINE_SYNTH_TABLE_LEN],
                         float freq_hz, float time_s)
{
    float raw = sine_synth_raw(table, freq_hz, cfg->phase_deg, time_s);
    float scaled = cfg->amplitude * raw;

    if (cfg->clip_fraction > 0.0f) {
        float level = cfg->amplitude * (1.0f - cfg->clip_fraction);
        if (level < 0.0f) {
            level = 0.0f;
        }
        if (scaled > level) scaled = level;
        if (scaled < -level) scaled = -level;
    }

    if (cfg->dropout_half_cycle) {
        bool drop_positive = !cfg->dropout_negative_half;
        if (drop_positive && raw >= 0.0f) {
            scaled = 0.0f;
        } else if (!drop_positive && raw < 0.0f) {
            scaled = 0.0f;
        }
    }

    return scaled + cfg->dc_offset;
}

bool sine_synth_zero_crossing(float prev_raw, float curr_raw, bool *out_rising)
{
    /* A sample landing exactly on zero is grouped with the non-negative
     * side (x < 0 is the only "negative" state). This is what keeps a
     * single physical zero crossing from being reported twice when a
     * sampled sequence happens to contain an exact-zero sample (e.g. the
     * table's own entry 0): entering zero from below is the crossing;
     * leaving zero upward afterward is not a second one, and symmetrically
     * for the falling side. Without this convention a caller stepping
     * through sine_synth's own 256-entry table (whose entry 0 is an exact
     * 0.0f) would see two "crossings" for the one place the waveform
     * actually crosses zero on the rising side. */
    bool prev_neg = prev_raw < 0.0f;
    bool curr_neg = curr_raw < 0.0f;

    if (prev_neg == curr_neg) {
        return false;
    }
    if (out_rising) {
        *out_rising = !curr_neg; /* prev was negative, curr is not: rising */
    }
    return true;
}

float sine_synth_next_zero_crossing_time(float freq_hz, float phase_deg, float time_s)
{
    float period = 1.0f / freq_hz;
    float half_period = period * 0.5f;
    float phase_offset_time = (phase_deg / 360.0f) * period;

    float t_shifted = time_s + phase_offset_time;
    float n = floorf(t_shifted / half_period) + 1.0f;
    float next_shifted = n * half_period;

    return next_shifted - phase_offset_time;
}
