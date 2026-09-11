// fuzzy_nine_cell_probe -- the offline, purely-numerical 9-cell probe
// docs/FUZZY_CONTROLLER_PLAN.md sec 5 "Stage 0 -- (v-a) offline rule-cell
// probe" calls for: "feed synthetic error/rate spanning all 9 cells through
// [pid_fuzzy_adjust()]; record the gain triple per cell at strength 50 ...
// a 9-row table in an audit doc." This is that probe. It is deliberately
// the cheapest possible step in that plan: no plant, no board, no kiln
// time, and it changes no controller behaviour.
//
// DIFFERENT PURPOSE FROM sim_fuzzy_closedloop.c. That harness (fbdc5bd0)
// answers "can a closed loop's own trajectory ever REACH all 9 cells" (via
// injected disturbances) and reports whether the strength=0 contract holds
// tick-by-tick. It never reports what a cell's rule actually DOES to the
// gains in physical units, and it never states a per-cell reachability
// verdict against this plant's own measured envelope. This file answers
// the complementary question the plan's Stage 0 actually asks: for EACH of
// the 9 cells, driven to fire at weight 1.0 (pure, isolated), (a) what rule
// consequent does it encode, (b) what is the REAL multiplicative effect on
// kp/ki/kd at strength_pct 25 and 50 -- not the rule table's raw +-1/0
// integers, which are directions, not gains -- (c) the physical (degC,
// degC/s) coordinates at which that cell dominates, given the documented
// band defaults, and (d) a reachability verdict against the measured
// ~0.083 degC/s max ramp rate (pid_fuzzy.c's own header comment) and this
// project's ~40 degC max bench rise (project_bench_is_a_4w_test_fixture).
//
// LINKS THE REAL, UNMODIFIED pid_fuzzy_adjust() -- never a reimplementation.
// A mirror would be vacuous here exactly the way
// project_negative_test_on_a_mirror_is_vacuous.md describes: the whole
// point is to observe what the SHIPPED rule table actually does.
//
// WHY PURE CELL-EDGE INPUTS. triangular_memberships() (pid_fuzzy.c) returns
// {1,0,0} at x=-band, {0,1,0} at x=0, {0,0,1} at x=+band -- exactly one
// bucket at full membership, the other two at exactly zero. Driving error
// and rate to these three values each (9 combinations) makes every cell
// fire ALONE, at weight_sum=1.0, with zero contribution from its
// neighbours. This is not an approximation of "the cell dominates"; at
// these exact coordinates it is the only cell active at all, so the
// probe's assertions are checking the rule table's OWN consequent in
// isolation, not a blend.
//
// ASSERTS, DOES NOT JUST PRINT. Per this repo's own standing finding
// (project_harness_prints_verdict_exits_zero.md), a probe that only prints
// numbers guards nothing. Two things are asserted here as contracts that
// must stay stable:
//   1. strength_pct == 0 reproduces base gains bit-for-bit (the safety
//      contract pid_fuzzy.h documents) -- checked at all 9 cell coordinates.
//   2. Each of the 9 cells' ACTUAL multiplicative factor, at strength_pct
//      25 and 50, matches a hand-computed expected literal derived from
//      pid_fuzzy.h's own documented rule-table comment (dir in {-1,0,+1},
//      factor = 1 + (strength/100)*MAX_NUDGE_FRACTION*dir, MAX_NUDGE_
//      FRACTION=0.5f per pid_fuzzy.c) -- NOT re-derived from RULE_TABLE
//      itself (this file cannot see that static array; it only calls the
//      public pid_fuzzy_adjust()), so a changed rule-table entry changes
//      the REAL function's output away from this file's independent
//      expected literal and the assertion fails. See the centre-cell
//      contract below for the single most consequential of the 9: it is
//      the only cell the real system has ever occupied on hardware
//      (fuzzy_ab_20260904d_s50_run1.jsonl, 100% of 2178 samples).
//
// NEGATIVE-TESTED: a deliberate one-line perturbation of pid_fuzzy.c's
// RULE_TABLE[1][1] (the centre cell) was confirmed to fail this probe's
// centre-cell assertion, then reverted by hand -- git diff on pid_fuzzy.c
// after the revert is empty. See the commit this file shipped in for the
// transcript.
//
// Usage: no arguments. Exit 0 = all assertions pass (prints the 9-row
// table either way). Non-zero = a named assertion failed.

#include "../drivers/control/pid_fuzzy.h"

#include <math.h>
#include <stdio.h>

// Documented firmware-default bands (pid_fuzzy.c's ERROR_BAND_C_DEFAULT /
// RATE_BAND_C_PER_S_DEFAULT) -- passed explicitly here (never 0) so this
// probe exercises the same numbers a caller with no per-zone override gets.
#define ERROR_BAND_C 20.0f
#define RATE_BAND_C_PER_S 0.5f

// pid_fuzzy.c's own MAX_NUDGE_FRACTION -- duplicated here as a literal
// (this probe cannot #include pid_fuzzy.c's private #define) ONLY to
// compute this file's OWN independent expected values below; it never
// feeds back into anything pid_fuzzy_adjust() itself computes.
#define MAX_NUDGE_FRACTION 0.5f

// This plant's own measured envelope (established facts this task was
// briefed with -- see pid_fuzzy.c's header comment for the ramp-rate figure
// and project_bench_is_a_4w_test_fixture.md for the bench rise figure).
#define MAX_REAL_RAMP_RATE_C_PER_S 0.083f
#define MAX_BENCH_RISE_C 40.0f

static int g_failures = 0;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            g_failures++; \
        } \
    } while (0)

typedef struct {
    int error_bucket; /* 0=NEG, 1=ZERO, 2=POS */
    int rate_bucket;  /* 0=FALLING, 1=STEADY, 2=RISING */
    float kp_dir, ki_dir, kd_dir; /* pid_fuzzy.h's documented rule table */
    const char *consequent;
} cell_spec_t;

// Directions transcribed from pid_fuzzy.h's own rule-table comment (the
// documented source of truth) -- NOT read from pid_fuzzy.c's static
// RULE_TABLE, which this file cannot see. This is the independent
// second source project_binding_a_python_mirror_to_c.md's convention
// calls for: compare the REAL function's output against a value computed
// from the DOCUMENTED contract, not from the implementation itself.
static const cell_spec_t CELLS[9] = {
    {0, 0,  1.0f, -1.0f,  1.0f, "NEG/FALLING: overshoot still growing -> attack (Kp+,Ki-,Kd+)"},
    {0, 1,  1.0f,  0.0f,  0.0f, "NEG/STEADY: steady overshoot -> push harder (Kp+,Ki=,Kd=)"},
    {0, 2, -1.0f,  1.0f, -1.0f, "NEG/RISING: overshoot recovering -> ease off (Kp-,Ki+,Kd-)"},
    {1, 0, -1.0f, -1.0f,  1.0f, "ZERO/FALLING: crossing target fast -> damp (Kp-,Ki-,Kd+)"},
    {1, 1, -1.0f,  1.0f, -1.0f, "ZERO/STEADY (CENTRE): settled -> coast on I (Kp-,Ki+,Kd-)"},
    {1, 2,  1.0f, -1.0f,  1.0f, "ZERO/RISING: just left target -> catch it (Kp+,Ki-,Kd+)"},
    {2, 0, -1.0f,  1.0f, -1.0f, "POS/FALLING: already closing in -> ease off (Kp-,Ki+,Kd-)"},
    {2, 1,  1.0f,  0.0f,  0.0f, "POS/STEADY: steady approach -> push harder (Kp+,Ki=,Kd=)"},
    {2, 2,  1.0f, -1.0f,  1.0f, "POS/RISING: far and getting worse -> attack (Kp+,Ki-,Kd+)"},
};

static const char *BUCKET_ERR_NAME[3] = {"NEG (-20 degC)", "ZERO (0 degC)", "POS (+20 degC)"};
static const char *BUCKET_RATE_NAME[3] = {"FALLING (-0.5 degC/s)", "STEADY (0 degC/s)", "RISING (+0.5 degC/s)"};

// Cell-edge coordinate that fires this bucket ALONE at weight 1.0.
static float error_coord(int bucket) { return (bucket == 0) ? -ERROR_BAND_C : (bucket == 2) ? ERROR_BAND_C : 0.0f; }
static float rate_coord(int bucket) { return (bucket == 0) ? -RATE_BAND_C_PER_S : (bucket == 2) ? RATE_BAND_C_PER_S : 0.0f; }

static float expected_factor(float dir, uint8_t strength_pct)
{
    float scale = ((float)strength_pct / 100.0f) * MAX_NUDGE_FRACTION;
    return 1.0f + scale * dir;
}

int main(void)
{
    printf("=== fuzzy_nine_cell_probe -- offline 9-cell rule-table probe (FUZZY_CONTROLLER_PLAN.md Stage 0) ===\n\n");
    printf("Bands: error_band_c=%.1f, rate_band_c_per_s=%.2f (documented firmware defaults)\n", ERROR_BAND_C, RATE_BAND_C_PER_S);
    printf("Plant envelope: max real ramp rate ~%.3f degC/s, max bench rise ~%.0f degC\n\n", MAX_REAL_RAMP_RATE_C_PER_S, MAX_BENCH_RISE_C);

    const uint8_t strengths[2] = {25, 50};

    for (int c = 0; c < 9; c++) {
        const cell_spec_t *cell = &CELLS[c];
        float error_c = error_coord(cell->error_bucket);
        float rate_c = rate_coord(cell->rate_bucket);

        printf("Cell %d: error=%s, rate=%s\n", c + 1, BUCKET_ERR_NAME[cell->error_bucket], BUCKET_RATE_NAME[cell->rate_bucket]);
        printf("  rule: %s\n", cell->consequent);

        /* strength_pct == 0 safety contract, checked at this cell's own
         * coordinates (not just at some generic point elsewhere). */
        {
            float kp, ki, kd;
            pid_fuzzy_adjust(error_c, rate_c, ERROR_BAND_C, RATE_BAND_C_PER_S,
                              1.0f, 1.0f, 1.0f, 0, &kp, &ki, &kd);
            CHECK(kp == 1.0f && ki == 1.0f && kd == 1.0f,
                  "cell %d: strength=0 did not reproduce base gains bit-for-bit (kp=%g ki=%g kd=%g)",
                  c + 1, (double)kp, (double)ki, (double)kd);
        }

        for (int s = 0; s < 2; s++) {
            uint8_t sp = strengths[s];
            float kp, ki, kd;
            /* base gains = 1.0 so the returned value IS the multiplicative
             * factor directly -- no separate division needed to read off
             * "what the controller would really do." */
            pid_fuzzy_adjust(error_c, rate_c, ERROR_BAND_C, RATE_BAND_C_PER_S,
                              1.0f, 1.0f, 1.0f, sp, &kp, &ki, &kd);

            float exp_kp = expected_factor(cell->kp_dir, sp);
            float exp_ki = expected_factor(cell->ki_dir, sp);
            float exp_kd = expected_factor(cell->kd_dir, sp);

            printf("  strength=%3u%%: kp x%.4f  ki x%.4f  kd x%.4f  (expected x%.4f/x%.4f/x%.4f)\n",
                   sp, (double)kp, (double)ki, (double)kd, (double)exp_kp, (double)exp_ki, (double)exp_kd);

            /* Float membership arithmetic at exact band edges should be
             * exact (0/1 weights, no interpolation), but allow a tiny
             * epsilon for the sum/divide chain in pid_fuzzy_adjust(). */
            CHECK(fabsf(kp - exp_kp) < 1e-5f, "cell %d strength=%u: kp factor %.6f != expected %.6f",
                  c + 1, sp, (double)kp, (double)exp_kp);
            CHECK(fabsf(ki - exp_ki) < 1e-5f, "cell %d strength=%u: ki factor %.6f != expected %.6f",
                  c + 1, sp, (double)ki, (double)exp_ki);
            CHECK(fabsf(kd - exp_kd) < 1e-5f, "cell %d strength=%u: kd factor %.6f != expected %.6f",
                  c + 1, sp, (double)kd, (double)exp_kd);
        }

        /* Reachability verdict, purely arithmetic against this plant's
         * measured envelope (no simulation needed): a bucket at a
         * non-zero rate edge requires |rate| >= RATE_BAND_C_PER_S, which is
         * ~6x MAX_REAL_RAMP_RATE_C_PER_S -- ordinary ramps cannot reach it,
         * only a genuine disturbance can (sim_fuzzy_closedloop.c's own
         * coverage scenario had to inject +-6/+-8C jumps for exactly this
         * reason). A bucket at a non-zero error edge (+-20 degC) sits well
         * inside the ~40 degC bench rise and is reachable from an ordinary
         * large setpoint gap (e.g. early in a ramp from ambient). */
        int rate_needs_disturbance = (cell->rate_bucket != 1) &&
            (RATE_BAND_C_PER_S > 6.0f * MAX_REAL_RAMP_RATE_C_PER_S);
        int error_reachable = (cell->error_bucket == 1) ||
            (ERROR_BAND_C <= MAX_BENCH_RISE_C);

        const char *verdict;
        if (!rate_needs_disturbance && error_reachable) {
            verdict = "REACHABLE IN NORMAL OPERATION";
        } else if (error_reachable) {
            verdict = "REACHABLE ONLY UNDER DISTURBANCE (rate axis needs a fault/shock, not an ordinary ramp)";
        } else {
            verdict = "UNREACHABLE ON THIS PLANT (error axis exceeds the bench's own rise)";
        }
        printf("  verdict: %s\n\n", verdict);
    }

    /* The centre cell's effect, printed prominently and asserted exactly,
     * per this task's own brief: it is the only cell the real system has
     * ever occupied (100% of the one real mode-3 capture's 2178
     * zone-samples), so its constant rescale IS the live behaviour of the
     * fuzzy layer today. */
    {
        float kp25, ki25, kd25, kp50, ki50, kd50;
        pid_fuzzy_adjust(0.0f, 0.0f, ERROR_BAND_C, RATE_BAND_C_PER_S, 1.0f, 1.0f, 1.0f, 25, &kp25, &ki25, &kd25);
        pid_fuzzy_adjust(0.0f, 0.0f, ERROR_BAND_C, RATE_BAND_C_PER_S, 1.0f, 1.0f, 1.0f, 50, &kp50, &ki50, &kd50);
        printf("=== CENTRE CELL (error=0, rate=0 -- the only cell ever observed on hardware) ===\n");
        printf("  strength=25%%: kp x%.4f  ki x%.4f  kd x%.4f\n", (double)kp25, (double)ki25, (double)kd25);
        printf("  strength=50%%: kp x%.4f  ki x%.4f  kd x%.4f\n", (double)kp50, (double)ki50, (double)kd50);
        CHECK(fabsf(kp50 - 0.75f) < 1e-5f, "centre cell strength=50: kp factor %.6f != documented 0.75", (double)kp50);
        CHECK(fabsf(ki50 - 1.25f) < 1e-5f, "centre cell strength=50: ki factor %.6f != documented 1.25", (double)ki50);
        CHECK(fabsf(kd50 - 0.75f) < 1e-5f, "centre cell strength=50: kd factor %.6f != documented 0.75", (double)kd50);
        CHECK(fabsf(kp25 - 0.875f) < 1e-5f, "centre cell strength=25: kp factor %.6f != expected 0.875", (double)kp25);
        CHECK(fabsf(ki25 - 1.125f) < 1e-5f, "centre cell strength=25: ki factor %.6f != expected 1.125", (double)ki25);
        CHECK(fabsf(kd25 - 0.875f) < 1e-5f, "centre cell strength=25: kd factor %.6f != expected 0.875", (double)kd25);
    }

    if (g_failures > 0) {
        printf("\n=== fuzzy_nine_cell_probe: FAIL (%d assertion(s)) ===\n", g_failures);
        return 1;
    }
    printf("\n=== fuzzy_nine_cell_probe: PASS ===\n");
    return 0;
}
