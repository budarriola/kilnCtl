#include "test_common.h"
#include "../drivers/pid.h"

#include <math.h>

void run_test_pid(void)
{
    TEST_SECTION("pid");

    /* Cold start: first tick must not see a derivative kick from an
     * uninitialized prev_measurement. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 1.0f, .ki = 0.0f, .kd = 5.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 50.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 100.0f, 20.0f, 1.0f, 0.0f);
        TEST_CHECK_NEAR(u, 1.0f, 1e-6, "80C error > pid_range_c -> functional-range full-on, no D kick from cold prev_measurement");
    }

    /* Pure P, no clamp: kp*error should come through untouched inside [0,1]. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.01f, .ki = 0.0f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 100.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 100.0f, 50.0f, 1.0f, 0.0f);
        TEST_CHECK_NEAR(u, 0.5f, 1e-5, "kp=0.01, error=50 -> u=0.5");
    }

    /* Output always clamped to [0,1] even with a large negative error. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 1.0f, .ki = 0.0f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 20.0f, 900.0f, 1.0f, 0.0f);
        TEST_CHECK_NEAR(u, 0.0f, 1e-6, "large negative error clamps to 0, not negative");
    }

    /* Functional range: error beyond pid_range_c returns a hard 1.0/0.0 and
     * does not touch the integrator. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.001f, .ki = 0.001f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 20.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 900.0f, 20.0f, 1.0f, 0.0f);
        TEST_CHECK_NEAR(u, 1.0f, 1e-6, "error >> pid_range_c -> full on");
        TEST_CHECK_NEAR(s.integral, 0.0f, 1e-6, "integrator held (untouched) outside pid_range_c");
    }

    /* Conditional-integration anti-windup: saturating error should not let
     * the integral run away to a huge number while output is pinned. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.5f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        float u = 0.0f;
        for (int i = 0; i < 50; i++) {
            u = pid_update(&s, &cfg, 500.0f, 20.0f, 1.0f, 0.0f);
        }
        TEST_CHECK_NEAR(u, 1.0f, 1e-6, "sustained large error saturates output at 1.0");
        TEST_CHECK(s.integral <= 1.0f / cfg.ki + 1e-3, "integral clamped near the ki*I<=1 boundary, not runaway");
    }

    /* Setpoint weighting: b=0 means a setpoint step does not jump the P
     * term (only measurement drives P), matching the header's "b: setpoint
     * weight on the P term" contract. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.01f, .ki = 0.0f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 0.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        float u1 = pid_update(&s, &cfg, 100.0f, 100.0f, 1.0f, 0.0f);
        float u2 = pid_update(&s, &cfg, 500.0f, 100.0f, 1.0f, 0.0f);
        TEST_CHECK_NEAR(u1, 0.0f, 1e-6, "b=0, setpoint==measurement -> P term 0 regardless of setpoint");
        TEST_CHECK_NEAR(u2, 0.0f, 1e-6, "b=0, setpoint jump alone must not move P term");
    }

    /* Bumpless transfer: seeding for a desired output should make the very
     * next tick reproduce that output closely, for a near-zero residual
     * error (the transition this exists for -- a mode change near
     * steady-state, not a fresh 300C-error cold start). */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.02f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        pid_seed_bumpless(&s, &cfg, 200.5f, 200.0f, 0.6f, 0.0f);
        float u = pid_update(&s, &cfg, 200.5f, 200.0f, 1.0f, 0.0f);
        TEST_CHECK_NEAR(u, 0.6f, 0.02, "bumpless-seeded tick reproduces u_desired closely");
    }

    /* Bumpless transfer with feedforward: the seed must subtract ff_u before
     * solving for the integral, so the reproduced tick (P + I + ff) still
     * lands on u_desired rather than u_desired + ff_u. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.02f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        pid_seed_bumpless(&s, &cfg, 200.5f, 200.0f, 0.6f, 0.25f);
        float u = pid_update(&s, &cfg, 200.5f, 200.0f, 1.0f, 0.25f);
        TEST_CHECK_NEAR(u, 0.6f, 0.02, "bumpless-seeded tick with ff_u still reproduces u_desired");
    }

    /* Feedforward simply adds, subject to the same clamp. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.0f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 500.0f, 500.0f, 1.0f, 0.3f);
        TEST_CHECK_NEAR(u, 0.3f, 1e-6, "zero gains + ff_u=0.3 -> output 0.3");
    }

    /* pid_update_terms(): term breakdown sums to the same u pid_update()
     * returns (within the clamp), and pid_update() itself is unaffected by
     * the out_terms parameter existing (same call, NULL out_terms). */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.01f, .ki = 0.02f, .kd = 0.05f, .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 100.0f};
        pid_reset(&s);
        pid_terms_t terms;
        float u = pid_update_terms(&s, &cfg, 500.0f, 480.0f, 1.0f, 0.1f, &terms);
        float sum = terms.p + terms.i + terms.d + terms.ff;
        float clamped_sum = sum < 0.0f ? 0.0f : (sum > 1.0f ? 1.0f : sum);
        TEST_CHECK_NEAR(u, clamped_sum, 1e-5, "u == clamp(p+i+d+ff)");
        TEST_CHECK_NEAR(terms.ff, 0.1f, 1e-6, "ff term passes through ff_u unchanged");
    }
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 1.0f, .ki = 0.0f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 10.0f};
        pid_reset(&s);
        pid_terms_t terms;
        float u = pid_update_terms(&s, &cfg, 500.0f, 20.0f, 1.0f, 0.0f, &terms);
        TEST_CHECK_NEAR(u, 1.0f, 1e-6, "functional-range clamp still returns 1.0 via the _terms path");
        TEST_CHECK(terms.i == 0.0f && terms.d == 0.0f, "functional-range terms report i=d=0, not fabricated PID math");
    }

    /* pid_rescale_integral_for_new_ki() (PID_EXPANSION_PLAN.md Phase 3 hazard
     * 3): rescaling should hold the I term's actual contribution (ki*integral)
     * constant across a Ki change, which is the whole point -- assert on the
     * output duty itself, the way the task requires, not just that the
     * function ran. Without the rescale, the same Ki jump steps u by exactly
     * the difference an un-rescaled i_term would produce. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.01f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        /* Run a few ticks with a sustained error to build up a real integral
         * (not a hand-set one), so this exercises the same state
         * pid_update_terms() itself produces. */
        float u_before = 0.0f;
        for (int i = 0; i < 20; i++) {
            u_before = pid_update(&s, &cfg, 300.0f, 250.0f, 1.0f, 0.0f);
        }
        float i_term_before = cfg.ki * s.integral;

        /* Ki cut by 40% (a fuzzy-adjust-sized move) -- WITHOUT the rescale,
         * i_term = ki*integral would immediately drop by 40% too, stepping
         * u down by that much on the very next tick despite nothing else
         * about the plant/error having changed. */
        float new_ki = cfg.ki * 0.6f;
        pid_rescale_integral_for_new_ki(&s, cfg.ki, new_ki);
        cfg.ki = new_ki;
        float i_term_after = cfg.ki * s.integral;
        TEST_CHECK_NEAR(i_term_after, i_term_before, 1e-6, "rescale holds ki*integral (the I term's actual contribution) constant across a Ki move");

        /* And the very next tick's output must not have stepped either --
         * this is the assertion the task requires: on the actual output,
         * not just the internal i_term arithmetic above. */
        float u_after = pid_update(&s, &cfg, 300.0f, 250.0f, 1.0f, 0.0f);
        TEST_CHECK(fabsf(u_after - u_before) < 0.01f, "rescaled Ki change: next tick's duty does not step (bump-transferred)");
    }
    {
        /* Negative test: prove the assertion above can actually fail --
         * the same Ki cut WITHOUT calling pid_rescale_integral_for_new_ki()
         * first must step the output by roughly the un-rescaled i_term drop
         * (0.4 * i_term_before here), not stay flat. If this ever starts
         * passing, the "no rescale" comparison stopped being a real bump. */
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.01f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        float u_before = 0.0f;
        for (int i = 0; i < 20; i++) {
            u_before = pid_update(&s, &cfg, 300.0f, 250.0f, 1.0f, 0.0f);
        }
        cfg.ki = cfg.ki * 0.6f; /* same 40% cut, no rescale call this time */
        float u_after = pid_update(&s, &cfg, 300.0f, 250.0f, 1.0f, 0.0f);
        TEST_CHECK(fabsf(u_after - u_before) > 0.02f, "sanity: an UN-rescaled Ki cut of this size really does step duty (proves the test above is not vacuous)");
    }

    /* pid_rescale_integral_for_new_ki() no-op guards: <=0 gains, or an
     * unchanged Ki, must not touch integral at all. */
    {
        pid_state_t s;
        pid_reset(&s);
        s.integral = 42.0f;
        pid_rescale_integral_for_new_ki(&s, 0.0f, 0.02f);
        TEST_CHECK_NEAR(s.integral, 42.0f, 1e-9, "old_ki<=0: no-op, integral untouched");
        pid_rescale_integral_for_new_ki(&s, 0.02f, 0.0f);
        TEST_CHECK_NEAR(s.integral, 42.0f, 1e-9, "new_ki<=0: no-op, integral untouched");
        pid_rescale_integral_for_new_ki(&s, 0.02f, 0.02f);
        TEST_CHECK_NEAR(s.integral, 42.0f, 1e-9, "old_ki==new_ki: no-op, integral untouched");
    }
}
