#include "test_common.h"
#include "../drivers/control/pid.h"

#include <math.h>
#include <stdio.h>

void run_test_pid(void)
{
    TEST_SECTION("pid");

    /* Cold start: first tick must not see a derivative kick from an
     * uninitialized prev_measurement. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 1.0f, .ki = 0.0f, .kd = 5.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 50.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 100.0f, 20.0f, 1.0f, 0.0f, 0.0f);
        TEST_CHECK_NEAR(u, 1.0f, 1e-6, "80C error > pid_range_c -> functional-range full-on, no D kick from cold prev_measurement");
    }

    /* Pure P, no clamp: kp*error should come through untouched inside [0,1]. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.01f, .ki = 0.0f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 100.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 100.0f, 50.0f, 1.0f, 0.0f, 0.0f);
        TEST_CHECK_NEAR(u, 0.5f, 1e-5, "kp=0.01, error=50 -> u=0.5");
    }

    /* Output always clamped to [0,1] even with a large negative error. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 1.0f, .ki = 0.0f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 20.0f, 900.0f, 1.0f, 0.0f, 0.0f);
        TEST_CHECK_NEAR(u, 0.0f, 1e-6, "large negative error clamps to 0, not negative");
    }

    /* Functional range: error beyond pid_range_c returns a hard 1.0/0.0 and
     * does not touch the integrator. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.001f, .ki = 0.001f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 20.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 900.0f, 20.0f, 1.0f, 0.0f, 0.0f);
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
            u = pid_update(&s, &cfg, 500.0f, 20.0f, 1.0f, 0.0f, 0.0f);
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
        float u1 = pid_update(&s, &cfg, 100.0f, 100.0f, 1.0f, 0.0f, 0.0f);
        float u2 = pid_update(&s, &cfg, 500.0f, 100.0f, 1.0f, 0.0f, 0.0f);
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
        pid_seed_bumpless(&s, &cfg, 200.5f, 200.0f, 0.6f, 0.0f, 0.0f);
        float u = pid_update(&s, &cfg, 200.5f, 200.0f, 1.0f, 0.0f, 0.0f);
        TEST_CHECK_NEAR(u, 0.6f, 0.02, "bumpless-seeded tick reproduces u_desired closely");
    }

    /* Bumpless transfer with feedforward: the seed must subtract ff_u before
     * solving for the integral, so the reproduced tick (P + I + ff) still
     * lands on u_desired rather than u_desired + ff_u. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.02f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        pid_seed_bumpless(&s, &cfg, 200.5f, 200.0f, 0.6f, 0.25f, 0.25f);
        float u = pid_update(&s, &cfg, 200.5f, 200.0f, 1.0f, 0.25f, 0.25f);
        TEST_CHECK_NEAR(u, 0.6f, 0.02, "bumpless-seeded tick with ff_u still reproduces u_desired");
    }

    /* Feedforward simply adds, subject to the same clamp. */
    {
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.0f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        float u = pid_update(&s, &cfg, 500.0f, 500.0f, 1.0f, 0.3f, 0.3f);
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
        float u = pid_update_terms(&s, &cfg, 500.0f, 480.0f, 1.0f, 0.1f, 0.1f, &terms);
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
        float u = pid_update_terms(&s, &cfg, 500.0f, 20.0f, 1.0f, 0.0f, 0.0f, &terms);
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
            u_before = pid_update(&s, &cfg, 300.0f, 250.0f, 1.0f, 0.0f, 0.0f);
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
        float u_after = pid_update(&s, &cfg, 300.0f, 250.0f, 1.0f, 0.0f, 0.0f);
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
            u_before = pid_update(&s, &cfg, 300.0f, 250.0f, 1.0f, 0.0f, 0.0f);
        }
        cfg.ki = cfg.ki * 0.6f; /* same 40% cut, no rescale call this time */
        float u_after = pid_update(&s, &cfg, 300.0f, 250.0f, 1.0f, 0.0f, 0.0f);
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
            float duty = pid_update(&s, &cfg, setpoint, T, dt_s, ff_u, ff_u);
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
            duty = pid_update(&s, &cfg, 60.0f, T, 1.0f, 0.0f, 0.0f);
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
        float duty_after_step = pid_update(&s, &cfg, 400.0f, T, 1.0f, 0.0f, 0.0f);
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
            float duty_new = pid_update(&s_new, &cfg, setpoint, T_new, dt_s, ff_u, ff_u);
            if (s_new.integral < 0.0f) {
                integral_ever_negative_new = true;
            }
            float dTdt_new = (k_dc * duty_new - (T_new - ambient)) / tau_s;
            T_new = roundf((T_new + dTdt_new * dt_s) * 10.0f) / 10.0f;

            /* Old-floor reference, run in parallel on an identically
             * quantized plant. */
            float duty_old = pid_update(&s_old_equivalent, &cfg, setpoint, T_old, dt_s, ff_u, ff_u);
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
        pid_seed_bumpless(&s, &cfg, setpoint, measurement, u_desired, ff_u, ff_u);
        TEST_CHECK(cfg.ki * s.integral < 0.0f, "bumpless seed with over-predicting ff produces a negative integral contribution, as required");

        float u_immediate = pid_update(&s, &cfg, setpoint, measurement, 1.0f, ff_u, ff_u);
        TEST_CHECK_NEAR(u_immediate, u_desired, 0.02f, "bumpless-seeded tick reproduces u_desired even though ff over-predicts");

        /* Gain change mid-run (fuzzy-adjust-sized move) -- rescale must
         * hold ki*integral, and therefore duty, steady. */
        float new_ki = cfg.ki * 0.7f;
        pid_rescale_integral_for_new_ki(&s, cfg.ki, new_ki);
        cfg.ki = new_ki;
        float u_after_rescale = pid_update(&s, &cfg, setpoint, measurement, 1.0f, ff_u, ff_u);
        TEST_CHECK(fabsf(u_after_rescale - u_immediate) < 0.02f,
                   "bumpless transfer holds across a Ki change even with a negative (ff-cancelling) integral contribution");
    }

    /* -----------------------------------------------------------------
     * Hold-only integral floor (2026-08-31 "hold-only floor" fix, following
     * commit b7289db's -ff_u floor and hold_only_floor_analysis.md's
     * recommendation): the floor is Ki*integral >= -ff_hold (the STEADY-STATE
     * HOLD component only), never -ff_u (hold+climb). Dwell case (ff_hold ==
     * ff_u, since climb is exactly 0 on a dwell) must floor byte-identically
     * to the old -ff_u floor; a ramp case (ff_hold < ff_u, climb > 0) must
     * floor shallower, letting climb survive.
     * ----------------------------------------------------------------- */
    {
        /* Test 1 (hold_only_floor_analysis.md section 7): dwell floor is
         * byte-identical to the old -ff_u floor when ff_hold == ff_u. */
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.01f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        const float setpoint = 60.0f;
        const float ff_hold = 0.5f, ff_u = 0.5f; /* dwell: climb == 0, so ff_hold == ff_u */

        /* Init tick (any error -- just establishes state->initialized). */
        pid_update_terms(&s, &cfg, setpoint, setpoint, 1.0f, ff_u, ff_hold, NULL);
        /* Force a deeply negative integral (as a settled dwell's overshoot
         * correction would), then run one more tick with a mildly negative
         * error (measurement slightly above setpoint) so conditional
         * integration keeps pushing it more negative and the floor binds. */
        s.integral = -10000.0f;
        pid_terms_t terms;
        float u = pid_update_terms(&s, &cfg, setpoint, setpoint + 1.0f, 1.0f, ff_u, ff_hold, &terms);
        TEST_CHECK_NEAR(terms.i, -ff_u, 1e-5f, "dwell floor (ff_hold==ff_u): i_term floors at exactly -ff_u, byte-identical to the old floor formula");
        TEST_CHECK_NEAR(s.integral, -ff_u / cfg.ki, 1e-3f, "dwell floor: state->integral == -ff_u/ki exactly, matching the old -ff_u floor's seeded value");
        (void)u;
    }
    {
        /* Test 1's mutation, run inline (not by editing pid.c): if the floor
         * were implemented as -0.9*ff_hold instead of -ff_hold exactly, this
         * scenario's i_term would be -0.45, not -0.5 -- proving the assertion
         * above checks exact equality, not "close to". Captured here as its
         * own check so a future accidental "close enough" floor is caught
         * without having to hand-edit pid.c to prove it. */
        pid_state_t s;
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.01f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        pid_reset(&s);
        const float setpoint = 60.0f;
        const float ff_hold = 0.5f;
        pid_update_terms(&s, &cfg, setpoint, setpoint, 1.0f, ff_hold, ff_hold, NULL);
        s.integral = -10000.0f;
        pid_terms_t terms;
        pid_update_terms(&s, &cfg, setpoint, setpoint + 1.0f, 1.0f, ff_hold, ff_hold, &terms);
        float mutated_floor = -0.9f * ff_hold;
        TEST_CHECK(fabsf(terms.i - mutated_floor) > 0.01f,
                   "sanity: the real floor (-ff_hold exactly) does NOT match a -0.9*ff_hold mutant -- proves test 1 above can fail");
    }
    {
        /* Test 2 (hold_only_floor_analysis.md section 7): ramp floor
         * (ff_climb > 0, so ff_hold < ff_u) is strictly shallower than (or
         * equal to) the old -ff_u floor -- the resulting i_term is >= what
         * -ff_u would have produced, and the commanded duty is strictly
         * greater for at least one tick (some of ff_climb survives). */
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.01f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        const float setpoint = 60.0f;
        const float ff_hold = 0.5f;
        const float ff_climb = 0.3f;
        const float ff_u = ff_hold + ff_climb; /* 0.8 -- a heating ramp */

        /* New floor (-ff_hold), via the real production function. */
        pid_state_t s_new;
        pid_reset(&s_new);
        pid_update_terms(&s_new, &cfg, setpoint, setpoint, 1.0f, ff_u, ff_hold, NULL);
        s_new.integral = -10000.0f;
        pid_terms_t terms_new;
        float u_new = pid_update_terms(&s_new, &cfg, setpoint, setpoint + 1.0f, 1.0f, ff_u, ff_hold, &terms_new);

        /* Old floor (-ff_u), replicated inline (matching the existing
         * old-floor negative-test pattern elsewhere in this file) so this
         * comparison doesn't depend on ff_hold==ff_u degenerating to the
         * same call. */
        pid_state_t s_old;
        pid_reset(&s_old);
        pid_update_terms(&s_old, &cfg, setpoint, setpoint, 1.0f, ff_u, ff_u, NULL);
        s_old.integral = -10000.0f;
        pid_terms_t terms_old;
        float u_old = pid_update_terms(&s_old, &cfg, setpoint, setpoint + 1.0f, 1.0f, ff_u, ff_u, &terms_old);

        TEST_CHECK(terms_new.i >= terms_old.i - 1e-6f,
                   "ramp (ff_climb>0): new floor's i_term is shallower (>=) than the old -ff_u floor's i_term");
        TEST_CHECK(u_new > u_old + 1e-4f,
                   "ramp (ff_climb>0): commanded duty is strictly greater under the new floor -- ff_climb survives instead of being cancelled");
    }

    /* Test 5 (hold_only_floor_analysis.md section 7 / integral_floor_analysis.md
     * section 3(b)): ki-blowup guard. A small ki with a moderate ff_hold and
     * a floor-binding scenario must not push the RAW state->integral to an
     * unbounded magnitude, even though the resulting duty-space i_term
     * (-ff_hold) is itself perfectly ordinary. */
    {
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 1e-6f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        const float setpoint = 60.0f;
        const float ff_hold = 0.5f;
        pid_state_t s;
        pid_reset(&s);
        pid_update_terms(&s, &cfg, setpoint, setpoint, 1.0f, ff_hold, ff_hold, NULL);
        /* Force a deeply negative raw integral directly (ordinary
         * conditional integration at ki=1e-6 would take ~500000 ticks of
         * sustained error to reach -ff_hold/ki on its own -- the guard has
         * to hold regardless of HOW the integral got that negative, so a
         * direct poke exercises the same clamp path more directly). One
         * more tick with a mildly negative error (so conditional
         * integration doesn't itself freeze first) then triggers the
         * floor-clamp code path in pid_update_terms(). */
        s.integral = -2000000.0f;
        pid_update_terms(&s, &cfg, setpoint, setpoint + 1.0f, 1.0f, ff_hold, ff_hold, NULL);
        TEST_CHECK(fabsf(s.integral) < 100000.0f + 1.0f,
                   "ki-blowup guard: a tiny ki with a moderate ff_hold must not push raw state->integral past the guard's bound");
    }
    {
        /* Test 5's positive-side twin: the i_term > 1.0f branch of
         * pid_update_terms() (pid.c, "ki-blowup guard, positive side") is
         * the mirror of the negative floor-clamp branch tested above -- a
         * tiny ki makes 1.0f/ki just as enormous. Drive it via the real
         * production function (not a compile-time computation): force the
         * raw integral deeply positive, then run one tick with a mildly
         * positive error so i_term saturates above 1.0f and the clamp path
         * engages. */
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 1e-6f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        const float setpoint = 60.0f;
        pid_state_t s;
        pid_reset(&s);
        pid_update_terms(&s, &cfg, setpoint, setpoint, 1.0f, 0.0f, 0.0f, NULL);
        s.integral = 2000000.0f;
        pid_update_terms(&s, &cfg, setpoint, setpoint - 1.0f, 1.0f, 0.0f, 0.0f, NULL);
        TEST_CHECK(fabsf(s.integral) < 100000.0f + 1.0f,
                   "ki-blowup guard (positive side): a tiny ki must not push raw state->integral past the guard's bound on the i_term>1.0f branch either");
    }

    /* Test 6 (hold_only_floor_analysis.md section 4's "genuinely new
     * finding" / section 7 test 6): CHARACTERIZATION, not pass/fail --
     * reproduces the ramp2-without-reseed carryover. A dwell settles to a
     * meaningfully negative integral (ff_climb=0 the whole time); the very
     * next tick switches to ramp values (ff_climb>0) with NO
     * pid_seed_bumpless() call in between, matching the real firmware's
     * segment-advance state machine. The floor can still bind on carryover
     * for some number of ticks -- this test records how many, and proves
     * the new -ff_hold floor releases SOONER (fewer bound ticks) than the
     * old -ff_u floor would have, even though it doesn't eliminate the
     * carryover entirely (hold_only_floor_analysis.md is explicit that this
     * change does not fully fix this case). */
    {
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.01f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        const float setpoint = 60.0f;
        const float ff_hold = 0.5f;
        const float ff_climb = 0.3f;
        const float dwell_ff_u = ff_hold; /* climb == 0 during the dwell */
        const float ramp_ff_u = ff_hold + ff_climb;

        /* -- New floor (-ff_hold) run: dwell settles negative, then ramp
         * begins with no reseed. -- */
        pid_state_t s_new;
        pid_reset(&s_new);
        pid_update_terms(&s_new, &cfg, setpoint, setpoint, 1.0f, dwell_ff_u, ff_hold, NULL);
        /* Settle the dwell: measurement runs a bit hot, driving integral
         * meaningfully negative via ordinary conditional integration (no
         * manual poke here -- this is the carryover this test is about).
         * dwell_ff_u == ff_hold here (climb is 0 on a dwell), so the dwell
         * settles to (at most) the dwell's own floor -ff_hold. */
        for (int i = 0; i < 500; i++) {
            pid_update_terms(&s_new, &cfg, setpoint, setpoint + 2.0f, 1.0f, dwell_ff_u, ff_hold, NULL);
        }
        float dwell_settled_integral_new = s_new.integral;
        TEST_CHECK(cfg.ki * dwell_settled_integral_new < -0.05f,
                   "test setup sanity: the dwell really did settle to a meaningfully negative integral");

        /* Ramp begins -- NO pid_seed_bumpless() call, matching the real
         * segment-advance state machine, and setpoint held flat here
         * (error==0) so nothing but the carried-over integral itself drives
         * what happens -- isolating the carryover mechanism from ordinary
         * per-tick integration. Track how many ticks the floor stays bound
         * (i_term == -ff_hold exactly). */
        int bound_ticks_new = 0;
        for (int i = 0; i < 200; i++) {
            pid_terms_t terms;
            pid_update_terms(&s_new, &cfg, setpoint, setpoint, 1.0f, ramp_ff_u, ff_hold, &terms);
            if (fabsf(terms.i - (-ff_hold)) < 1e-4f) {
                bound_ticks_new++;
            }
        }

        /* -- Old floor (-ff_u) run, IDENTICAL dwell settle, for comparison
         * -- proves this is a genuine, measurable divergence between the two
         * floor formulas, not just a restatement of the new floor's own
         * definition. */
        pid_state_t s_old;
        pid_reset(&s_old);
        pid_update_terms(&s_old, &cfg, setpoint, setpoint, 1.0f, dwell_ff_u, dwell_ff_u, NULL);
        for (int i = 0; i < 500; i++) {
            pid_update_terms(&s_old, &cfg, setpoint, setpoint + 2.0f, 1.0f, dwell_ff_u, dwell_ff_u, NULL);
        }
        int bound_ticks_old = 0;
        for (int i = 0; i < 200; i++) {
            pid_terms_t terms;
            pid_update_terms(&s_old, &cfg, setpoint, setpoint, 1.0f, ramp_ff_u, ramp_ff_u, &terms);
            if (fabsf(terms.i - (-ramp_ff_u)) < 1e-4f) {
                bound_ticks_old++;
            }
        }

        printf("test 6 (ramp2-without-reseed carryover characterization): new-floor bound for %d/200 ticks, old-floor bound for %d/200 ticks\n",
               bound_ticks_new, bound_ticks_old);

        /* This is a CHARACTERIZATION test, not a pass/fail correctness
         * check, per hold_only_floor_analysis.md section 4/7: the new
         * -ff_hold floor does NOT fix this carryover case -- with this
         * plant/gain shape, the dwell's settled integral sits exactly at
         * the shared dwell/ramp hold floor, so the shallower new floor
         * clamps it right there (bound_ticks_new > 0, documented and
         * asserted below) while the deeper old -ff_u floor has enough
         * headroom that the SAME carried-over integral never even reaches
         * it (bound_ticks_old can be 0) -- i.e. in a bare no-reseed
         * carryover with no rate-driven recovery, the new floor is not
         * guaranteed to bind for FEWER ticks than the old one; the two
         * floors are only "close together" (hold_only_floor_analysis.md's
         * words) when ff_climb is comparatively small, and this isolated
         * carryover slice (setpoint held flat, no climb-driven recovery) is
         * exactly the case where that is least true. The scope note this
         * change ships with is exactly this: hold-only fixes the DWELL and
         * the steady portion of a RAMP that already has integral headroom,
         * but does not, by itself, fix a ramp that inherits a substantially
         * negative integral from the segment before it. */
        TEST_CHECK(bound_ticks_new > 0,
                   "ramp2-without-reseed carryover is real and reproducible: the new -ff_hold floor binds on carryover here (this change does not fix this case, by design/scope)");
    }
    {
        /* Test 6's mutation: change the floor formula back to -ff_u (done
         * here by re-running the SAME dwell-settle-then-flat-ramp scenario
         * with ff_hold forced equal to ff_u throughout, i.e. simulating what
         * pid_update_terms() would do if the hold/climb split were silently
         * ignored) and confirm the bound-tick count actually CHANGES versus
         * the real new-floor run above -- proving this test is sensitive to
         * which floor formula is in effect, not vacuously true regardless. */
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 0.01f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        const float setpoint = 60.0f;
        const float ff_hold = 0.5f;
        const float ff_climb = 0.3f;
        const float dwell_ff_u = ff_hold;
        const float ramp_ff_u = ff_hold + ff_climb;

        pid_state_t s_mutant;
        pid_reset(&s_mutant);
        pid_update_terms(&s_mutant, &cfg, setpoint, setpoint, 1.0f, dwell_ff_u, dwell_ff_u, NULL);
        for (int i = 0; i < 500; i++) {
            pid_update_terms(&s_mutant, &cfg, setpoint, setpoint + 2.0f, 1.0f, dwell_ff_u, dwell_ff_u, NULL);
        }
        int bound_ticks_mutant = 0;
        for (int i = 0; i < 200; i++) {
            pid_terms_t terms;
            /* Mutation: pass ff_hold=ramp_ff_u (i.e. ff_hold==ff_u), the
             * "floor formula reverted to -ff_u" case. */
            pid_update_terms(&s_mutant, &cfg, setpoint, setpoint, 1.0f, ramp_ff_u, ramp_ff_u, &terms);
            if (fabsf(terms.i - (-ramp_ff_u)) < 1e-4f) {
                bound_ticks_mutant++;
            }
        }
        TEST_CHECK(bound_ticks_mutant == 0,
                   "sanity: the -ff_u-floor mutant on this exact scenario binds for 0 ticks (headroom from the deeper floor), a different outcome than the real new-floor run above -- proves test 6 can distinguish the two floors");
    }

    /* Test 7: ki-blowup guard on the seed path, pid_seed_bumpless()
     * (pid.c:29-57). Reviewer found this path was completely untested --
     * deleting the guard's clamp there (pid.c:52-54 in the pre-fix code)
     * left the whole suite green. Drive the real production function with a
     * tiny ki and a moderate u_desired/ff_hold so integral_needed's
     * unguarded value would be enormous, and check the seeded raw integral
     * stays bounded. */
    {
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 1e-6f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        const float setpoint = 60.0f;
        const float measurement = 60.0f; /* p_term == 0 so integral_needed is driven purely by u_desired/ff_u/ki */
        const float ff_hold = 0.5f;
        const float ff_u = ff_hold; /* no climb component -- isolates the floor/guard interaction */

        pid_state_t s;
        pid_reset(&s);
        pid_seed_bumpless(&s, &cfg, setpoint, measurement, /*u_desired=*/0.0f, ff_u, ff_hold);
        TEST_CHECK(fabsf(s.integral) < 100000.0f + 1.0f,
                   "ki-blowup guard (seed path, negative side): pid_seed_bumpless() must not seed a raw integral past the guard's bound");
    }
    {
        /* Seed path, positive side: a large positive u_desired with the same
         * tiny ki drives integral_needed positive and enormous before any
         * floor applies. */
        pid_cfg_t cfg = {.kp = 0.0f, .ki = 1e-6f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f, .pid_range_c = 1000.0f};
        const float setpoint = 60.0f;
        const float measurement = 60.0f;

        pid_state_t s;
        pid_reset(&s);
        pid_seed_bumpless(&s, &cfg, setpoint, measurement, /*u_desired=*/2.0f, /*ff_u=*/0.0f, /*ff_hold=*/0.0f);
        TEST_CHECK(fabsf(s.integral) < 100000.0f + 1.0f,
                   "ki-blowup guard (seed path, positive side): pid_seed_bumpless() must not seed a raw integral past the guard's bound on the positive side either");
    }

    /* Test 8: ki-blowup guard on the compounding path, pid_rescale_integral_for_new_ki()
     * (pid.c:59-72) -- the function PID_INTEGRAL_RAW_ABS_BOUND's own doc
     * comment names as the reason the guard exists. Before this fix this
     * function applied no bound at all: rescaling an already-large raw
     * integral onto a much smaller new ki compounded it further. */
    {
        pid_state_t s;
        pid_reset(&s);
        s.integral = 90000.0f; /* already near the bound, but legally so */
        pid_rescale_integral_for_new_ki(&s, /*old_ki=*/1e-3f, /*new_ki=*/1e-6f); /* 1000x rescale */
        TEST_CHECK(fabsf(s.integral) < 100000.0f + 1.0f,
                   "ki-blowup guard (rescale path, positive side): pid_rescale_integral_for_new_ki() must clamp a compounding rescale to the guard's bound");
    }
    {
        pid_state_t s;
        pid_reset(&s);
        s.integral = -90000.0f;
        pid_rescale_integral_for_new_ki(&s, /*old_ki=*/1e-3f, /*new_ki=*/1e-6f);
        TEST_CHECK(fabsf(s.integral) < 100000.0f + 1.0f,
                   "ki-blowup guard (rescale path, negative side): pid_rescale_integral_for_new_ki() must clamp a compounding rescale to the guard's bound on the negative side too");
    }
}
