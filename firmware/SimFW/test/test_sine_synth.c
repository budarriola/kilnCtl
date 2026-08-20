// Host tests for sine_synth.c: table correctness, distortion modes, and
// zero-crossing detection (PLAN.md section 13.1 / section 3.3).
#include "test_common.h"
#include "../src/sim/sine_synth.h"

static void test_table_correctness(void)
{
    TEST_SECTION("sine_synth -- table peak/zero values");

    float table[SINE_SYNTH_TABLE_LEN];
    sine_synth_init_table(table);

    TEST_CHECK_NEAR(table[0], 0.0, 1e-5, "entry 0: sin(0) == 0 (rising through zero)");
    TEST_CHECK_NEAR(table[64], 1.0, 1e-5, "entry 64 (1/4 cycle): sin(pi/2) == +1 (peak)");
    TEST_CHECK_NEAR(table[128], 0.0, 1e-5, "entry 128 (1/2 cycle): sin(pi) == 0 (falling through zero)");
    TEST_CHECK_NEAR(table[192], -1.0, 1e-5, "entry 192 (3/4 cycle): sin(3pi/2) == -1 (trough)");

    for (uint32_t i = 0; i < SINE_SYNTH_TABLE_LEN; i++) {
        TEST_CHECK(table[i] >= -1.0001f && table[i] <= 1.0001f, "every table entry stays within [-1,1]");
    }
}

static void test_raw_sample_at_phase_points(void)
{
    TEST_SECTION("sine_synth -- raw sample at known (freq, time) phase points");

    float table[SINE_SYNTH_TABLE_LEN];
    sine_synth_init_table(table);

    float freq = 60.0f;
    float period = 1.0f / freq;

    TEST_CHECK_NEAR(sine_synth_raw(table, freq, 0.0f, 0.0f), 0.0, 1e-2, "t=0, phase=0: raw ~= 0");
    TEST_CHECK_NEAR(sine_synth_raw(table, freq, 0.0f, period / 4.0f), 1.0, 1e-2, "t=T/4: raw ~= peak (+1)");
    TEST_CHECK_NEAR(sine_synth_raw(table, freq, 0.0f, period / 2.0f), 0.0, 1e-2, "t=T/2: raw ~= 0 (falling)");
    TEST_CHECK_NEAR(sine_synth_raw(table, freq, 0.0f, 3.0f * period / 4.0f), -1.0, 1e-2, "t=3T/4: raw ~= trough (-1)");

    /* A 120-degree phase offset shifts the zero crossing by T/3. */
    TEST_CHECK_NEAR(sine_synth_raw(table, freq, 120.0f, 0.0f), 0.866, 2e-2, "phase=120deg at t=0: sin(120deg) ~= 0.866");
}

static void test_distortion_dc_offset_and_clipping(void)
{
    TEST_SECTION("sine_synth -- DC offset and clipping distortion");

    float table[SINE_SYNTH_TABLE_LEN];
    sine_synth_init_table(table);
    float freq = 60.0f;
    float period = 1.0f / freq;

    sine_channel_cfg_t cfg;
    cfg.amplitude = 10.0f;
    cfg.phase_deg = 0.0f;
    cfg.dc_offset = 2.5f;
    cfg.clip_fraction = 0.0f;
    cfg.dropout_half_cycle = false;
    cfg.dropout_negative_half = false;

    float peak = sine_synth_sample(&cfg, table, freq, period / 4.0f);
    TEST_CHECK_NEAR(peak, 12.5, 1e-1, "DC offset: peak (amplitude*1 + dc_offset) shifts by dc_offset");

    cfg.dc_offset = 0.0f;
    cfg.clip_fraction = 0.2f; /* clip level = amplitude * 0.8 = 8.0 */
    float clipped_peak = sine_synth_sample(&cfg, table, freq, period / 4.0f);
    TEST_CHECK_NEAR(clipped_peak, 8.0, 1e-1, "clip_fraction=0.2 clips the peak to amplitude*(1-0.2)");

    float clipped_trough = sine_synth_sample(&cfg, table, freq, 3.0f * period / 4.0f);
    TEST_CHECK_NEAR(clipped_trough, -8.0, 1e-1, "clip_fraction clips the trough symmetrically");

    /* A mid-swing sample (not near the peak) is well under the clip level
     * and must be unaffected. */
    cfg.clip_fraction = 0.9f; /* clip level = 1.0, would clip almost everything except very small values */
    float small = sine_synth_sample(&cfg, table, freq, period / 100.0f); /* small raw value */
    TEST_CHECK(small > -1.0f && small < 1.0f, "a sample under the clip level is not itself clipped");
}

static void test_distortion_half_cycle_dropout(void)
{
    TEST_SECTION("sine_synth -- half-cycle dropout distortion");

    float table[SINE_SYNTH_TABLE_LEN];
    sine_synth_init_table(table);
    float freq = 60.0f;
    float period = 1.0f / freq;

    sine_channel_cfg_t cfg;
    cfg.amplitude = 5.0f;
    cfg.phase_deg = 0.0f;
    cfg.dc_offset = 0.0f;
    cfg.clip_fraction = 0.0f;
    cfg.dropout_half_cycle = true;
    cfg.dropout_negative_half = false; /* drop the positive half */

    float pos_peak = sine_synth_sample(&cfg, table, freq, period / 4.0f); /* raw >= 0 */
    TEST_CHECK_NEAR(pos_peak, 0.0, 1e-6, "dropping the positive half zeroes a positive-half sample");

    float neg_peak = sine_synth_sample(&cfg, table, freq, 3.0f * period / 4.0f); /* raw < 0 */
    TEST_CHECK_NEAR(neg_peak, -5.0, 1e-1, "dropping the positive half leaves the negative half untouched");

    cfg.dropout_negative_half = true; /* now drop the negative half instead */
    float pos_peak2 = sine_synth_sample(&cfg, table, freq, period / 4.0f);
    TEST_CHECK_NEAR(pos_peak2, 5.0, 1e-1, "dropping the negative half leaves the positive half untouched");
    float neg_peak2 = sine_synth_sample(&cfg, table, freq, 3.0f * period / 4.0f);
    TEST_CHECK_NEAR(neg_peak2, 0.0, 1e-6, "dropping the negative half zeroes a negative-half sample");
}

static void test_zero_crossing_detection(void)
{
    TEST_SECTION("sine_synth -- zero-crossing detection");

    bool rising = false;
    TEST_CHECK(sine_synth_zero_crossing(-1.0f, 1.0f, &rising), "sign change negative->positive is a crossing");
    TEST_CHECK(rising, "negative->positive crossing is reported rising");

    TEST_CHECK(sine_synth_zero_crossing(1.0f, -1.0f, &rising), "sign change positive->negative is a crossing");
    TEST_CHECK(!rising, "positive->negative crossing is reported falling");

    TEST_CHECK(!sine_synth_zero_crossing(1.0f, 0.5f, NULL), "staying positive is not a crossing");
    TEST_CHECK(!sine_synth_zero_crossing(-1.0f, -0.1f, NULL), "staying negative is not a crossing");
    TEST_CHECK(!sine_synth_zero_crossing(0.3f, 0.3f, NULL), "an unchanged sample is not a crossing");

    /* Zero is grouped with the non-negative side (only x < 0 is
     * "negative"): a crossing is a transition between the negative state
     * and the non-negative state as a whole. Leaving negative (going to
     * exactly 0, or past it) is rising; entering negative (from exactly 0,
     * or from positive) is falling. This keeps a single physical crossing
     * from being reported twice when a sample lands exactly on 0, e.g. the
     * sine table's own entry 0. */
    TEST_CHECK(sine_synth_zero_crossing(-1.0f, 0.0f, &rising), "leaving negative (to exactly 0) is a rising crossing");
    TEST_CHECK(rising, "negative->zero is reported rising");
    TEST_CHECK(!sine_synth_zero_crossing(0.0f, 1.0f, NULL), "zero->positive stays on the non-negative side: not a second crossing");
    TEST_CHECK(!sine_synth_zero_crossing(1.0f, 0.0f, NULL), "positive->zero stays on the non-negative side: not a crossing yet");
    TEST_CHECK(sine_synth_zero_crossing(0.0f, -1.0f, &rising), "entering negative (from exactly 0) is a falling crossing");
    TEST_CHECK(!rising, "zero->negative is reported falling");

    /* Walking a full cycle of the table must produce exactly two crossings
     * (one rising, one falling). */
    float table[SINE_SYNTH_TABLE_LEN];
    sine_synth_init_table(table);
    int crossings = 0, rising_count = 0, falling_count = 0;
    for (uint32_t i = 0; i < SINE_SYNTH_TABLE_LEN; i++) {
        float prev = table[i];
        float curr = table[(i + 1u) % SINE_SYNTH_TABLE_LEN];
        bool r = false;
        if (sine_synth_zero_crossing(prev, curr, &r)) {
            crossings++;
            if (r) rising_count++; else falling_count++;
        }
    }
    TEST_CHECK(crossings == 2, "one full table cycle has exactly two zero crossings");
    TEST_CHECK(rising_count == 1 && falling_count == 1, "one full cycle: one rising, one falling crossing");
}

static void test_next_zero_crossing_time(void)
{
    TEST_SECTION("sine_synth -- next zero-crossing time helper");

    float freq = 60.0f;
    float half_period = 1.0f / (2.0f * freq);

    float t1 = sine_synth_next_zero_crossing_time(freq, 0.0f, 0.0f);
    TEST_CHECK_NEAR(t1, half_period, 1e-4, "phase=0: next crossing after t=0 is one half-period later");

    float t2 = sine_synth_next_zero_crossing_time(freq, 0.0f, t1);
    TEST_CHECK(t2 > t1, "next-crossing time strictly increases when queried again at a crossing");
    TEST_CHECK_NEAR(t2 - t1, half_period, 1e-4, "consecutive crossings are exactly one half-period apart");

    float t3 = sine_synth_next_zero_crossing_time(freq, 0.0f, t1 + 0.25f * half_period);
    TEST_CHECK(t3 > t1 + 0.25f * half_period, "queried mid-cycle, the next crossing is still strictly ahead");
    TEST_CHECK(t3 <= t1 + half_period + 1e-4f, "queried mid-cycle, the next crossing is not more than one half-period further out");
}

void run_test_sine_synth(void)
{
    test_table_correctness();
    test_raw_sample_at_phase_points();
    test_distortion_dc_offset_and_clipping();
    test_distortion_half_cycle_dropout();
    test_zero_crossing_detection();
    test_next_zero_crossing_time();
}
