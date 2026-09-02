// Host tests for touch_dev.c -- DISPLAY_ST7796_PLAN.md section 7 (Phase 5):
// the pure math split out of lvgl_port.c's touch_read_cb() so it can be
// exercised off-target. Three things are covered:
//   1. touch_dev_axis_to_px(): the raw-count -> panel-pixel scaling, both in
//      its old NS2009-ADC-range shape and in the identity-plus-invert shape
//      a self-calibrating controller's already-panel-space sample needs.
//   2. touch_dev_map_uncalibrated(): the swap/invert axis mapping shared by
//      NS2009's bootstrap fallback and any self-calibrating controller.
//   3. touch_dev_use_calibrated_fit(): the self_calibrating/calibrated
//      branch selection itself -- the one decision the whole Phase 5 design
//      hinges on (touch_dev.h's header comment).
#include "test_common.h"
#include "../drivers/touch_dev.h"

void run_test_touch_dev(void)
{
    TEST_SECTION("touch_dev");

    /* --- touch_dev_axis_to_px: NS2009-shaped (raw_max = 4095) ------------ */
    {
        /* Old touch_raw_to_px()'s exact formula, reproduced here as a fixed
         * expected value: raw=2048 of 4095 onto a 480-wide panel, no
         * invert. (2048*479)/4095 = 239 (integer division). */
        int32_t px = touch_dev_axis_to_px(2048, 4095, 480, false);
        TEST_CHECK(px == 239, "NS2009-range midpoint raw maps to mid-panel px (no invert)");
    }
    {
        /* raw=0 -> px=0 with no invert; raw=0 -> px=(extent-1) WITH invert.
         * This is the negative test for the invert branch: flip invert and
         * watch the expected output flip too, not stay the same by
         * accident. */
        int32_t px_noinvert = touch_dev_axis_to_px(0, 4095, 480, false);
        int32_t px_invert = touch_dev_axis_to_px(0, 4095, 480, true);
        TEST_CHECK(px_noinvert == 0, "raw=0, no invert -> px=0");
        TEST_CHECK(px_invert == 479, "raw=0, invert -> px=panel_extent-1 (479)");
    }
    {
        /* Out-of-range raw (above raw_max) clamps rather than wrapping or
         * overflowing -- same clamp the old touch_raw_to_px() had. */
        int32_t px = touch_dev_axis_to_px(9999, 4095, 480, false);
        TEST_CHECK(px == 479, "raw above raw_max clamps to panel_extent-1");
    }

    /* --- touch_dev_axis_to_px: self-calibrating-shaped (raw_max = extent-1)
     * This is the identity-plus-invert-plus-clamp case a self-calibrating
     * controller's already-panel-space sample needs -- raw_max equal to
     * panel_extent-1 makes the scale step a no-op. */
    {
        int32_t px = touch_dev_axis_to_px(240, 479, 480, false);
        TEST_CHECK(px == 240, "self-calibrating shape (raw_max=extent-1): identity, no invert");

        int32_t px_inv = touch_dev_axis_to_px(240, 479, 480, true);
        TEST_CHECK(px_inv == 239, "self-calibrating shape: identity then invert (479-240=239)");
    }

    /* --- touch_dev_map_uncalibrated: swap_xy ------------------------------
     * Same raw pair, only swap_xy flipped -- proves the flag actually swaps
     * which raw axis lands on which screen axis, not a no-op left over from
     * a copy-paste. */
    {
        int32_t px = 0, py = 0;
        touch_dev_map_uncalibrated(100, 300, 4095, 4095, 480, 320, false, false, false, &px, &py);
        int32_t px_swapped = 0, py_swapped = 0;
        touch_dev_map_uncalibrated(100, 300, 4095, 4095, 480, 320, true, false, false, &px_swapped,
                                    &py_swapped);
        TEST_CHECK(px != px_swapped || py != py_swapped,
                   "swap_xy actually changes the mapped point (negative test for a no-op swap)");
        /* Specifically: swapped run's px should track raw_y's scaling onto
         * width, and py should track raw_x's scaling onto height. */
        int32_t expect_px_swapped = touch_dev_axis_to_px(300, 4095, 480, false);
        int32_t expect_py_swapped = touch_dev_axis_to_px(100, 4095, 320, false);
        TEST_CHECK(px_swapped == expect_px_swapped, "swap_xy: px comes from raw_y");
        TEST_CHECK(py_swapped == expect_py_swapped, "swap_xy: py comes from raw_x");
    }

    /* --- touch_dev_map_uncalibrated: invert_x/invert_y independently ----- */
    {
        int32_t px = 0, py = 0;
        touch_dev_map_uncalibrated(0, 0, 479, 319, 480, 320, false, true, false, &px, &py);
        TEST_CHECK(px == 479, "invert_x alone: raw_x=0 -> px=width-1");
        TEST_CHECK(py == 0, "invert_x alone: invert_y unset -> py unaffected");
    }

    /* --- touch_dev_use_calibrated_fit: the self_calibrating/calibrated
     * branch selection itself. Four combinations, each meaningfully
     * different -- this is the decision touch_dev.h's header comment says
     * is "the whole point" of the self_calibrating flag, so every quadrant
     * gets its own check rather than one combined assertion that could pass
     * for the wrong reason. */
    {
        touch_dev_t resistive_uncal = { .self_calibrating = false };
        touch_dev_t resistive_cal = { .self_calibrating = false };
        touch_dev_t capacitive = { .self_calibrating = true };

        TEST_CHECK(!touch_dev_use_calibrated_fit(&resistive_uncal, false),
                   "resistive, no fit loaded -> uncalibrated fallback (not the affine fit)");
        TEST_CHECK(touch_dev_use_calibrated_fit(&resistive_cal, true),
                   "resistive, fit loaded -> the real per-board affine fit");
        TEST_CHECK(!touch_dev_use_calibrated_fit(&capacitive, true),
                   "self-calibrating device -> NEVER the affine fit, even if touch_cal_store "
                   "happens to hold a stale fit from a previously-attached controller");
        TEST_CHECK(!touch_dev_use_calibrated_fit(&capacitive, false),
                   "self-calibrating device, no fit either -> still not the affine fit "
                   "(same answer as the previous case, for a different reason)");
        TEST_CHECK(!touch_dev_use_calibrated_fit(NULL, true), "NULL device -> false, not a crash");
    }

    /* --- TOUCH_DEV_NO_PRESSURE_SENTINEL: out of any real 12-bit ADC range,
     * so a caller can tell "no pressure channel" apart from "a real zero
     * reading" -- negative test proves it is NOT just zero. */
    {
        TEST_CHECK(TOUCH_DEV_NO_PRESSURE_SENTINEL != 0,
                   "no-pressure sentinel is not the same value as a real zero reading");
        TEST_CHECK(TOUCH_DEV_NO_PRESSURE_SENTINEL > 4095u,
                   "no-pressure sentinel is out of NS2009's real 12-bit ADC range");
    }
}
