// Host tests for ct_i2s_gen.c -- the I2S sample-block generator that
// replaces ct_wave_pwm.h's PWM+RC path (see ct_i2s_gen.h's top comment for
// the hardware-decision background and the explicit zero-crossing-gating
// rationale this test suite exists to pin down).
//
// Sampling-grid facts used throughout (mains 60 Hz @ 16 kHz,
// CT_I2S_GEN_MAINS_FREQ_HZ / CT_I2S_GEN_SAMPLE_RATE_HZ):
//   samples per cycle = 16000/60 = 266.666...7 (NOT an integer -- the whole
//   reason this module's gate has to be explicit rather than block-implicit,
//   see ct_i2s_gen.h). A handful of exact sample indices below (267, 178,
//   89) are derived from that ratio in each test's comment.
#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../src/sim/ct_i2s_gen.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static ct_i2s_gen_channel_cfg_t make_cfg(float amps, float phase_deg, bool apply_immediately)
{
    ct_i2s_gen_channel_cfg_t cfg;
    cfg.mode = CT_I2S_GEN_MODE_MANUAL;
    cfg.amps = amps;
    cfg.synth.amplitude = 0.0f; /* ignored by ct_i2s_gen -- documented in the header */
    cfg.synth.phase_deg = phase_deg;
    cfg.synth.dc_offset = 0.0f;
    cfg.synth.clip_fraction = 0.0f;
    cfg.synth.dropout_half_cycle = false;
    cfg.synth.dropout_negative_half = false;
    cfg.apply_immediately = apply_immediately;
    return cfg;
}

static void test_continuity_across_block_boundaries(void)
{
    TEST_SECTION("ct_i2s_gen -- continuity across block boundaries (resumable fill)");

    const uint32_t total = 200;

    /* Run A: one single fill_block() call for the whole span. */
    ct_i2s_gen_ctx_t ctxA;
    ct_i2s_gen_init(&ctxA, NULL);
    ct_i2s_gen_channel_cfg_t cfg = make_cfg(0.7f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctxA, 0, &cfg);
    ct_i2s_gen_stage_config(&ctxA, 1, &cfg);
    ct_i2s_gen_stage_config(&ctxA, 2, &cfg);

    int16_t a_moduleA[400], a_moduleB[400];
    ct_i2s_gen_fill_block(&ctxA, a_moduleA, a_moduleB, total);

    /* Run B: same starting state, split into two fill_block() calls of 97
     * and 103 frames (deliberately not a clean divisor of anything, so a
     * bug that only works on aligned block sizes would still show up). */
    ct_i2s_gen_ctx_t ctxB;
    ct_i2s_gen_init(&ctxB, NULL);
    ct_i2s_gen_stage_config(&ctxB, 0, &cfg);
    ct_i2s_gen_stage_config(&ctxB, 1, &cfg);
    ct_i2s_gen_stage_config(&ctxB, 2, &cfg);

    int16_t b_moduleA[400], b_moduleB[400];
    ct_i2s_gen_fill_block(&ctxB, b_moduleA, b_moduleB, 97);
    ct_i2s_gen_fill_block(&ctxB, b_moduleA + 97 * 2, b_moduleB + 97 * 2, total - 97);

    bool identical = (memcmp(a_moduleA, b_moduleA, total * 2 * sizeof(int16_t)) == 0) &&
                      (memcmp(a_moduleB, b_moduleB, total * 2 * sizeof(int16_t)) == 0);
    TEST_CHECK(identical, "splitting one 200-frame fill into 97+103 calls produces bit-identical output "
                           "to a single 200-frame call -- no seam discontinuity, no re-derived/drifting phase");

    /* Also directly check no single-sample step exceeds what two *whole*
     * runs' worst-case per-sample delta can be: since a pure 60 Hz sine at
     * amplitude 0.7 has a bounded slope, no consecutive samples in the
     * continuous (single-call) run should differ by more than a small
     * multiple of the max per-sample delta implied by the waveform's peak
     * slope (2*pi*f*amplitude*32767/fs). This catches a coarser class of
     * bug (e.g. an accidental time-cursor reset) even if the bit-identical
     * check above were ever loosened. */
    double max_delta = 2.0 * M_PI * CT_I2S_GEN_MAINS_FREQ_HZ * 0.7 * 32767.0 / CT_I2S_GEN_SAMPLE_RATE_HZ;
    for (uint32_t n = 1; n < total; n++) {
        int32_t d0 = (int32_t)a_moduleA[2 * n] - (int32_t)a_moduleA[2 * (n - 1)];
        TEST_CHECK(fabs((double)d0) <= max_delta + 50.0, "no single-sample step exceeds the waveform's bounded slope");
    }
}

static void test_no_frequency_drift_over_5_seconds(void)
{
    TEST_SECTION("ct_i2s_gen -- no frequency drift: zero crossings stay at 120/s over 5s of audio");

    const uint32_t total = (uint32_t)(5.0 * CT_I2S_GEN_SAMPLE_RATE_HZ); /* 80000 frames = 5.000s */

    ct_i2s_gen_ctx_t ctx;
    ct_i2s_gen_init(&ctx, NULL);
    ct_i2s_gen_channel_cfg_t cfg = make_cfg(1.0f, 0.0f, true); /* full scale, no distortion: output sign == raw sign */
    ct_i2s_gen_stage_config(&ctx, 0, &cfg);
    ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctx, 1, &inert);
    ct_i2s_gen_stage_config(&ctx, 2, &inert);

    /* Independent ground truth: continuous-time double-precision sin(),
     * NOT sine_synth's table, counted with the exact same "x<0 is negative,
     * else non-negative" convention sine_synth_zero_crossing() documents.
     * This is deliberately not calling any of the module under test. */
    int expected_crossings = 0;
    bool prev_neg = false;
    bool first = true;
    for (uint32_t n = 0; n < total; n++) {
        double t = (double)n / CT_I2S_GEN_SAMPLE_RATE_HZ;
        double raw = sin(2.0 * M_PI * (double)CT_I2S_GEN_MAINS_FREQ_HZ * t);
        bool neg = raw < 0.0;
        if (!first && neg != prev_neg) {
            expected_crossings++;
        }
        prev_neg = neg;
        first = false;
    }
    /* Sanity on the ground truth itself: 5s at 60Hz is close to 600
     * crossings (2 per cycle * 60 * 5), minus at most the trailing partial
     * crossing that a half-open [0, 5s) sample window can miss. */
    TEST_CHECK(expected_crossings >= 598 && expected_crossings <= 600,
               "ground-truth crossing count is close to the analytic 2*60*5=600");

    int actual_crossings = 0;
    int32_t prev_sample = 0;
    bool prev_sample_neg = false;
    bool first_sample = true;
    const uint32_t chunk = 4000;
    int16_t moduleA[4000 * 2], moduleB[4000 * 2];
    for (uint32_t off = 0; off < total; off += chunk) {
        uint32_t n = (total - off < chunk) ? (total - off) : chunk;
        ct_i2s_gen_fill_block(&ctx, moduleA, moduleB, n);
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = moduleA[2 * i]; /* channel 0, left */
            bool neg = s < 0;
            if (!first_sample && neg != prev_sample_neg) {
                actual_crossings++;
            }
            prev_sample_neg = neg;
            first_sample = false;
            (void)prev_sample;
            prev_sample = s;
        }
    }

    TEST_CHECK(actual_crossings == expected_crossings,
               "actual output crossing count over 5s matches the independent continuous-time ground truth exactly "
               "-- a naive fixed-samples-per-cycle assumption would drift this off by several crossings");

    double rate = (double)actual_crossings / 5.0;
    TEST_CHECK_NEAR(rate, 120.0, 0.5, "crossing rate stays at 120/s (2*60Hz) within a tight tolerance over 5s");
}

static void test_zero_crossing_gating_exact_landing_sample(void)
{
    TEST_SECTION("ct_i2s_gen -- staged change gates at the exact next RISING zero crossing, not mid-cycle");

    ct_i2s_gen_ctx_t ctx;
    ct_i2s_gen_init(&ctx, NULL);

    ct_i2s_gen_channel_cfg_t cfgA = make_cfg(0.9f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctx, 0, &cfgA);
    ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctx, 1, &inert);
    ct_i2s_gen_stage_config(&ctx, 2, &inert);

    int16_t a[2], b[2];
    /* Generate 50 frames on cfgA (samples n=0..49). n=0 applies cfgA
     * immediately (apply_immediately=true); it stays active with nothing
     * else pending. */
    for (uint32_t n = 0; n < 50; n++) {
        ct_i2s_gen_fill_block(&ctx, a, b, 1);
    }
    ct_i2s_gen_channel_cfg_t active;
    ct_i2s_gen_get_active_config(&ctx, 0, &active);
    TEST_CHECK_NEAR(active.amps, 0.9, 1e-6, "cfgA is still active after 50 frames, well inside the first cycle");

    /* Stage cfgB (gated, not immediate) mid-first-cycle (the first cycle
     * spans samples 0..266). The next RISING zero crossing of a 0-phase 60Hz
     * wave sampled at 16kHz is at continuous time t=1/60s; the first sample
     * index n with n/16000 >= 1/60 is n=ceil(16000/60)=ceil(266.667)=267 --
     * i.e. the switch must land exactly at generated sample #267, not #266
     * (still mid-cycle, one sample early) and not #268 (one sample late). */
    ct_i2s_gen_channel_cfg_t cfgB = make_cfg(0.1f, 0.0f, false);
    ct_i2s_gen_stage_config(&ctx, 0, &cfgB);

    bool pending = false;
    ct_i2s_gen_has_pending_change(&ctx, 0, &pending);
    TEST_CHECK(pending, "staging a gated change immediately marks it pending");

    int landing_index = -1;
    for (uint32_t n = 50; n < 400; n++) {
        ct_i2s_gen_fill_block(&ctx, a, b, 1);
        ct_i2s_gen_get_active_config(&ctx, 0, &active);
        if (landing_index < 0 && active.amps < 0.5f) {
            landing_index = (int)n;
        }
        if ((int)n < 267) {
            TEST_CHECK_NEAR(active.amps, 0.9, 1e-6, "cfgB must not appear before sample 267 (still mid-cycle)");
        }
    }
    TEST_CHECK(landing_index == 267, "cfgB lands at exactly sample index 267, the first rising crossing after staging");

    ct_i2s_gen_has_pending_change(&ctx, 0, &pending);
    TEST_CHECK(!pending, "once applied, the channel no longer reports a pending change");
}

static void test_apply_immediately_lands_on_very_next_sample(void)
{
    TEST_SECTION("ct_i2s_gen -- apply_immediately steps on the very next sample, mid-cycle");

    ct_i2s_gen_ctx_t ctx;
    ct_i2s_gen_init(&ctx, NULL);
    ct_i2s_gen_channel_cfg_t cfgA = make_cfg(0.9f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctx, 0, &cfgA);
    ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctx, 1, &inert);
    ct_i2s_gen_stage_config(&ctx, 2, &inert);

    int16_t a[2], b[2];
    for (uint32_t n = 0; n < 50; n++) {
        ct_i2s_gen_fill_block(&ctx, a, b, 1); /* well inside the first cycle, nowhere near a crossing */
    }

    ct_i2s_gen_channel_cfg_t cfgC = make_cfg(0.2f, 0.0f, true); /* apply_immediately = true */
    ct_i2s_gen_stage_config(&ctx, 0, &cfgC);

    ct_i2s_gen_fill_block(&ctx, a, b, 1); /* generates sample #50 */
    ct_i2s_gen_channel_cfg_t active;
    ct_i2s_gen_get_active_config(&ctx, 0, &active);
    TEST_CHECK_NEAR(active.amps, 0.2, 1e-6, "apply_immediately lands on the very next generated sample, mid-cycle");
}

static void test_channel_independence(void)
{
    TEST_SECTION("ct_i2s_gen -- changing channel 1 leaves channels 0 and 2 bit-identical");

    const uint32_t total = 300;

    /* Run A: three distinct, never-changed configs. */
    ct_i2s_gen_ctx_t ctxA;
    ct_i2s_gen_init(&ctxA, NULL);
    ct_i2s_gen_channel_cfg_t c0 = make_cfg(0.3f, 0.0f, true);
    ct_i2s_gen_channel_cfg_t c1 = make_cfg(0.6f, 90.0f, true);
    ct_i2s_gen_channel_cfg_t c2 = make_cfg(0.9f, 45.0f, true);
    ct_i2s_gen_stage_config(&ctxA, 0, &c0);
    ct_i2s_gen_stage_config(&ctxA, 1, &c1);
    ct_i2s_gen_stage_config(&ctxA, 2, &c2);
    int16_t Aa[600], Ab[600];
    ct_i2s_gen_fill_block(&ctxA, Aa, Ab, total);

    /* Run B: identical start, but channel 1 gets a completely different
     * config partway through. */
    ct_i2s_gen_ctx_t ctxB;
    ct_i2s_gen_init(&ctxB, NULL);
    ct_i2s_gen_stage_config(&ctxB, 0, &c0);
    ct_i2s_gen_stage_config(&ctxB, 1, &c1);
    ct_i2s_gen_stage_config(&ctxB, 2, &c2);
    int16_t Ba[600], Bb[600];
    ct_i2s_gen_fill_block(&ctxB, Ba, Bb, 100);

    ct_i2s_gen_channel_cfg_t c1_changed = make_cfg(0.05f, 200.0f, true);
    c1_changed.synth.dropout_half_cycle = true;
    ct_i2s_gen_stage_config(&ctxB, 1, &c1_changed);
    ct_i2s_gen_fill_block(&ctxB, Ba + 100 * 2, Bb + 100 * 2, total - 100);

    bool ch0_identical = true, ch2_identical = true, ch1_differs = false;
    for (uint32_t n = 0; n < total; n++) {
        if (Aa[2 * n] != Ba[2 * n]) {
            ch0_identical = false; /* module A left = channel 0 */
        }
        if (Bb[2 * n] != Ab[2 * n]) {
            ch2_identical = false; /* module B left = channel 2 */
        }
        if (n >= 100 && Aa[2 * n + 1] != Ba[2 * n + 1]) {
            ch1_differs = true; /* module A right = channel 1 */
        }
    }
    TEST_CHECK(ch0_identical, "channel 0's output is bit-identical whether or not channel 1 was reconfigured");
    TEST_CHECK(ch2_identical, "channel 2's output is bit-identical whether or not channel 1 was reconfigured");
    TEST_CHECK(ch1_differs, "channel 1's own output DOES change after its reconfiguration (sanity: the change was real)");
}

static void test_phase_offsets_produce_expected_crossing_positions(void)
{
    TEST_SECTION("ct_i2s_gen -- phase 0/120/240deg produce the expected relative first-crossing sample index");

    /* Derivation (see file header for the 266.667 samples/cycle fact):
     * a channel at phase P leads phase 0 by P/360 of a cycle, so its raw
     * waveform's argument reaches the next rising-crossing multiple of 2*pi
     * that much sooner. First rising crossing strictly after t=0:
     *   phase=0:   t=1/60s       -> ceil(t*16000) = ceil(266.667) = 267
     *   phase=120: t=(2/3)/60s   -> ceil(t*16000) = ceil(177.778) = 178
     *   phase=240: t=(1/3)/60s   -> ceil(t*16000) = ceil(88.889)  = 89
     */
    const float phases[3] = {0.0f, 120.0f, 240.0f};
    const int expected_index[3] = {267, 178, 89};

    for (int i = 0; i < 3; i++) {
        ct_i2s_gen_ctx_t ctx;
        ct_i2s_gen_init(&ctx, NULL);
        ct_i2s_gen_channel_cfg_t cfg = make_cfg(1.0f, phases[i], true);
        ct_i2s_gen_stage_config(&ctx, 0, &cfg);
        ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);
        ct_i2s_gen_stage_config(&ctx, 1, &inert);
        ct_i2s_gen_stage_config(&ctx, 2, &inert);

        int16_t a[2], b[2];
        int found = -1;
        bool prev_neg = false;
        bool first = true;
        for (uint32_t n = 0; n < 400 && found < 0; n++) {
            ct_i2s_gen_fill_block(&ctx, a, b, 1);
            bool neg = a[0] < 0;
            if (!first && prev_neg && !neg) {
                found = (int)n;
            }
            prev_neg = neg;
            first = false;
        }
        TEST_CHECK(found == expected_index[i], "phase offset produces the expected first rising-crossing sample index");
    }
}

static void test_module_b_right_channel_always_silent(void)
{
    TEST_SECTION("ct_i2s_gen -- module B's right channel is exactly 0 in every frame, always");

    ct_i2s_gen_ctx_t ctx;
    ct_i2s_gen_init(&ctx, NULL);
    /* Deliberately drive channel 2 (module B left) hard, to make sure a bug
     * that leaked channel 2's (or any channel's) signal into module B's
     * right slot would show up loudly. */
    ct_i2s_gen_channel_cfg_t hot = make_cfg(1.0f, 0.0f, true);
    hot.synth.dc_offset = 3.0f; /* forces saturation -- as far from 0 as this module can produce */
    ct_i2s_gen_stage_config(&ctx, 0, &hot);
    ct_i2s_gen_stage_config(&ctx, 1, &hot);
    ct_i2s_gen_stage_config(&ctx, 2, &hot);

    const uint32_t total = 500;
    int16_t moduleA[1000], moduleB[1000];
    ct_i2s_gen_fill_block(&ctx, moduleA, moduleB, total);

    for (uint32_t n = 0; n < total; n++) {
        TEST_CHECK(moduleB[2 * n + 1] == 0, "module B right channel is exactly 0 for every frame");
    }
}

static void test_int16_saturation_clamps_not_wraps(void)
{
    TEST_SECTION("ct_i2s_gen -- over-range amplitude saturates to INT16_MIN/MAX, never wraps");

    ct_i2s_gen_ctx_t ctx;
    ct_i2s_gen_init(&ctx, NULL);
    ct_i2s_gen_channel_cfg_t hot_pos = make_cfg(1.0f, 0.0f, true);
    hot_pos.synth.dc_offset = 5.0f; /* amplitude(<=1) + dc_offset(5) never dips below +4: always saturates positive */
    ct_i2s_gen_stage_config(&ctx, 0, &hot_pos);

    ct_i2s_gen_channel_cfg_t hot_neg = make_cfg(1.0f, 0.0f, true);
    hot_neg.synth.dc_offset = -5.0f; /* symmetric: always saturates negative */
    ct_i2s_gen_stage_config(&ctx, 1, &hot_neg);

    ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctx, 2, &inert);

    const uint32_t total = 300; /* > 1 full cycle, so both peak and trough are exercised */
    int16_t moduleA[600], moduleB[600];
    ct_i2s_gen_fill_block(&ctx, moduleA, moduleB, total);

    for (uint32_t n = 0; n < total; n++) {
        TEST_CHECK(moduleA[2 * n] == INT16_MAX, "channel 0 (dc_offset=+5) saturates to exactly INT16_MAX, every sample");
        TEST_CHECK(moduleA[2 * n + 1] == INT16_MIN, "channel 1 (dc_offset=-5) saturates to exactly INT16_MIN, every sample");
        /* The wrap failure mode this guards against: a naive (int16_t) cast
         * of an out-of-range float wraps sign, which would show up here as
         * channel 0 reading some large-magnitude NEGATIVE value instead of
         * INT16_MAX -- so this is not merely "channel 0 is nonzero". */
        TEST_CHECK(moduleA[2 * n] > 0, "channel 0's saturated sample is positive, not a sign-wrapped negative");
        TEST_CHECK(moduleA[2 * n + 1] < 0, "channel 1's saturated sample is negative, not a sign-wrapped positive");
    }
}

static void test_distortion_knobs_change_output_as_expected(void)
{
    TEST_SECTION("ct_i2s_gen -- each distortion knob individually changes output in the expected direction");

    const uint32_t cycle = 300; /* > one full 266.667-sample cycle */

    /* dropout_half_cycle: samples on the dropped-raw-sign half must be
     * exactly 0; the other half must be untouched (matches the plain-sine
     * magnitude within the sampling grid's normal quantization). */
    {
        ct_i2s_gen_ctx_t ctx;
        ct_i2s_gen_init(&ctx, NULL);
        ct_i2s_gen_channel_cfg_t cfg = make_cfg(1.0f, 0.0f, true);
        cfg.synth.dropout_half_cycle = true;
        cfg.synth.dropout_negative_half = false; /* drop the positive-raw half */
        ct_i2s_gen_stage_config(&ctx, 0, &cfg);
        ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);
        ct_i2s_gen_stage_config(&ctx, 1, &inert);
        ct_i2s_gen_stage_config(&ctx, 2, &inert);

        int16_t moduleA[600], moduleB[600];
        ct_i2s_gen_fill_block(&ctx, moduleA, moduleB, cycle);

        bool saw_dropped_zero = false, saw_negative_half_nonzero = false;
        for (uint32_t n = 0; n < cycle; n++) {
            double t = (double)n / CT_I2S_GEN_SAMPLE_RATE_HZ;
            double raw = sin(2.0 * M_PI * (double)CT_I2S_GEN_MAINS_FREQ_HZ * t);
            if (raw >= 0.0) {
                TEST_CHECK(moduleA[2 * n] == 0, "dropout_half_cycle(positive): every positive-raw sample is exactly 0");
                saw_dropped_zero = true;
            } else if (raw < -0.05) { /* stay clear of the sampling grid's zero-crossing quantization */
                TEST_CHECK(moduleA[2 * n] < 0, "dropout_half_cycle(positive): the negative half is left untouched (still negative)");
                saw_negative_half_nonzero = true;
            }
        }
        TEST_CHECK(saw_dropped_zero, "test actually exercised the dropped half (sanity)");
        TEST_CHECK(saw_negative_half_nonzero, "test actually exercised the untouched half (sanity)");
    }

    /* clip_fraction: flattens at amplitude*(1-clip_fraction), a genuine
     * plateau of repeated identical samples near the peak/trough. */
    {
        ct_i2s_gen_ctx_t ctx;
        ct_i2s_gen_init(&ctx, NULL);
        ct_i2s_gen_channel_cfg_t cfg = make_cfg(1.0f, 0.0f, true); /* identity cal: amplitude fraction = 1.0 */
        cfg.synth.clip_fraction = 0.5f;                             /* clip level = 1.0*(1-0.5) = 0.5 */
        ct_i2s_gen_stage_config(&ctx, 0, &cfg);
        ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);
        ct_i2s_gen_stage_config(&ctx, 1, &inert);
        ct_i2s_gen_stage_config(&ctx, 2, &inert);

        int16_t moduleA[600], moduleB[600];
        ct_i2s_gen_fill_block(&ctx, moduleA, moduleB, cycle);

        int16_t max_val = 0;
        for (uint32_t n = 0; n < cycle; n++) {
            if (moduleA[2 * n] > max_val) {
                max_val = moduleA[2 * n];
            }
        }
        TEST_CHECK_NEAR(max_val, 0.5 * 32767.0, 2.0, "clip_fraction=0.5 flattens the peak at exactly the clip level");
    }

    /* dc_offset: shifts the mean of a full cycle by (roughly) dc_offset in
     * amplitude units, i.e. dc_offset*32767 in int16 units, when small
     * enough that nothing saturates. */
    {
        ct_i2s_gen_ctx_t ctx_plain, ctx_offset;
        ct_i2s_gen_init(&ctx_plain, NULL);
        ct_i2s_gen_init(&ctx_offset, NULL);

        ct_i2s_gen_channel_cfg_t plain = make_cfg(0.3f, 0.0f, true);
        ct_i2s_gen_channel_cfg_t offset = make_cfg(0.3f, 0.0f, true);
        offset.synth.dc_offset = 0.1f;
        ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);

        ct_i2s_gen_stage_config(&ctx_plain, 0, &plain);
        ct_i2s_gen_stage_config(&ctx_plain, 1, &inert);
        ct_i2s_gen_stage_config(&ctx_plain, 2, &inert);
        ct_i2s_gen_stage_config(&ctx_offset, 0, &offset);
        ct_i2s_gen_stage_config(&ctx_offset, 1, &inert);
        ct_i2s_gen_stage_config(&ctx_offset, 2, &inert);

        int16_t mA1[600], mB1[600], mA2[600], mB2[600];
        ct_i2s_gen_fill_block(&ctx_plain, mA1, mB1, cycle);
        ct_i2s_gen_fill_block(&ctx_offset, mA2, mB2, cycle);

        double sum_plain = 0.0, sum_offset = 0.0;
        for (uint32_t n = 0; n < cycle; n++) {
            sum_plain += mA1[2 * n];
            sum_offset += mA2[2 * n];
        }
        double mean_plain = sum_plain / cycle;
        double mean_offset = sum_offset / cycle;
        TEST_CHECK_NEAR(mean_offset - mean_plain, 0.1 * 32767.0, 200.0,
                         "dc_offset=0.1 shifts the cycle mean by ~0.1*32767 int16 units");
    }
}

static void test_uncalibrated_channels_are_identity(void)
{
    TEST_SECTION("ct_i2s_gen -- uncalibrated channels (default/NULL table) behave as identity");

    const uint32_t cycle = 300;

    /* NULL table and ct_cal_default_table() (all-uncalibrated) must produce
     * bit-identical output -- both are ct_calibration.h's identity path. */
    ct_i2s_gen_ctx_t ctx_null, ctx_default;
    ct_i2s_gen_init(&ctx_null, NULL);
    ct_i2s_gen_init(&ctx_default, ct_cal_default_table());

    ct_i2s_gen_channel_cfg_t cfg = make_cfg(0.5f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctx_null, 0, &cfg);
    ct_i2s_gen_stage_config(&ctx_default, 0, &cfg);
    ct_i2s_gen_channel_cfg_t inert = make_cfg(0.0f, 0.0f, true);
    ct_i2s_gen_stage_config(&ctx_null, 1, &inert);
    ct_i2s_gen_stage_config(&ctx_null, 2, &inert);
    ct_i2s_gen_stage_config(&ctx_default, 1, &inert);
    ct_i2s_gen_stage_config(&ctx_default, 2, &inert);

    int16_t mAn[600], mBn[600], mAd[600], mBd[600];
    ct_i2s_gen_fill_block(&ctx_null, mAn, mBn, cycle);
    ct_i2s_gen_fill_block(&ctx_default, mAd, mBd, cycle);
    TEST_CHECK(memcmp(mAn, mAd, sizeof(mAn)) == 0, "NULL cal table and the all-uncalibrated default table produce bit-identical output");

    /* Identity means 0.5 simulated amps -> 0.5 amplitude fraction -> peak
     * int16 magnitude ~= 0.5*32767 (a bit under, since the 266.667-sample
     * grid rarely lands exactly on the true peak). */
    int16_t max_val = 0;
    for (uint32_t n = 0; n < cycle; n++) {
        if (mAn[2 * n] > max_val) {
            max_val = mAn[2 * n];
        }
    }
    TEST_CHECK_NEAR(max_val, 0.5 * 32767.0, 50.0, "uncalibrated amps=0.5 peaks near 0.5*32767 (identity mapping)");

    /* A genuinely calibrated channel (mirrors test_ct_calibration.c's own
     * fixture: gain 0.05 -> 10A reaches full scale) must NOT be identity:
     * 10 simulated amps here should peak near full scale (32767), not near
     * 10*32767 clamped, and definitely not near the uncalibrated identity's
     * treatment of "10" as already being in the [0,1] fraction domain
     * (which would clamp to 1.0 too, coincidentally -- so use a value where
     * identity and calibrated clearly diverge: 4A). */
    ct_cal_table_t cal;
    cal.channels[0].calibrated = true;
    cal.channels[0].gain = 0.05f;
    cal.channels[0].offset = 0.0f;
    cal.channels[1].calibrated = false;
    cal.channels[1].gain = 0.0f;
    cal.channels[1].offset = 0.0f;
    cal.channels[2].calibrated = false;
    cal.channels[2].gain = 0.0f;
    cal.channels[2].offset = 0.0f;

    ct_i2s_gen_ctx_t ctx_cal;
    ct_i2s_gen_init(&ctx_cal, &cal);
    ct_i2s_gen_channel_cfg_t cfg4a = make_cfg(4.0f, 0.0f, true); /* identity would clamp this to amplitude=1.0 */
    ct_i2s_gen_stage_config(&ctx_cal, 0, &cfg4a);
    ct_i2s_gen_stage_config(&ctx_cal, 1, &inert);
    ct_i2s_gen_stage_config(&ctx_cal, 2, &inert);

    int16_t mAc[600], mBc[600];
    ct_i2s_gen_fill_block(&ctx_cal, mAc, mBc, cycle);
    int16_t max_cal = 0;
    for (uint32_t n = 0; n < cycle; n++) {
        if (mAc[2 * n] > max_cal) {
            max_cal = mAc[2 * n];
        }
    }
    /* 4A * gain 0.05 = 0.2 amplitude fraction -> peak ~= 0.2*32767, clearly
     * different from an identity channel's clamped 1.0*32767 peak. */
    TEST_CHECK_NEAR(max_cal, 0.2 * 32767.0, 50.0, "calibrated channel: 4A -> 0.2 fraction, not identity's clamped 1.0");
    TEST_CHECK(max_cal < max_val, "the calibrated channel's peak is well below the uncalibrated identity channel's peak");
}

static void test_calibration_applied_per_own_channel_not_leaked(void)
{
    TEST_SECTION("ct_i2s_gen -- each channel's calibration is applied against ITS OWN channel index, not another's");

    /* Asymmetric per-channel table, mirroring test_ct_calibration.c's own
     * fixture: every channel has a different gain, so a generator that
     * mistakenly applied one channel's calibration to another's amps would
     * show up as a wrong peak on that channel, not a coincidence. */
    ct_cal_table_t cal;
    cal.channels[0].calibrated = true;
    cal.channels[0].gain = 0.05f; /* 4A -> 0.2 fraction */
    cal.channels[0].offset = 0.0f;
    cal.channels[1].calibrated = true;
    cal.channels[1].gain = 0.20f; /* 4A -> 0.8 fraction -- clearly different from ch0 */
    cal.channels[1].offset = 0.0f;
    cal.channels[2].calibrated = true;
    cal.channels[2].gain = 0.10f; /* 4A -> 0.4 fraction -- clearly different from both */
    cal.channels[2].offset = 0.0f;

    ct_i2s_gen_ctx_t ctx;
    ct_i2s_gen_init(&ctx, &cal);
    ct_i2s_gen_channel_cfg_t cfg4a = make_cfg(4.0f, 0.0f, true); /* same amps on all 3 channels */
    ct_i2s_gen_stage_config(&ctx, 0, &cfg4a);
    ct_i2s_gen_stage_config(&ctx, 1, &cfg4a);
    ct_i2s_gen_stage_config(&ctx, 2, &cfg4a);

    const uint32_t cycle = 300;
    int16_t moduleA[600], moduleB[600];
    ct_i2s_gen_fill_block(&ctx, moduleA, moduleB, cycle);

    int16_t max0 = 0, max1 = 0, max2 = 0;
    for (uint32_t n = 0; n < cycle; n++) {
        if (moduleA[2 * n] > max0) max0 = moduleA[2 * n];
        if (moduleA[2 * n + 1] > max1) max1 = moduleA[2 * n + 1];
        if (moduleB[2 * n] > max2) max2 = moduleB[2 * n];
    }

    /* Same input amps, three different fractions (0.2/0.8/0.4) -> three
     * clearly different peaks. If the generator ever applied, say, channel
     * 0's calibration to every channel, max1 and max2 would collapse to
     * max0's value instead. */
    TEST_CHECK_NEAR(max0, 0.2 * 32767.0, 50.0, "channel 0 uses its OWN gain (0.05): 4A -> 0.2 fraction");
    TEST_CHECK_NEAR(max1, 0.8 * 32767.0, 50.0, "channel 1 uses its OWN gain (0.20): 4A -> 0.8 fraction");
    TEST_CHECK_NEAR(max2, 0.4 * 32767.0, 50.0, "channel 2 uses its OWN gain (0.10): 4A -> 0.4 fraction");
    TEST_CHECK(max0 != max1 && max1 != max2 && max0 != max2,
               "all three channels' peaks are distinct -- no channel's calibration leaked into another's");
}

void run_test_ct_i2s_gen(void)
{
    test_continuity_across_block_boundaries();
    test_no_frequency_drift_over_5_seconds();
    test_zero_crossing_gating_exact_landing_sample();
    test_apply_immediately_lands_on_very_next_sample();
    test_channel_independence();
    test_phase_offsets_produce_expected_crossing_positions();
    test_module_b_right_channel_always_silent();
    test_int16_saturation_clamps_not_wraps();
    test_distortion_knobs_change_output_as_expected();
    test_uncalibrated_channels_are_identity();
    test_calibration_applied_per_own_channel_not_leaked();
}
