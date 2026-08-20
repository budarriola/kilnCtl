// Host tests pinning the K4-safety-pilot-relay gating rules added to
// sim_engine.c's sim_engine_tick() (duty[]/current_a[] now consult K4, not
// just K1/K2/K3), plus the composition rule that keeps
// FAULT_SCHED_TYPE_WELDED_K4_CURRENT_PERSIST (fault_sched.c) working
// correctly once that gating exists.
//
// Why this file mirrors rather than calls the real code: see
// test_gap_closure_logic.c's file header -- sim_engine.c and fault_sched.c
// are FreeRTOS task files, out of this host-test harness's source list.
// Each function below is a deliberately small, byte-for-byte mirror of the
// corresponding block in the real .c file (cited in each function's
// comment) -- keep the two in sync by hand if either changes.
#include <stdbool.h>
#include <string.h>

#include "test_common.h"

// Mirrors sim_engine.c's sim_engine_tick(): base relay-derived duty for one
// zone (K1/K2/K3 <-> zone0/1/2), then the fault_sched duty override if
// active, then K4's veto -- open K4 forces duty to 0 regardless of the base
// relay reading or any override (docs/PLAN.md's "closed AND K4 permits").
static float zone_duty(bool zone_relay_closed, bool duty_override_active, float duty_override_value,
                        bool k4_closed)
{
    float duty = zone_relay_closed ? 1.0f : 0.0f;
    if (duty_override_active) {
        duty = duty_override_value;
    }
    if (!k4_closed) {
        duty = 0.0f;
    }
    return duty;
}

// Mirrors sim_engine.c's current_a[] formula (PLAN.md 3.3), fed the
// *effective* (already K4-gated) duty -- so K4 opening zeroes both heat and
// CT current together, from the same duty[] value, with no separate K4
// check needed in this formula itself.
static float zone_current_a(float v_mains, float r_element, float duty, float element_health)
{
    if (r_element <= 0.0f) {
        return 0.0f;
    }
    return (v_mains / r_element) * duty * element_health;
}

// Mirrors wave_owner.c's CT-channel amplitude selection (ct_wave_tick()-ish
// read path) composed with fault_sched.c's WELDED_K4_CURRENT_PERSIST
// handling: while that fault is active, fault_sched.c drives the channel
// into CT_WAVE_MODE_MANUAL with a forced amps value (its own edge-tracked
// ct_wave_set_mode()/_set_amps() calls), so wave_owner.c never reads
// snap.zones[ch].current_a for that channel at all -- the S9 "current
// persists after K4 opens despite a welded contactor" case is expressed
// entirely below sim_engine.c's duty/current_a computation, on a completely
// separate path, and is therefore unaffected by K4 gating duty/current_a
// upstream.
static float ct_channel_amps(bool manual_mode, float manual_amps, float model_current_a)
{
    return manual_mode ? manual_amps : model_current_a;
}

static void test_k4_closed_zone_relay_closed_heats(void)
{
    TEST_SECTION("K4 gating -- K4 closed + zone relay closed -> heat and current");

    float duty = zone_duty(/*zone_relay_closed=*/true, /*override_active=*/false, 0.0f, /*k4_closed=*/true);
    TEST_CHECK_NEAR(duty, 1.0f, 1e-6, "K4 closed + relay closed -> full duty");

    float amps = zone_current_a(240.0f, 24.0f, duty, 1.0f);
    TEST_CHECK_NEAR(amps, 10.0f, 1e-6, "duty=1, healthy element -> V/R current flows");
}

static void test_k4_open_zone_relay_closed_no_heat(void)
{
    TEST_SECTION("K4 gating -- K4 open -> no heat, no current, even with the zone relay closed");

    float duty = zone_duty(/*zone_relay_closed=*/true, /*override_active=*/false, 0.0f, /*k4_closed=*/false);
    TEST_CHECK_NEAR(duty, 0.0f, 1e-6, "K4 open forces duty to 0 regardless of the zone relay reading");

    float amps = zone_current_a(240.0f, 24.0f, duty, 1.0f);
    TEST_CHECK_NEAR(amps, 0.0f, 1e-6, "zero duty -> zero CT current");
}

static void test_k4_open_zone_relay_open_still_no_heat(void)
{
    TEST_SECTION("K4 gating -- K4 open + zone relay open -> still no heat (both agree)");

    float duty = zone_duty(false, false, 0.0f, false);
    TEST_CHECK_NEAR(duty, 0.0f, 1e-6, "both open -> duty stays 0");
}

static void test_k4_open_overrides_veto_duty_force1(void)
{
    TEST_SECTION("K4 gating -- K4 open vetoes a fault_sched duty-force-1 override (welded zone relay / runaway)");

    // WELDED_RELAY/RUNAWAY_ZONE forces duty=1 regardless of the zone relay
    // reading -- but K4 is the last-resort mechanical cutoff and must still
    // win: a welded *zone* relay or a runaway heater cannot conduct once the
    // safety pilot has opened.
    float duty = zone_duty(/*zone_relay_closed=*/false, /*override_active=*/true, 1.0f, /*k4_closed=*/false);
    TEST_CHECK_NEAR(duty, 0.0f, 1e-6, "K4 open overrides even a forced-on duty override");

    float amps = zone_current_a(240.0f, 24.0f, duty, 1.0f);
    TEST_CHECK_NEAR(amps, 0.0f, 1e-6, "current follows the K4-gated duty, not the raw override");
}

static void test_k4_closed_duty_override_still_applies(void)
{
    TEST_SECTION("K4 gating -- K4 closed lets a fault_sched duty override still take effect");

    // STUCK_OPEN_RELAY forces duty=0 even though the relay reads closed;
    // K4 being closed does not change that -- K4 only ever forces duty
    // toward 0, never toward 1.
    float duty = zone_duty(/*zone_relay_closed=*/true, /*override_active=*/true, 0.0f, /*k4_closed=*/true);
    TEST_CHECK_NEAR(duty, 0.0f, 1e-6, "K4 closed does not resurrect a duty-force-0 override");

    // PARTIAL_ELEMENT-style forced partial duty still passes through with
    // K4 closed.
    duty = zone_duty(true, true, 0.5f, true);
    TEST_CHECK_NEAR(duty, 0.5f, 1e-6, "K4 closed lets a partial duty override through unchanged");
}

static void test_welded_k4_current_persist_bypasses_gating(void)
{
    TEST_SECTION("K4 gating -- welded-K4 fault (S9): current persists through a welded contactor despite K4 open");

    // sim_engine.c's own duty/current_a computation: K4 reads open, so this
    // zone's model-path current collapses to 0, exactly as it should (the
    // *pilot relay* correctly commanded the cutoff).
    float duty = zone_duty(/*zone_relay_closed=*/true, /*override_active=*/false, 0.0f, /*k4_closed=*/false);
    float model_amps = zone_current_a(240.0f, 24.0f, duty, 1.0f);
    TEST_CHECK_NEAR(model_amps, 0.0f, 1e-6, "sim_engine's own model path correctly sees K4 open as zero current");

    // fault_sched.c's WELDED_K4_CURRENT_PERSIST puts the CT channel into
    // MANUAL with a forced amps value -- independent of (and not fighting)
    // the model path above, which is exactly why S9 (contactor physically
    // welded, current keeps flowing despite K4's sense reading open) can
    // still be expressed even though K4 gating now zeroes the model path.
    float amps = ct_channel_amps(/*manual_mode=*/true, /*manual_amps=*/8.5f, model_amps);
    TEST_CHECK_NEAR(amps, 8.5f, 1e-6, "the welded-contactor fault's forced CT amps read through regardless of the K4-gated model current");

    // And with the fault inactive again (CLEARED), the channel goes back to
    // MODEL mode and correctly reports the K4-gated zero.
    amps = ct_channel_amps(/*manual_mode=*/false, 8.5f, model_amps);
    TEST_CHECK_NEAR(amps, 0.0f, 1e-6, "once the fault clears, the channel reports the model's own (K4-gated) current again");
}

static void test_k4_gating_composes_with_health_override(void)
{
    TEST_SECTION("K4 gating -- composes with the existing element-health override");

    // BROKEN_ELEMENT-style health override (health forced to 0) combined
    // with K4 closed: current is 0 because the element is dead, not because
    // of K4.
    float duty = zone_duty(true, false, 0.0f, true);
    float amps = zone_current_a(240.0f, 24.0f, duty, /*element_health=*/0.0f);
    TEST_CHECK_NEAR(amps, 0.0f, 1e-6, "K4 closed + dead element -> zero current from the health override alone");

    // Partial health (PARTIAL_ELEMENT) with K4 closed: current scales by
    // the health fraction.
    duty = zone_duty(true, false, 0.0f, true);
    amps = zone_current_a(240.0f, 24.0f, duty, /*element_health=*/0.5f);
    TEST_CHECK_NEAR(amps, 5.0f, 1e-6, "K4 closed + half-healthy element -> half the healthy current");

    // Same partial health, but K4 open: health no longer matters, current
    // is 0 because duty was already gated to 0.
    duty = zone_duty(true, false, 0.0f, false);
    amps = zone_current_a(240.0f, 24.0f, duty, 0.5f);
    TEST_CHECK_NEAR(amps, 0.0f, 1e-6, "K4 open zeroes current regardless of element health");
}

void run_test_k4_gating_logic(void)
{
    test_k4_closed_zone_relay_closed_heats();
    test_k4_open_zone_relay_closed_no_heat();
    test_k4_open_zone_relay_open_still_no_heat();
    test_k4_open_overrides_veto_duty_force1();
    test_k4_closed_duty_override_still_applies();
    test_welded_k4_current_persist_bypasses_gating();
    test_k4_gating_composes_with_health_override();
}
