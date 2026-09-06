// test_current_sense_hal_adc.c -- HAL Phase 1b (docs/HW_ABSTRACTION_PLAN.md
// "hal_adc -- pico-only" section): current_sense.c is now a hal_adc client.
// This is the host-testable oversample/discard-first-sample property the
// plan promises: current_sense.c itself must keep that policy (hal_adc.h's
// contract is "one raw sample, no filtering"), and this test proves it by
// driving current_sense_sample() for real against
// firmware/hwAbstraction/host/fake_adc.c and checking, from fake_adc's own
// event/read counters, that:
//   1. exactly (1 + CURRENT_SENSE_OVERSAMPLE_N) hal_adc_read_raw() calls
//      happen per channel per sample pass (1 discarded + N averaged)
//   2. the discarded first sample is NOT included in the reported average
//      (proven by scripting a first sample far outside the rest of the
//      series and checking the reported amps/counts reflect the other N
//      only)
//   3. scripted samples use realistic 12-bit quantization (0..4095)
//
// This test uses current_sense.h's own CURRENT_SENSE_OVERSAMPLE_N constant
// throughout -- NOT a test-local mirror -- so there is nothing here to
// silently drift out of sync with the real oversample factor
// cs_read_channel_counts() (current_sense.c) actually uses; a change to
// the real constant changes what this test builds and checks too, in one
// place.
#include "test_common.h"

#include "fake_adc.h"
#include "hal_adc.h"

#include "../src/board/board_pins.h"
#include "../src/current_sense.h"
#include "../src/tasks/current_task.h"

// All-identical scripted samples were the original (weaker) version of this
// test's series: current_sense.c averages with plain integer division
// (uint32_t sum / CS_OVERSAMPLE_N -- see current_sense.c's cs_read_channel_
// counts()), and an identical-16 series can't distinguish "summed all 16
// correctly" from an off-by-one denominator/accumulator bug that happens to
// still land on the same repeated value. A dithered series with a
// non-integral TRUE mean (base + 0..3 repeating, true mean base+1.5)
// exercises the real integer-truncation behavior and gives a single exact
// expected average this test computes independently, so any accumulation
// bug that lands on the wrong integer is caught.
static uint32_t script_channel_dithered(int channel, uint16_t discard_value, uint16_t base)
{
    // First sample: the one current_sense.c must discard. Scripted far away
    // from the real series so an accidental inclusion in the average is
    // visible.
    uint16_t discard[1] = { discard_value };
    TEST_CHECK(fake_adc_script_samples(channel, discard, 1) == HAL_OK,
               "scripting the discard-first sample succeeds");

    // 16 dithered, realistic 12-bit samples: base + {0,1,2,3} repeating.
    // Sum of the dither term alone is 4*(0+1+2+3) = 24, so the true mean is
    // base + 24/16 = base + 1.5 -- non-integral, unlike a flat series.
    uint16_t reals[CURRENT_SENSE_OVERSAMPLE_N];
    uint32_t sum = 0;
    for (unsigned i = 0; i < CURRENT_SENSE_OVERSAMPLE_N; i++) {
        uint16_t v = (uint16_t)(base + (i % 4));
        reals[i] = v;
        sum += v;
    }
    TEST_CHECK(fake_adc_script_samples(channel, reals, CURRENT_SENSE_OVERSAMPLE_N) == HAL_OK,
               "scripting the 16 dithered oversampled reads succeeds");

    // current_sense.c's own integer-truncating average (sum / N), computed
    // independently here so the assertion doesn't just restate the
    // production formula.
    return sum / CURRENT_SENSE_OVERSAMPLE_N;
}

// counts -> amps with gain=1/k_ct=1/zero_counts=0 (this test's calibration):
// amps = counts * 3.3 / 4096 / sqrt(2).
static float counts_to_amps(uint32_t counts)
{
    return ((float)counts * 3.3f / 4096.0f) / 1.41421356f;
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

    // Commission all three channels with a trivial 1:1 counts->amps mapping
    // (gain=1, k_ct=1, zero_counts=0) so each reported amps figure directly
    // exposes exactly what counts value that channel's average resolved to.
    current_sense_cal_t cal = {0};
    for (unsigned n = 0; n < 3; n++) {
        cal.gain[n] = 1.0f;
        cal.k_ct_v_per_a[n] = 1.0f;
        cal.zero_counts[n] = 0;
    }
    cal.i_present_a = 0.0f;
    cal.calibrated = true;
    current_sense_set_cal(&cal);

    // Realistic 12-bit quantized dithered series (0..4095), one discard
    // value far from each channel's real series so inclusion would visibly
    // skew the average. Different bases per channel so a channel mix-up
    // (e.g. channel 1 reading channel 2's script) is also visible.
    uint32_t expected_counts[3];
    expected_counts[0] = script_channel_dithered(0, /*discard=*/4095, /*base=*/1000);
    expected_counts[1] = script_channel_dithered(1, /*discard=*/0,    /*base=*/500);
    expected_counts[2] = script_channel_dithered(2, /*discard=*/2047, /*base=*/250);

    current_sense_sample();

    // 1 discard + CURRENT_SENSE_OVERSAMPLE_N oversampled reads per channel,
    // 3 channels total. This is current_sense.h's real, public constant
    // (not a test-local mirror -- see the file header comment), so this
    // assertion is checking real production behavior against itself, not a
    // separately-typed copy that could silently go stale.
    TEST_CHECK(fake_adc_read_count() == 3u * (1u + CURRENT_SENSE_OVERSAMPLE_N),
               "exactly (1 discard + CURRENT_SENSE_OVERSAMPLE_N oversample) x "
               "3 channels reads happened");

    // Every scripted sample was consumed (none left pending) -- proves the
    // discard sample was actually read, not skipped/left in the queue.
    TEST_CHECK(fake_adc_pending_count(0) == 0, "channel 0's script is fully consumed");
    TEST_CHECK(fake_adc_pending_count(1) == 0, "channel 1's script is fully consumed");
    TEST_CHECK(fake_adc_pending_count(2) == 0, "channel 2's script is fully consumed");

    // The averaged counts must reflect ONLY the 16 dithered real samples,
    // not the discarded outlier, and must match the exact integer-truncated
    // average this test computed independently -- not just "close to the
    // base value", which an all-identical series could pass with a broken
    // accumulator.
    current_snapshot_t snap;
    current_sense_get_snapshot(&snap);
    for (unsigned n = 0; n < 3; n++) {
        float expected_amps = counts_to_amps(expected_counts[n]);
        // Tolerance must be tighter than one whole count's worth of amps
        // (counts_to_amps(1) ~= 5.7e-4 A here) -- this dithered series'
        // true average is base+1.5, truncating to base+1, so a broken
        // accumulator that instead reports the flat base value (base+0)
        // differs from the correct answer by exactly one count, ~5.7e-4 A.
        // The previous 0.001f tolerance was wider than that whole-count gap
        // and could not tell the two apart; 2e-4 is under a third of it.
        TEST_CHECK(fabsf(snap.amps[n] - expected_amps) < 0.0002f,
                   "channel's reported amps matches the exact dithered-average "
                   "counts, not the discarded outlier and not just the flat base "
                   "value -- if this fails, the discard-first-sample or the "
                   "oversample accumulation broke on the hal_adc path");
    }
}

// current_task_start() (Phase 6/HAL Phase 1b) is the ONLY place hal_adc_
// gpio_enable() is called for ADC0/1/2 -- current_sense.c itself never
// calls it (see current_task.c's own comment). Without this check, a
// deleted or mis-wired hal_adc_gpio_enable() call would leave those three
// GPIOs on their digital function, silently producing garbage ADC readings
// on real hardware while every current_sense.c-only test above stays green
// (fake_adc doesn't need pins "enabled" to serve scripted samples, so it
// can't catch this by itself).
static void test_current_task_start_enables_all_three_adc_gpios(void)
{
    TEST_SECTION("current_task_start() -- hal_adc_gpio_enable() called for "
                 "ADC0/1/2 (HAL Phase 1b)");

    fake_adc_reset();

    TEST_CHECK(!fake_adc_is_gpio_enabled(SAFTYFW_PIN_ADC0_GPIO),
               "ADC0's GPIO starts disabled (fresh fake_adc_reset())");
    TEST_CHECK(!fake_adc_is_gpio_enabled(SAFTYFW_PIN_ADC1_GPIO),
               "ADC1's GPIO starts disabled (fresh fake_adc_reset())");
    TEST_CHECK(!fake_adc_is_gpio_enabled(SAFTYFW_PIN_ADC2_GPIO),
               "ADC2's GPIO starts disabled (fresh fake_adc_reset())");

    bool ok = current_task_start();
    TEST_CHECK(ok, "current_task_start() succeeds against the host stubs");

    TEST_CHECK(fake_adc_is_gpio_enabled(SAFTYFW_PIN_ADC0_GPIO),
               "ADC0's GPIO is enabled after current_task_start()");
    TEST_CHECK(fake_adc_is_gpio_enabled(SAFTYFW_PIN_ADC1_GPIO),
               "ADC1's GPIO is enabled after current_task_start()");
    TEST_CHECK(fake_adc_is_gpio_enabled(SAFTYFW_PIN_ADC2_GPIO),
               "ADC2's GPIO is enabled after current_task_start()");
}

void run_test_current_sense_hal_adc(void)
{
    test_current_sense_discards_first_sample_and_oversamples_16x();
    test_current_task_start_enables_all_three_adc_gpios();
}
