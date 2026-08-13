#include "test_common.h"
#include "../drivers/pid.h"

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
        pid_seed_bumpless(&s, &cfg, 200.5f, 200.0f, 0.6f);
        float u = pid_update(&s, &cfg, 200.5f, 200.0f, 1.0f, 0.0f);
        TEST_CHECK_NEAR(u, 0.6f, 0.02, "bumpless-seeded tick reproduces u_desired closely");
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
}
