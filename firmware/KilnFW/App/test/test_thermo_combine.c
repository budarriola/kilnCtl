#include "test_common.h"
#include "../drivers/thermo_combine.h"

void run_test_thermo_combine(void)
{
    TEST_SECTION("thermo_combine");

    /* All channels in the mask valid: plain arithmetic mean, no drops. */
    {
        float c[3] = {100.0f, 200.0f, 300.0f};
        bool ok[3] = {true, true, true};
        bool valid = false;
        float m = thermo_combine(c, ok, 3, 0x07u, &valid);
        TEST_CHECK(valid, "all-valid channels -> out_valid true");
        TEST_CHECK_NEAR(m, 200.0f, 1e-5, "all-valid channels -> mean of 100/200/300 == 200");
    }

    /* One masked-in channel faulted: dropped from the average, not zeroed --
     * mean of the two survivors, not (100+200+0)/3. */
    {
        float c[3] = {100.0f, 200.0f, 9999.0f}; /* channel 2's reading is garbage; channel_ok says so */
        bool ok[3] = {true, true, false};
        bool valid = false;
        float m = thermo_combine(c, ok, 3, 0x07u, &valid);
        TEST_CHECK(valid, "one faulted channel among three -> still valid (2 survivors)");
        TEST_CHECK_NEAR(m, 150.0f, 1e-5, "faulted channel dropped, not zeroed -> mean(100,200) == 150, not skewed by the 9999 reading");
    }

    /* Every masked-in channel faulted: zero valid readings -> NaN + out_valid=false,
     * the same "thermocouple invalid" case a single-channel zone has always had. */
    {
        float c[3] = {100.0f, 200.0f, 300.0f};
        bool ok[3] = {false, false, false};
        bool valid = true; /* deliberately pre-seeded true, so the check below proves thermo_combine() clears it */
        float m = thermo_combine(c, ok, 3, 0x07u, &valid);
        TEST_CHECK(!valid, "all masked-in channels faulted -> out_valid false");
        TEST_CHECK(isnan(m), "all masked-in channels faulted -> NaN return, mirrors zones_config_apply_cal()'s convention");
    }

    /* Mask excludes a channel that is otherwise perfectly valid: it must
     * never be silently pulled into the average just because channel_ok[]
     * says it's fine -- only bits actually set in thermo_mask may
     * contribute. */
    {
        float c[3] = {100.0f, 200.0f, 300.0f};
        bool ok[3] = {true, true, true};
        bool valid = false;
        /* mask = 0x03 -> channels 0 and 1 only; channel 2 (valid, 300.0f)
         * is NOT in the mask and must not affect the result. */
        float m = thermo_combine(c, ok, 3, 0x03u, &valid);
        TEST_CHECK(valid, "two masked-in channels, both valid -> out_valid true");
        TEST_CHECK_NEAR(m, 150.0f, 1e-5, "unmasked channel 2 (300.0f, channel_ok=true) excluded -> mean(100,200) == 150, not 200");
    }

    /* Mask names a channel that is itself faulted, alongside a masked-out
     * channel that happens to be valid: confirms the mask AND the validity
     * flag are both required, independently, not one substituting for the
     * other. */
    {
        float c[3] = {100.0f, 9999.0f, 300.0f};
        bool ok[3] = {true, false, true};
        bool valid = false;
        /* mask = 0x03 -> channels 0 and 1; channel 1 is masked-in but
         * faulted (dropped), channel 2 is masked-out (excluded regardless
         * of its own validity) -- only channel 0 should survive. */
        float m = thermo_combine(c, ok, 3, 0x03u, &valid);
        TEST_CHECK(valid, "one masked-in-but-faulted channel plus one masked-out-but-valid channel -> the remaining single masked-in-and-valid channel still counts");
        TEST_CHECK_NEAR(m, 100.0f, 1e-5, "only channel 0 contributes: channel 1 faulted (dropped), channel 2 unmasked (excluded)");
    }

    /* Bits at or past channel_count are ignored, not treated as an error --
     * the header's documented contract ("validating a mask against the
     * current channel count is the config layer's job, not this pure
     * function's"). */
    {
        float c[2] = {100.0f, 200.0f};
        bool ok[2] = {true, true};
        bool valid = false;
        /* mask bit 2 (0x04) is past channel_count=2; must be silently
         * ignored rather than reading out of bounds or erroring. */
        float m = thermo_combine(c, ok, 2, 0x07u, &valid);
        TEST_CHECK(valid, "mask bits past channel_count ignored -> still valid from the in-range bits");
        TEST_CHECK_NEAR(m, 150.0f, 1e-5, "mask bits past channel_count ignored -> mean(100,200) == 150");
    }
}
