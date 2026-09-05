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
        pid_fuzzy_adjust(500.0f, 0.3f, 20.0f, 0.5f, 0.012f, 0.0034f, 0.056f, 0, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.012f, "strength=0: kp bit-exact to base_kp");
        TEST_CHECK(ki == 0.0034f, "strength=0: ki bit-exact to base_ki");
        TEST_CHECK(kd == 0.056f, "strength=0: kd bit-exact to base_kd");
    }
    {
        /* Same, at an extreme/near-degenerate input, to make sure the
         * strength=0 short-circuit really is input-independent. */
        float kp, ki, kd;
        pid_fuzzy_adjust(-9999.0f, 500.0f, 20.0f, 0.5f, 0.02f, 0.0f, 0.1f, 0, &kp, &ki, &kd);
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
        pid_fuzzy_adjust(10.0f, 0.1f, 20.0f, 0.5f, NAN, -1.0f, INFINITY, 0, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.0f, "strength=0: NaN base_kp sanitized to 0, not passed through");
        TEST_CHECK(ki == 0.0f, "strength=0: negative base_ki sanitized to 0");
        TEST_CHECK(kd == 0.0f, "strength=0: inf base_kd sanitized to 0");
    }

    /* Rule-table corner: large POSITIVE error (far below setpoint), rate
     * RISING (getting worse) -> Kp up, Ki down, Kd up, per the documented
     * table's POS/RISING cell. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 0.01f, 0.01f, 0.01f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp > 0.01f, "POS error, RISING rate: kp nudged up");
        TEST_CHECK(ki < 0.01f, "POS error, RISING rate: ki nudged down");
        TEST_CHECK(kd > 0.01f, "POS error, RISING rate: kd nudged up");
    }

    /* Rule-table corner: large POSITIVE error, rate FALLING (already
     * closing in) -> Kp down, Ki up, Kd down. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, -1.0f, 20.0f, 0.5f, 0.01f, 0.01f, 0.01f, 100, &kp, &ki, &kd);
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
        pid_fuzzy_adjust(-500.0f, -1.0f, 20.0f, 0.5f, 0.01f, 0.01f, 0.01f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp > 0.01f, "NEG error, FALLING rate: kp nudged up");
        TEST_CHECK(ki < 0.01f, "NEG error, FALLING rate: ki nudged down");
        TEST_CHECK(kd > 0.01f, "NEG error, FALLING rate: kd nudged up");
    }

    /* Rule-table corner: large NEGATIVE error, rate RISING (overshoot
     * recovering) -> Kp down, Ki up, Kd down. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(-500.0f, 1.0f, 20.0f, 0.5f, 0.01f, 0.01f, 0.01f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp < 0.01f, "NEG error, RISING rate: kp nudged down");
        TEST_CHECK(ki > 0.01f, "NEG error, RISING rate: ki nudged up");
        TEST_CHECK(kd < 0.01f, "NEG error, RISING rate: kd nudged down");
    }

    /* Rule-table cells not covered by the four "large error" corners above:
     * the ZERO (near-setpoint) row and the STEADY-rate column. These are the
     * cells that actually fire during well-tracked dwells/ramps -- exactly
     * where a 2026-09-02 plant_sim.py sweep found strength=0 beating every
     * higher strength, monotonically, on every zone at both tested starts
     * (PID_EXPANSION_PLAN.md §3.6). A transposed or inverted table would
     * produce exactly that symptom, so these cells -- previously untested --
     * are the ones that mattered most to check. Expected directions are
     * transcribed from pid_fuzzy.h's documented table (the source of truth),
     * not from pid_fuzzy.c's RULE_TABLE, so an inversion in the .c file has
     * something independent to be caught against. */
    {
        /* ZERO error, rate FALLING (crossing target fast) -> Kp-, Ki-, Kd+. */
        float kp, ki, kd;
        pid_fuzzy_adjust(0.0f, -1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp < 0.02f, "ZERO error, FALLING rate: kp nudged down");
        TEST_CHECK(ki < 0.02f, "ZERO error, FALLING rate: ki nudged down");
        TEST_CHECK(kd > 0.02f, "ZERO error, FALLING rate: kd nudged up");
    }
    {
        /* ZERO error, rate STEADY (settled) -> Kp-, Ki+, Kd- ("coast on I,
         * ease P/D"). This is the cell a well-tracked dwell sits in almost
         * the whole time it is dwelling. */
        float kp, ki, kd;
        pid_fuzzy_adjust(0.0f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp < 0.02f, "ZERO error, STEADY rate: kp nudged down");
        TEST_CHECK(ki > 0.02f, "ZERO error, STEADY rate: ki nudged up");
        TEST_CHECK(kd < 0.02f, "ZERO error, STEADY rate: kd nudged down");
    }
    {
        /* ZERO error, rate RISING (just left target) -> Kp+, Ki-, Kd+. */
        float kp, ki, kd;
        pid_fuzzy_adjust(0.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp > 0.02f, "ZERO error, RISING rate: kp nudged up");
        TEST_CHECK(ki < 0.02f, "ZERO error, RISING rate: ki nudged down");
        TEST_CHECK(kd > 0.02f, "ZERO error, RISING rate: kd nudged up");
    }
    {
        /* POS (large) error, rate STEADY (steady approach) -> Kp+, Ki=, Kd=.
         * The "=" entries are the discriminator a corner-only suite would
         * miss entirely: a table that nudges ki/kd here anyway (e.g. an
         * off-by-one row/column shift) passes every corner test but fails
         * this one. */
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp > 0.02f, "POS error, STEADY rate: kp nudged up");
        TEST_CHECK(ki == 0.02f, "POS error, STEADY rate: ki unchanged");
        TEST_CHECK(kd == 0.02f, "POS error, STEADY rate: kd unchanged");
    }
    {
        /* NEG (large) error, rate STEADY (steady overshoot) -> Kp+, Ki=, Kd=. */
        float kp, ki, kd;
        pid_fuzzy_adjust(-500.0f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp > 0.02f, "NEG error, STEADY rate: kp nudged up");
        TEST_CHECK(ki == 0.02f, "NEG error, STEADY rate: ki unchanged");
        TEST_CHECK(kd == 0.02f, "NEG error, STEADY rate: kd unchanged");
    }

    /* Monotonicity in strength: same inputs, a larger strength_pct must
     * move each gain at least as far from base as a smaller one (and
     * strictly further here, since the rule membership is nonzero). */
    {
        float kp30, ki30, kd30;
        float kp80, ki80, kd80;
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 30, &kp30, &ki30, &kd30);
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 80, &kp80, &ki80, &kd80);
        TEST_CHECK(fabsf(kp80 - 0.02f) > fabsf(kp30 - 0.02f), "higher strength moves kp further from base");
        TEST_CHECK(fabsf(ki80 - 0.02f) > fabsf(ki30 - 0.02f), "higher strength moves ki further from base");
        TEST_CHECK(fabsf(kd80 - 0.02f) > fabsf(kd30 - 0.02f), "higher strength moves kd further from base");
    }

    /* Output gains never negative/inf/NaN, including absurd inputs and
     * strength_pct=100. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(1.0e9f, -1.0e9f, 20.0f, 0.5f, 0.001f, 0.001f, 0.001f, 100, &kp, &ki, &kd);
        TEST_CHECK(isfinite(kp) && kp >= 0.0f, "huge error/rate: kp finite and non-negative");
        TEST_CHECK(isfinite(ki) && ki >= 0.0f, "huge error/rate: ki finite and non-negative");
        TEST_CHECK(isfinite(kd) && kd >= 0.0f, "huge error/rate: kd finite and non-negative");
    }
    {
        /* A rule cell that nudges a gain toward zero must not undershoot
         * past zero into negative territory even at strength=100. */
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, -1.0f, 20.0f, 0.5f, 0.0001f, 0.0001f, 0.0001f, 100, &kp, &ki, &kd);
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
        pid_fuzzy_adjust(nan_val, 0.0f, 20.0f, 0.5f, 0.02f, 0.03f, 0.04f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.02f, "NaN error_c: kp held at base, no adjustment");
        TEST_CHECK(ki == 0.03f, "NaN error_c: ki held at base -- must NOT wind up harder on a dead sensor");
        TEST_CHECK(kd == 0.04f, "NaN error_c: kd held at base, no adjustment");
    }
    {
        float kp, ki, kd;
        float inf_val = INFINITY;
        pid_fuzzy_adjust(0.0f, inf_val, 20.0f, 0.5f, 0.02f, 0.03f, 0.04f, 100, &kp, &ki, &kd);
        TEST_CHECK(kp == 0.02f, "+inf error_rate: kp held at base, no adjustment");
        TEST_CHECK(ki == 0.03f, "+inf error_rate: ki held at base");
        TEST_CHECK(kd == 0.04f, "+inf error_rate: kd held at base");
    }
    {
        float kp, ki, kd;
        float neg_inf = -INFINITY;
        pid_fuzzy_adjust(neg_inf, neg_inf, 20.0f, 0.5f, 0.02f, 0.03f, 0.04f, 100, &kp, &ki, &kd);
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
        pid_fuzzy_adjust(300.0f, -0.0278f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_ramp, &ki_ramp, &kd_ramp);
        /* Same error, rate fully saturated RISING (what the old 0.05 band
         * would have made an ordinary ramp look like). */
        pid_fuzzy_adjust(300.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_sat, &ki_sat, &kd_sat);
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

    /* ------------------------------------------------------------------
     * Everything above drives the 9 rule-table cells only at their
     * SATURATED corners (|error| >= 20, |rate| >= 0.5 -- pure membership
     * 1.0 in one bucket). The archive finding this file exists to answer
     * (logs/coupling/fuzzy_bands_envelope_20260904e_report.md,
     * PID_EXPANSION_PLAN.md 3.6g) is that this rig's real error/rate never
     * leaves the CENTRE cell at all, let alone reaches a saturated corner --
     * so the corner tests above establish "the 9 cells are correct in
     * isolation" but say nothing about the blended region a real, in-band
     * excursion would actually traverse. The blocks below exercise that
     * region: partial membership, the boundary between buckets, and the
     * strength-scaled ceiling.
     * ------------------------------------------------------------------ */

    /* Blending: an error/rate pair strictly BETWEEN bucket centers must
     * produce a gain strictly between the two adjacent corner outputs
     * (weighted-average defuzzification, not a hard switch). error=10 is
     * exactly halfway between ZERO's center (0) and POS's edge (20) on the
     * ERROR_BAND_C=20 scale -- e_zero = e_pos = 0.5, e_neg = 0. Rate held at
     * the RISING corner (1.0, i.e. >= RATE_BAND_C_PER_S so r_pos = 1.0
     * purely) isolates the blend to the error axis alone. */
    {
        float kp_zero_rising, ki_zero_rising, kd_zero_rising;
        float kp_pos_rising, ki_pos_rising, kd_pos_rising;
        float kp_mid, ki_mid, kd_mid;
        pid_fuzzy_adjust(0.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_zero_rising, &ki_zero_rising, &kd_zero_rising);
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_pos_rising, &ki_pos_rising, &kd_pos_rising);
        pid_fuzzy_adjust(10.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_mid, &ki_mid, &kd_mid);
        /* ZERO/RISING is {Kp+,Ki-,Kd+}; POS/RISING is also {Kp+,Ki-,Kd+} --
         * same directions, so this pair alone can't show a blend; use it
         * only to confirm the midpoint stays within [min,max] of the two
         * corners rather than overshooting past either. */
        float kp_lo = fminf(kp_zero_rising, kp_pos_rising), kp_hi = fmaxf(kp_zero_rising, kp_pos_rising);
        float ki_lo = fminf(ki_zero_rising, ki_pos_rising), ki_hi = fmaxf(ki_zero_rising, ki_pos_rising);
        float kd_lo = fminf(kd_zero_rising, kd_pos_rising), kd_hi = fmaxf(kd_zero_rising, kd_pos_rising);
        TEST_CHECK(kp_mid >= kp_lo - 1e-6f && kp_mid <= kp_hi + 1e-6f, "blend at error=10 (halfway ZERO/POS): kp stays within the two adjacent corners' range");
        TEST_CHECK(ki_mid >= ki_lo - 1e-6f && ki_mid <= ki_hi + 1e-6f, "blend at error=10: ki stays within the two adjacent corners' range");
        TEST_CHECK(kd_mid >= kd_lo - 1e-6f && kd_mid <= kd_hi + 1e-6f, "blend at error=10: kd stays within the two adjacent corners' range");
    }

    /* Real blending discriminator: ZERO/STEADY is {Kp-,Ki+,Kd-} and
     * POS/STEADY is {Kp+,Ki=,Kd=} -- opposite kp direction, so a midpoint
     * error at rate=0 (pure STEADY) must land strictly BETWEEN the two kp
     * outputs, not equal to either one, and ki/kd must sit strictly between
     * "nudged" and "unchanged". This is the case a hard bucket-switch
     * (round to nearest bucket instead of blending) would fail: it would
     * jump straight to one corner's exact output. */
    {
        float kp_zero, ki_zero, kd_zero;
        float kp_pos, ki_pos, kd_pos;
        float kp_mid, ki_mid, kd_mid;
        pid_fuzzy_adjust(0.0f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_zero, &ki_zero, &kd_zero);
        pid_fuzzy_adjust(500.0f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_pos, &ki_pos, &kd_pos);
        pid_fuzzy_adjust(10.0f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_mid, &ki_mid, &kd_mid);
        TEST_CHECK(kp_mid > fminf(kp_zero, kp_pos) + 1e-6f && kp_mid < fmaxf(kp_zero, kp_pos) - 1e-6f,
                  "blend at error=10, rate=0 (halfway ZERO/POS, opposite kp directions): kp strictly between the two corners, not snapped to either");
        TEST_CHECK(ki_mid > fminf(ki_zero, ki_pos) + 1e-6f && ki_mid < fmaxf(ki_zero, ki_pos) - 1e-6f,
                  "blend at error=10, rate=0: ki strictly between the two corners");
    }

    /* Boundary continuity: approaching the error=+20 (ZERO/POS) boundary
     * from just inside vs just outside must NOT produce a discontinuous
     * jump in any gain. A real controller sees error cross this boundary
     * continuously as the kiln tracks; a step here would be audible as
     * chattering. Check both directly adjacent to the boundary and confirm
     * output varies smoothly (bounded step for a small input step). */
    {
        float kp_below, ki_below, kd_below;
        float kp_at, ki_at, kd_at;
        float kp_above, ki_above, kd_above;
        pid_fuzzy_adjust(19.99f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_below, &ki_below, &kd_below);
        pid_fuzzy_adjust(20.00f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_at, &ki_at, &kd_at);
        pid_fuzzy_adjust(20.01f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_above, &ki_above, &kd_above);
        /* A 0.02 degC step in error must move kp by far less than the full
         * corner-to-corner swing (~0.005 at strength=100 here) -- anything
         * near that magnitude would mean a near-discontinuity at exactly
         * the boundary the membership function is defined to saturate at. */
        TEST_CHECK(fabsf(kp_at - kp_below) < 0.0005f, "error boundary at +20 (ZERO/POS edge): no discontinuity approaching from below");
        TEST_CHECK(fabsf(kp_above - kp_at) < 0.0005f, "error boundary at +20: no discontinuity leaving into fully-saturated POS");
        TEST_CHECK(fabsf(ki_at - ki_below) < 0.0005f, "error boundary at +20: ki continuous approaching from below");
        TEST_CHECK(fabsf(ki_above - ki_at) < 0.0005f, "error boundary at +20: ki continuous leaving into fully-saturated POS");
    }

    /* Same boundary check on the rate axis, at its +/-0.5 degC/s edge
     * (RISING/STEADY boundary), since the rate axis is the one the
     * archive's finding and the RATE_BAND_C_PER_S rescale both concern. */
    {
        float kp_below, ki_below, kd_below;
        float kp_above, ki_above, kd_above;
        pid_fuzzy_adjust(0.0f, 0.499f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_below, &ki_below, &kd_below);
        pid_fuzzy_adjust(0.0f, 0.501f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp_above, &ki_above, &kd_above);
        TEST_CHECK(fabsf(kp_above - kp_below) < 0.0005f, "rate boundary at +0.5 (STEADY/RISING edge): kp continuous crossing it");
        TEST_CHECK(fabsf(ki_above - ki_below) < 0.0005f, "rate boundary at +0.5: ki continuous crossing it");
    }

    /* ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g): this is the
     * test that proves the new error_band_c/rate_band_c_per_s parameters do
     * something, not merely that they are accepted -- a rescaled band must
     * actually move a given (error, rate) pair to a different point in the
     * membership function, which changes which rule cell dominates and
     * therefore the gain that comes out.
     *
     * Uses logs/coupling/fuzzy_bands_envelope_20260904e_report.md's own
     * measured figures, not arbitrary numbers: an error of 5.0 degC is deep
     * in this rig's observed envelope (peak measured 5.72 degC).
     *
     * At the SHIPPED default band (20.0), 5/20=0.25 -- ZERO's membership
     * (0.75) dominates POS's (0.25), and ZERO/STEADY's rule direction is
     * Kp- (attack this cell's own header table), so kp is nudged BELOW
     * base_kp (computed: 0.75*(-1) + 0.25*(+1) = -0.5 net direction).
     *
     * At a band RESCALED to the report's own recommendation (7.0, the low
     * end of its ~6-8 degC range), the SAME 5.0 degC error is 5/7=0.714 --
     * POS's membership (0.714) now dominates ZERO's (0.286), and POS/STEADY
     * is Kp+, so kp is nudged ABOVE base_kp (0.286*(-1) + 0.714*(+1) =
     * +0.429 net direction) -- the OPPOSITE side of base_kp from the
     * band=20 case. This is a stronger proof than merely "the numbers
     * differ": a caller that ignored the band argument (e.g. forgot to pass
     * zone-resolved bands and fell back to some other constant) could still
     * accidentally produce two different-but-same-sign numbers by chance;
     * flipping which side of base_kp the result lands on cannot happen by
     * accident. */
    {
        float kp_band20, ki_band20, kd_band20;
        float kp_band7, ki_band7, kd_band7;
        pid_fuzzy_adjust(5.0f, 0.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100,
                         &kp_band20, &ki_band20, &kd_band20);
        pid_fuzzy_adjust(5.0f, 0.0f, 7.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100,
                         &kp_band7, &ki_band7, &kd_band7);
        TEST_CHECK(fabsf(kp_band7 - kp_band20) > 0.001f,
                  "rescaling error_band_c from 20 to 7 (measured-envelope figures) must change "
                  "the effective gain for the SAME 5.0 degC error -- the band argument is not "
                  "a no-op");
        TEST_CHECK(kp_band20 < 0.02f - 1e-6f,
                  "at the shipped band (20), a 5 degC error sits closer to the ZERO/STEADY cell "
                  "(Kp- rule) than POS/STEADY -- kp nudges BELOW base_kp");
        TEST_CHECK(kp_band7 > 0.02f + 1e-6f,
                  "at the rescaled band (7), the SAME 5 degC error now sits closer to the "
                  "POS/STEADY cell (Kp+ rule) than ZERO/STEADY -- kp nudges ABOVE base_kp, the "
                  "opposite side from the band=20 case -- proof the band argument actually "
                  "changes which rule cell dominates, not just the exact number");
    }

    /* strength_pct=50 ceiling: the header/plan's "at most +/-25% at
     * strength 50" contract, checked directly rather than inferred from
     * monotonicity. Use the fully-saturated POS/RISING corner (Kp+, Ki-,
     * Kd+, all at full 1.0 rule membership) so the nudge is exactly
     * scale = (50/100)*MAX_NUDGE_FRACTION with no partial-membership
     * damping to obscure the number. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 50, &kp, &ki, &kd);
        TEST_CHECK(fabsf(kp - 0.02f) <= 0.02f * 0.25f + 1e-6f, "strength=50: kp nudge does not exceed the documented +/-25% ceiling");
        TEST_CHECK(fabsf(ki - 0.02f) <= 0.02f * 0.25f + 1e-6f, "strength=50: ki nudge does not exceed the documented +/-25% ceiling");
        TEST_CHECK(fabsf(kd - 0.02f) <= 0.02f * 0.25f + 1e-6f, "strength=50: kd nudge does not exceed the documented +/-25% ceiling");
        /* And it should be CLOSE to 25%, not just under some looser bound
         * -- at full rule membership the ceiling should be reached almost
         * exactly, otherwise "at most 25%" would be true of a much weaker
         * effect too and this assertion would be vacuous. */
        TEST_CHECK(fabsf(kp - 0.02f) > 0.02f * 0.24f, "strength=50, full membership: kp nudge is close to the 25% ceiling, not far under it");
    }

    /* strength_pct=100 ceiling: same corner, must reach close to the full
     * +/-50% MAX_NUDGE_FRACTION bound documented in pid_fuzzy.c. */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp, &ki, &kd);
        TEST_CHECK(fabsf(kp - 0.02f) <= 0.02f * 0.5f + 1e-6f, "strength=100: kp nudge does not exceed the documented +/-50% ceiling");
        TEST_CHECK(fabsf(kp - 0.02f) > 0.02f * 0.49f, "strength=100, full membership: kp nudge is close to the 50% ceiling");
    }

    /* strength_pct above the documented 0-100 range (uint8_t allows up to
     * 255) must clamp to the strength=100 behavior, not extrapolate past
     * it -- the ceiling is a safety bound, not a formula that happens to
     * be evaluated at <=100 in practice. */
    {
        float kp100, ki100, kd100;
        float kp200, ki200, kd200;
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 100, &kp100, &ki100, &kd100);
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 0.02f, 0.02f, 0.02f, 200, &kp200, &ki200, &kd200);
        TEST_CHECK(kp200 == kp100, "strength_pct=200 (out of documented range): kp clamps to the strength=100 result");
        TEST_CHECK(ki200 == ki100, "strength_pct=200: ki clamps to the strength=100 result");
        TEST_CHECK(kd200 == kd100, "strength_pct=200: kd clamps to the strength=100 result");
    }

    /* Symmetric monotonicity: the plan's stated NEG/POS symmetry ("far
     * from target behaves the same regardless of direction, only error's
     * own sign -- handled in pid.c, not here -- decides which way duty
     * moves") is a property of the RULE TABLE's saturated corners, checked
     * above (the NEG-error and POS-error corner blocks share magnitudes).
     * It does NOT extend to partial-membership points that also straddle
     * the ZERO bucket: ZERO/FALLING's direction ({Kp-,Ki-,Kd+}) is shared
     * unmirrored by both a slightly-positive and a slightly-negative
     * error, so blending it in with POS/FALLING ({Kp-,Ki+,Kd-}) vs.
     * NEG/FALLING ({Kp+,Ki-,Kd+}) does NOT generally cancel to equal and
     * opposite magnitudes (confirmed by hand: error=+/-10, rate=-1.0,
     * strength=100 gives kp_pos=0.01 (nudged down) but kp_neg=0.02
     * (exactly unchanged) -- 0.5*(-1)+0.5*(-1)=-1 vs 0.5*(-1)+0.5*(+1)=0.
     * This is arithmetically correct weighted-average defuzzification, not
     * a bug -- documented here instead of asserted on, since the original
     * version of this test asserted the wrong (naive mirror) expectation
     * and failed against correct code. */

    /* Ceiling clamp under an EXTREME base gain: clamp_gain's job is to
     * catch a bad OUTPUT, not just an input already near zero -- confirm a
     * huge base gain nudged further up stays finite (no overflow-adjacent
     * behavior in the (1 + scale*dir) multiply). */
    {
        float kp, ki, kd;
        pid_fuzzy_adjust(500.0f, 1.0f, 20.0f, 0.5f, 1.0e6f, 1.0e6f, 1.0e6f, 100, &kp, &ki, &kd);
        TEST_CHECK(isfinite(kp) && kp > 0.0f, "huge base_kp nudged up: still finite and positive");
        TEST_CHECK(isfinite(kd) && kd > 0.0f, "huge base_kd nudged up: still finite and positive");
    }
}
