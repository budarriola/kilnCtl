// test_current_sense_hal_adc.c -- HAL Phase 1b (docs/HW_ABSTRACTION_PLAN.md
// "hal_adc -- pico-only" section): current_sense.c is now a hal_adc client.
// This is the host-testable oversample/discard-first-sample property the
// plan promises: current_sense.c itself must keep that policy (hal_adc.h's
// contract is "one raw sample, no filtering"), and this test proves it by
// driving current_sense_sample() for real against
// firmware/hwAbstraction/host/fake_adc.c and checking, from fake_adc's own
// event/read counters, that:
//   1. exactly 17 hal_adc_read_raw() calls happen per channel per sample
//      pass (1 discarded + CS_OVERSAMPLE_N=16 averaged), so 51 total across
//      the three channels
//   2. the discarded first sample is NOT included in the reported average
//      (proven by scripting a first sample far outside the rest of the
//      series and checking the reported amps/counts reflect the other 16
//      only)
//   3. scripted samples use realistic 12-bit quantization (0..4095)
#include "test_common.h"

#include "fake_adc.h"
#include "hal_adc.h"

#include "../src/current_sense.h"

// Mirrors current_sense.c's own CS_OVERSAMPLE_N -- not included via header
// (it is a private #define in the .c file), so restated here with a comment
// pointing back at the source of truth.
#define TEST_CS_OVERSAMPLE_N 16u

static void script_channel(int channel, uint16_t discard_value, uint16_t real_value)
{
    // First sample: the one current_sense.c must discard. Scripted far away
    // from real_value so an accidental inclusion in the average is visible.
    uint16_t discard[1] = { discard_value };
    TEST_CHECK(fake_adc_script_samples(channel, discard, 1) == HAL_OK,
               "scripting the discard-first sample succeeds");

    // 16 identical realistic 12-bit samples -- average is exactly real_value
    // if (and only if) the discard sample is excluded.
    uint16_t reals[TEST_CS_OVERSAMPLE_N];
    for (unsigned i = 0; i < TEST_CS_OVERSAMPLE_N; i++) {
        reals[i] = real_value;
    }
    TEST_CHECK(fake_adc_script_samples(channel, reals, TEST_CS_OVERSAMPLE_N) == HAL_OK,
               "scripting the 16 oversampled reads succeeds");
}

static void test_current_sense_discards_first_sample_and_oversamples_16x(void)
{
    TEST_SECTION("current_sense_sample() -- hal_adc client: discard-first-sample "
                 "+ 16x oversample (HAL Phase 1b)");

    fake_adc_reset();
    // On target, current_task_start() calls hal_adc_init()/hal_adc_gpio_
    // enable() once at bring-up, before current_sense_sample() ever runs
    // (current_sense.c itself never calls hal_adc_init() -- see
    // current_task.c). Mirror that ordering here: fake_adc_select()/read_
    // raw() are no-ops (return without consuming the script) until
    // hal_adc_init() has been called.
    hal_adc_init();
    current_sense_init();

    // Commission channel 0 only, with a trivial 1:1 counts->amps mapping
    // (gain=1, k_ct=1, zero_counts=0) so the reported amps figure directly
    // exposes exactly what counts value the average resolved to.
    current_sense_cal_t cal = {0};
    cal.gain[0] = 1.0f;
    cal.k_ct_v_per_a[0] = 1.0f;
    cal.zero_counts[0] = 0;
    cal.i_present_a = 0.0f;
    cal.calibrated = true;
    current_sense_set_cal(&cal);

    // Realistic 12-bit quantized values (0..4095), one discard value far
    // from the real series so inclusion would visibly skew the average.
    script_channel(0, /*discard=*/4095, /*real=*/1000);
    script_channel(1, /*discard=*/0,    /*real=*/500);
    script_channel(2, /*discard=*/2047, /*real=*/250);

    current_sense_sample();

    // 1 discard + 16 oversampled reads per channel, 3 channels = 51 total.
    TEST_CHECK(fake_adc_read_count() == 3u * (1u + TEST_CS_OVERSAMPLE_N),
               "exactly (1 discard + 16 oversample) x 3 channels reads happened");

    // Every scripted sample was consumed (none left pending) -- proves the
    // discard sample was actually read, not skipped/left in the queue.
    TEST_CHECK(fake_adc_pending_count(0) == 0, "channel 0's script is fully consumed");
    TEST_CHECK(fake_adc_pending_count(1) == 0, "channel 1's script is fully consumed");
    TEST_CHECK(fake_adc_pending_count(2) == 0, "channel 2's script is fully consumed");

    // The averaged counts must reflect ONLY the 16 real samples, not the
    // discarded one -- channel 0's real value (1000) with gain=1/k_ct=1/
    // zero_counts=0 maps to amps = counts * 3.3 / 4096 / sqrt(2).
    current_snapshot_t snap;
    current_sense_get_snapshot(&snap);
    float expected_amps_ch0 = (1000.0f * 3.3f / 4096.0f) / 1.41421356f;
    TEST_CHECK(fabsf(snap.amps[0] - expected_amps_ch0) < 0.01f,
               "channel 0's reported amps reflects the 16 real samples only, "
               "not the discarded outlier (4095) -- if this fails, the "
               "discard-first-sample policy broke on the hal_adc path");
}

void run_test_current_sense_hal_adc(void)
{
    test_current_sense_discards_first_sample_and_oversamples_16x();
}
