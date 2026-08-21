// Host tests pinning the pure arithmetic/composition rules added to
// sim_engine.c and fault_sched.c by the sim_engine/fault_sched gap-closure
// pass (safety-TC blend+lag+fault-override+MANUAL; the thermal-mass-surprise
// C<=0 guard; the MAIN_SAFETY_DISAGREE offset/gain composition rule; and the
// WELDED_K4_CURRENT_PERSIST vs PHASE_LOSS CT-channel precedence rule).
//
// Why this file mirrors rather than calls the real code: sim_engine.c and
// fault_sched.c are FreeRTOS task files (they include FreeRTOS.h/queue.h/
// semphr.h/task.h and call i2c_owner.h/wave_owner.h task APIs), so they are
// not part of this host-test harness's source list -- build_host_tests.ps1
// compiles only src/sim/'s pure modules, the exact same pure/task boundary
// every existing test_*.c here already respects (there is no test_sim_engine
// .c or test_fault_sched.c in this repo either, for the same reason: those
// two task files have zero pre-existing host coverage, verified integration-
// only on hardware, same as every other SimFW/SaftyFW FreeRTOS task file).
// Each function below is a deliberately small, byte-for-byte mirror of the
// corresponding block in the real .c file (cited in each function's comment)
// -- keep the two in sync by hand if either changes.
#include <stdbool.h>
#include <string.h>

#include "test_common.h"

// Mirrors sim_engine.c's sim_engine_tick() "Safety-side TC" block: one
// first-order lag step toward blend_target (same formula as
// thermal_model.c's own TC lag: dT/dt = (target - current) / lag_s),
// then the fault_sched-only offset/gain override, then MANUAL (always wins
// if active).
static float safety_tc_step(float blend_target, float lag_s, float dt_s, float prev_c,
                             bool fault_active, float fault_offset_c, float fault_gain,
                             bool manual_active, float manual_temp_c)
{
    float next_c;
    if (lag_s > 0.0f) {
        float dT_dt = (blend_target - prev_c) / lag_s;
        next_c = prev_c + dT_dt * dt_s;
    } else {
        next_c = blend_target;
    }

    float reported = next_c;
    if (fault_active) {
        reported = reported * fault_gain + fault_offset_c;
    }
    if (manual_active) {
        reported = manual_temp_c;
    }
    return reported;
}

// Mirrors sim_engine.c's sim_engine_tick() thermal-mass-surprise/tc-lag-
// stress override application (the block right after the health-override
// application, inside the eff_params zone loop). Only the thermal one needs
// mirroring here -- tc_lag has no clamp to test (thermal_model.c already
// treats any value <= 0 as "no lag", verbatim passthrough is correct).
static void thermal_override_apply(bool active, float C_over, float k_loss_over, float *C, float *k_loss)
{
    if (active) {
        if (C_over > 0.0f) {
            *C = C_over;
        }
        *k_loss = k_loss_over;
    }
}

// Mirrors fault_sched.c's recompute_overrides_locked() FAULT_SCHED_TYPE_
// MAIN_SAFETY_DISAGREE case + the post-loop
// sim_engine_set_safety_tc_fault_override() call: offsets accumulate
// additively across every ACTIVE slot of this type (slot order, matching
// DESIGN_NOTES.md 7.3's deterministic tiebreak), and params[1]==0.0 (the value an
// unspecified gain param is left at) means "leave the default gain of 1.0
// alone" rather than "use gain 0".
static void safety_disagree_compose(const float *offsets, const float *gains, int n,
                                     bool *out_active, float *out_offset, float *out_gain)
{
    bool active = false;
    float offset = 0.0f;
    float gain = 1.0f;
    for (int i = 0; i < n; i++) {
        active = true;
        offset += offsets[i];
        if (gains[i] != 0.0f) {
            gain = gains[i];
        }
    }
    *out_active = active;
    *out_offset = offset;
    *out_gain = active ? gain : 1.0f;
}

// Mirrors fault_sched.c's recompute_overrides_locked() CT-channel loop's
// WELDED_K4_CURRENT_PERSIST precedence check: PHASE_LOSS (force to zero)
// wins over a same-channel K4-weld request, same "more severe/definite
// claim wins" doctrine PHASE_LOSS already applies against HALF_WAVE_SSR and
// STUCK_OPEN_RELAY already applies against WELDED_RELAY.
static bool ct_k4_weld_effective(bool k4_weld_requested, bool phase_loss_requested)
{
    return k4_weld_requested && !phase_loss_requested;
}

static void test_safety_tc_no_lag_tracks_instantly(void)
{
    TEST_SECTION("gap-closure -- safety-TC step, lag_s <= 0");

    float out = safety_tc_step(123.0f, 0.0f, 0.1f, 25.0f, false, 0.0f, 1.0f, false, 0.0f);
    TEST_CHECK_NEAR(out, 123.0f, 1e-6, "lag_s<=0 tracks the blend target instantaneously");

    out = safety_tc_step(50.0f, -5.0f, 0.1f, 25.0f, false, 0.0f, 1.0f, false, 0.0f);
    TEST_CHECK_NEAR(out, 50.0f, 1e-6, "negative lag_s is treated the same as zero");
}

static void test_safety_tc_first_order_lag(void)
{
    TEST_SECTION("gap-closure -- safety-TC step, first-order lag");

    // dT/dt = (target - prev) / lag_s; one dt_s=0.1s step at lag_s=5s from
    // prev=25 toward target=125 -> delta = (125-25)/5 * 0.1 = 2.0
    float out = safety_tc_step(125.0f, 5.0f, 0.1f, 25.0f, false, 0.0f, 1.0f, false, 0.0f);
    TEST_CHECK_NEAR(out, 27.0f, 1e-4, "one lag step matches (target-prev)/lag_s*dt_s");

    // Already at the target: no further movement.
    out = safety_tc_step(125.0f, 5.0f, 0.1f, 125.0f, false, 0.0f, 1.0f, false, 0.0f);
    TEST_CHECK_NEAR(out, 125.0f, 1e-4, "steady state: value at target stays at target");
}

static void test_safety_tc_fault_override_gain_offset(void)
{
    TEST_SECTION("gap-closure -- safety-TC fault override (offset/gain)");

    // prev already at target (no lag movement this step) so the override's
    // effect on the raw lag output is isolated: reported = 100*1.5 + 80.
    float out = safety_tc_step(100.0f, 5.0f, 0.1f, 100.0f, true, 80.0f, 1.5f, false, 0.0f);
    TEST_CHECK_NEAR(out, 230.0f, 1e-4, "fault override applies gain then offset to the lag output");

    // DESIGN_NOTES.md section 8 scenario 8: a pure +80 degC skew (gain left at 1.0).
    out = safety_tc_step(400.0f, 5.0f, 0.1f, 400.0f, true, 80.0f, 1.0f, false, 0.0f);
    TEST_CHECK_NEAR(out, 480.0f, 1e-4, "gain==1.0 leaves the pure offset skew from main_safety_skew.yaml intact");
}

static void test_safety_tc_manual_wins(void)
{
    TEST_SECTION("gap-closure -- safety-TC MANUAL override always wins");

    float out = safety_tc_step(400.0f, 5.0f, 0.1f, 400.0f, true, 80.0f, 2.0f, true, 999.0f);
    TEST_CHECK_NEAR(out, 999.0f, 1e-6, "MANUAL pins the reported value regardless of blend/lag/fault override");

    out = safety_tc_step(400.0f, 5.0f, 0.1f, 400.0f, false, 0.0f, 1.0f, true, -40.0f);
    TEST_CHECK_NEAR(out, -40.0f, 1e-6, "MANUAL wins even with no fault override active");
}

static void test_thermal_mass_surprise_clamp(void)
{
    TEST_SECTION("gap-closure -- thermal-mass-surprise C<=0 guard");

    float C = 2000.0f, k_loss = 8.0f;
    thermal_override_apply(true, 500.0f, 20.0f, &C, &k_loss);
    TEST_CHECK_NEAR(C, 500.0f, 1e-6, "a positive forced C is applied");
    TEST_CHECK_NEAR(k_loss, 20.0f, 1e-6, "k_loss is applied verbatim alongside a positive C");

    C = 2000.0f;
    k_loss = 8.0f;
    thermal_override_apply(true, 0.0f, 20.0f, &C, &k_loss);
    TEST_CHECK_NEAR(C, 2000.0f, 1e-6, "C==0 is ignored (stored C stays in effect, avoids a divide-by-zero downstream)");
    TEST_CHECK_NEAR(k_loss, 20.0f, 1e-6, "k_loss is still applied even when the C half of the override is ignored");

    C = 2000.0f;
    k_loss = 8.0f;
    thermal_override_apply(true, -1.0f, 0.0f, &C, &k_loss);
    TEST_CHECK_NEAR(C, 2000.0f, 1e-6, "negative C is ignored the same as C==0");
    TEST_CHECK_NEAR(k_loss, 0.0f, 1e-6, "k_loss==0 is a legitimate override value (no clamp)");

    C = 2000.0f;
    k_loss = 8.0f;
    thermal_override_apply(false, 500.0f, 20.0f, &C, &k_loss);
    TEST_CHECK_NEAR(C, 2000.0f, 1e-6, "inactive override touches nothing");
    TEST_CHECK_NEAR(k_loss, 8.0f, 1e-6, "inactive override touches nothing (k_loss)");
}

static void test_main_safety_disagree_composition(void)
{
    TEST_SECTION("gap-closure -- MAIN_SAFETY_DISAGREE offset/gain composition");

    bool active;
    float offset, gain;

    // No active slots.
    safety_disagree_compose(NULL, NULL, 0, &active, &offset, &gain);
    TEST_CHECK(!active, "no slots -> not active");
    TEST_CHECK_NEAR(gain, 1.0f, 1e-6, "no slots -> gain reported as the neutral default");

    // Single slot, offset only (params[1] left at 0 -- the scenario's own
    // documented usage, main_safety_skew.yaml's "+80 degC").
    {
        float offsets[1] = { 80.0f };
        float gains[1] = { 0.0f };
        safety_disagree_compose(offsets, gains, 1, &active, &offset, &gain);
        TEST_CHECK(active, "single offset-only slot is active");
        TEST_CHECK_NEAR(offset, 80.0f, 1e-6, "offset passes through verbatim");
        TEST_CHECK_NEAR(gain, 1.0f, 1e-6, "params[1]==0 means gain defaults to 1.0, not 0.0");
    }

    // Two concurrent slots: offsets accumulate additively; the later slot's
    // explicit gain wins over the earlier one's.
    {
        float offsets[2] = { 30.0f, 50.0f };
        float gains[2] = { 2.0f, 3.0f };
        safety_disagree_compose(offsets, gains, 2, &active, &offset, &gain);
        TEST_CHECK(active, "two concurrent slots -> active");
        TEST_CHECK_NEAR(offset, 80.0f, 1e-6, "offsets accumulate additively across concurrent slots");
        TEST_CHECK_NEAR(gain, 3.0f, 1e-6, "later slot's explicit gain overrides the earlier one's (slot-order tiebreak)");
    }

    // A later slot with an unspecified gain (0.0) does not clobber an
    // earlier slot's explicit gain.
    {
        float offsets[2] = { 10.0f, 10.0f };
        float gains[2] = { 4.0f, 0.0f };
        safety_disagree_compose(offsets, gains, 2, &active, &offset, &gain);
        TEST_CHECK_NEAR(gain, 4.0f, 1e-6, "an unspecified (0.0) gain on a later slot does not override an earlier explicit gain");
    }
}

static void test_welded_k4_vs_phase_loss_precedence(void)
{
    TEST_SECTION("gap-closure -- WELDED_K4_CURRENT_PERSIST vs PHASE_LOSS precedence");

    TEST_CHECK(ct_k4_weld_effective(true, false), "K4-weld alone is effective");
    TEST_CHECK(!ct_k4_weld_effective(false, true), "phase-loss alone: K4-weld was never requested");
    TEST_CHECK(!ct_k4_weld_effective(true, true), "phase-loss wins when both target the same channel");
    TEST_CHECK(!ct_k4_weld_effective(false, false), "neither requested -> not effective");
}

void run_test_gap_closure_logic(void)
{
    test_safety_tc_no_lag_tracks_instantly();
    test_safety_tc_first_order_lag();
    test_safety_tc_fault_override_gain_offset();
    test_safety_tc_manual_wins();
    test_thermal_mass_surprise_clamp();
    test_main_safety_disagree_composition();
    test_welded_k4_vs_phase_loss_precedence();
}
