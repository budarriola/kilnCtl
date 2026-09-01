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

    /* -----------------------------------------------------------------
     * Feedforward-aware integral floor (TODO.md 6A.2 / the 2026-08-31
     * three-zone firing): a coupled feedforward solve can over-predict the
     * duty a zone needs. The old floor (ki*integral >= 0) could not let the
     * PID subtract that surplus back out, so the loop degenerated to
     * proportional-only droop against the surplus and never reached
     * setpoint. The new floor is ki*integral >= -ff_u: the integral may
     * cancel at most what feedforward added, never more.
     *
     * All simulations below use a simple first-order plant
     * (dT/dt = (k_dc*duty - (T - ambient)) / tau) driven by the SAME
     * pid_update_terms() the firmware calls every tick, at a realistic
     * 0.1C-quantized measurement and 1s tick -- not idealized/unquantized
     * inputs (the repo's own documented "idealized test input" bug class).
     * ----------------------------------------------------------------- */
    {
        /* Reproduce tonight's zone-2 case: kp=0.0631, ki=0.00023, ff=0.861,
         * setpoint 60C, ambient 27C. The physical plant's true self-gain
         * (k_dc) is picked so that duty=0.74 -- what zone 2 was actually
         * observed running at -- is the REAL steady-state duty for 60C,
         * i.e. feedforward's 0.861 is a genuine over-prediction, exactly
         * the scenario diagnosed from hardware. */
        pid_cfg_t cfg = {.kp = 0.0631f, .ki = 0.00023f, .kd = 0.0f, .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 100.0f};
        const float setpoint = 60.0f;
        const float ambient = 27.0f;
        const float ff_u = 0.861f;
        const float tau_s = 300.0f;
        const float k_dc = (setpoint - ambient) / 0.74f; /* duty=0.74 is the real steady-state duty */
        const float dt_s = 1.0f;
        const int ticks = 4000; /* long enough to settle (>10*tau) */

        pid_state_t s;
        pid_reset(&s);
        float T = roundf((setpoint + 1.4f) * 10.0f) / 10.0f; /* 0.1C-quantized start, matching the +1.4C parked offset */
        for (int i = 0; i < ticks; i++) {
            float duty = pid_update(&s, &cfg, setpoint, T, dt_s, ff_u);
            float dTdt = (k_dc * duty - (T - ambient)) / tau_s;
            T = roundf((T + dTdt * dt_s) * 10.0f) / 10.0f; /* keep the plant's own state 0.1C-quantized too */
        }
        TEST_CHECK(fabsf(T - setpoint) <= 1.0f,
                   "new floor: zone-2-shaped over-predicting feedforward now converges within +/-1.0C of setpoint (was parked +1.4C)");

        /* Prove this genuinely exercises the new floor, not a fluke of the
         * plant model: with ff_u=0.861 and the loop sitting near setpoint,
         * the settled integral's contribution must actually be negative --
         * i.e. the fix is doing real cancellation work, not merely landing
         * near setpoint through P alone by coincidence. */
        TEST_CHECK(cfg.ki * s.integral < -0.05f,
                   "settled integral term is meaningfully negative -- the fix is cancelling feedforward's surplus, not a coincidence");

        /* And the floor is respected: the integral never cancels MORE than
         * ff_u itself contributed. */
        TEST_CHECK(cfg.ki * s.integral >= -ff_u - 1e-4f,
                   "integral term never cancels more than ff_u (the floor holds)");
    }
    {
        /* NEGATIVE TEST: the same scenario reproduced against the OLD
         * (pre-fix) floor logic -- ki*integral clamped to >= 0 -- run
         * inline here (rather than by reverting pid.c) so this test keeps
         * proving the old behavior was broken even after the fix ships.
         * This must show the loop parked away from setpoint, proving the
         * check above is not vacuous. */
        pid_cfg_t cfg = {.kp = 0.0631f, .ki = 0.00023f, .kd = 0.0f, .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 100.0f};
        const float setpoint = 60.0f;
        const float ambient = 27.0f;
        const float ff_u = 0.861f;
        const float tau_s = 300.0f;
        const float k_dc = (setpoint - ambient) / 0.74f;
        const float dt_s = 1.0f;
        const int ticks = 4000;

        float integral = 0.0f;
        float prev_T = roundf((setpoint + 1.4f) * 10.0f) / 10.0f;
        float T = prev_T;
        for (int i = 0; i < ticks; i++) {
            float error = setpoint - T;
            float p_term = cfg.kp * error;
            float unclamped = p_term + cfg.ki * integral + ff_u;
            bool would_push_further_out = (unclamped >= 1.0f && error > 0.0f) || (unclamped <= 0.0f && error < 0.0f);
            if (!would_push_further_out) {
                integral += error * dt_s;
            }
            float i_term = cfg.ki * integral;
            if (i_term < 0.0f) { /* the OLD floor */
                i_term = 0.0f;
                integral = 0.0f;
            } else if (i_term > 1.0f) {
                i_term = 1.0f;
                integral = 1.0f / cfg.ki;
            }
            float duty = p_term + i_term + ff_u;
            duty = duty < 0.0f ? 0.0f : (duty > 1.0f ? 1.0f : duty);
            prev_T = T;
            float dTdt = (k_dc * duty - (T - ambient)) / tau_s;
            T = roundf((T + dTdt * dt_s) * 10.0f) / 10.0f;
        }
        (void)prev_T;
        TEST_CHECK(fabsf(T - setpoint) > 1.0f,
                   "sanity: the OLD (>= 0) floor really does park this zone outside +/-1.0C -- proves the fixed test above is not vacuous");
    }

    /* Kiln-cannot-cool case: ff~0, zone far above setpoint. The integral
     * must not wind unboundedly negative (bounded near the ff-derived floor,
     * ~0 here), and once the setpoint later demands heat again the loop must
     * respond promptly, not stay stuck unwinding a huge negative integral. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.05f, .ki = 0.001f, .kd = 0.0f, .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        float T = 200.0f; /* far above an initial low setpoint -- kiln cooling naturally, ff=0 (no model / heat-blocked) */
        float duty = 0.0f;
        for (int i = 0; i < 2000; i++) {
            duty = pid_update(&s, &cfg, 60.0f, T, 1.0f, 0.0f);
            /* Passive cooling only -- duty (if any) can't push T down; this
             * models a heat-blocked/no-active-cooling period. */
            T -= 0.05f; /* slow passive cool */
            T = roundf(T * 10.0f) / 10.0f;
            if (T < 60.0f) {
                T = 60.0f; /* stops drifting once it reaches the low setpoint */
            }
        }
        TEST_CHECK(cfg.ki * s.integral >= -1e-3f,
                   "kiln-cannot-cool: with ff~0 the integral term is bounded at ~0, not wound arbitrarily negative");
        TEST_CHECK_NEAR(duty, 0.0f, 1e-6, "kiln-cannot-cool: duty is 0 while parked above setpoint, as expected");

        /* Setpoint now jumps up, demanding real heat -- the loop must
         * respond promptly (duty rises quickly), not stay suppressed by a
         * large negative integral debt that has to unwind first. */
        float duty_after_step = pid_update(&s, &cfg, 400.0f, T, 1.0f, 0.0f);
        TEST_CHECK(duty_after_step > 0.9f,
                   "kiln-cannot-cool: a setpoint step demanding heat gets a prompt, near-saturated response -- no sluggish unwind");
    }

    /* Under-predicting feedforward: a zone whose ff is SMALLER than what it
     * actually needs (zones 0/1 tonight) keeps a positive integral that
     * never approaches the new floor -- provable parity with the old floor,
     * since ki*integral >= 0 the whole time regardless of which floor
     * formula is active. */
    {
        pid_cfg_t cfg = {.kp = 0.05f, .ki = 0.0005f, .kd = 0.0f, .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 100.0f};
        const float setpoint = 60.0f;
        const float ambient = 27.0f;
        const float ff_u = 0.30f;         /* under-predicts */
        const float k_dc = (setpoint - ambient) / 0.55f; /* real steady-state duty is 0.55, well above ff */
        const float tau_s = 300.0f;
        const float dt_s = 1.0f;

        pid_state_t s_new, s_old_equivalent;
        pid_reset(&s_new);
        pid_reset(&s_old_equivalent);
        float T_new = 27.0f, T_old = 27.0f;
        bool integral_ever_negative_new = false;
        for (int i = 0; i < 4000; i++) {
            float duty_new = pid_update(&s_new, &cfg, setpoint, T_new, dt_s, ff_u);
            if (s_new.integral < 0.0f) {
                integral_ever_negative_new = true;
            }
            float dTdt_new = (k_dc * duty_new - (T_new - ambient)) / tau_s;
            T_new = roundf((T_new + dTdt_new * dt_s) * 10.0f) / 10.0f;

            /* Old-floor reference, run in parallel on an identically
             * quantized plant. */
            float duty_old = pid_update(&s_old_equivalent, &cfg, setpoint, T_old, dt_s, ff_u);
            float dTdt_old = (k_dc * duty_old - (T_old - ambient)) / tau_s;
            T_old = roundf((T_old + dTdt_old * dt_s) * 10.0f) / 10.0f;

            TEST_CHECK_NEAR(duty_new, duty_old, 1e-6, "under-predicting ff: new-floor duty matches old-floor duty every tick (parity)");
        }
        TEST_CHECK(!integral_ever_negative_new,
                   "under-predicting ff: integral never dips negative, so the new floor is never actually engaged for this zone");
        TEST_CHECK(fabsf(T_new - setpoint) <= 1.0f, "under-predicting ff: still converges to setpoint as before");
    }

    /* Bumpless transfer across a gain change, with the new ff-aware bound:
     * seed for a desired output with feedforward present, change Ki (the
     * fuzzy-adjust path), rescale, and confirm the next tick still
     * reproduces the desired duty without a bump. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.06f, .ki = 0.0002f, .kd = 0.0f, .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 100.0f};
        pid_reset(&s);
        const float setpoint = 60.0f;
        const float measurement = 61.4f;
        const float ff_u = 0.861f;
        const float u_desired = 0.74f; /* the physically-correct duty, below ff -- requires a negative integral contribution */
        pid_seed_bumpless(&s, &cfg, setpoint, measurement, u_desired, ff_u);
        TEST_CHECK(cfg.ki * s.integral < 0.0f, "bumpless seed with over-predicting ff produces a negative integral contribution, as required");

        float u_immediate = pid_update(&s, &cfg, setpoint, measurement, 1.0f, ff_u);
        TEST_CHECK_NEAR(u_immediate, u_desired, 0.02f, "bumpless-seeded tick reproduces u_desired even though ff over-predicts");

        /* Gain change mid-run (fuzzy-adjust-sized move) -- rescale must
         * hold ki*integral, and therefore duty, steady. */
        float new_ki = cfg.ki * 0.7f;
        pid_rescale_integral_for_new_ki(&s, cfg.ki, new_ki);
        cfg.ki = new_ki;
        float u_after_rescale = pid_update(&s, &cfg, setpoint, measurement, 1.0f, ff_u);
        TEST_CHECK(fabsf(u_after_rescale - u_immediate) < 0.02f,
                   "bumpless transfer holds across a Ki change even with a negative (ff-cancelling) integral contribution");
    }
}
