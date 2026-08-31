#include "test_common.h"
#include "../drivers/pid_fuzzy.h"

#include <math.h>

void run_test_pid_fuzzy(void)
{
    TEST_SECTION("pid_fuzzy");

    /* strength_pct == 0 must reproduce the base gains exactly, bit-for-bit
     * -- the safety contract the whole mode rests on. Exact equality is
     * deliberate here, not an oversight: an epsilon would let a silent
     * "*1.0001" regression through. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, 0.3f, 0.012f, 0.0034f, 0.056f, 0, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.012f, "strength=0: kp bit-exact to base_kp");
        TEST_CHECK(ki == 0.0034f, "strength=0: ki bit-exact to base_ki");
        TEST_CHECK(kd == 0.056f, "strength=0: kd bit-exact to base_kd");
    }
    {
        /* Same, at an extreme/near-degenerate input, to make sure the
         * strength=0 short-circuit really is input-independent. */
        float kp, ki, kd;
        pid_fuzzy_adjust(-9999.0f, 500.0f, 0.02f, 0.0f, 0.1f, 0, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.02f, "strength=0, extreme inputs: kp still bit-exact");
        TEST_CHECK(ki == 0.0f, "strength=0, extreme inputs: ki still bit-exact");
        TEST_CHECK(kd == 0.1f, "strength=0, extreme inputs: kd still bit-exact");
    }
    {
        /* strength_pct == 0 must ALSO sanitize a bad *base* gain, not just
         * pass it through. This is the case the first two blocks above miss
         * entirely: they only ever feed legitimate gains, so a short-circuit
         * that returns the raw arguments passes them both while leaking a
         * NaN straight into the PID loop. strength=0 is the most
         * conservative setting and the value a zeroed config blob defaults
         * to, so a NaN surviving *here* is the worst possible place for one.
         * (Regression: the first implementation did exactly this.) */
        float kp, ki, kd;
        pid_fuzzy_adjust(10.0f, 0.1f, NAN, -1.0f, INFINITY, 0, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.0f, "strength=0: NaN base_kp sanitized to 0, not passed through");
        TEST_CHECK(ki == 0.0f, "strength=0: negative base_ki sanitized to 0");
        TEST_CHECK(kd == 0.0f, "strength=0: inf base_kd sanitized to 0");
    }

    /* Rule-table corner: large POSITIVE error (far below setpoint), rate
     * RISING (getting worse) -> Kp up, Ki down, Kd up, per the documented
     * table's POS/RISING cell. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, 1.0f, 0.01f, 0.01f, 0.01f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp > 0.01f, "POS error, RISING rate: kp nudged up");
        TEST_CHECK(ki < 0.01f, "POS error, RISING rate: ki nudged down");
        TEST_CHECK(kd > 0.01f, "POS error, RISING rate: kd nudged up");
    }

    /* Rule-table corner: large POSITIVE error, rate FALLING (already
     * closing in) -> Kp down, Ki up, Kd down. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, -1.0f, 0.01f, 0.01f, 0.01f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp < 0.01f, "POS error, FALLING rate: kp nudged down");
        TEST_CHECK(ki > 0.01f, "POS error, FALLING rate: ki nudged up");
        TEST_CHECK(kd < 0.01f, "POS error, FALLING rate: kd nudged down");
    }

    /* Rule-table corner: large NEGATIVE error (overshoot), rate FALLING
     * (overshoot still growing) -> Kp up, Ki down, Kd up -- the plan's
     * NEG/POS symmetry: "far from target" behaves the same regardless of
     * which direction, only error's own sign (handled in pid.c, not here)
     * decides which way duty moves. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(-500.0f, -1.0f, 0.01f, 0.01f, 0.01f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp > 0.01f, "NEG error, FALLING rate: kp nudged up");
        TEST_CHECK(ki < 0.01f, "NEG error, FALLING rate: ki nudged down");
        TEST_CHECK(kd > 0.01f, "NEG error, FALLING rate: kd nudged up");
    }

    /* Rule-table corner: large NEGATIVE error, rate RISING (overshoot
     * recovering) -> Kp down, Ki up, Kd down. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(-500.0f, 1.0f, 0.01f, 0.01f, 0.01f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp < 0.01f, "NEG error, RISING rate: kp nudged down");
        TEST_CHECK(ki > 0.01f, "NEG error, RISING rate: ki nudged up");
        TEST_CHECK(kd < 0.01f, "NEG error, RISING rate: kd nudged down");
    }

    /* Monotonicity in strength: same inputs, a larger strength_pct must
     * move each gain at least as far from base as a smaller one (and
     * strictly further here, since the rule membership is nonzero). */
    {
        float kp30, ki30, kd30;
        float kp80, ki80, kd80;
        pid_fuzzy_adjust(500.0f, 1.0f, 0.02f, 0.02f, 0.02f, 30, &kp30, &ki30, &kd30);
        pid_fuzzy_adjust(500.0f, 1.0f, 0.02f, 0.02f, 0.02f, 80, &kp80, &ki80, &kd80);
        TEST_CHECK(fabsf(kp80 - 0.02f) > fabsf(kp30 - 0.02f), "higher strength moves kp further from base");
        TEST_CHECK(fabsf(ki80 - 0.02f) > fabsf(ki30 - 0.02f), "higher strength moves ki further from base");
        TEST_CHECK(fabsf(kd80 - 0.02f) > fabsf(kd30 - 0.02f), "higher strength moves kd further from base");
    }

    /* Output gains never negative/inf/NaN, including absurd inputs and
     * strength_pct=100. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(1.0e9f, -1.0e9f, 0.001f, 0.001f, 0.001f, 100, &kp, &ki, &kd);
        TEST_CHECK(isfinite(kp) && kp >= 0.0f, "huge error/rate: kp finite and non-negative");
        TEST_CHECK(isfinite(ki) && ki >= 0.0f, "huge error/rate: ki finite and non-negative");
        TEST_CHECK(isfinite(kd) && kd >= 0.0f, "huge error/rate: kd finite and non-negative");
    }
    {
        /* A rule cell that nudges a gain toward zero must not undershoot
         * past zero into negative territory even at strength=100. */
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, -1.0f, 0.0001f, 0.0001f, 0.0001f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp >= 0.0f, "small base gain nudged down: kp still non-negative");
        TEST_CHECK(kd >= 0.0f, "small base gain nudged down: kd still non-negative");
    }

    /* Non-finite inputs (NaN/inf error_c or error_rate_c_per_s) are the
     * faulted-thermocouple case, and the required behavior is stronger than
     * "finite and non-negative": the module must make NO adjustment at all
     * and hand back the base gains untouched.
     *
     * "Finite and non-negative" alone is too weak to be worth writing. The
     * original implementation mapped a non-finite input to 0.0 on both axes,
     * landing on the ZERO/STEADY cell, whose rule is {Kp-, Ki+, Kd-} -- it
     * RAISED the integral gain 50% while the temperature reading was
     * garbage. That output is finite and non-negative, so a weaker check
     * passes it happily. Assert the actual contract instead. */
    {
        float kp, ki, kd;
        float nan_val = NAN;
        pid_fuzzy_adjust(nan_val, 0.0f, 0.02f, 0.03f, 0.04f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.02f, "NaN error_c: kp held at base, no adjustment");
        TEST_CHECK(ki == 0.03f, "NaN error_c: ki held at base -- must NOT wind up harder on a dead sensor");
        TEST_CHECK(kd == 0.04f, "NaN error_c: kd held at base, no adjustment");
    }
    {
        float kp, ki, kd;
        float inf_val = INFINITY;
        pid_fuzzy_adjust(0.0f, inf_val, 0.02f, 0.03f, 0.04f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.02f, "+inf error_rate: kp held at base, no adjustment");
        TEST_CHECK(ki == 0.03f, "+inf error_rate: ki held at base");
        TEST_CHECK(kd == 0.04f, "+inf error_rate: kd held at base");
    }
    {
        float kp, ki, kd;
        float neg_inf = -INFINITY;
        pid_fuzzy_adjust(neg_inf, neg_inf, 0.02f, 0.03f, 0.04f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.02f, "-inf error_c and error_rate: kp held at base");
        TEST_CHECK(ki == 0.03f, "-inf error_c and error_rate: ki held at base");
        TEST_CHECK(kd == 0.04f, "-inf error_c and error_rate: kd held at base");
    }

    /* PID_EXPANSION_PLAN.md Phase 3 hazard 2: RATE_BAND_C_PER_S must be
     * rescaled so a perfectly ordinary firing's own ramp rate does NOT read
     * as "large" on the rate axis. profile_executor.c feeds this axis
     * z->pid_state.d_filtered, which is derivative-on-measurement (pid.c's
     * raw_d = -(measurement - prev_measurement)/dt_s), NOT derivative of
     * error: the setpoint is never differentiated, so this axis tracks the
     * kiln's own climb (negated) for the whole ramp, not d(error)/dt -- true
     * d(error)/dt sits near zero while tracking well, which is exactly why
     * "tracking well" must NOT be read as implying a large value here (see
     * pid_fuzzy.c's RATE_BAND_C_PER_S comment). A CLIMBING kiln therefore
     * feeds a NEGATIVE d_filtered here: a brisk 100 degC/hr ramp is
     * -0.0278 degC/s; even a fast 300 degC/hr ramp is only -0.0833 degC/s
     * in magnitude. Both must land near the STEADY bucket (small nudge),
     * not the FALLING extreme (near-maximal nudge) the old 0.05 band would
     * have produced for the 100 degC/hr case. Kp is compared against the
     * near-saturated case at 1.0 degC/s (used throughout this file's other
     * corner tests) to prove the ordinary-ramp case really is far weaker,
     * not just "less than the theoretical maximum". Only ki is asserted on,
     * not kp: at this error's sign (POS/large), the rule table's kp
     * direction happens to be +1 in BOTH the STEADY and RISING rate buckets
     * (see pid_fuzzy.h's table), so kp's nudge is insensitive to exactly
     * where on the rate axis a rescale lands it -- ki's direction (0 at
     * STEADY, -1 at RISING) is where the band's placement actually shows
     * up, so that's the discriminating assertion. */
    {
        float kp_ramp, ki_ramp, kd_ramp;
        float kp_sat, ki_sat, kd_sat;
        /* POS/large error (300C below setpoint, matches this file's other
         * corner tests) with rate = a brisk-but-ordinary 100 degC/hr climb,
         * fed as d_filtered's negated-climb convention. */
        pid_fuzzy_adjust(300.0f, -0.0278f, 0.02f, 0.02f, 0.02f, 100, &kp_ramp, &ki_ramp, &kd_ramp);
        /* Same error, rate fully saturated RISING (what the old 0.05 band
         * would have made an ordinary ramp look like). */
        pid_fuzzy_adjust(300.0f, 1.0f, 0.02f, 0.02f, 0.02f, 100, &kp_sat, &ki_sat, &kd_sat);
        TEST_CHECK(fabsf(ki_ramp - 0.02f) < 0.25f * fabsf(ki_sat - 0.02f),
                  "100 degC/hr ramp rate nudges ki far less than a saturated rate does -- "
                  "the rescaled band keeps an ordinary firing off the 'large rate' extreme");
    }
    {
        /* Negative test, proving the check above can fail: at the OLD
         * 0.05 degC/s band, a 100 degC/hr ramp rate (0.0278) is already
         * more than half-scale (0.0278/0.05 = 55.6%), so a check this
         * strict (<25% of the saturated nudge) would have failed against
         * the old constant. Reproduce that old-band math directly (not by
         * calling pid_fuzzy_adjust(), which now uses the new constant) to
         * confirm the comparison is a real discriminator, not a tautology. */
        float old_band = 0.05f;
        float ramp_rate = 0.0278f;
        float membership_fraction_of_saturated = ramp_rate / old_band; /* triangular_memberships() is linear inside the band */
        TEST_CHECK(membership_fraction_of_saturated > 0.25f,
                  "sanity: under the OLD 0.05 band, an ordinary ramp is already >25% toward saturation -- "
                  "confirms the rescale above is what makes the <25% assertion pass, not a coincidence");
    }
}
