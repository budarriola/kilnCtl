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
#include <string.h> /* strcmp() -- touch_cal_support_name()'s wire spellings */

#include "test_common.h"
#include "../drivers/hw/touch_dev.h"

/* A stand-in `read` function, only ever used as a non-NULL member: what
 * touch_dev_cal_support() checks is PRESENCE (a controller came up and
 * populated the struct), never the function's behaviour. A real function
 * rather than a cast integer so the pointer is genuinely valid. */
static esp_err_t fake_touch_read(void *ctx, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                                 uint16_t *out_z1)
{
    (void)ctx;
    if (out_pressed) *out_pressed = false;
    if (out_x) *out_x = 0;
    if (out_y) *out_y = 0;
    if (out_z1) *out_z1 = TOUCH_DEV_NO_PRESSURE_SENTINEL;
    return 0;
}

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

    /* --- touch_dev_map_uncalibrated: swap_xy with ASYMMETRIC maxes -------
     * Every other swap_xy check above uses symmetric maxes (4095, 4095) or
     * swap_xy=false, so a bug that fails to swap ax_max/ay_max along with
     * ax/ay (touch_dev.c:25-26) is invisible to them -- this is the negative
     * test an opus review found missing (J2). raw_x_max=479 (a 480-wide
     * panel's ceiling), raw_y_max=319 (a 320-tall panel's ceiling), swapped:
     * ax = raw_y (300 of true max 479... but ax_max must be raw_y_max=319,
     * not raw_x_max=479), landing on screen width 480. */
    {
        int32_t px = 0, py = 0;
        touch_dev_map_uncalibrated(100, 300, 479, 319, 480, 320, true, false, false, &px, &py);
        /* ax=raw_y=300 scaled against ax_max=raw_y_max=319 (the swapped
         * max), not against raw_x_max=479 -- if the swap-along-with-the-
         * axes step is dropped, this scales against 479 instead and the
         * two disagree (300 is not close to raw_max in either case, but
         * (300*479)/319=450 != (300*479)/479... use exact expected values
         * to avoid any ambiguity). */
        int32_t expect_px = touch_dev_axis_to_px(300, 319, 480, false);
        int32_t expect_py = touch_dev_axis_to_px(100, 479, 320, false);
        int32_t wrong_px = touch_dev_axis_to_px(300, 479, 480, false);
        TEST_CHECK(expect_px != wrong_px,
                   "sanity: asymmetric maxes actually change the expected px "
                   "(otherwise this test cannot distinguish swapped from unswapped maxes)");
        TEST_CHECK(px == expect_px,
                   "swap_xy with asymmetric maxes: px uses raw_y_max (the swapped max), "
                   "not raw_x_max -- negative test for touch_dev.c:25-26's max swap");
        TEST_CHECK(py == expect_py,
                   "swap_xy with asymmetric maxes: py uses raw_x_max (the swapped max), "
                   "not raw_y_max");
    }

    /* --- touch_dev_uncalibrated_max: the touch_read_cb() caller's own
     * raw_x_max/raw_y_max assignment (lvgl_port.c), pulled out here so it is
     * host-testable -- this is what an opus review found wrong (J1): the
     * caller assigned POST-swap screen extents (raw_x_max = width-1
     * unconditionally) when touch_dev_map_uncalibrated's raw_x_max/
     * raw_y_max contract wants each axis's PRE-swap ceiling. */
    {
        uint16_t x_max = 0, y_max = 0;

        /* Not self-calibrating: always symmetric NS2009 maxes, swap_xy
         * irrelevant. */
        touch_dev_uncalibrated_max(false, true, 480, 320, 4095, &x_max, &y_max);
        TEST_CHECK(x_max == 4095 && y_max == 4095,
                   "not self-calibrating: both maxes are ns2009_adc_max regardless of swap_xy");

        /* Self-calibrating, no swap: maxes are the panel's own extents,
         * unswapped (width-1/height-1) -- the case that was already correct
         * and must stay correct. */
        touch_dev_uncalibrated_max(true, false, 480, 320, 4095, &x_max, &y_max);
        TEST_CHECK(x_max == 479 && y_max == 319,
                   "self-calibrating, no swap: raw_x_max=width-1, raw_y_max=height-1");

        /* Self-calibrating, swap_xy set -- this is J1: raw_x's native range
         * runs along the screen's height, not its width. A regression back
         * to the width/height-only assignment reddens this exact check. */
        touch_dev_uncalibrated_max(true, true, 480, 320, 4095, &x_max, &y_max);
        TEST_CHECK(x_max == 319 && y_max == 479,
                   "self-calibrating, swap_xy set: raw_x_max=height-1, raw_y_max=width-1 "
                   "(J1 -- was width-1/height-1, the wrong way round for the swapped case)");
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
        /* resistive_uncal and resistive_cal are identical structs -- the
         * only thing touch_dev_use_calibrated_fit() looks at is
         * self_calibrating, and the "no fit loaded" vs. "fit loaded"
         * distinction between these two cases lives entirely in the second
         * (touch_cal_calibrated) ARGUMENT below, not in the struct. Kept as
         * two separately-named locals (rather than one reused variable) so
         * each TEST_CHECK below reads as its own named scenario. */
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

    /* TOUCH_DEV_NO_PRESSURE_SENTINEL's two "sentinel != 0" / "sentinel >
     * 4095u" checks used to live here, but both are compile-time arithmetic
     * on a #define (0xFFFFu) -- the compiler folds them to a constant 1,
     * they cannot fail for any code change to this file or touch_dev.h, and
     * deleted rather than kept as dead weight (opus review, J3). Nothing
     * here or elsewhere host-tests that FT6336U_read() (FT6336U.c) actually
     * WRITES this sentinel into *out_z1 when out_z1 is non-NULL --
     * FT6336U.c is not in this host-test build at all (it talks directly to
     * ESP-IDF's i2c_master_bus_handle_t, unlike touch_dev.c's pure math),
     * so that producer side is genuinely untested off-target today. */

    /* --- touch_dev_cal_support: the shared "is calibration offerable?"
     * predicate every surface consults (the LCD config hub's nav cell,
     * kiln_ui.c's boot gate, ui_page_touch_cal_build()'s refusal, and
     * /api/status's touch_cal_supported field). Those surfaces themselves
     * cannot be host-tested -- the UI files pull in LVGL and the HTTP
     * handlers are target-build-only -- which is exactly why the decision
     * was factored down into this pure function: this is the only place it
     * CAN be covered, so all three states plus NULL are covered here. */
    {
        /* A present resistive controller: `read` non-NULL is what "a
         * controller actually came up" means (lvgl_port.c's touch_dev
         * comment), self_calibrating false means it needs the per-board
         * affine fit. The one case that may offer calibration. */
        touch_dev_t resistive = { .read = fake_touch_read, .self_calibrating = false };
        touch_dev_t capacitive = { .read = fake_touch_read, .self_calibrating = true };
        /* A zeroed struct -- precisely what main_boot_early.c leaves behind
         * when FT6336U_start()/NS2009_start() fails. */
        touch_dev_t absent = { 0 };

        TEST_CHECK(touch_dev_cal_support(&resistive) == TOUCH_CAL_SUPPORT_SUPPORTED,
                   "present resistive controller -> SUPPORTED");
        TEST_CHECK(touch_cal_support_is_offerable(touch_dev_cal_support(&resistive)),
                   "present resistive controller -> calibration IS offered (the supported case)");

        TEST_CHECK(touch_dev_cal_support(&capacitive) == TOUCH_CAL_SUPPORT_SELF_CALIBRATING,
                   "present self-calibrating controller -> SELF_CALIBRATING");
        TEST_CHECK(!touch_cal_support_is_offerable(touch_dev_cal_support(&capacitive)),
                   "self-calibrating controller -> calibration is NOT offered (the bench unit's "
                   "own FT6336U panel -- the motivating case)");

        /* THE ORDERING CHECK. A zeroed touch_dev_t has self_calibrating ==
         * false, so a predicate that tested that flag before testing
         * presence would answer SUPPORTED here -- sending a board whose
         * touch bring-up failed into a 3x3 grid it cannot register a single
         * tap on. Reversing those two tests in touch_dev.c reddens exactly
         * this check and the is_offerable one below it. */
        TEST_CHECK(touch_dev_cal_support(&absent) == TOUCH_CAL_SUPPORT_NO_TOUCH,
                   "zeroed touch_dev_t (failed/absent bring-up) -> NO_TOUCH, NOT SUPPORTED -- "
                   "presence is tested before self_calibrating");
        TEST_CHECK(!touch_cal_support_is_offerable(touch_dev_cal_support(&absent)),
                   "no touch controller -> calibration is NOT offered (a grid nothing can tap)");
        /* ...and it must stay DISTINGUISHABLE from the self-calibrating
         * case, not merely equally-unsupported: an undetected controller is
         * an unknown, and must never be reported as a confident "no
         * calibration needed". */
        TEST_CHECK(touch_dev_cal_support(&absent) != touch_dev_cal_support(&capacitive),
                   "NO_TOUCH and SELF_CALIBRATING are distinct states, so a bring-up failure "
                   "cannot be shown as 'self-calibrates, none needed'");

        TEST_CHECK(touch_dev_cal_support(NULL) == TOUCH_CAL_SUPPORT_NO_TOUCH,
                   "NULL device -> NO_TOUCH, not a crash");
    }

    /* --- touch_cal_support_name: the wire spelling /api/status emits.
     * Pinned because diagnostics_page.html compares against these exact
     * strings -- a rename on either side silently reverts that page to the
     * old "NOT CALIBRATED" message for every panel. */
    {
        TEST_CHECK(strcmp(touch_cal_support_name(TOUCH_CAL_SUPPORT_SUPPORTED), "supported") == 0,
                   "wire name: SUPPORTED -> \"supported\"");
        TEST_CHECK(strcmp(touch_cal_support_name(TOUCH_CAL_SUPPORT_SELF_CALIBRATING),
                          "self_calibrating") == 0,
                   "wire name: SELF_CALIBRATING -> \"self_calibrating\"");
        TEST_CHECK(strcmp(touch_cal_support_name(TOUCH_CAL_SUPPORT_NO_TOUCH), "no_touch") == 0,
                   "wire name: NO_TOUCH -> \"no_touch\"");
        TEST_CHECK(touch_cal_support_name((touch_cal_support_t)99) != NULL,
                   "out-of-range value still returns a printable string, never NULL");
    }
}
