// ct_i2s_gen.c -- see ct_i2s_gen.h for the contract this implements
// (zero-crossing gating rule, channel-to-module mapping, precision notes).
#include "ct_i2s_gen.h"

#include <math.h>

// CT_I2S_GEN_NUM_CHANNELS must track ct_calibration.h's CT_CAL_NUM_CHANNELS
// (ct_cal_apply() is indexed by the same channel numbers). ct_i2s_gen.h's
// top comment explains why this isn't a shared #include.
#if CT_I2S_GEN_NUM_CHANNELS != CT_CAL_NUM_CHANNELS
#error "CT_I2S_GEN_NUM_CHANNELS must equal CT_CAL_NUM_CHANNELS -- see ct_i2s_gen.h"
#endif

static ct_i2s_gen_channel_cfg_t default_channel_cfg(void)
{
    ct_i2s_gen_channel_cfg_t cfg;
    cfg.mode = CT_I2S_GEN_MODE_MODEL;
    cfg.amps = 0.0f;
    cfg.synth.amplitude = 0.0f; /* ignored/overwritten every sample anyway */
    cfg.synth.phase_deg = 0.0f;
    cfg.synth.dc_offset = 0.0f;
    cfg.synth.clip_fraction = 0.0f;
    cfg.synth.dropout_half_cycle = false;
    cfg.synth.dropout_negative_half = false;
    cfg.apply_immediately = false;
    return cfg;
}

void ct_i2s_gen_init(ct_i2s_gen_ctx_t *ctx, const ct_cal_table_t *cal_table)
{
    sine_synth_init_table(ctx->sine_table);
    ctx->cal_table = cal_table;
    ctx->time_s = 0.0;

    for (uint8_t ch = 0; ch < CT_I2S_GEN_NUM_CHANNELS; ch++) {
        ct_i2s_gen_channel_state_t *st = &ctx->channels[ch];
        st->active = default_channel_cfg();
        st->staged = default_channel_cfg();
        st->staged_pending = false;
        st->prev_raw = 0.0f;
        st->prev_raw_valid = false;
    }
}

bool ct_i2s_gen_stage_config(ct_i2s_gen_ctx_t *ctx, uint8_t channel, const ct_i2s_gen_channel_cfg_t *cfg)
{
    if (channel >= CT_I2S_GEN_NUM_CHANNELS) {
        return false;
    }
    ctx->channels[channel].staged = *cfg;
    ctx->channels[channel].staged_pending = true;
    return true;
}

bool ct_i2s_gen_get_active_config(const ct_i2s_gen_ctx_t *ctx, uint8_t channel, ct_i2s_gen_channel_cfg_t *out)
{
    if (channel >= CT_I2S_GEN_NUM_CHANNELS) {
        return false;
    }
    *out = ctx->channels[channel].active;
    return true;
}

bool ct_i2s_gen_has_pending_change(const ct_i2s_gen_ctx_t *ctx, uint8_t channel, bool *out_pending)
{
    if (channel >= CT_I2S_GEN_NUM_CHANNELS) {
        return false;
    }
    *out_pending = ctx->channels[channel].staged_pending;
    return true;
}

/* Saturating float -> int16 conversion: rounds to nearest, then clamps to
 * [INT16_MIN, INT16_MAX]. Clamping (not a cast) is what keeps an overdriven
 * sample from wrapping into a phase-inverted artefact that would look like a
 * real signal instead of an obviously-clipped one. */
static int16_t saturate_to_int16(float x)
{
    float scaled = x * 32767.0f;
    float rounded = (scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f);

    if (rounded >= 32767.0f) {
        return INT16_MAX;
    }
    if (rounded <= -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)rounded;
}

/* sine_synth's API takes a float time_s. Casting an ever-growing double
 * cursor straight to float would throw away exactly the precision the
 * double cursor exists to keep (see ct_i2s_gen.h's top comment) once t gets
 * large. But sine_synth_raw()/_sample() only ever care about t's fractional
 * position within one mains cycle (they compute frac01(freq*t + phase) --
 * see sine_synth.c), so reducing t modulo the mains period IN DOUBLE first,
 * then casting the small (<1/60s) remainder to float, keeps the value
 * handed to the float API bounded and precise indefinitely, no matter how
 * long ctx->time_s has been running. (fmod(f*t, 1) and f*fmod(t, 1/f) mod 1
 * are the same fractional cycle, since fmod(t, 1/f) only ever removes whole
 * multiples of 1/f from t, i.e. whole cycles.) */
static float reduce_time_for_synth(double t)
{
    double period = 1.0 / (double)CT_I2S_GEN_MAINS_FREQ_HZ;
    double r = fmod(t, period);
    if (r < 0.0) {
        r += period;
    }
    return (float)r;
}

/* Produces the next output sample for one channel at absolute time t,
 * applying the explicit zero-crossing (or apply_immediately) gate described
 * in ct_i2s_gen.h's top comment, and advances that channel's gating state
 * (prev_raw/prev_raw_valid) for the following call. */
static int16_t generate_one_sample(ct_i2s_gen_ctx_t *ctx, uint8_t channel, double t)
{
    ct_i2s_gen_channel_state_t *st = &ctx->channels[channel];

    /* Step 1: does a pending staged change take effect at this sample? */
    if (st->staged_pending) {
        bool apply_now = st->staged.apply_immediately;

        if (!apply_now) {
            if (!st->prev_raw_valid) {
                /* No established waveform yet (first sample ever for this
                 * channel) -- there is nothing to be discontinuous WITH, so
                 * apply immediately rather than gate forever on a crossing
                 * that has no "before" state to be gated relative to. */
                apply_now = true;
            } else {
                float raw_before_switch = sine_synth_raw(ctx->sine_table, CT_I2S_GEN_MAINS_FREQ_HZ,
                                                           st->active.synth.phase_deg, reduce_time_for_synth(t));
                bool rising = false;
                bool crossed = sine_synth_zero_crossing(st->prev_raw, raw_before_switch, &rising);
                /* Only a RISING crossing gates, mirroring ct_wave_pwm.h's
                 * "DMA finished the table" contract: table index 0 is a
                 * rising crossing, so that is the one moment a swap is
                 * inherently aligned to. */
                apply_now = crossed && rising;
            }
        }

        if (apply_now) {
            st->active = st->staged;
            st->staged_pending = false;
        }
    }

    /* Step 2: compute this sample under whatever is now active, and record
     * its RAW value as next call's "previous" for crossing detection. */
    float t_reduced = reduce_time_for_synth(t);
    float raw = sine_synth_raw(ctx->sine_table, CT_I2S_GEN_MAINS_FREQ_HZ, st->active.synth.phase_deg, t_reduced);

    sine_channel_cfg_t synth_now = st->active.synth;
    synth_now.amplitude = ct_cal_apply(ctx->cal_table, channel, st->active.amps);

    float sample_f = sine_synth_sample(&synth_now, ctx->sine_table, CT_I2S_GEN_MAINS_FREQ_HZ, t_reduced);

    st->prev_raw = raw;
    st->prev_raw_valid = true;

    return saturate_to_int16(sample_f);
}

void ct_i2s_gen_fill_block(ct_i2s_gen_ctx_t *ctx, int16_t *module_a_frames, int16_t *module_b_frames,
                            uint32_t num_frames)
{
    const double dt = 1.0 / CT_I2S_GEN_SAMPLE_RATE_HZ;

    for (uint32_t n = 0; n < num_frames; n++) {
        double t = ctx->time_s;

        int16_t s0 = generate_one_sample(ctx, 0, t);
        int16_t s1 = generate_one_sample(ctx, 1, t);
        int16_t s2 = generate_one_sample(ctx, 2, t);

        module_a_frames[2u * n + 0u] = s0;
        module_a_frames[2u * n + 1u] = s1;
        module_b_frames[2u * n + 0u] = s2;
        module_b_frames[2u * n + 1u] = 0; /* unused right channel: always silence, never uninitialised */

        ctx->time_s += dt;
    }
}
