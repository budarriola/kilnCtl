// Host tests closing gaps found auditing GUARD_TEST_MATRIX.md section 1
// ("Write the nuisance tests first") row by row against test_safety_guards.c
// (2026-09-04 pass). This file is separate from test_safety_guards.c on
// purpose: that file (and safety_guards.c/.h) were held by another
// concurrent session at audit time, so every addition below drives the
// real, unmodified safety_guards_tick() through its public header rather
// than touching either.
//
// Two rows were found genuinely uncovered by anything at the
// safety_guard_input_t/safety_guard_cfg_t boundary and are added here:
//
//  - S3/S4: "a 60s heater window at 15% duty, for an hour" -- the matrix's
//    own words call this "the single most important nuisance test in the
//    suite". test_safety_guards.c's existing S3/S4 coverage holds
//    relay_commanded_recently/_continuously at a constant value for the
//    whole run, which is a faithful *reduction* (the audit note in
//    GUARD_TEST_MATRIX.md's own completion checklist argues these booleans
//    arrive pre-windowed, so "recently" 100s ago and "recently" at any time
//    in the window are literally the same input) but never actually runs
//    the tick-by-tick correlation-window arithmetic the matrix describes
//    word for word. test_s3_s4_hour_at_various_duties() below reproduces it
//    directly: a real 3,600s clock, dt_s=0.1s (the real safety_core tick
//    period), an actual 60s on/off relay cycle at several duty values, and
//    a correlation window computed the same way link_task/safety_core
//    describe it (relay_commanded_recently true iff a commanded-on edge
//    happened within correlation_window_s of "now"). This is the test that
//    would have caught a correlation_window_s regressed below the 60s cycle
//    period -- the exact hazard GUARD_TEST_MATRIX.md section 1 calls out
//    ("the reason correlation_window_s is 150s rather than something that
//    'looks long enough'") -- where the constant-true reduction cannot.
//
//  - S10: the matrix's stated magnitude is "a 150C stratification held for
//    a whole firing", not the 50C offset the existing S10 nuisance test
//    uses. 150C is much closer to tc_disagreement_c's 200C default, so it
//    is the more searching case for a threshold-comparison bug (e.g. a
//    stray `>=` vs `>`, or a units mix-up that only shows up near the
//    boundary) -- and it is run for a full simulated 8-hour firing rather
//    than the 20 minutes the existing test covers.
//
// What is NOT added here, and why -- both are genuinely uncoverable at this
// module's boundary, not vacuous coverage dressed up as a limitation:
//
//  - S6a ("mainFault glitching for 100ms") and S7 ("30ms of contact bounce
//    on both edges"): safety_guards_tick() only ever sees an
//    already-debounced level (main_fault_asserted / estop_pressed), by
//    design -- see test_safety_guards.c's own test_s6()/test_s7() comments.
//    The actual debounce that must reject a sub-window glitch is
//    discrete_task.c's static debounce_update() (200ms window for
//    mainFault, 50ms for E-stop, firmware/SaftyFW/src/tasks/
//    discrete_task.c). That function is `static`, is called only from
//    discrete_task_fn() which is itself gated on FreeRTOS+RP2040 GPIO
//    headers, and is not declared in any header or referenced from any
//    file under test/ -- there is no host-reachable entry point for it at
//    all. Exercising the 100ms/30ms glitch-rejection property for real
//    would require either extracting debounce_update() into a pure,
//    header-exposed module (the same treatment relay_grace.c and
//    link_frame.c already got, per GUARD_TEST_MATRIX.md's completion
//    checklist) or a hardware bench test. Both are out of scope here: the
//    former edits discrete_task.c's public surface, which is a real
//    behavior-preserving refactor deserving its own reviewed change, not a
//    side effect of a test-only pass; the latter is explicitly listed as
//    the only avenue in GUARD_TEST_MATRIX.md's own S6a hardware row ("the
//    one guard virtual_dut/SimFW never could exercise"). Left unchecked.
//
//  - S9 ("current decays with the 1s peak-hold time constant"): 2026-09-04
//    audit found the "hardware-only" verdict above (and in
//    GUARD_TEST_MATRIX.md's own §3.4/checklist text) imprecise, the same
//    way two other claims turned out to be imprecise the same day. The
//    physical AD8542 + R77||C57 decay waveform genuinely needs hardware to
//    produce -- but safety_guards_tick()'s S9 branch (safety_guards.c,
//    the trip_ineffective block under `if (state->is_tripped)`) never
//    touches raw ADC counts at all: it consumes only
//    in->any_current_present (a bool), in->relay_deenergized,
//    in->context_valid and in->current_sensing_commissioned. That decision
//    is exactly the kind of physics-to-bool translation this file already
//    drives synthetically for S3/S4 above (current_decay_s, modelling the
//    same tau=1s peak-hold). test_s9_current_decay_after_normal_trip()
//    below reuses that technique with a real exponential (CURRENT_SENSE.md
//    section 3's own tau=1s / 37%-at-1s / 5%-at-3s / 1%-at-4.6s), proving
//    a genuinely healthy post-trip decay never escalates S9, with a
//    welded-contactor positive control proving S9 still can and does fire.
//
// S6b's "one dropped telemetry frame; three dropped frames with no current"
// row was audited too: link_task_link_up() (firmware/SaftyFW/src/tasks/
// link_task.c) is a pure elapsed-time check ("has any valid frame arrived
// within LINK_UP_RECENCY_MS"), not a frame-count check, so at
// safety_guards_tick()'s boundary a dropped frame or three is
// indistinguishable from "link still up, just quiet for under a second" --
// already covered by test_safety_guards.c's test_s6()'s "quiet link with no
// current does not trip before the backstop" case, which runs the link
// silent for far longer (115s) than a few missed frame periods. No new test
// needed for that row.
#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../src/safety_guards.h"

static safety_guard_input_t nuisance_base_input(void)
{
    safety_guard_input_t in;
    memset(&in, 0, sizeof(in));
    in.tc_valid = true;
    in.tc_c = 20.0f;
    in.cj_c = 25.0f;
    in.context_valid = false;
    in.any_current_present = false;
    in.relay_commanded_recently = false;
    in.relay_commanded_continuously = false;
    in.current_sensing_commissioned = true;
    in.sample_counter_advancing = true;
    in.link_up = true;
    in.dt_s = 0.1f; /* safety_core's real tick period */
    return in;
}

static safety_guard_cfg_t nuisance_base_cfg(void)
{
    safety_guard_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tc_placement_valid = true;
    cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
    cfg.abs_max_temp_c = 1300.0f;
    cfg.tc_source = SAFETY_TC_SOURCE_OWN_J7;
    return cfg;
}

// Runs one duty-cycled hour (or shorter, caller-specified, run_s) of a
// heater window at `duty_pct` percent, `window_s` seconds per cycle,
// against S3 and S4 together, using a correlation-window computation
// modelled the same way LINK_PROTOCOL.md section 4 / safety_link_frames.c
// describe relay_recent_mask: "relays commanded on at any point in the last
// correlation_window_s". current_present tracks the commanded relay with a
// tiny lag (the element does not respond instantaneously, but the exact lag
// does not matter here -- the correlation window's whole point is to absorb
// far bigger gaps than any real thermal lag). Returns true if either S3 or
// S4 ever trips.
// current_decay_s models CURRENT_SENSE.md section 5's own measured tail
// ("<5% within ~4s, tau ~= 1s") -- current does not vanish the instant the
// relay opens, it decays. force_recency_bug, when true, deliberately
// breaks the recency computation (used only by the negative test below to
// prove this harness actually catches the regression it exists to catch;
// always false in the real assertions).
static bool run_duty_window_ex(float duty_pct, float window_s, float run_s, float correlation_window_s,
                                float current_decay_s, bool force_recency_bug, bool *out_s4_warned)
{
    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = nuisance_base_cfg();
    cfg.correlation_window_s = correlation_window_s;

    float on_s = window_s * (duty_pct / 100.0f);
    float last_commanded_on_elapsed = 1.0e9f; /* "never yet", effectively outside any window */
    float continuous_on_elapsed = 0.0f;       /* time since the relay last transitioned to OFF, while it is currently ON */
    float since_off_elapsed = 1.0e9f;         /* time since the relay last transitioned to OFF at all */
    float cycle_t = 0.0f;
    const float dt = 0.1f;
    bool tripped = false;
    bool s4_warned = false;

    for (float t = 0.0f; t < run_s && !tripped; t += dt) {
        bool relay_on = cycle_t < on_s;

        if (relay_on) {
            last_commanded_on_elapsed = 0.0f;
            continuous_on_elapsed += dt;
            since_off_elapsed = 1.0e9f;
        } else {
            continuous_on_elapsed = 0.0f;
            if (since_off_elapsed > 1.0e8f) {
                since_off_elapsed = 0.0f; /* just transitioned off */
            } else {
                since_off_elapsed += dt;
            }
        }
        bool current_present = relay_on || (since_off_elapsed < current_decay_s);

        safety_guard_input_t in = nuisance_base_input();
        in.context_valid = true;
        in.dt_s = dt;
        in.any_current_present = current_present;
        in.relay_commanded_recently =
            force_recency_bug ? false : (last_commanded_on_elapsed <= correlation_window_s);
        in.relay_commanded_continuously = (continuous_on_elapsed >= correlation_window_s);

        tripped = safety_guards_tick(&s, &cfg, &in);
        if (s.s4_warn) {
            s4_warned = true;
        }

        last_commanded_on_elapsed += dt;
        cycle_t += dt;
        if (cycle_t >= window_s) {
            cycle_t = 0.0f;
        }
    }

    if (out_s4_warned) {
        *out_s4_warned = s4_warned;
    }
    return tripped;
}

static bool run_duty_window(float duty_pct, float window_s, float run_s, float correlation_window_s,
                             bool *out_s4_warned)
{
    return run_duty_window_ex(duty_pct, window_s, run_s, correlation_window_s, 1.5f, false, out_s4_warned);
}

static void test_s3_s4_hour_at_various_duties(void)
{
    TEST_SECTION("S3/S4 -- a real 60s heater window at 15% duty for an hour "
                  "(GUARD_TEST_MATRIX.md section 1's own words)");

    /* The exact scenario named in the matrix: 60s window, 15% duty, one
     * full simulated hour, correlation_window_s at its real default
     * (150s). Neither S3 nor S4 may ever trip; S4 must not even WARN,
     * because relay_commanded_continuously can only ever be true at (close
     * to) 100% duty. */
    {
        bool s4_warned = false;
        bool tripped = run_duty_window(15.0f, 60.0f, 3600.0f, 150.0f, &s4_warned);
        TEST_CHECK(!tripped, "60s window @ 15% duty for a full simulated hour never trips S3 or S4");
        TEST_CHECK(!s4_warned, "15% duty never even reaches S4's WARN (relay is never commanded continuously)");
    }

    /* GUARD_TEST_MATRIX.md section 1's S4 row: "The same, plus every duty
     * from 5% to 95%." Sweep it -- a shorter run per duty (20 minutes,
     * comfortably more than one full 150s correlation window) is enough
     * once the hour-long case above has proven the long-run shape. */
    {
        const float duties[] = { 5.0f, 15.0f, 25.0f, 35.0f, 50.0f, 65.0f, 75.0f, 85.0f, 95.0f };
        for (size_t i = 0; i < sizeof(duties) / sizeof(duties[0]); i++) {
            bool s4_warned = false;
            bool tripped = run_duty_window(duties[i], 60.0f, 1200.0f, 150.0f, &s4_warned);
            TEST_CHECK(!tripped, "60s window at this duty (5-95% sweep) never trips S3/S4");
            TEST_CHECK(!s4_warned, "this duty never reaches S4's WARN either");
        }
    }

    /* The property GUARD_TEST_MATRIX.md section 1 explicitly credits for
     * why this works at all: "correlation_window_s is 150s rather than
     * something that 'looks long enough'" -- a 60s cycle leaves the OFF
     * span (up to 57s at 5% duty) well inside a 150s correlation window, so
     * relay_commanded_recently never lapses. Prove the margin is real, not
     * assumed, by shrinking correlation_window_s toward the cycle period:
     * a 65s correlation window is still (barely) longer than the 60s
     * cycle, so it must still never trip. */
    {
        bool tripped = run_duty_window(15.0f, 60.0f, 1200.0f, 65.0f, NULL);
        TEST_CHECK(!tripped, "correlation_window_s just longer than the cycle period still absorbs the gaps");
    }
}

static void test_s3_s4_positive_controls(void)
{
    TEST_SECTION("S3/S4 -- POSITIVE CONTROLS: the duty harness above can actually register a trip/WARN");

    /* Without this, every "never trips" assertion in
     * test_s3_s4_hour_at_various_duties() is indistinguishable from S3
     * being structurally inert in this harness's configuration -- and it
     * nearly is: current_present tracks the commanded relay closely, so
     * S3's (current present AND nothing commanded recently) conjunction is
     * never satisfied there. force_recency_bug is exactly the regression
     * those tests exist to catch (a correlation window that stops covering
     * the OFF span, so relay_commanded_recently lapses while current is
     * still flowing); drive it deliberately and S3 MUST trip. 50% duty
     * gives a 30s ON phase, past stuck_on_time_s's 20s default. */
    {
        bool tripped = run_duty_window_ex(50.0f, 60.0f, 1200.0f, 150.0f, 1.5f, true, NULL);
        TEST_CHECK(tripped, "with relay_commanded_recently wrongly false, current during the ON phase "
                             "DOES trip S3 -- proves the nuisance runs above were rejecting something "
                             "the harness can genuinely produce, not passing on an inert guard");
    }
    {
        /* Same duty, recency computed correctly: must not trip. The pair
         * differs in exactly one input, so the passing case cannot be
         * explained by anything but the guard's own decision. */
        bool tripped = run_duty_window_ex(50.0f, 60.0f, 1200.0f, 150.0f, 1.5f, false, NULL);
        TEST_CHECK(!tripped, "the same run with a correct recency computation does not trip");
    }

    /* S4's WARN can never be reached by a 60s-window duty cycle (the ON
     * phase caps at 57s, far short of the 150s correlation window), which
     * is why every duty above asserts !s4_warned. That makes those
     * assertions vacuous on their own -- so prove separately that s4_warn
     * is reachable at all with these same inputs. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = nuisance_base_cfg();
        safety_guard_input_t in = nuisance_base_input();
        in.context_valid = true;
        in.relay_commanded_continuously = true;
        in.any_current_present = false; /* dead element */
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(s.s4_warn, "commanded continuously with no current DOES raise S4's WARN -- so the "
                               "!s4_warned assertions above are rejecting a reachable state");
        TEST_CHECK(!tripped, "and S4 warns without ever tripping, by design");
    }
}

static void test_s10_150c_stratification_whole_firing(void)
{
    TEST_SECTION("S10 -- a 150C stratification held for a whole firing "
                  "(GUARD_TEST_MATRIX.md section 1's stated magnitude, not a smaller offset)");

    /* 150C is well below tc_disagreement_c's 200C default, but much closer
     * to it than the 50C offset test_safety_guards.c's own S10 nuisance
     * case uses -- the more searching value for a boundary-comparison bug.
     * Run it for a simulated 8-hour firing (dt_s=60s x 480 ticks). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = nuisance_base_cfg();
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = nuisance_base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.tc_c = 900.0f;
        in.max_zone_setpoint_c = 900.0f; /* keep S2 out of this test */
        in.nearest_zone_measured_c = 750.0f; /* exactly 150C off */
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 480 && !tripped; i++) { /* 480*60s = 8 hours */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "S10 never trips (WARN only) at 150C sustained for a whole 8h firing");
        TEST_CHECK(!s.s10_warn, "150C is still under tc_disagreement_c(200C) -- no WARN either");
    }

    /* Same 150C offset, but in EXTERNAL_OVERHEAT -- must stay silent for
     * the same reason the existing (larger-offset) EXTERNAL_OVERHEAT case
     * does: a shell TC has no obligation to agree with the chamber. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = nuisance_base_cfg();
        cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
        safety_guard_input_t in = nuisance_base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.tc_c = 750.0f;
        in.nearest_zone_measured_c = 900.0f; /* 150C off */
        in.dt_s = 60.0f;
        for (int i = 0; i < 480; i++) safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!s.s10_warn, "150C disagreement in EXTERNAL_OVERHEAT: S10 stays off for the full 8h firing");
    }
}

static void test_s10_positive_control(void)
{
    TEST_SECTION("S10 -- POSITIVE CONTROL: a disagreement past tc_disagreement_c DOES raise the WARN");

    /* The 150C cases above assert only "no WARN". Identical setup with the
     * offset pushed past tc_disagreement_c's 200C default must warn, or
     * "no WARN at 150C" would be equally true of an S10 that can never
     * warn at all in this configuration (wrong placement mode, zone_count
     * ignored, nearest_zone_measured_c never read). */
    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = nuisance_base_cfg();
    cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
    safety_guard_input_t in = nuisance_base_input();
    in.context_valid = true;
    in.zone_count = 1;
    in.tc_c = 900.0f;
    in.max_zone_setpoint_c = 900.0f; /* keep S2 out of this test */
    in.nearest_zone_measured_c = 650.0f; /* 250C off -- past the 200C default */
    in.dt_s = 60.0f;
    bool tripped = false;
    for (int i = 0; i < 10 && !tripped; i++) { /* 600s > tc_disagreement_time_s (300s) */
        tripped = safety_guards_tick(&s, &cfg, &in);
    }
    TEST_CHECK(s.s10_warn, "250C sustained past tc_disagreement_time_s raises S10's WARN");
    TEST_CHECK(!tripped, "S10 is WARN-only and still never trips");
}

// -----------------------------------------------------------------------
// S9 -- "a normal trip where current decays with the 1s peak-hold time
// constant" (GUARD_TEST_MATRIX.md section 1's own row). See this file's
// header comment for why the earlier "hardware-only" verdict was
// imprecise: safety_guards_tick()'s S9 branch consumes only
// in->any_current_present (bool) plus relay_deenergized/context_valid/
// current_sensing_commissioned -- none of which requires the physical CT.
//
// amps(t)/i_present_a = initial_ratio * exp(-t/tau), tau = 1.0s, per
// CURRENT_SENSE.md section 3 ("Exponential decay, tau = 1s. 37% at 1s, 5%
// at 3s, 1% at 4.6s"). "Present" iff still above the i_present_a threshold
// (ratio > 1). initial_ratio models how many multiples of i_present_a
// (default 2.0A) the running current was before the relay opened.
static bool current_present_after_decay(float elapsed_s, float initial_ratio)
{
    if (elapsed_s < 0.0f) {
        return true;
    }
    float ratio = initial_ratio * expf(-elapsed_s / 1.0f);
    return ratio > 1.0f;
}

static void test_s9_current_decay_after_normal_trip(void)
{
    TEST_SECTION("S9 -- current decays with the 1s peak-hold time constant after a normal trip "
                  "(GUARD_TEST_MATRIX.md section 1's own S9 row)");

    /* Nuisance: a genuinely healthy shutdown. Trip via S7 (E-stop), K4
     * reports de-energized from the same tick on (the realistic case), and
     * the CT reading decays exponentially from a representative running
     * current. initial_ratio=15 models a ~30A element against the default
     * i_present_a=2.0A threshold: current crosses back under the threshold
     * at tau*ln(15) ~= 2.7s, nowhere near trip_verify_s's 10s. Run well
     * past trip_verify_s plus the streak debounce to prove it never
     * escalates across the whole window, not just at one sampled instant. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = nuisance_base_cfg();

        safety_guard_input_t estop = nuisance_base_input();
        estop.estop_pressed = true;
        bool tripped_now = safety_guards_tick(&s, &cfg, &estop);
        TEST_CHECK(tripped_now && s.is_tripped, "sanity: tripped via S7 (E-stop)");

        const float dt = 0.1f;
        const float run_s = 20.0f; /* comfortably > trip_verify_s(10s) + decay tail */
        bool escalated = false;
        bool any_current_present_seen = false;
        bool verify_window_progressed_past_trip_verify_s = false;
        for (float t = 0.0f; t < run_s && !escalated; t += dt) {
            safety_guard_input_t in = nuisance_base_input();
            in.context_valid = true;
            in.current_sensing_commissioned = true;
            in.relay_deenergized = true; /* K4 verifiably open from t=0 */
            in.dt_s = dt;
            in.any_current_present = current_present_after_decay(t, 15.0f);
            if (in.any_current_present) {
                any_current_present_seen = true;
            }
            escalated = safety_guards_tick(&s, &cfg, &in);
            if (s.s9_verify_elapsed_s >= 10.0f) {
                verify_window_progressed_past_trip_verify_s = true;
            }
        }
        TEST_CHECK(!escalated, "a normal tau=1s current decay after K4 opens never escalates S9");
        TEST_CHECK(!s.trip_ineffective, "trip_ineffective stays false through the whole decay + long tail");
        TEST_CHECK(any_current_present_seen,
                   "S9 was LIVE for this run: any_current_present was true for part of it (proves the guard "
                   "actually had something to reject, not an inert always-false input)");
        TEST_CHECK(verify_window_progressed_past_trip_verify_s,
                   "S9's own verify window (trip_verify_s) was actually reached during the run -- the guard "
                   "was armed and watching, not skipped");
    }

    /* Positive control: the welded-contactor case, using the SAME
     * decay-model helper but with current that never decays below the
     * threshold (a stuck/shorted reading) -- current genuinely persists
     * past trip_verify_s, and S9 MUST escalate. Without this, "never
     * escalated" above would be indistinguishable from S9 being unable to
     * fire at all in this configuration -- the inert-guard trap
     * GUARD_TEST_MATRIX.md itself warns about. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = nuisance_base_cfg();

        safety_guard_input_t estop = nuisance_base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);
        TEST_CHECK(s.is_tripped, "sanity: tripped via S7 (E-stop)");

        const float dt = 0.1f;
        const float run_s = 20.0f;
        bool escalated = false;
        for (float t = 0.0f; t < run_s && !escalated; t += dt) {
            safety_guard_input_t in = nuisance_base_input();
            in.context_valid = true;
            in.current_sensing_commissioned = true;
            in.relay_deenergized = true;
            in.dt_s = dt;
            in.any_current_present = true; /* welded contactor: never decays */
            escalated = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(escalated, "current that genuinely persists (welded contactor) DOES escalate S9 -- proves "
                               "the nuisance test above was not passing because S9 can never fire");
        TEST_CHECK(s.trip_ineffective, "trip_ineffective latches on the welded-contactor control");
        TEST_CHECK(s.reason == SAFETY_TRIP_INEFFECTIVE, "reason escalates to SAFETY_TRIP_INEFFECTIVE");
    }
}

void run_test_guard_nuisance(void)
{
    test_s3_s4_hour_at_various_duties();
    test_s3_s4_positive_controls();
    test_s10_150c_stratification_whole_firing();
    test_s10_positive_control();
    test_s9_current_decay_after_normal_trip();
}
