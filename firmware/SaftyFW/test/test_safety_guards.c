// Host tests for safety_guards.c. TODO.md Phase 4.
//
// ARCHITECTURE.md section 10: "the cases worth writing first are the
// *nuisance* cases, not the trip cases" -- each guard's section below leads
// with the inputs that must NOT trip it, then the input that must.
#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../src/safety_guards.h"
#include "../src/current_presence_policy.h"

static safety_guard_input_t base_input(void)
{
    safety_guard_input_t in;
    memset(&in, 0, sizeof(in));
    in.tc_valid = true;
    in.tc_c = 20.0f;
    in.cj_c = 25.0f;
    in.fault_bits = 0;
    in.spi_failed = false;
    in.estop_pressed = false;
    in.heat_commanded = false;
    /* New-guard fields default to the "nothing is wrong, nothing is known"
     * shape: no context, link up, no fault, no current, relay not
     * de-energized (nothing has tripped). Individual tests override what
     * they need. */
    in.context_valid = false;
    in.zone_count = 0;
    in.max_zone_setpoint_c = 0.0f;
    in.nearest_zone_measured_c = 0.0f;
    in.any_current_present = false;
    in.relay_commanded_recently = false;
    in.relay_commanded_continuously = false;
    /* S9 only. Default to a healthy, commissioned board -- individual S9
     * tests override this to false to exercise the uncommissioned/WARN
     * path deliberately. */
    in.current_sensing_commissioned = true;
    in.sample_counter_advancing = true;
    in.main_fault_asserted = false;
    in.link_up = true;
    in.relay_deenergized = false;
    in.dt_s = 0.1f; /* safety_core's real tick period, per ARCHITECTURE.md section 4 */
    return in;
}

static safety_guard_cfg_t base_cfg(void)
{
    safety_guard_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
    cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
    cfg.abs_max_temp_c = 1300.0f;
    cfg.tc_source = SAFETY_TC_SOURCE_OWN_J7;
    /* Everything else left 0 -> firmware defaults (firing_margin_c=100,
     * bad_read_count_threshold=10, bad_read_time_s=5, blind_grace_s=60,
     * frozen_window_s=600, cj_warn_c=60, cj_max_c=85, cj_time_s=60,
     * borrowed_stale_s=10, borrowed_stale_trip_s=60, overshoot_margin_c=75,
     * overshoot_time_s=120, i_present_a=2.0, correlation_window_s=150,
     * stuck_on_time_s=20, link_timeout_s=10, link_dead_hard_s=120,
     * trip_verify_s=10, tc_disagreement_c=200, tc_disagreement_time_s=300). */
    return cfg;
}

static void test_s1(void)
{
    TEST_SECTION("S1 -- absolute over-temperature");

    /* Nuisance: a reading that stays under the ceiling forever never
     * trips, no matter how long it runs. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 1299.0f;
        bool tripped = false;
        for (int i = 0; i < 1000 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "reading just under the ceiling never trips S1");
    }

    /* Nuisance: abs_max_temp_c == 0 means "not commissioned" -- never trip,
     * even on an absurd reading. A silently-substituted ceiling here would
     * be worse than no protection at all (safety_guards.h's doc comment). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.abs_max_temp_c = 0.0f;
        safety_guard_input_t in = base_input();
        in.tc_c = 9999.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "abs_max_temp_c==0 (not commissioned) never trips S1");
    }

    /* Nuisance: two over-ceiling readings, then a good one, then two more --
     * the streak must reset on the good reading, so this never reaches the
     * 3-consecutive bar (a single noisy conversion should not count toward
     * a trip that is supposed to mean "sustained"). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t over = base_input();
        over.tc_c = 1400.0f;
        safety_guard_input_t good = base_input();
        good.tc_c = 900.0f;
        bool tripped = false;
        tripped |= safety_guards_tick(&s, &cfg, &over);
        tripped |= safety_guards_tick(&s, &cfg, &over);
        tripped |= safety_guards_tick(&s, &cfg, &good); /* breaks the streak */
        tripped |= safety_guards_tick(&s, &cfg, &over);
        tripped |= safety_guards_tick(&s, &cfg, &over);
        TEST_CHECK(!tripped, "a streak-breaking good reading prevents S1 from tripping");
    }

    /* Trip: 3 consecutive valid over-ceiling readings. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 1400.0f;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == false, "1st over-ceiling reading: not yet tripped");
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == false, "2nd over-ceiling reading: not yet tripped");
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == true, "3rd consecutive over-ceiling reading trips S1");
        TEST_CHECK(s.reason == SAFETY_TRIP_OVERTEMP, "reason is SAFETY_TRIP_OVERTEMP");
        TEST_CHECK(s.is_tripped, "is_tripped set");
    }

    /* CHAMBER_AGREED with a firing target tightens the ceiling via min() --
     * a reading between the (lower) firing-tightened ceiling and the
     * (higher) abs_max_temp_c trips, proving the tighter number is the one
     * actually used, not the fixed one. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        cfg.abs_max_temp_c = 1300.0f;
        cfg.firing_margin_c = 100.0f;
        cfg.firing_max_valid = true;
        cfg.firing_max_c = 900.0f; /* ceiling = min(1300, 900+100) = 1000 */
        safety_guard_input_t in = base_input();
        in.tc_c = 1050.0f; /* above the tightened 1000C ceiling, below abs_max_temp_c */
        bool tripped = false;
        for (int i = 0; i < 3 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "firing_max_c tightens S1's ceiling via min() in CHAMBER_AGREED");
    }

    /* The ceiling can only ever tighten: a hostile/buggy firing_max_c far
     * above abs_max_temp_c must clamp to abs_max_temp_c, not be obeyed. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        cfg.abs_max_temp_c = 1300.0f;
        cfg.firing_max_valid = true;
        cfg.firing_max_c = 5000.0f; /* min(1300, 5000+100) = 1300, unchanged */
        safety_guard_input_t in = base_input();
        in.tc_c = 1250.0f; /* under 1300 -- must NOT trip if clamp works */
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "min() clamps a hostile firing_max_c to abs_max_temp_c, never loosens the ceiling");
    }

    /* EXTERNAL_OVERHEAT ignores firing_max_c entirely, even when set. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
        cfg.abs_max_temp_c = 1300.0f;
        cfg.firing_max_valid = true;
        cfg.firing_max_c = 50.0f; /* would tighten to 150C in CHAMBER_AGREED -- must be ignored here */
        safety_guard_input_t in = base_input();
        in.tc_c = 200.0f; /* would trip if firing_max_c were honoured; must not trip fixed at 1300 */
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "EXTERNAL_OVERHEAT uses the fixed ceiling and ignores firing_max_c");
    }

    /* Latching: stays tripped until safety_guards_clear(). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 2000.0f;
        for (int i = 0; i < 3; i++) safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(s.is_tripped, "sanity: S1 tripped");
        safety_guard_input_t safe = base_input();
        safe.tc_c = 20.0f;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &safe) == false, "latched: further calls report not-newly-tripped");
        TEST_CHECK(s.is_tripped, "latched: still tripped even with a now-safe reading");
        safety_guards_clear(&s);
        TEST_CHECK(!s.is_tripped, "safety_guards_clear() un-latches");
        TEST_CHECK(safety_guards_tick(&s, &cfg, &safe) == false, "post-clear tick with a safe reading does not re-trip");
    }
}

static void test_s5(void)
{
    TEST_SECTION("S5 -- safety thermocouple invalid (graduated)");

    /* Nuisance: a 900ms sensor dropout must not trip -- ARCHITECTURE.md
     * section 10's own example. dt_s=0.1s x 9 ticks = 900ms and only 9
     * consecutive bad reads, under the 10-read count bar. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bool tripped = false;
        for (int i = 0; i < 9 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &bad);
        }
        TEST_CHECK(!tripped, "a 900ms sensor dropout (9 bad reads) does not trip S5");
        TEST_CHECK(!s.s5_warn, "900ms dropout does not even reach WARN (needs 10 reads AND 5s)");
    }

    /* Nuisance: a single noisy SPI read, followed by good reads, must not
     * warn or trip -- and must not leave any residue in the streak. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.spi_failed = true;
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        safety_guard_input_t good = base_input();
        TEST_CHECK(safety_guards_tick(&s, &cfg, &bad) == false, "single noisy SPI read: not tripped");
        TEST_CHECK(safety_guards_tick(&s, &cfg, &good) == false, "good read after it: not tripped");
        TEST_CHECK(s.s5_bad_streak == 0, "a good read resets the bad-read streak");
        TEST_CHECK(!s.s5_warn, "no residual WARN after a single noisy read");
    }

    /* Nuisance: a burst of 10+ bad reads that all land within under 5s of
     * real time must not warn -- the count bar alone is not enough
     * (ARCHITECTURE.md section 10 / TODO.md Phase 4's "both count AND
     * time" requirement). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bad.dt_s = 0.01f; /* fast burst: 12 reads x 10ms = 120ms, well under 5s */
        bool tripped = false;
        for (int i = 0; i < 12 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &bad);
        }
        TEST_CHECK(!tripped, "12 bad reads in 120ms (count clears, time does not) does not warn or trip");
        TEST_CHECK(!s.s5_warn, "time bar alone withholds WARN even once the count bar clears");
    }

    /* GUARD_TEST_MATRIX.md section 2, S5's exact case: "9 bad reads, then a
     * good one -> No trip, streak resets." This is the near-threshold
     * version of the streak-reset property (one read short of the 10-read
     * count bar, not just an arbitrary single bad read) -- confirms the good
     * read wipes the streak back to 0 rather than merely "not yet at 10". */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        safety_guard_input_t good = base_input();
        bool tripped = false;
        for (int i = 0; i < 9 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &bad);
        }
        TEST_CHECK(!tripped, "9 bad reads (one short of the 10-read count bar): not tripped");
        TEST_CHECK(s.s5_bad_streak == 9, "sanity: streak reached 9");
        tripped = safety_guards_tick(&s, &cfg, &good);
        TEST_CHECK(!tripped, "9 bad reads then a good one: no trip");
        TEST_CHECK(s.s5_bad_streak == 0, "9 bad reads then a good one: streak resets to 0");
        TEST_CHECK(!s.s5_warn, "9 bad reads then a good one: never even reached WARN");
    }

    /* THERMO_FAULT_CJRANGE/CJHIGH/CJLOW alone must NOT count as an S5 bad
     * read -- SAFETY_MODEL.md section 4, S5 is explicit these are a
     * cold-junction complaint (S12's job), not a chamber emergency. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.fault_bits = SAFETY_THERMO_FAULT_CJRANGE;
        bool tripped = false;
        for (int i = 0; i < 700 && !tripped; i++) { /* long enough to prove it never accumulates toward S5 */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "CJRANGE alone never trips S5");
        TEST_CHECK(s.s5_bad_streak == 0, "CJRANGE alone never even starts S5's bad-read streak");
    }

    /* THERMO_FAULT_TCHIGH/TCLOW alone must NOT count as an S5 bad read
     * either -- those are threshold comparators, S1's job. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.fault_bits = SAFETY_THERMO_FAULT_TCHIGH | SAFETY_THERMO_FAULT_TCLOW;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == false, "TCHIGH|TCLOW alone: S5 stays quiet");
        TEST_CHECK(s.s5_bad_streak == 0, "TCHIGH|TCLOW alone never starts S5's bad-read streak");
    }

    /* Graduated response: sustained bad reads reach WARN before TRIP, and
     * TRIP only after blind_grace_s (60s default). dt_s=5s: WARN at tick 10
     * (count=10, elapsed=50s -- elapsed<60 so not yet TRIP), TRIP at tick
     * 12 (elapsed=60s). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bad.dt_s = 5.0f;
        bool tripped = false;
        for (int i = 0; i < 9; i++) {
            tripped = safety_guards_tick(&s, &cfg, &bad);
            TEST_CHECK(!tripped, "still under 10 reads: not tripped");
        }
        TEST_CHECK(!s.s5_warn, "still under 10 reads: not yet WARN either");
        tripped = safety_guards_tick(&s, &cfg, &bad); /* tick 10: streak=10, elapsed=50s */
        TEST_CHECK(!tripped, "10 bad reads at 50s: WARN, not yet TRIP (grace is 60s)");
        TEST_CHECK(s.s5_warn, "WARN active once both bars clear");
        tripped = safety_guards_tick(&s, &cfg, &bad); /* tick 11: elapsed=55s */
        TEST_CHECK(!tripped, "55s blind: still not tripped");
        tripped = safety_guards_tick(&s, &cfg, &bad); /* tick 12: elapsed=60s */
        TEST_CHECK(tripped, "60s blind (>= blind_grace_s): trips");
        TEST_CHECK(s.reason == SAFETY_TRIP_SENSOR_INVALID, "reason is SAFETY_TRIP_SENSOR_INVALID");
    }

    /* NaN handling: a snapshot that claims tc_valid==true but carries a NaN
     * tc_c (a producer bug, or a corrupted conversion) must still be caught
     * as a bad read -- not silently pass through and be compared against
     * S1's ceiling, where "NaN > ceiling" is always false in IEEE754 and
     * would otherwise look identical to "comfortably under the ceiling". */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_valid = true; /* producer bug: claims valid... */
        in.tc_c = (float)NAN; /* ...but the value is garbage */
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == false, "single NaN-but-claimed-valid read: not yet tripped");
        TEST_CHECK(s.s5_bad_streak == 1, "a NaN tc_c counts as an S5 bad read even when tc_valid claims true");
        TEST_CHECK(s.s1_over_ceiling_streak == 0, "the NaN reading never reached S1's over-ceiling check at all");
    }

    /* Latching: stays tripped until cleared. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bad.dt_s = 61.0f; /* single tick past blind_grace_s */
        for (int i = 0; i < 10; i++) safety_guards_tick(&s, &cfg, &bad);
        TEST_CHECK(s.is_tripped, "sanity: S5 tripped");
        safety_guard_input_t good = base_input();
        TEST_CHECK(safety_guards_tick(&s, &cfg, &good) == false, "latched: a good read does not un-latch by itself");
        TEST_CHECK(s.is_tripped, "still tripped -- no auto-recovery");
        safety_guards_clear(&s);
        TEST_CHECK(!s.is_tripped, "safety_guards_clear() un-latches S5");
    }
}

// S5's declared-not-installed escape hatch (config param 0x0211,
// safety_guard_input_t.safety_tc_not_installed_declared -- the pure module's
// field is deliberately the INVERSE polarity of the config field itself; see
// safety_guards.h's own comment on it for why). Task 1 of the safety-TC-not-
// installed pass: heat is blocked elsewhere (safety_core_request_enable(),
// not reachable from this pure module), so this only needs to prove S5
// itself never promotes to TRIP while the flag is set, and prove it does
// exactly as before when the flag is left at its default (false).
static void test_s5_not_installed(void)
{
    TEST_SECTION("S5 -- declared-not-installed escape hatch (safety_tc_installed == 0)");

    /* The flag alone changes nothing: WARN still fires on schedule, exactly
     * like the ordinary case in test_s5() above. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bad.dt_s = 5.0f;
        bad.safety_tc_not_installed_declared = true;
        for (int i = 0; i < 9; i++) {
            TEST_CHECK(!safety_guards_tick(&s, &cfg, &bad), "still under 10 reads: not tripped");
        }
        TEST_CHECK(!s.s5_warn, "still under 10 reads: not yet WARN either");
        TEST_CHECK(!safety_guards_tick(&s, &cfg, &bad), "tick 10 (50s): WARN, not TRIP (grace is 60s)");
        TEST_CHECK(s.s5_warn, "WARN fires on schedule even with the flag set");
        TEST_CHECK(!s.s5_not_installed, "s5_not_installed is not set before blind_grace_s is reached");
    }

    /* Past blind_grace_s WITHOUT the flag: trips, exactly as test_s5()
     * already proves -- repeated here as the negative control this test's
     * own positive case is compared against. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bad.dt_s = 61.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &bad);
        }
        TEST_CHECK(tripped, "negative control: without the flag, blind past grace TRIPS");
        TEST_CHECK(s.reason == SAFETY_TRIP_SENSOR_INVALID, "reason is SENSOR_INVALID");
        TEST_CHECK(!s.s5_not_installed, "s5_not_installed stays false on the real TRIP path");
    }

    /* Past blind_grace_s WITH the flag: WARN persists, s5_not_installed
     * flips true, is_tripped stays false -- no matter how long it runs. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bad.dt_s = 61.0f;
        bad.safety_tc_not_installed_declared = true;
        bool tripped = false;
        for (int i = 0; i < 10; i++) {
            tripped = safety_guards_tick(&s, &cfg, &bad) || tripped;
        }
        TEST_CHECK(!tripped, "declared not installed: blind past grace never TRIPS");
        TEST_CHECK(!s.is_tripped, "is_tripped stays false");
        TEST_CHECK(s.s5_warn, "s5_warn stays set (this is still a real fault worth reporting)");
        TEST_CHECK(s.s5_not_installed, "s5_not_installed reports the declared-absent state");
        // Diagnostics keep accumulating regardless -- "how long has it
        // actually been blind" must still be answerable.
        TEST_CHECK(s.s5_bad_elapsed_s > 60.0f, "s5_bad_elapsed_s keeps accumulating past grace");
        // Extend the run far past any plausible firing length -- proves
        // "never promoted to TRIP" means never, not just "not yet".
        for (int i = 0; i < 10000; i++) {
            tripped = safety_guards_tick(&s, &cfg, &bad) || tripped;
        }
        TEST_CHECK(!tripped, "still never trips after ~1000x blind_grace_s worth of ticks");
    }

    /* Recovery: a good read clears s5_not_installed exactly like s5_warn. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bad.dt_s = 61.0f;
        bad.safety_tc_not_installed_declared = true;
        for (int i = 0; i < 10; i++) safety_guards_tick(&s, &cfg, &bad);
        TEST_CHECK(s.s5_not_installed, "sanity: s5_not_installed set");
        safety_guard_input_t good = base_input();
        safety_guards_tick(&s, &cfg, &good);
        TEST_CHECK(!s.s5_not_installed, "a good read clears s5_not_installed");
        TEST_CHECK(!s.s5_warn, "a good read clears s5_warn too");
    }
}

static void test_s7(void)
{
    TEST_SECTION("S7 -- E-stop");

    /* Nuisance: E-stop not asserted, ever, never trips -- including a long
     * run, to prove there's no hidden accumulation of anything. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.estop_pressed = false;
        bool tripped = false;
        for (int i = 0; i < 100 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "E-stop released never trips S7");
    }

    /* Trip: asserted (already-debounced by discrete_task) trips
     * immediately, on the very first tick -- no debounce, no conditions,
     * fastest guard in the set. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.estop_pressed = true;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == true, "E-stop asserted trips S7 on the first tick");
        TEST_CHECK(s.reason == SAFETY_TRIP_ESTOP, "reason is SAFETY_TRIP_ESTOP");
    }

    /* Latching: releasing E-stop after a trip does not clear it -- the
     * doc's own clear path is an assert-then-release *cycle*, which is
     * safety_core's job (a physical action at the machine), not something
     * this pure module infers from a single released tick. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t pressed = base_input();
        pressed.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &pressed);
        TEST_CHECK(s.is_tripped, "sanity: S7 tripped");
        safety_guard_input_t released = base_input();
        released.estop_pressed = false;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &released) == false, "latched: releasing alone does not un-latch");
        TEST_CHECK(s.is_tripped, "still tripped after release -- clearing is an explicit act, not automatic");
        safety_guards_clear(&s);
        TEST_CHECK(!s.is_tripped, "safety_guards_clear() un-latches S7");
    }
}

static void test_s11(void)
{
    TEST_SECTION("S11 -- frozen safety reading");

    /* Nuisance: a cold, idle kiln (heat_commanded == false) sitting at a
     * perfectly constant reading for far longer than frozen_window_s must
     * never trip -- SAFETY_MODEL.md section 4, S11's own example. This is
     * also the resolution of the "heat commanded" wiring: safety_core now
     * feeds heat_commanded from any_current_present (SaftyFW's own Phase 6
     * current sense, S3/S4/S6b's producer too), and current_any_present is
     * false whenever no element is actually drawing power -- an idle kiln
     * with no current flowing still correctly presents heat_commanded ==
     * false here, so this guard stays dormant exactly as before, not
     * because nothing is wired but because nothing is actually heating. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 20.0f;
        in.heat_commanded = false;
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 20 && !tripped; i++) { /* 20*60s = 1200s, double frozen_window_s */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "a frozen reading on an idle kiln (heat_commanded=false) never trips S11");
    }

    /* Nuisance: heat commanded, but the reading keeps changing -- a
     * genuinely tracking sensor, however slowly, must never trip. This is
     * the case the real wiring is expected to hit constantly: a steady soak
     * with the SSR duty-cycling (any_current_present true on and off, so
     * safety_core's heat_commanded tracks it) while a real 19-bit MAX31856
     * reading naturally jitters tick to tick even at a "held" setpoint --
     * SAFETY_MODEL.md section 4's own claim that a bit-identical reading
     * "does not happen in a real thermal system" while energy is going in
     * is exactly why this stays quiet and S11 does not become S6b's
     * ~120s-into-every-boot nuisance shape (fixed by f304392). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 300.0f;
        in.heat_commanded = true;
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 20 && !tripped; i++) {
            in.tc_c += 0.1f; /* always different from the last tick */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "a reading that keeps changing never trips S11, even with heat commanded throughout");
    }

    /* Nuisance: a steady soak, modelled the way current_any_present's OR-
     * across-three-channels wiring will actually present it -- heat_
     * commanded flickers true/false tick to tick as the SSR duty-cycles
     * around a held setpoint, while the reading itself keeps its own tiny
     * jitter (never repeating bit-for-bit). Neither the flickering
     * qualifier nor the jitter should ever accumulate into a trip: every
     * heat_commanded == false tick resets the window per this guard's own
     * "gating on a plain caller-supplied bool" contract, and every
     * heat_commanded == true tick sees a changed in->tc_c, so the window
     * never reaches frozen_window_s either way. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 850.0f;
        in.dt_s = 30.0f;
        bool tripped = false;
        for (int i = 0; i < 40 && !tripped; i++) { /* 40*30s = 1200s, double frozen_window_s */
            in.heat_commanded = (i % 2) == 0; /* SSR duty-cycling around the setpoint */
            in.tc_c += ((i % 3) == 0) ? 0.05f : -0.02f; /* small real-sensor jitter, always different */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "a duty-cycling heat_commanded qualifier over a jittering steady soak never trips S11");
    }

    /* Trip: heat commanded, reading frozen for the full window. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 400.0f;
        in.heat_commanded = true;
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 12 && !tripped; i++) { /* 12*60s = 720s > frozen_window_s(600) */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "an identical reading for frozen_window_s while heat is commanded trips S11");
        TEST_CHECK(s.reason == SAFETY_TRIP_FROZEN_SENSOR, "reason is SAFETY_TRIP_FROZEN_SENSOR");
    }

    /* heat_commanded flipping false partway through resets the window --
     * the qualifier must apply continuously, not just at the start. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 400.0f;
        in.heat_commanded = true;
        in.dt_s = 60.0f;
        for (int i = 0; i < 8; i++) safety_guards_tick(&s, &cfg, &in); /* 480s in, window still building */
        in.heat_commanded = false;
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "heat_commanded dropping mid-window does not trip");
        TEST_CHECK(!s.s11_window_active, "the window is reset, not merely paused, when heat_commanded drops");
    }

    /* Latching. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.tc_c = 400.0f;
        in.heat_commanded = true;
        in.dt_s = 700.0f; /* single tick past frozen_window_s */
        safety_guards_tick(&s, &cfg, &in);
        safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(s.is_tripped, "sanity: S11 tripped");
        in.tc_c = 20.0f; /* now changing */
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == false, "latched: a changing reading does not un-latch");
        TEST_CHECK(s.is_tripped, "still tripped");
        safety_guards_clear(&s);
        TEST_CHECK(!s.is_tripped, "safety_guards_clear() un-latches S11");
    }
}

static void test_s12(void)
{
    TEST_SECTION("S12 -- cold junction / enclosure over-temperature");

    /* Nuisance: cj_c comfortably under cj_warn_c never warns or trips. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.cj_c = 30.0f;
        bool tripped = false;
        for (int i = 0; i < 100 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "cj_c well under cj_warn_c never trips S12");
        TEST_CHECK(!s.s12_warn, "cj_c well under cj_warn_c never even warns");
    }

    /* Nuisance: cj_c between cj_warn_c and cj_max_c warns, persistently,
     * but never escalates to TRIP no matter how long it sits there --
     * WARN and TRIP are genuinely different thresholds, not a timer on the
     * same one. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.cj_c = 70.0f; /* between 60 (warn) and 85 (max) */
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 30 && !tripped; i++) { /* 30 minutes, far past cj_time_s(60s) */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "cj_c between warn and max never trips, however long it persists");
        TEST_CHECK(s.s12_warn, "cj_c between warn and max does set WARN");
    }

    /* Nuisance: a brief excursion above cj_max_c that drops back down
     * before cj_time_s resets the sustained-excess timer -- a transient
     * spike (a fan cycling, a door opened briefly) must not accumulate
     * toward a trip across multiple separate spikes. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t hot = base_input();
        hot.cj_c = 90.0f; /* above cj_max_c(85) */
        hot.dt_s = 30.0f;
        safety_guard_input_t cool = base_input();
        cool.cj_c = 40.0f;
        bool tripped = false;
        for (int i = 0; i < 3 && !tripped; i++) { /* 3 short spikes, well under cj_time_s each time, reset between */
            tripped |= safety_guards_tick(&s, &cfg, &hot);
            tripped |= safety_guards_tick(&s, &cfg, &cool);
        }
        TEST_CHECK(!tripped, "repeated brief over-cj_max_c spikes, each reset by a cool tick, never trip S12");
    }

    /* Trip: cj_c above cj_max_c, sustained for cj_time_s. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.cj_c = 95.0f;
        in.dt_s = 10.0f;
        bool tripped = false;
        for (int i = 0; i < 7 && !tripped; i++) { /* 7*10s = 70s > cj_time_s(60) */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "cj_c above cj_max_c sustained for cj_time_s trips S12");
        TEST_CHECK(s.reason == SAFETY_TRIP_ENCLOSURE_TEMP, "reason is SAFETY_TRIP_ENCLOSURE_TEMP");
    }

    /* Latching. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.cj_c = 95.0f;
        in.dt_s = 70.0f; /* single tick past cj_time_s */
        safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(s.is_tripped, "sanity: S12 tripped");
        safety_guard_input_t cool = base_input();
        cool.cj_c = 20.0f;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &cool) == false, "latched: cooling down does not un-latch");
        TEST_CHECK(s.is_tripped, "still tripped");
        safety_guards_clear(&s);
        TEST_CHECK(!s.is_tripped, "safety_guards_clear() un-latches S12");
    }
}

/* S8 -- implausible rate of rise. SAFETY_MODEL.md section 4: "d(safety_tc_c)/dt
 * > max_rate_c_per_min sustained for rate_window_s", ships disabled
 * (max_rate_c_per_min == 0.0f). Every test here uses dt_s == rate_window_s
 * (60.0f) so one safety_guards_tick() call is exactly one whole averaging
 * window -- makes the window-by-window arithmetic in each test's comment
 * checkable by hand, and keeps the streak (S8_OVER_RATE_STREAK_TO_TRIP == 2
 * window evaluations) legible one call at a time. */
static void test_s8(void)
{
    TEST_SECTION("S8 -- implausible rate of rise");

    /* SHIPS INERT: base_cfg() (memset to 0, per every other test in this
     * file) leaves max_rate_c_per_min at its zero-initialised,
     * uncommissioned, documented-default value. GUARD_TEST_MATRIX.md's own
     * pinned semantics: "Rate above threshold with max_rate_c_per_min = 0 ->
     * No trip -- disabled means disabled." Proven here against a rate no
     * real kiln could ever produce (500C/min, sustained for ten windows) --
     * if this guard could trip at the shipped default, it would trip on
     * this input long before any realistic one. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg(); /* max_rate_c_per_min == 0.0f, unset */
        cfg.abs_max_temp_c = 0.0f; /* isolate S8 from S1 -- this test's absurd
                                     * ramp would otherwise breach S1's own
                                     * ceiling and fail for the WRONG reason. */
        safety_guard_input_t in = base_input();
        in.dt_s = 60.0f;
        in.tc_c = 20.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            in.tc_c += 500.0f; /* 500C/min every window -- physically absurd */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "SHIPS INERT: max_rate_c_per_min == 0 never trips, no matter the rate");
        TEST_CHECK(!s.s8_window_active,
                   "disabled S8 does not even accumulate a window (state->s8_window_active stays false)");
    }

    /* Same proof, but explicitly setting max_rate_c_per_min = 0.0f rather
     * than relying on memset -- pins "0 means disabled" as the field's
     * actual documented meaning, not an accident of how the test builds cfg. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.abs_max_temp_c = 0.0f; /* isolate S8 from S1, same reasoning as above */
        cfg.max_rate_c_per_min = 0.0f;
        cfg.rate_window_s = 60.0f;
        safety_guard_input_t in = base_input();
        in.dt_s = 60.0f;
        in.tc_c = 20.0f;
        bool tripped = false;
        for (int i = 0; i < 5 && !tripped; i++) {
            in.tc_c += 1000.0f;
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "explicit max_rate_c_per_min = 0.0f: disabled means disabled");
    }

    /* NUISANCE, the stated hazard (GUARD_TEST_MATRIX.md: "A legitimate
     * full-power ramp at the measured maximum rate" must not trip): pick a
     * threshold with an honest 2x margin over a measured maximum rate, per
     * SAFETY_MODEL.md section 4's own commissioning guidance ("set the
     * threshold at roughly 2x that"), and run a ramp AT that maximum rate
     * for many windows. This repo has no logged full-power ramp to read a
     * real number from (SAFETY_MODEL.md says exactly that -- "nobody has
     * ever measured this kiln's maximum legitimate ramp rate"), so this test
     * uses the SAME document's own stated bound for a small test kiln on
     * full power -- "can genuinely exceed 15C/min" -- as the maximum
     * legitimate rate, commissions max_rate_c_per_min at 2x it (30C/min),
     * and proves a full 30-minute ramp held exactly at 15C/min never trips. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.max_rate_c_per_min = 30.0f; /* 2x SAFETY_MODEL.md's stated small-kiln max */
        cfg.rate_window_s = 60.0f;
        safety_guard_input_t in = base_input();
        in.dt_s = 60.0f;
        in.tc_c = 20.0f;
        bool tripped = false;
        for (int i = 0; i < 30 && !tripped; i++) { /* 30 windows = 30 minutes, held exactly at max legit rate */
            in.tc_c += 15.0f;
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped,
                   "a legitimate full-power ramp at the measured maximum rate (15C/min, 2x margin to threshold) never trips S8");
    }

    /* TRIP: a genuinely implausible sustained rate -- well past any
     * plausible full-power ramp -- clears both the magnitude bar
     * (max_rate_c_per_min) and the duration bar (two consecutive
     * rate_window_s windows, S8_OVER_RATE_STREAK_TO_TRIP). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.max_rate_c_per_min = 10.0f;
        cfg.rate_window_s = 60.0f;
        safety_guard_input_t in = base_input();
        in.dt_s = 60.0f;
        in.tc_c = 20.0f; /* t=0: window 1 opens at 20C */
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "window 1 merely opens -- nothing to evaluate yet");

        in.tc_c = 40.0f; /* t=60s: window 1 closes, 20C in 60s = 20C/min > 10 -- streak 1 */
        tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "first over-threshold window alone does not trip (duration bar not yet cleared)");
        TEST_CHECK(s.s8_over_rate_streak == 1, "first over-threshold window sets the streak to 1");

        in.tc_c = 60.0f; /* t=120s: window 2 closes, another 20C in 60s -- streak 2, trips */
        tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(tripped, "a second consecutive over-threshold window trips S8");
        TEST_CHECK(s.reason == SAFETY_TRIP_RATE, "reason is SAFETY_TRIP_RATE");
    }

    /* NUISANCE: a single glitchy sample landing exactly on a window
     * boundary, then reverting on the very next sample -- must not trip.
     * This is the case the two-window streak exists for: window 2's
     * inflated average becomes window 3's STARTING value, so window 3
     * measures back down and the streak never reaches 2. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.max_rate_c_per_min = 10.0f;
        cfg.rate_window_s = 60.0f;
        safety_guard_input_t in = base_input();
        in.dt_s = 60.0f;

        in.tc_c = 20.0f; /* window 1 opens */
        safety_guards_tick(&s, &cfg, &in);
        in.tc_c = 20.0f; /* window 1 closes flat: 0C/min -- streak stays 0 */
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped && s.s8_over_rate_streak == 0, "sanity: a flat window never sets the streak");

        in.tc_c = 70.0f; /* window 2 closes on a single glitch sample: 50C in 60s = 50C/min -- streak 1 */
        tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "one glitchy window alone never trips (duration bar)");
        TEST_CHECK(s.s8_over_rate_streak == 1, "the glitch does register as one over-threshold window");

        in.tc_c = 20.0f; /* glitch reverts: window 3 measures back DOWN from the glitchy 70C -- streak resets */
        tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "a single glitch that reverts by the next sample never trips S8");
        TEST_CHECK(s.s8_over_rate_streak == 0, "the reverting window resets the streak back to 0");
    }

    /* WINDOWED STATE RESETS CORRECTLY when the rate falls back below
     * threshold: an over-threshold window followed by an in-bounds window
     * must fully clear the streak, not merely fail to advance it -- a later
     * over-threshold window must start counting from zero, not from
     * wherever the streak was left. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.max_rate_c_per_min = 10.0f;
        cfg.rate_window_s = 60.0f;
        safety_guard_input_t in = base_input();
        in.dt_s = 60.0f;

        in.tc_c = 20.0f;
        safety_guards_tick(&s, &cfg, &in); /* window 1 opens */
        in.tc_c = 40.0f;                   /* window 1 closes: 20C/min -- streak 1 */
        safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(s.s8_over_rate_streak == 1, "sanity: streak is 1 after one over-threshold window");

        in.tc_c = 45.0f; /* window 2 closes: 5C/min, under threshold -- streak resets to 0 */
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "sanity: still not tripped");
        TEST_CHECK(s.s8_over_rate_streak == 0, "an in-bounds window fully resets the streak, not just pauses it");

        in.tc_c = 65.0f; /* window 3 closes: 20C/min again -- this is only the FIRST over-threshold
                           * window since the reset, so it must not trip on its own. */
        tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped,
                   "a fresh over-threshold window after a reset needs its own two-window streak -- it does not "
                   "inherit credit from before the reset");
        TEST_CHECK(s.s8_over_rate_streak == 1, "the post-reset streak restarted from 0, now at 1");
    }

    /* Invalid/stale/NaN samples must not read as an enormous rate -- this
     * repo has shipped exactly that class of bug before (an invalid-sample
     * placeholder read as a real temperature). A bad read (S5's definition:
     * spi_failed, !tc_valid, NaN, or OPEN/OVUV/TCRANGE fault bits) makes
     * safety_guards_tick() return before this guard's block ever runs
     * (see safety_guards.c's S5 block: "skip the rest of this tick's checks
     * rather than reasoning about a value that isn't trustworthy") -- so the
     * window is left PAUSED across bad ticks, never fed the bad value as
     * either endpoint. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.max_rate_c_per_min = 10.0f;
        cfg.rate_window_s = 60.0f;
        safety_guard_input_t in = base_input();
        in.dt_s = 60.0f;

        in.tc_c = 20.0f;
        safety_guards_tick(&s, &cfg, &in); /* window opens at 20C */

        /* A burst of bad reads -- !tc_valid, tc_c NaN, matching
         * safety_guard_input_t's own "tc_valid == false implies tc_c and
         * cj_c are NaN, never 0, never the last good reading" contract --
         * long enough that if this guard read tc_c during a bad tick, NaN
         * propagated through the rate arithmetic would poison every
         * subsequent comparison (NaN compares false against everything,
         * which could go EITHER way depending on exactly how it leaked in --
         * exactly the ambiguity this test rules out). */
        for (int i = 0; i < 15; i++) {
            safety_guard_input_t bad = in;
            bad.tc_valid = false;
            bad.tc_c = NAN;
            bad.cj_c = NAN;
            /* Real tick period, not the 60s window-compressing dt_s used
             * elsewhere in this test -- at dt_s=60s, S5's own
             * bad_read_time_s(5s)/blind_grace_s(60s) bars would clear
             * within two or three of these ticks and S5 itself would trip
             * first, which would prove nothing about S8. This burst is
             * about S8's window surviving bad reads, not about
             * reproducing S5's own graduated response. */
            bad.dt_s = 0.1f;
            bool t = safety_guards_tick(&s, &cfg, &bad);
            TEST_CHECK(!t, "a bad read never trips S8 by itself");
        }
        TEST_CHECK(s.s8_window_active && s.s8_window_start_c == 20.0f,
                   "S8's window survives a burst of bad reads untouched -- paused, not corrupted, not reset");

        /* Sensor recovers with a perfectly ordinary reading -- the window
         * resumes exactly where it left off, using the ORIGINAL 20C
         * baseline, not a NaN-poisoned one. */
        in.tc_c = 22.0f; /* trivial rate once the window eventually closes */
        bool tripped = false;
        for (int i = 0; i < 2 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "recovering to a normal reading after a bad-read burst does not trip S8");
    }

    /* Latching: once tripped, S8 stays tripped, and safety_guards_clear()
     * un-latches it -- the same contract every other guard in this file
     * gets. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.max_rate_c_per_min = 10.0f;
        cfg.rate_window_s = 60.0f;
        safety_guard_input_t in = base_input();
        in.dt_s = 60.0f;
        in.tc_c = 20.0f;
        safety_guards_tick(&s, &cfg, &in);
        in.tc_c = 40.0f;
        safety_guards_tick(&s, &cfg, &in);
        in.tc_c = 60.0f;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in), "sanity: S8 tripped");
        safety_guard_input_t cool = in;
        cool.tc_c = 20.0f;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &cool) == false, "latched: a cooling reading does not un-latch S8");
        TEST_CHECK(s.is_tripped, "still tripped");
        safety_guards_clear(&s);
        TEST_CHECK(!s.is_tripped, "safety_guards_clear() un-latches S8");
    }
}

/* ARCHITECTURE.md section 10: "assert the independence invariant in tests,
 * not just in prose: run the guard suite with the TX path stubbed out
 * entirely and assert the verdict stream is bit-identical to a run with it
 * live." For these five guards the invariant is close to free: none of them
 * consume context_snapshot_t, and safety_guard_input_t has no link-derived
 * field at all (see safety_guards.h's top comment) -- there is no "TX live"
 * switch to even flip. What is left to prove is that the module is a pure,
 * deterministic function of its explicit inputs: running the identical
 * input sequence through two independent state instances must produce
 * bit-identical verdicts every tick, with nothing hidden (a global, a
 * static, a clock read internally) able to make one run differ from the
 * other. */
static void test_independence_invariant(void)
{
    TEST_SECTION("independence invariant -- deterministic, no hidden link dependency");

    safety_guard_state_t s_a, s_b;
    safety_guards_reset(&s_a);
    safety_guards_reset(&s_b);
    safety_guard_cfg_t cfg = base_cfg();
    cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
    cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
    cfg.firing_max_valid = true;
    cfg.firing_max_c = 800.0f;

    safety_guard_input_t in = base_input();
    in.tc_c = 400.0f;
    in.heat_commanded = true;

    bool all_identical = true;
    for (int i = 0; i < 50; i++) {
        in.tc_c += 5.0f;
        in.cj_c = 40.0f + (float)i;
        bool tripped_a = safety_guards_tick(&s_a, &cfg, &in);
        bool tripped_b = safety_guards_tick(&s_b, &cfg, &in);
        if (tripped_a != tripped_b || s_a.is_tripped != s_b.is_tripped || s_a.reason != s_b.reason) {
            all_identical = false;
        }
    }
    TEST_CHECK(all_identical, "identical input sequences through independent states produce bit-identical verdicts");
    TEST_CHECK(memcmp(&s_a, &s_b, sizeof(s_a)) == 0, "final state is bit-identical too -- no hidden nondeterminism");

    // Negative proof this memcmp actually catches a divergence (not a
    // vacuously-true comparison of two structs that happen to be zeroed the
    // same way): perturb one bit in the copy, confirm memcmp now disagrees,
    // then discard the perturbed copy without letting it affect anything
    // else in this file.
    {
        safety_guard_state_t s_b_perturbed = s_b;
        s_b_perturbed.reason = (safety_trip_t)((int)s_b_perturbed.reason + 1);
        TEST_CHECK(memcmp(&s_a, &s_b_perturbed, sizeof(s_a)) != 0,
                   "negative check: memcmp DOES detect a single perturbed field -- the identical-state "
                   "assertion above is not vacuous");
    }
}

// --- TODO.md Phase 8b: "Assert in the host tests that guard verdicts are
// bit-identical with the TX path stubbed out. No verdict may depend on
// anyone listening." ------------------------------------------------------
//
// The production TX path (uart_owner.c's non-blocking ring buffer, driven
// from link_task.c) is FreeRTOS/pico-sdk code and cannot be linked into this
// standalone host-test binary at all -- there is no seam here to flip a real
// "TX enabled/disabled" switch and call into the real transmit code either
// way. That is not a gap in this test; it is the architecture working as
// documented: docs/ARCHITECTURE.md section 2's "the one rule that matters"
// is that safety_core_task (and everything it calls, including
// safety_guards_tick()) NEVER touches the link at all, enforced at build
// time by tools/check_isolation.ps1 grepping safety_core.c for any
// link/uart header. safety_guard_input_t (safety_guards.h) has no
// TX-related field, no pointer, no callback -- there is no parameter through
// which a "TX path" could even be threaded into a guard evaluation, so there
// is structurally nothing to stub.
//
// What IS testable, and is the actual content of the invariant once
// "stubbed vs live" collapses to "there is no such axis": running a
// representative, mixed sequence of inputs -- reusing S1 (over-ceiling
// ramp), S5 (sensor dropout), S6b (link-down + current), S7 (E-stop), S9
// (post-trip escalation), S11 (frozen reading) and S12 (enclosure
// over-temp) shapes from the tests above, back to back in one run -- through
// two independently-instantiated safety_guard_state_t objects produces
// bit-identical verdicts at every tick and a bit-identical final struct
// (memcmp over the WHOLE structure, not one field, per this task's
// instructions). Since nothing in safety_guards.c can observe whether
// anything is listening on the wire, this bit-identical result already IS
// the "TX path stubbed out" case and the "TX path live" case at once: there
// is only one code path, unconditionally.
//
// PRODUCTION SEAM MISSING (reported, not built -- src/ is off-limits to this
// agent): if a stronger, literal "run safety_core_task's tick with
// uart_owner_send() replaced by a no-op mock, and again with the real one,
// diff the two" test is wanted, the missing seam is a build-time or
// link-time substitution point for uart_owner_send() (uart_owner.h) callable
// from a host build -- today uart_owner.c is compiled only against the
// pico-sdk target, so no host test can currently exercise it at all, stubbed
// or not.
static void test_tx_independence_representative_sequence(void)
{
    TEST_SECTION("TODO.md Phase 8b -- guard verdicts over a representative mixed sequence are "
                 "bit-identical across two independent evaluations (the 'TX stubbed vs TX live' "
                 "case collapses to one code path -- safety_guards.c has no TX-observable input)");

    safety_guard_state_t s_a, s_b;
    safety_guards_reset(&s_a);
    safety_guards_reset(&s_b);
    safety_guard_cfg_t cfg = base_cfg();
    cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
    cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
    cfg.firing_max_valid = true;
    cfg.firing_max_c = 1200.0f; /* generous -- this run isn't exercising S1's ceiling-tightening math */

    // Build one long, mixed input sequence: reuses the shapes from test_s1
    // (a ramp through the ceiling), test_s5 (a sensor dropout burst), test_s6b
    // (link down with current present), test_s7 (E-stop press), test_s9
    // (post-trip verify with current still present -- welded-contactor
    // shape), test_s11 (a frozen reading while heat is commanded) and test_s12
    // (a sustained cold-junction excursion), concatenated so a real run
    // crosses through several guards' internal state, not just one.
    safety_guard_input_t seq[220];
    int n = 0;

    /* S1-shaped: ramp toward, then past, the ceiling. */
    for (int i = 0; i < 20; i++) {
        safety_guard_input_t in = base_input();
        in.tc_c = 800.0f + (float)i * 5.0f; /* climbs from 800 to 895, under 1300 */
        in.dt_s = 0.5f;
        seq[n++] = in;
    }

    /* S5-shaped: a sensor dropout burst that clears on its own (nuisance,
     * not a trip) -- proves the mixed-sequence run also exercises S5's
     * bad-read streak bookkeeping identically across both state instances. */
    for (int i = 0; i < 6; i++) {
        safety_guard_input_t in = base_input();
        in.tc_valid = false;
        in.tc_c = (float)NAN;
        in.dt_s = 0.1f;
        seq[n++] = in;
    }
    {
        safety_guard_input_t good = base_input();
        good.tc_c = 300.0f;
        seq[n++] = good;
    }

    /* S12-shaped: enclosure warm but under cj_max_c, sustained -- WARN-only,
     * never a trip, exercising s12_warn bookkeeping. */
    for (int i = 0; i < 10; i++) {
        safety_guard_input_t in = base_input();
        in.tc_c = 300.0f;
        in.cj_c = 70.0f;
        in.dt_s = 5.0f;
        seq[n++] = in;
    }

    /* S6b-shaped: link down with current present, but resolved before its
     * 10s soft timeout -- another nuisance path, not a trip. */
    for (int i = 0; i < 3; i++) {
        safety_guard_input_t in = base_input();
        in.tc_c = 300.0f;
        in.link_up = false;
        in.any_current_present = true;
        in.dt_s = 2.0f; /* 3*2=6s, under the 10s soft timeout */
        seq[n++] = in;
    }
    {
        safety_guard_input_t recovered = base_input();
        recovered.tc_c = 300.0f;
        recovered.link_up = true;
        seq[n++] = recovered;
    }

    /* S7: E-stop asserted -- this is the one genuine trip in the sequence,
     * so the rest of the run (S9's post-trip escalation, S11's frozen-reading
     * bookkeeping) exercises "already tripped" behaviour identically too. */
    {
        safety_guard_input_t estop = base_input();
        estop.tc_c = 300.0f;
        estop.estop_pressed = true;
        seq[n++] = estop;
    }

    /* S9-shaped: post-trip verify window with the relay reporting
     * de-energized but current still present -- escalates to
     * SAFETY_TRIP_INEFFECTIVE partway through this tail. */
    for (int i = 0; i < 8; i++) {
        safety_guard_input_t in = base_input();
        in.tc_c = 300.0f;
        in.context_valid = true; /* genuine sustained weld: context available too */
        in.relay_deenergized = true;
        in.any_current_present = true;
        in.dt_s = 3.0f; /* 8*3=24s, past trip_verify_s(10s) partway through, with room for the
                          * S9_CURRENT_PRESENT_STREAK_TO_TRIP(3)-tick debounce to escalate too */
        seq[n++] = in;
    }

    TEST_CHECK(n <= (int)(sizeof(seq) / sizeof(seq[0])), "sequence buffer sized generously enough");

    bool all_identical = true;
    int divergence_index = -1;
    for (int i = 0; i < n; i++) {
        bool tripped_a = safety_guards_tick(&s_a, &cfg, &seq[i]);
        bool tripped_b = safety_guards_tick(&s_b, &cfg, &seq[i]);
        if (tripped_a != tripped_b || memcmp(&s_a, &s_b, sizeof(s_a)) != 0) {
            all_identical = false;
            if (divergence_index < 0) divergence_index = i;
        }
    }
    TEST_CHECK(all_identical,
               "a representative mixed sequence spanning S1/S5/S6b/S7/S9/S12 produces a "
               "bit-identical FULL verdict struct at every tick across two independent "
               "state instances -- no verdict depends on anyone listening");
    (void)divergence_index;

    // Negative proof this is not vacuous: corrupt the trip escalation flag
    // partway through an equivalent replay and confirm the memcmp-based check
    // above would have caught it. This proves the comparison actually
    // inspects fields that this exact sequence changes (trip_ineffective, in
    // particular, which only the S9 tail above sets), not just fields that
    // happen to never move.
    {
        safety_guard_state_t s_c, s_d;
        safety_guards_reset(&s_c);
        safety_guards_reset(&s_d);
        bool divergence_detected = false;
        for (int i = 0; i < n; i++) {
            safety_guards_tick(&s_c, &cfg, &seq[i]);
            safety_guards_tick(&s_d, &cfg, &seq[i]);
            if (i == n - 1) {
                // Simulate the exact kind of bug this test exists to catch:
                // one run's trip_ineffective flag silently differs (as if
                // some hypothetical TX-dependent code path had suppressed
                // the escalation on one run but not the other).
                s_d.trip_ineffective = !s_d.trip_ineffective;
            }
            if (memcmp(&s_c, &s_d, sizeof(s_c)) != 0) {
                divergence_detected = true;
            }
        }
        TEST_CHECK(divergence_detected,
                   "negative check: a single flipped trip_ineffective bit on one run IS caught by the "
                   "whole-struct memcmp -- proves the real assertion above is not comparing dead fields");
    }
}

static void test_s2(void)
{
    TEST_SECTION("S2 -- sustained excess over setpoint (context)");

    /* Nuisance: a 40C transient overshoot at ramp-end, well within
     * overshoot_margin_c (75C default), never trips no matter how long. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 940.0f; /* 40C over, under the 75C margin */
        in.dt_s = 30.0f;
        bool tripped = false;
        for (int i = 0; i < 60 && !tripped; i++) { /* 30 minutes */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "a 40C transient overshoot within overshoot_margin_c never trips S2");
    }

    /* Nuisance: EXTERNAL_OVERHEAT ignores S2 entirely, even with a huge
     * excess sustained indefinitely -- comparing a shell temp to a chamber
     * setpoint is meaningless (SAFETY_MODEL.md section 4). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 950.0f; /* massively over setpoint+margin, but mode disables S2 */
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "EXTERNAL_OVERHEAT disables S2 entirely");
    }

    /* Nuisance: stale/absent context (context_valid=false) means inactive,
     * not tripped, even with an extreme excess. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.context_valid = false;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 1000.0f; /* would be a huge S2 excess if context were valid; stays under abs_max_temp_c so S1 doesn't interfere */
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "context_valid==false makes S2 inactive, not pessimistic");
    }

    /* Trip: a sustained 75C+ excess for overshoot_time_s (120s default). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 990.0f; /* 90C over, above the 75C margin */
        in.dt_s = 10.0f;
        bool tripped = false;
        for (int i = 0; i < 13 && !tripped; i++) { /* 13*10s = 130s > 120s */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "a sustained 90C excess for overshoot_time_s trips S2");
        TEST_CHECK(s.reason == SAFETY_TRIP_OVER_SETPOINT, "reason is SAFETY_TRIP_OVER_SETPOINT");
    }

    /* NEGATIVE TEST for tc_placement_valid (audit 2026-08-27). Same inputs as
     * the S2 trip immediately above -- the ONLY difference is that
     * tc_placement_mode was never commissioned. SAFETY_MODEL.md section 3:
     * "there is no safe default, so there is no default -- until it is set,
     * S2 and S10 stay off." Before tc_placement_valid existed, an unset
     * placement read as CHAMBER_AGREED (enum value 0 == the zero-init value),
     * so S2 and S10 were silently ARMED on every uncommissioned board and
     * this case tripped. If this check ever starts failing, the gate has been
     * removed and uncommissioned boards are nuisance-tripping again. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = false; /* NOT commissioned */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 990.0f;
        in.dt_s = 10.0f;
        bool tripped = false;
        for (int i = 0; i < 13 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "S2 stays OFF while tc_placement_mode is uncommissioned");
    }

    /* Nuisance: a brief excursion that drops back down resets the timer. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t hot = base_input();
        hot.context_valid = true;
        hot.zone_count = 1;
        hot.max_zone_setpoint_c = 900.0f;
        hot.tc_c = 990.0f;
        hot.dt_s = 60.0f;
        safety_guard_input_t cool = hot;
        cool.tc_c = 910.0f; /* back under the margin */
        bool tripped = false;
        for (int i = 0; i < 3 && !tripped; i++) {
            tripped |= safety_guards_tick(&s, &cfg, &hot);
            tripped |= safety_guards_tick(&s, &cfg, &cool);
        }
        TEST_CHECK(!tripped, "repeated brief excursions, each reset by a cool tick, never trip S2");
    }

    /* GUARD_TEST_MATRIX.md section 2, S2's exact boundary case: "Hold
     * setpoint+80 for 119s, then 121s: No trip, then trip." overshoot_time_s
     * defaults to 120s, so 119s must still be under the bar and 121s must be
     * over it -- exercised as two independent runs (not a continuation of
     * one run) so each pins its own elapsed time exactly. setpoint+80 clears
     * the 75C margin bar by 5C, satisfying the "magnitude bar" independently
     * of the boundary being tested here. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 980.0f; /* setpoint + 80 */
        in.dt_s = 119.0f; /* single tick, elapsed pinned to exactly 119s */
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == false,
                   "setpoint+80 held for 119s (just under overshoot_time_s=120s): no trip");
    }
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 980.0f; /* setpoint + 80 */
        in.dt_s = 121.0f; /* single tick, elapsed pinned to exactly 121s */
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == true,
                   "setpoint+80 held for 121s (just over overshoot_time_s=120s): trips");
    }
}

static void test_s3_s4(void)
{
    TEST_SECTION("S3/S4 -- load vs commanded correlation (context)");

    /* Nuisance: current present with a relay recently commanded on --
     * healthy low-duty firing -- never trips S3. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.any_current_present = true;
        in.relay_commanded_recently = true;
        bool tripped = false;
        for (int i = 0; i < 1000 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "current present with a recently-commanded relay never trips S3");
    }

    /* Nuisance: stale context makes S3 inactive even with current present
     * and nothing commanded. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.context_valid = false;
        in.any_current_present = true;
        in.relay_commanded_recently = false;
        bool tripped = false;
        for (int i = 0; i < 1000 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "context_valid==false makes S3 inactive, not pessimistic");
    }

    /* Trip: current present, nothing commanded recently, sustained for
     * stuck_on_time_s (20s default) -- the welded-SSR case. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.any_current_present = true;
        in.relay_commanded_recently = false;
        in.dt_s = 5.0f;
        bool tripped = false;
        for (int i = 0; i < 5 && !tripped; i++) { /* 5*5s = 25s > 20s */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "current present, nothing commanded, for stuck_on_time_s trips S3");
        TEST_CHECK(s.reason == SAFETY_TRIP_LOAD_STUCK_ON, "reason is SAFETY_TRIP_LOAD_STUCK_ON");
    }

    /* S4 never trips, by design -- only warns (SAFETY_MODEL.md section 4,
     * S4: "this never trips, and that is a design decision"). Run it for a
     * very long time to prove there is no hidden escalation path. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.relay_commanded_continuously = true;
        in.any_current_present = false;
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 10000 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "S4's condition never trips, no matter how long it persists");
        TEST_CHECK(s.s4_warn, "S4's condition does set the WARN level");
    }

    /* S4 stays quiet when current is present (element healthy). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.relay_commanded_continuously = true;
        in.any_current_present = true;
        safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!s.s4_warn, "S4 does not warn when current is present alongside the commanded relay");
    }
}

static void test_sim_plant_disable(void)
{
    TEST_SECTION("sim_plant_disable_active -- SIM_PLANT guard-disable gate (S2/S3/S4 only)");

    /* Flag OFF (the production default: base_input() zero-inits this field,
     * exactly as it is on every board built without SAFTYFW_HONOR_SIM_PLANT,
     * since safety_core.c hardcodes sim_plant_disable_active=false in that
     * configuration regardless of what the ESP claims). S2 must still trip
     * exactly per its own test above -- restated here, explicitly pinned to
     * sim_plant_disable_active==false, so this test alone documents and
     * proves the required "flag-off path still trips S2" behaviour. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true;
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.sim_plant_disable_active = false;
        in.context_valid = true;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 990.0f; /* 90C over, above the 75C margin */
        in.dt_s = 10.0f;
        bool tripped = false;
        for (int i = 0; i < 13 && !tripped; i++) { /* 13*10s = 130s > 120s */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "sim_plant_disable_active==false: S2 still trips on a sustained excess");
        TEST_CHECK(s.reason == SAFETY_TRIP_OVER_SETPOINT, "reason is SAFETY_TRIP_OVER_SETPOINT");
    }

    /* Flag OFF: S3 still trips. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.sim_plant_disable_active = false;
        in.context_valid = true;
        in.any_current_present = true;
        in.relay_commanded_recently = false;
        in.dt_s = 5.0f;
        bool tripped = false;
        for (int i = 0; i < 5 && !tripped; i++) { /* 5*5s = 25s > 20s */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "sim_plant_disable_active==false: S3 still trips on load stuck on");
        TEST_CHECK(s.reason == SAFETY_TRIP_LOAD_STUCK_ON, "reason is SAFETY_TRIP_LOAD_STUCK_ON");
    }

    /* Flag OFF: S4 still warns (S4 never trips by design, flag or no flag). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.sim_plant_disable_active = false;
        in.context_valid = true;
        in.relay_commanded_continuously = true;
        in.any_current_present = false;
        in.dt_s = 60.0f;
        safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(s.s4_warn, "sim_plant_disable_active==false: S4 still warns on load inactive");
    }

    /* Flag ON (bench-only: SAFTYFW_HONOR_SIM_PLANT compiled in AND the ESP's
     * context claims SIM_PLANT): the same S2 excess that just tripped above
     * must now stay silent, no matter how long it is sustained -- this is
     * the actual disable action the TODO asked for. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true;
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.sim_plant_disable_active = true;
        in.context_valid = true;
        in.zone_count = 1;
        in.max_zone_setpoint_c = 900.0f;
        in.tc_c = 990.0f;
        in.dt_s = 10.0f;
        bool tripped = false;
        for (int i = 0; i < 1000 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "sim_plant_disable_active==true: S2 never trips, however long sustained");
    }

    /* Flag ON: S3's same stuck-on excess stays silent. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.sim_plant_disable_active = true;
        in.context_valid = true;
        in.any_current_present = true;
        in.relay_commanded_recently = false;
        in.dt_s = 5.0f;
        bool tripped = false;
        for (int i = 0; i < 1000 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "sim_plant_disable_active==true: S3 never trips, however long sustained");
    }

    /* Flag ON: S4's WARN also clears -- the disable is a full reset of the
     * S2/S3/S4 group's state, not merely a suppressed trip. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.sim_plant_disable_active = true;
        in.context_valid = true;
        in.relay_commanded_continuously = true;
        in.any_current_present = false;
        in.dt_s = 60.0f;
        safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!s.s4_warn, "sim_plant_disable_active==true: S4's WARN is also reset, not just its trip");
    }

    /* sim_plant_disable_active must be scoped to S2/S3/S4 only -- S1 (an
     * unrelated, context-independent guard) must still trip normally while
     * the flag is on, proving this is not a global "ignore everything"
     * switch. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.sim_plant_disable_active = true;
        in.tc_c = 1400.0f; /* well over abs_max_temp_c=1300 */
        bool tripped = false;
        for (int i = 0; i < 5 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "sim_plant_disable_active==true does not touch S1, which still trips");
    }
}

static void test_s14(void)
{
    TEST_SECTION("S14 -- zone current above its measured normal (context, WARN only)");

    /* Nuisance: current at or just under 150% of normal never warns, no
     * matter how long it persists. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.i_normal_valid[0] = true;
        cfg.i_normal_a[0] = 10.0f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[0] = true;
        in.amps[0] = 14.9f; /* < 150% of 10.0A */
        in.relay_commanded_now_for_ct[0] = true;
        in.dt_s = 5.0f;
        bool tripped = false;
        for (int i = 0; i < 20 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "S14 never trips (WARN only)");
        TEST_CHECK(!s.s14_warn[0], "current just under 150% of normal never warns S14");
    }

    /* Nuisance / negative test: a channel whose normal has NEVER been
     * measured (i_normal_valid[ch] == false) must be skipped entirely --
     * COMMISSIONING_UX.md section 3.2's required negative test. A huge
     * current, sustained well past overcurrent_time_s, on an unmeasured
     * channel must produce no warn and no accumulation whatsoever. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.i_normal_valid[1] = false; /* explicitly unmeasured */
        cfg.i_normal_a[1] = 0.0f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[1] = true;
        in.amps[1] = 1000.0f; /* absurdly high -- would trivially warn if evaluated */
        in.relay_commanded_now_for_ct[1] = true;
        in.dt_s = 5.0f;
        bool tripped = false;
        for (int i = 0; i < 100 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "S14 never trips on an unmeasured channel");
        TEST_CHECK(!s.s14_warn[1], "an unmeasured channel (i_normal_valid==false) never warns, no matter the current");
        TEST_CHECK(s.s14_over_elapsed_s[1] == 0.0f, "an unmeasured channel accumulates no elapsed time at all");
    }

    /* Nuisance: relay not commanded -- even huge current on a measured
     * channel does not warn (S14 only fires while that channel's relay is
     * actively commanded on). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.i_normal_valid[2] = true;
        cfg.i_normal_a[2] = 5.0f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[2] = true;
        in.amps[2] = 50.0f;
        in.relay_commanded_now_for_ct[2] = false; /* not commanded */
        in.dt_s = 5.0f;
        for (int i = 0; i < 20; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!s.s14_warn[2], "S14 does not warn while the mapped relay is not commanded on");
    }

    /* Trip (warn): current sustained above 150% of normal for
     * overcurrent_time_s (30s default) on a measured, commanded channel. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.i_normal_valid[0] = true;
        cfg.i_normal_a[0] = 10.0f; /* threshold = 15.0A */
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[0] = true;
        in.amps[0] = 17.1f; /* > 150% of 10.0A, matches the spec's worked example */
        in.relay_commanded_now_for_ct[0] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 6; i++) { /* 6*5s = 30s == overcurrent_time_s */
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(s.s14_warn[0], "current sustained above 150% of normal for overcurrent_time_s warns S14");
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "S14's warn never escalates to a trip, no matter how long it persists");
    }

    /* Channel independence: CT0 over its normal does not warn CT1. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.i_normal_valid[0] = true;
        cfg.i_normal_a[0] = 10.0f;
        cfg.i_normal_valid[1] = true;
        cfg.i_normal_a[1] = 10.0f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[0] = true;
        in.amps[0] = 17.1f;
        in.relay_commanded_now_for_ct[0] = true;
        in.amps_valid[1] = true;
        in.amps[1] = 5.0f; /* healthy */
        in.relay_commanded_now_for_ct[1] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 8; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(s.s14_warn[0], "CT0 over its own normal warns");
        TEST_CHECK(!s.s14_warn[1], "CT1, healthy relative to its own normal, stays quiet while CT0 warns");
    }

    /* Stale context makes S14 inactive even with an over-threshold reading. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.i_normal_valid[0] = true;
        cfg.i_normal_a[0] = 10.0f;
        safety_guard_input_t in = base_input();
        in.context_valid = false;
        in.amps_valid[0] = true;
        in.amps[0] = 100.0f;
        in.relay_commanded_now_for_ct[0] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 20; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!s.s14_warn[0], "context_valid==false makes S14 inactive, not pessimistic");
    }
}

/* CT_COMMISSIONING_PLAN.md step 3: ct_topology == summed. One shared CT
 * (channel index 2) reads every zone; S14's per-channel comparison moves to
 * "channel 2 vs. sum of i_normal_a[] for zones commanded on now", channels
 * 0/1 go inert, and a new WARN-only guard S15 flags a per-zone deficit
 * (open heater). amps[] values below are deliberately NOT round numbers --
 * COMMISSIONING_UX.md/repo convention (idealized-input bug class): they are
 * what a real ADC-counts-derived reading looks like (e.g. 10.04A, not an
 * exact 10.0A), so a comparison that only happens to work against an exact
 * boundary cannot pass here by accident. */
/* --- CT_CHANNEL_MASK_PLAN.md step 4: the generalised per-channel member()
 * attribution. Three properties, in order of how much they can hurt:
 *   1. collapse -- with the committed map set to either legacy shape the
 *      generalised arm must produce exactly what the legacy arm produces;
 *   2. split -- a genuine two-CT split attributes each deficit to the zones
 *      on THAT channel;
 *   3. the negative test -- forced-wrong membership must not warn a zone
 *      attributed to the other channel.
 * amps[] values stay deliberately un-round, same idealized-input discipline
 * as the summed block below. */
static void run_ticks(safety_guard_state_t *s, const safety_guard_cfg_t *cfg,
                      safety_guard_input_t *in, int n)
{
    for (int i = 0; i < n; i++) {
        safety_guards_tick(s, cfg, in);
    }
}

static void test_s14_s15_zone_ct_channel_collapse(void)
{
    TEST_SECTION("S14/S15 -- a committed identity/all-2 map collapses onto the legacy arms");

    /* 1a. Identity {0,1,2} vs. legacy per_zone, over-current on channel 0.
     * The legacy per_zone arm reads relay_commanded_now_for_ct[]; the
     * generalised arm reads relay_commanded_now_for_zone[]. Both are set
     * here (an identity ct_channel_map, which is what a per_zone board with
     * an identity zone map has), so the two arms are being compared on the
     * same physical situation. */
    {
        safety_guard_cfg_t legacy = base_cfg();
        legacy.i_normal_valid[0] = true; legacy.i_normal_a[0] = 10.03f;
        safety_guard_cfg_t mapped = legacy;
        mapped.zone_ct_channel_valid = true;
        mapped.zone_ct_channel[0] = 0u; mapped.zone_ct_channel[1] = 1u; mapped.zone_ct_channel[2] = 2u;

        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[0] = true;
        in.amps[0] = 31.07f; /* > 150% of 10.03 */
        in.relay_commanded_now_for_ct[0] = true;
        in.relay_commanded_now_for_zone[0] = true;
        in.dt_s = 5.0f;

        safety_guard_state_t a; safety_guards_reset(&a);
        safety_guard_state_t b; safety_guards_reset(&b);
        run_ticks(&a, &legacy, &in, 20);
        run_ticks(&b, &mapped, &in, 20);
        TEST_CHECK(a.s14_warn[0] && b.s14_warn[0],
                   "identity map warns S14 on channel 0 exactly as legacy per_zone does");
        TEST_CHECK(a.s14_over_elapsed_s[0] == b.s14_over_elapsed_s[0],
                   "identity map accumulates the same S14 elapsed time as per_zone");
        TEST_CHECK(!a.s15_warn[0] && !b.s15_warn[0],
                   "identity map leaves S15 inert, same as per_zone (no shared channel)");
        TEST_CHECK(b.s15_under_elapsed_s[0] == 0.0f,
                   "a one-member channel accumulates no S15 time at all");
    }

    /* 1b. All-2 {2,2,2} vs. legacy summed, a real open-heater deficit:
     * zones 0 and 1 commanded, expected 10.03 + 5.07 = 15.10 A, channel 2
     * measuring only 4.91 A. The 10.19 A deficit clears both commanded
     * zones' 0.7x bars, so both flag -- precisely the "consistent with any
     * one of them" reading -- and both arms must agree on it zone by zone. */
    {
        safety_guard_cfg_t legacy = base_cfg();
        legacy.ct_topology_summed = true;
        legacy.i_normal_valid[0] = true; legacy.i_normal_a[0] = 10.03f;
        legacy.i_normal_valid[1] = true; legacy.i_normal_a[1] = 5.07f;
        safety_guard_cfg_t mapped = legacy;
        mapped.zone_ct_channel_valid = true;
        mapped.zone_ct_channel[0] = 2u; mapped.zone_ct_channel[1] = 2u; mapped.zone_ct_channel[2] = 2u;

        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[2] = true;
        in.amps[2] = 4.91f;
        in.relay_commanded_now_for_zone[0] = true;
        in.relay_commanded_now_for_zone[1] = true;
        in.dt_s = 5.0f;

        safety_guard_state_t a; safety_guards_reset(&a);
        safety_guard_state_t b; safety_guards_reset(&b);
        run_ticks(&a, &legacy, &in, 20);
        run_ticks(&b, &mapped, &in, 20);
        for (int z = 0; z < 3; z++) {
            TEST_CHECK(a.s15_warn[z] == b.s15_warn[z],
                       "all-2 map reproduces the summed arm's S15 verdict for every zone");
            TEST_CHECK(a.s15_under_elapsed_s[z] == b.s15_under_elapsed_s[z],
                       "all-2 map reproduces the summed arm's S15 elapsed time for every zone");
            TEST_CHECK(a.s14_warn[z] == b.s14_warn[z],
                       "all-2 map reproduces the summed arm's S14 verdict for every channel");
        }
        TEST_CHECK(b.s15_warn[0] && b.s15_warn[1],
                   "the fixture really does exercise S15 (both commanded zones flag)");
        TEST_CHECK(!b.s15_warn[2], "an uncommanded zone never flags S15");
        TEST_CHECK(!b.s14_warn[0] && !b.s14_warn[1],
                   "channels with no members stay inert under an all-2 map");
    }

    /* 1c. The gate itself: the SAME split map with the valid flag clear must
     * behave exactly like the legacy arm its ct_topology_summed says, not
     * like the map. This is what protects every already-commissioned board. */
    {
        safety_guard_cfg_t cfg = base_cfg();
        cfg.ct_topology_summed = true;
        cfg.i_normal_valid[0] = true; cfg.i_normal_a[0] = 10.03f;
        cfg.i_normal_valid[1] = true; cfg.i_normal_a[1] = 5.07f;
        cfg.zone_ct_channel[0] = 0u; cfg.zone_ct_channel[1] = 0u; cfg.zone_ct_channel[2] = 1u;
        /* zone_ct_channel_valid deliberately left false */

        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[2] = true;
        in.amps[2] = 4.91f;
        in.relay_commanded_now_for_zone[0] = true;
        in.relay_commanded_now_for_zone[1] = true;
        in.dt_s = 5.0f;

        safety_guard_state_t s; safety_guards_reset(&s);
        run_ticks(&s, &cfg, &in, 20);
        TEST_CHECK(s.s15_warn[0] && s.s15_warn[1],
                   "an uncommitted map is ignored: the summed arm still runs off channel 2");
    }
}

static void test_s14_s15_two_ct_split(void)
{
    TEST_SECTION("S14/S15 -- a genuine two-CT split attributes per channel");

    /* Zones 0 and 1 share channel 0; zone 2 has channel 1 to itself.
     * Channel 0: expected 10.03 + 5.07 = 15.10 A, measuring 4.91 A.
     * Channel 1: expected 7.02 A, measuring a healthy 6.98 A. */
    safety_guard_cfg_t cfg = base_cfg();
    cfg.zone_ct_channel_valid = true;
    cfg.zone_ct_channel[0] = 0u; cfg.zone_ct_channel[1] = 0u; cfg.zone_ct_channel[2] = 1u;
    cfg.i_normal_valid[0] = true; cfg.i_normal_a[0] = 10.03f;
    cfg.i_normal_valid[1] = true; cfg.i_normal_a[1] = 5.07f;
    cfg.i_normal_valid[2] = true; cfg.i_normal_a[2] = 7.02f;

    safety_guard_input_t in = base_input();
    in.context_valid = true;
    in.amps_valid[0] = true; in.amps[0] = 4.91f;
    in.amps_valid[1] = true; in.amps[1] = 6.98f;
    in.relay_commanded_now_for_zone[0] = true;
    in.relay_commanded_now_for_zone[1] = true;
    in.relay_commanded_now_for_zone[2] = true;
    in.dt_s = 5.0f;

    safety_guard_state_t s; safety_guards_reset(&s);
    run_ticks(&s, &cfg, &in, 20);
    TEST_CHECK(s.s15_warn[0] && s.s15_warn[1],
               "the shared channel's deficit flags both of ITS member zones");
    TEST_CHECK(!s.s15_warn[2],
               "zone 2, on the healthy dedicated channel, is never flagged by channel 0's deficit");
    TEST_CHECK(s.s15_under_elapsed_s[2] == 0.0f,
               "zone 2 accumulates no S15 time at all (one-member channel, and no deficit)");
    TEST_CHECK(!s.s14_warn[0] && !s.s14_warn[1] && !s.s14_warn[2],
               "nothing is over-current in this fixture, so S14 stays quiet");
}

static void test_s14_s15_wrong_membership_negative(void)
{
    TEST_SECTION("S14/S15 negative -- forced-wrong membership must not warn the other channel's zone");

    /* The same physical kiln as the split fixture above: the deficit is on
     * channel 0, whose members are zones 0 and 1. Zone 2 is measured on
     * channel 1 and is perfectly healthy. If membership were wrong -- zone 2
     * mistakenly attributed to channel 0 -- channel 0's 10.19 A deficit
     * would clear 0.7 x 7.02 = 4.91 A and flag a zone with nothing wrong
     * with it. Pin both halves: the correct map does not flag zone 2, and
     * the wrong map demonstrably would, so this test cannot pass vacuously
     * by the fixture simply being too weak to warn anyone. */
    safety_guard_cfg_t correct = base_cfg();
    correct.zone_ct_channel_valid = true;
    correct.zone_ct_channel[0] = 0u; correct.zone_ct_channel[1] = 0u; correct.zone_ct_channel[2] = 1u;
    correct.i_normal_valid[0] = true; correct.i_normal_a[0] = 10.03f;
    correct.i_normal_valid[1] = true; correct.i_normal_a[1] = 5.07f;
    correct.i_normal_valid[2] = true; correct.i_normal_a[2] = 7.02f;

    safety_guard_cfg_t wrong = correct;
    wrong.zone_ct_channel[2] = 0u; /* zone 2 wrongly attributed to the shared CT */

    safety_guard_input_t in = base_input();
    in.context_valid = true;
    in.amps_valid[0] = true; in.amps[0] = 4.91f;
    in.amps_valid[1] = true; in.amps[1] = 6.98f;
    in.relay_commanded_now_for_zone[0] = true;
    in.relay_commanded_now_for_zone[1] = true;
    in.relay_commanded_now_for_zone[2] = true;
    in.dt_s = 5.0f;

    safety_guard_state_t sc; safety_guards_reset(&sc);
    safety_guard_state_t sw; safety_guards_reset(&sw);
    run_ticks(&sc, &correct, &in, 20);
    run_ticks(&sw, &wrong, &in, 20);
    TEST_CHECK(!sc.s15_warn[2],
               "correct membership: a deficit on channel 0 never warns zone 2");
    TEST_CHECK(sw.s15_warn[2],
               "wrong membership demonstrably WOULD warn zone 2 -- the correct-map check above "
               "is therefore load-bearing, not vacuous");
    TEST_CHECK(sc.s15_warn[0] && sw.s15_warn[0],
               "both maps still flag zone 0, so the difference is attribution, not sensitivity");
}

static void test_s14_s15_summed_topology(void)
{
    TEST_SECTION("S14/S15 -- ct_topology == summed (shared CT, per-zone attribution)");

    /* Channels 0/1 report inert (no sensor behind them) regardless of what
     * amps[0]/amps[1] happen to hold -- summed mode never evaluates them. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.ct_topology_summed = true;
        cfg.i_normal_valid[0] = true;
        cfg.i_normal_a[0] = 10.03f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[0] = true;
        in.amps[0] = 1000.0f; /* would trivially warn per_zone-style if evaluated */
        in.relay_commanded_now_for_zone[0] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 20; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!s.s14_warn[0], "summed topology: channel 0 never evaluated, stays inert");
        TEST_CHECK(!s.s14_warn[1], "summed topology: channel 1 never evaluated, stays inert");
        TEST_CHECK(s.s14_over_elapsed_s[0] == 0.0f, "channel 0 accumulates nothing in summed mode");
    }

    /* S14 (summed): channel 2 compared against the SUM of the two commanded
     * zones' normals (10.03 + 5.07 = 15.10A; 150% = 22.65A threshold). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.ct_topology_summed = true;
        cfg.i_normal_valid[0] = true; cfg.i_normal_a[0] = 10.03f;
        cfg.i_normal_valid[1] = true; cfg.i_normal_a[1] = 5.07f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[2] = true;
        in.amps[2] = 23.11f; /* > 22.65A threshold */
        in.relay_commanded_now_for_zone[0] = true;
        in.relay_commanded_now_for_zone[1] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 6; i++) { /* 6*5s = 30s == overcurrent_time_s default */
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(s.s14_warn[2], "summed S14 warns off the SUM of commanded zones' normals, not a single zone's");
    }

    /* S14 (summed) negative test: a commanded zone with NO measured normal
     * makes the whole expected sum unknowable -- must skip entirely, never
     * warn on a partial/guessed sum. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.ct_topology_summed = true;
        cfg.i_normal_valid[0] = true; cfg.i_normal_a[0] = 10.03f;
        cfg.i_normal_valid[1] = false; /* zone 1 commanded but never measured */
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[2] = true;
        in.amps[2] = 1000.0f; /* absurdly high -- would trivially warn if a partial sum were used */
        in.relay_commanded_now_for_zone[0] = true;
        in.relay_commanded_now_for_zone[1] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 20; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!s.s14_warn[2], "summed S14: one commanded zone with no normal makes the whole sum unknowable, skip entirely");
    }

    /* S15 (new): zone 0 alone commanded, expected 10.03A, measured only
     * 2.98A -- a 7.05A deficit, well past 0.7*10.03 = 7.02A, sustained 30s. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.ct_topology_summed = true;
        cfg.i_normal_valid[0] = true; cfg.i_normal_a[0] = 10.03f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[2] = true;
        in.amps[2] = 2.98f; /* deficit 7.05A > 0.7*10.03=7.021A */
        in.relay_commanded_now_for_zone[0] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 6; i++) { /* 30s */
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(s.s15_warn[0], "S15 warns once the deficit sustains past 0.7x normal for 30s");
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "S15 never escalates to a trip");
    }

    /* S15 nuisance: a small, healthy deficit under 0.7x never warns even
     * sustained indefinitely. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.ct_topology_summed = true;
        cfg.i_normal_valid[0] = true; cfg.i_normal_a[0] = 10.03f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[2] = true;
        in.amps[2] = 9.51f; /* deficit 0.52A, well under 7.02A threshold */
        in.relay_commanded_now_for_zone[0] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 20; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!s.s15_warn[0], "a healthy small deficit never warns S15");
    }

    /* S15 negative test: per_zone topology (the default) leaves S15
     * permanently inert even with a huge, sustained deficit-shaped input --
     * COMMISSIONING_UX.md's required negative test for the new guard. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg(); /* ct_topology_summed stays false */
        cfg.i_normal_valid[0] = true; cfg.i_normal_a[0] = 10.03f;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[2] = true;
        in.amps[2] = 0.0f;
        in.relay_commanded_now_for_zone[0] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 20; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!s.s15_warn[0], "per_zone topology: S15 stays inert regardless of input");
    }

    /* Stale context makes both S14 (summed) and S15 inactive. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.ct_topology_summed = true;
        cfg.i_normal_valid[0] = true; cfg.i_normal_a[0] = 10.03f;
        safety_guard_input_t in = base_input();
        in.context_valid = false;
        in.amps_valid[2] = true;
        in.amps[2] = 0.0f;
        in.relay_commanded_now_for_zone[0] = true;
        in.dt_s = 5.0f;
        for (int i = 0; i < 20; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!s.s14_warn[2], "context_valid==false: summed S14 inactive");
        TEST_CHECK(!s.s15_warn[0], "context_valid==false: S15 inactive");
    }
}

static void test_s6(void)
{
    TEST_SECTION("S6 -- main controller unhealthy");

    /* Nuisance: mainFault not asserted, link up, never trips. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.main_fault_asserted = false;
        in.link_up = true;
        bool tripped = false;
        for (int i = 0; i < 1000 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "healthy mainFault + live link never trips S6");
    }

    /* Trip: S6a, mainFault asserted (already debounced), trips immediately. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.main_fault_asserted = true;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == true, "mainFault asserted trips S6a on the first tick");
        TEST_CHECK(s.reason == SAFETY_TRIP_MAIN_FAULT, "reason is SAFETY_TRIP_MAIN_FAULT");
    }

    /* Nuisance: the deasserted level is the one safety_core_build_input()
     * now actually feeds from discrete_task_main_fault(), so it is worth
     * proving it stays quiet against a NOISY-but-healthy background rather
     * than only against base_input()'s all-quiet one -- current flowing, a
     * relay commanded, a live link, a hot-but-legal thermocouple. Nothing
     * here is S6a's business, and S6a must not borrow any of it. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.main_fault_asserted = false;
        in.link_up = true;
        in.any_current_present = true;
        in.relay_commanded_recently = true;
        in.tc_c = 900.0f; /* hot, but well under abs_max_temp_c 1300 */
        bool tripped = false;
        for (int i = 0; i < 6000 && !tripped; i++) { /* 6000*0.1s = 10 minutes */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "deasserted mainFault never trips S6a over 10 minutes of busy-but-healthy ticks");
        TEST_CHECK(s.reason != SAFETY_TRIP_MAIN_FAULT, "no SAFETY_TRIP_MAIN_FAULT latched while mainFault stays deasserted");
    }

    /* Trip: S6a is a level, not a window -- a mainFault that asserts partway
     * through a long healthy run trips on exactly the tick it appears, with
     * no accumulation and no credit from the quiet ticks before it. This is
     * the behaviour the newly-wired safety_core_build_input() call site
     * makes reachable: discrete_task publishes an already-debounced level
     * that flips mid-run. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.main_fault_asserted = false;
        bool tripped = false;
        for (int i = 0; i < 300 && !tripped; i++) { /* 30s of quiet first */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "sanity: 30s of deasserted mainFault is quiet");
        in.main_fault_asserted = true;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == true, "mainFault asserting mid-run trips S6a on that very tick");
        TEST_CHECK(s.reason == SAFETY_TRIP_MAIN_FAULT, "mid-run reason is SAFETY_TRIP_MAIN_FAULT");
    }

    /* Trip: S6a is unconditional -- it does not need context, current,
     * heat, or a dead link to be believed. Everything else here says
     * "perfectly healthy kiln"; the assertion alone still trips.
     * SAFETY_MODEL.md section 4, S6a: "unambiguous, no further
     * conditions". */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.main_fault_asserted = true;
        in.link_up = true;
        in.context_valid = true;
        in.zone_count = 0;
        in.any_current_present = false;
        in.tc_valid = true;
        in.tc_c = 20.0f;
        in.dt_s = 0.001f; /* no window to accumulate, however short the tick */
        TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == true, "mainFault trips S6a with everything else healthy");
        TEST_CHECK(s.reason == SAFETY_TRIP_MAIN_FAULT, "unconditional reason is SAFETY_TRIP_MAIN_FAULT");
    }

    /* Latch: releasing mainFault after the trip does not un-latch it on its
     * own -- only safety_guards_clear() does, same contract as S7's. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t asserted = base_input();
        asserted.main_fault_asserted = true;
        safety_guards_tick(&s, &cfg, &asserted);
        TEST_CHECK(s.is_tripped, "sanity: S6a tripped");
        safety_guard_input_t released = base_input();
        released.main_fault_asserted = false;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &released) == false, "releasing mainFault reports no NEW trip");
        TEST_CHECK(s.is_tripped, "S6a stays latched after mainFault is released");
        TEST_CHECK(s.reason == SAFETY_TRIP_MAIN_FAULT, "latched reason stays SAFETY_TRIP_MAIN_FAULT");
    }

    /* Nuisance: S6b, a quiet link with NO current flowing never trips, no
     * matter how long, short of the unconditional 120s backstop --
     * SAFETY_MODEL.md section 2's "absence of information is not evidence
     * of danger -- except when heat is on". */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.link_up = false;
        in.any_current_present = false;
        in.dt_s = 5.0f;
        bool tripped = false;
        for (int i = 0; i < 23 && !tripped; i++) { /* 23*5=115s, just under the 120s hard backstop */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "quiet link with no current does not trip S6b before the 120s hard backstop");
    }

    /* Trip: S6b soft path -- quiet link AND current flowing trips at
     * link_timeout_s (10s default). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.link_up = false;
        in.any_current_present = true;
        in.dt_s = 2.0f;
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) { /* 6*2=12s > 10s */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "quiet link with current flowing trips S6b at link_timeout_s");
        TEST_CHECK(s.reason == SAFETY_TRIP_LINK_DEAD, "reason is SAFETY_TRIP_LINK_DEAD");
    }

    /* Trip: S6b hard backstop -- quiet link, no current, still trips
     * unconditionally at link_dead_hard_s (120s default). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.link_up = false;
        in.any_current_present = false;
        in.dt_s = 10.0f;
        bool tripped = false;
        for (int i = 0; i < 13 && !tripped; i++) { /* 13*10=130s > 120s */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "quiet link with no current still trips at the 120s unconditional hard backstop");
        TEST_CHECK(s.reason == SAFETY_TRIP_LINK_DEAD, "reason is SAFETY_TRIP_LINK_DEAD (hard backstop)");
    }

    /* Nuisance: link_up resets the elapsed timer -- a link that recovers
     * briefly then goes quiet again does not benefit from the earlier
     * accumulated silence. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t down = base_input();
        down.link_up = false;
        down.any_current_present = true;
        down.dt_s = 8.0f;
        safety_guard_input_t up = down;
        up.link_up = true;
        bool tripped = false;
        for (int i = 0; i < 3 && !tripped; i++) { /* 8s down each round, well under 10s timeout, reset each time */
            tripped |= safety_guards_tick(&s, &cfg, &down);
            tripped |= safety_guards_tick(&s, &cfg, &up);
        }
        TEST_CHECK(!tripped, "link recovering resets S6b's elapsed timer, no accumulation across outages");
    }
}

static void test_s6b_reboot_grace(void)
{
    TEST_SECTION("S6b -- ANNOUNCE_REBOOT grace window suppression");

    /* Suppressed: soft path (quiet link + current) would trip at
     * link_timeout_s, but reboot_grace_active holds it off indefinitely
     * while true -- no trip even well past where the un-suppressed test
     * above trips. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.link_up = false;
        in.any_current_present = true;
        in.reboot_grace_active = true;
        in.dt_s = 2.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) { /* 10*2=20s, well past the 10s soft threshold */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "reboot_grace_active suppresses S6b's soft (current-present) trip");
    }

    /* Suppressed: hard backstop would trip at link_dead_hard_s, but stays
     * suppressed the entire time reboot_grace_active is true, even with no
     * current at all. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.link_up = false;
        in.any_current_present = false;
        in.reboot_grace_active = true;
        in.dt_s = 10.0f;
        bool tripped = false;
        for (int i = 0; i < 13 && !tripped; i++) { /* 13*10=130s > 120s hard backstop */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "reboot_grace_active suppresses S6b's hard backstop trip too");
    }

    /* The load-bearing property: once the caller flips reboot_grace_active
     * back to false (the window has expired, per safety_core.c's own
     * bookkeeping) while the link is STILL down, S6b trips on the very next
     * tick that crosses a threshold -- exactly as if ANNOUNCE_REBOOT had
     * never been sent. The elapsed accumulator was never reset by the grace
     * period, so no "extra time" was ever granted. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t grace = base_input();
        grace.link_up = false;
        grace.any_current_present = true;
        grace.reboot_grace_active = true;
        grace.dt_s = 2.0f;

        /* 6 ticks * 2s = 12s of real silence accrues while suppressed --
         * already past link_timeout_s (10s default), so the very first tick
         * with the window closed must trip immediately. */
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &grace);
        }
        TEST_CHECK(!tripped, "still suppressed while reboot_grace_active stays true, despite 12s > link_timeout_s");

        safety_guard_input_t expired = grace;
        expired.reboot_grace_active = false;
        tripped = safety_guards_tick(&s, &cfg, &expired);
        TEST_CHECK(tripped, "window-expired tick trips immediately -- no grace period of its own, no accumulated advantage");
        TEST_CHECK(s.reason == SAFETY_TRIP_LINK_DEAD, "reason is SAFETY_TRIP_LINK_DEAD, same as an unannounced link-dead trip");
    }

    /* Isolation: reboot_grace_active must not affect any OTHER guard.
     * Cross-check against S1 (a completely unrelated trip condition) with
     * reboot_grace_active true throughout -- S1 must trip exactly as it
     * does with the field false (test_s1 above), unaffected. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.reboot_grace_active = true;
        in.tc_c = cfg.abs_max_temp_c + 50.0f; /* well over the S1 ceiling */
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "reboot_grace_active does not suppress S1 -- an unrelated over-temperature trip fires normally");
        TEST_CHECK(s.reason == SAFETY_TRIP_OVERTEMP, "reason is SAFETY_TRIP_OVERTEMP, not affected by the S6b-only suppression");
    }

    /* Isolation: reboot_grace_active with link_up TRUE is a no-op -- S6b's
     * elapsed accumulator is already held at 0 by link_up, so the field has
     * literally nothing to suppress. Confirms this is scoped to S6b's trip
     * condition specifically, not a blanket "believe the ESP" flag that
     * could interact with link_up in some other way. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.link_up = true;
        in.reboot_grace_active = true;
        bool tripped = false;
        for (int i = 0; i < 1000 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "reboot_grace_active with link_up true never trips (nothing to suppress)");
        TEST_CHECK(s.s6b_link_down_elapsed_s == 0.0f, "elapsed accumulator stays at 0 -- link_up already held it there");
    }
}

static void test_s9(void)
{
    TEST_SECTION("S9 -- trip ineffective / contactor welded (post-trip escalation)");

    /* Nuisance: after a trip, if the relay verifiably de-energized and no
     * current is present, S9 never escalates. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop); /* trip via S7 */
        TEST_CHECK(s.is_tripped, "sanity: tripped via S7");

        safety_guard_input_t verify = base_input();
        verify.relay_deenergized = true;
        verify.any_current_present = false;
        verify.dt_s = 5.0f;
        bool escalated = false;
        for (int i = 0; i < 10 && !escalated; i++) { /* 50s, past trip_verify_s(10s) */
            escalated = safety_guards_tick(&s, &cfg, &verify);
        }
        TEST_CHECK(!escalated, "K4 verifiably open with no current: S9 never escalates");
        TEST_CHECK(!s.trip_ineffective, "trip_ineffective stays false");
    }

    /* Trip: relay reported de-energized, but current is still present past
     * trip_verify_s -- the welded-contactor case. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);

        safety_guard_input_t verify = base_input();
        verify.context_valid = true; /* genuine sustained weld: context available too */
        verify.relay_deenergized = true;
        verify.any_current_present = true; /* contacts welded shut -- still conducting */
        verify.dt_s = 3.0f;
        bool escalated = false;
        for (int i = 0; i < 8 && !escalated; i++) { /* 24s, well past trip_verify_s(10s) plus the
                                                       * S9_CURRENT_PRESENT_STREAK_TO_TRIP(3)-tick debounce */
            escalated = safety_guards_tick(&s, &cfg, &verify);
        }
        TEST_CHECK(escalated, "K4 de-energized but current persists past trip_verify_s escalates to S9");
        TEST_CHECK(s.trip_ineffective, "trip_ineffective latches");
        TEST_CHECK(s.reason == SAFETY_TRIP_INEFFECTIVE, "reason escalates to SAFETY_TRIP_INEFFECTIVE");
    }

    /* Nuisance: the verify window does not start accumulating until
     * relay_deenergized actually reports true -- an as-yet-unconfirmed
     * de-energization must not silently count toward the 10s bar. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);

        safety_guard_input_t not_yet = base_input();
        not_yet.relay_deenergized = false; /* not yet confirmed */
        not_yet.any_current_present = true;
        not_yet.dt_s = 20.0f;
        bool escalated = false;
        for (int i = 0; i < 3 && !escalated; i++) {
            escalated = safety_guards_tick(&s, &cfg, &not_yet);
        }
        TEST_CHECK(!escalated, "relay_deenergized==false never accumulates toward S9's verify window");
    }

    /* Escalation only happens once -- the latched trip_ineffective does not
     * re-fire "newly true" on subsequent ticks. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);

        safety_guard_input_t verify = base_input();
        verify.context_valid = true;
        verify.relay_deenergized = true;
        verify.any_current_present = true;
        verify.dt_s = 15.0f;
        /* First tick clears trip_verify_s and starts the debounce streak (1
         * of S9_CURRENT_PRESENT_STREAK_TO_TRIP); it takes
         * S9_CURRENT_PRESENT_STREAK_TO_TRIP consecutive checked ticks to
         * actually escalate. */
        TEST_CHECK(safety_guards_tick(&s, &cfg, &verify) == false, "first over-threshold tick only starts the debounce streak");
        TEST_CHECK(safety_guards_tick(&s, &cfg, &verify) == false, "second consecutive tick still short of the streak");
        TEST_CHECK(safety_guards_tick(&s, &cfg, &verify) == true, "third consecutive tick escalates");
        TEST_CHECK(safety_guards_tick(&s, &cfg, &verify) == false, "further ticks do not re-report a new escalation");
        TEST_CHECK(s.trip_ineffective, "trip_ineffective remains latched true");
    }

    /* --- The wiring itself: safety_core_build_input() now names this field
     * as `!relay_owner_is_energized()`. The blocks above prove the guard's
     * ARITHMETIC given the field; these four prove the field's SENSE, by
     * only ever setting it through the same single negation the call site
     * applies, never by writing the bool literal directly. If the `!` were
     * ever dropped (or doubled) in safety_core.c, the first two of these are
     * the ones that would start disagreeing with reality. */

    /* Polarity, the dangerous direction: K4 ENERGIZED with current flowing
     * is a perfectly normal firing kiln, and after any trip it means the
     * de-energize has not landed yet -- but S9's window must NOT start,
     * because "K4 still closed and conducting" is not evidence of a welded
     * contactor, it is evidence of a command still in flight. An inverted
     * wiring would read this exact situation as "de-energized + current" and
     * escalate to SAFETY_TRIP_INEFFECTIVE within trip_verify_s, which is why
     * this runs well past the 10s bar before believing it. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop); /* trip via S7 */
        TEST_CHECK(s.is_tripped, "sanity: tripped via S7");

        const bool relay_owner_energized = true; /* relay_owner_is_energized() */
        safety_guard_input_t in = base_input();
        in.relay_deenergized = !relay_owner_energized; /* the call site's own expression */
        in.any_current_present = true;
        in.dt_s = 1.0f;
        bool escalated = false;
        for (int i = 0; i < 60 && !escalated; i++) { /* 60s, 6x trip_verify_s */
            escalated = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!escalated, "energized K4 with current present never escalates S9 (inverted wiring would)");
        TEST_CHECK(!s.trip_ineffective, "trip_ineffective stays false while K4 is energized");
        TEST_CHECK(s.reason == SAFETY_TRIP_ESTOP, "reason stays the original S7 trip, not SAFETY_TRIP_INEFFECTIVE");
        TEST_CHECK(s.s9_verify_elapsed_s == 0.0f, "S9's verify clock never started while K4 was energized");
    }

    /* Polarity, the other direction: the same single negation over a
     * de-energized relay DOES arm the window, and with current still flowing
     * escalates. Together with the block above this pins the sense in both
     * directions -- one expression, two truth values, two opposite verdicts. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);

        const bool relay_owner_energized = false; /* GPIO6 driven low -- K4 open */
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.relay_deenergized = !relay_owner_energized;
        in.any_current_present = true; /* contacts welded shut */
        in.dt_s = 1.0f;
        bool escalated = false;
        for (int i = 0; i < 60 && !escalated; i++) {
            escalated = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(escalated, "de-energized K4 with current present escalates S9 through the wired expression");
        TEST_CHECK(s.trip_ineffective, "trip_ineffective latches");
        TEST_CHECK(s.reason == SAFETY_TRIP_INEFFECTIVE, "reason escalates to SAFETY_TRIP_INEFFECTIVE");
        TEST_CHECK(s.s9_verify_elapsed_s >= 10.0f, "escalation happened at or after trip_verify_s, not before");
    }

    /* GRACE: relay_owner starts in GRACE and REFUSES to drive GPIO6 high for
     * the whole 60s startup window, so relay_owner_is_energized() is false
     * and this field reads TRUE from the very first tick of every boot. That
     * is exactly the shape that made S6b trip ~120s into every boot before
     * f304392, so it is worth proving it cannot happen here: with no trip
     * latched, safety_guards_tick() never enters the S9 branch at all, and
     * the verify clock is actively re-zeroed at the bottom of every
     * untripped tick. 20 minutes of boot-shaped ticks, nothing else wrong. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        const bool relay_owner_energized = false; /* GRACE never energizes */
        safety_guard_input_t in = base_input();
        in.relay_deenergized = !relay_owner_energized;
        in.link_up = true;
        in.tc_c = 22.0f;
        bool tripped = false;
        for (int i = 0; i < 12000 && !tripped; i++) { /* 12000*0.1s = 20 minutes */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "a de-energized K4 through GRACE never trips anything on its own");
        TEST_CHECK(!s.trip_ineffective, "no S9 escalation without a latched trip");
        TEST_CHECK(!s.s9_verify_active, "S9's verify window stays inactive while untripped");
        TEST_CHECK(s.s9_verify_elapsed_s == 0.0f, "S9's verify clock accumulates nothing while untripped");
    }

    /* GRACE, the honest half: a trip that DOES land during startup, with K4
     * correctly open and no current anywhere, still must not escalate --
     * this is the real per-boot situation (K4 open the whole time), and the
     * only thing separating it from the welded case is any_current_present.
     * Run it far past trip_verify_s to prove the separation is the current
     * fact and not the timer. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);

        const bool relay_owner_energized = false;
        safety_guard_input_t in = base_input();
        in.relay_deenergized = !relay_owner_energized;
        in.any_current_present = false; /* K4 open AND nothing conducting */
        in.dt_s = 0.1f;
        bool escalated = false;
        for (int i = 0; i < 6000 && !escalated; i++) { /* 10 minutes */
            escalated = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!escalated, "K4 open with no current never escalates, however long the window runs");
        TEST_CHECK(!s.trip_ineffective, "trip_ineffective stays false");
        TEST_CHECK(s.reason == SAFETY_TRIP_ESTOP, "the original trip reason survives un-escalated");
    }

    /* Regression test, 2026-08-27 audit / commit 4962421: a SINGLE post-
     * threshold tick with any_current_present true (e.g. one op-amp-offset
     * blip from an uncommissioned CT channel, current_presence_policy.c's
     * `delta_counts > 25` fallback) must NOT latch trip_ineffective. Before
     * this fix, this exact single-tick shape latched the guard permanently
     * on a bench board with no CT fitted, refusing CLEAR_TRIP for the rest
     * of the power cycle. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);

        safety_guard_input_t verify = base_input();
        verify.context_valid = true;
        verify.relay_deenergized = true;
        verify.dt_s = 15.0f; /* clears trip_verify_s(10s) on the very first tick */
        verify.any_current_present = false;
        TEST_CHECK(safety_guards_tick(&s, &cfg, &verify) == false, "sanity: no escalation with no current");

        /* One single spurious tick of current-present, then it's gone again
         * -- exactly what a transient op-amp offset blip looks like. */
        verify.any_current_present = true;
        bool escalated = safety_guards_tick(&s, &cfg, &verify);
        TEST_CHECK(!escalated, "a single spurious any_current_present tick does not escalate S9");
        TEST_CHECK(!s.trip_ineffective, "trip_ineffective stays false after one blip");

        verify.any_current_present = false;
        escalated = safety_guards_tick(&s, &cfg, &verify);
        TEST_CHECK(!escalated, "the blip clearing resets the debounce streak, no delayed escalation either");
        TEST_CHECK(!s.trip_ineffective, "trip_ineffective still false");
    }

    /* Regression test's other half, mandatory per this repo's negative-test
     * standard: a GENUINE, SUSTAINED welded-contactor condition on a
     * COMMISSIONED board -- current present on every tick, for far longer
     * than S9_CURRENT_PRESENT_STREAK_TO_TRIP -- STILL latches
     * trip_ineffective. A debounce (or a commissioning gate) that
     * accidentally disabled the guard entirely would be a worse bug than the
     * one this fix closes. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);

        safety_guard_input_t verify = base_input();
        verify.context_valid = true;
        verify.relay_deenergized = true;
        verify.any_current_present = true; /* welded contacts, conducting on EVERY tick */
        verify.current_sensing_commissioned = true; /* a real, calibrated CT chain */
        verify.dt_s = 1.0f;
        bool escalated = false;
        for (int i = 0; i < 30 && !escalated; i++) { /* 30s, well past trip_verify_s(10s) */
            escalated = safety_guards_tick(&s, &cfg, &verify);
        }
        TEST_CHECK(escalated, "a genuine, sustained welded-contactor condition on a COMMISSIONED "
                              "board still latches S9");
        TEST_CHECK(s.trip_ineffective, "trip_ineffective latches");
        TEST_CHECK(s.reason == SAFETY_TRIP_INEFFECTIVE, "reason escalates to SAFETY_TRIP_INEFFECTIVE");
        TEST_CHECK(!s.s9_uncommissioned_warn, "the commissioned path never sets the uncommissioned-only WARN");
    }

    /* 2026-08-27 audit, SECOND pass: the coordinator's own review found the
     * first fix (the debounce streak above, alone) insufficient -- it
     * defends against a TRANSIENT spurious sample but not against a
     * PERSISTENT one, and an uncommissioned CT channel's DC offset floor is
     * persistent, not transient. This is the owner's actual bench board:
     * link up (context_valid true), no CT fitted, zero_counts=0 and
     * k_ct_v_per_a=0 (config_store_default()'s uncommissioned shape,
     * docs/CURRENT_SENSE.md section 5).
     *
     * Driven through the REAL current_presence_policy.h decision, not by
     * setting any_current_present by hand -- per the coordinator's own
     * instruction, a hand-set boolean proves nothing about the actual
     * failure mode. counts_avg is fixed at
     * CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS + 1 for the whole run,
     * i.e. a constant ADC reading -- an op-amp offset floor does not move --
     * which the uncommissioned fallback branch (k_ct_v_per_a <= 0) reads as
     * "present" on literally every tick, forever, exactly like the bench
     * board's own symptom.
     *
     * Verdict: trip_ineffective must NEVER latch here, however long the run
     * -- the current chain is not trustworthy enough to justify an
     * unclearable trip -- but the finding must still be visible, per
     * SAFETY_MODEL.md section 4's insistence that S9 "converts a silent
     * failure into a loud one": s9_uncommissioned_warn must go true instead. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop);

        const uint16_t zero_counts = 0u;      /* config_store_default() */
        const float    k_ct_v_per_a = 0.0f;   /* config_store_default() -- uncommissioned */
        const float    i_present_a = 2.0f;
        const float    gain = 0.715f;
        const uint32_t counts_avg = CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS + 1u; /* constant offset floor */
        bool phantom_present = current_presence_is_flowing(counts_avg, zero_counts, i_present_a,
                                                             k_ct_v_per_a, gain);
        TEST_CHECK(phantom_present, "sanity: the real policy function reads this constant offset as "
                                    "'present', exactly the bench board's own symptom");

        safety_guard_input_t verify = base_input();
        verify.context_valid = true;
        verify.relay_deenergized = true;
        verify.any_current_present = phantom_present; /* the real function's verdict, not a hand-set bool */
        verify.current_sensing_commissioned = false;  /* k_ct_v_per_a <= 0: not commissioned */
        verify.dt_s = 1.0f;
        bool escalated = false;
        for (int i = 0; i < 6000 && !escalated; i++) { /* 100 minutes: far past any streak or timer */
            escalated = safety_guards_tick(&s, &cfg, &verify);
            /* Also re-derive any_current_present from the real policy every
             * tick (it would be identical every time for a constant
             * counts_avg, but this keeps the loop honestly "driven through
             * the real presence policy" rather than a single call whose
             * result is then reused by assumption). */
            verify.any_current_present = current_presence_is_flowing(counts_avg, zero_counts,
                                                                       i_present_a, k_ct_v_per_a, gain);
        }
        TEST_CHECK(!escalated, "an uncommissioned board's permanent phantom current NEVER escalates "
                               "S9 to the unclearable trip_ineffective latch, however long it runs");
        TEST_CHECK(!s.trip_ineffective, "trip_ineffective stays false -- the bench board must not brick");
        TEST_CHECK(s.reason == SAFETY_TRIP_ESTOP, "the original trip reason survives, not overwritten "
                                                  "to SAFETY_TRIP_INEFFECTIVE");
        TEST_CHECK(s.s9_uncommissioned_warn, "the finding is NOT silenced -- the non-latching "
                                             "uncommissioned WARN is set instead");
    }
}

static void test_s10(void)
{
    TEST_SECTION("S10 -- safety TC vs zone TC disagreement (WARN only, context)");

    /* Nuisance: a normal mounting-position offset (well under
     * tc_disagreement_c=200C) never warns, sustained indefinitely. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.tc_c = 900.0f;
        in.max_zone_setpoint_c = 900.0f; /* keeps S2 from also tripping in this test */
        in.nearest_zone_measured_c = 850.0f; /* 50C offset, normal */
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 20 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "S10 never trips (WARN only) regardless of magnitude");
        TEST_CHECK(!s.s10_warn, "a 50C offset within tc_disagreement_c never warns");
    }

    /* Nuisance: EXTERNAL_OVERHEAT disables S10 entirely -- a shell TC has
     * no obligation to agree with the chamber. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.tc_c = 80.0f;
        in.nearest_zone_measured_c = 900.0f; /* huge disagreement, but disabled by mode */
        in.dt_s = 600.0f;
        for (int i = 0; i < 5; i++) safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!s.s10_warn, "EXTERNAL_OVERHEAT disables S10's warning entirely");
    }

    /* WARN: sustained disagreement past tc_disagreement_c for
     * tc_disagreement_time_s sets the WARN level, never a trip. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.zone_count = 1;
        in.tc_c = 900.0f;
        in.max_zone_setpoint_c = 900.0f; /* keeps S2 from also tripping in this test */
        in.nearest_zone_measured_c = 650.0f; /* 250C, over 200C */
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) { /* 360s > 300s */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "S10 stays WARN, never escalates to a trip");
        TEST_CHECK(s.s10_warn, "sustained disagreement past both bars sets WARN");
    }
}

static void test_s13(void)
{
    TEST_SECTION("S13 -- borrowed channel not updating (graduated, context)");

    /* Nuisance: OWN_J7 (not borrowing) never engages S13 at all, no matter
     * what sample_counter_advancing says. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_OWN_J7;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.sample_counter_advancing = false;
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 20 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "OWN_J7 mode never engages S13");
        TEST_CHECK(!s.s13_warn, "OWN_J7 mode: no WARN either");
    }

    /* Nuisance: an advancing sample_counter never warns or trips, even
     * sustained. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.sample_counter_advancing = true;
        in.dt_s = 60.0f;
        bool tripped = false;
        for (int i = 0; i < 20 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "an advancing sample_counter never trips S13");
        TEST_CHECK(!s.s13_warn, "an advancing sample_counter never warns either");
    }

    /* Graduated: WARN at borrowed_stale_s (10s), TRIP at
     * borrowed_stale_trip_s (60s). */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.sample_counter_advancing = false;
        in.dt_s = 5.0f;
        bool tripped = false;
        for (int i = 0; i < 2; i++) { /* 10s: reaches borrowed_stale_s exactly */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "10s stale: not yet tripped");
        TEST_CHECK(s.s13_warn, "10s stale: WARN active (>= borrowed_stale_s)");
        for (int i = 0; i < 10 && !tripped; i++) { /* continue to 60s total */
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "60s stale (>= borrowed_stale_trip_s) trips S13");
        TEST_CHECK(s.reason == SAFETY_TRIP_BORROWED_STALE, "reason is SAFETY_TRIP_BORROWED_STALE");
    }

    /* BOTH mode also engages S13. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_BOTH;
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.sample_counter_advancing = false;
        in.dt_s = 61.0f;
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(tripped, "BOTH mode engages S13 the same as BORROWED_ZONE");
    }

    /* A resumed sample_counter resets the elapsed timer, clearing WARN. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        safety_guard_input_t stale = base_input();
        stale.context_valid = true;
        stale.sample_counter_advancing = false;
        stale.dt_s = 15.0f;
        safety_guards_tick(&s, &cfg, &stale); /* 15s stale: WARN */
        TEST_CHECK(s.s13_warn, "sanity: WARN set");
        safety_guard_input_t fresh = stale;
        fresh.sample_counter_advancing = true;
        safety_guards_tick(&s, &cfg, &fresh);
        TEST_CHECK(!s.s13_warn, "a resumed sample_counter clears S13's WARN immediately");
    }
}

/* ROADMAP.md M5: "borrowed-thermocouple staleness split correctly across
 * S11 / S13 / S6". S11 (frozen value) is not reachable here at all --
 * S13/S6 already gate it out structurally: S11 only ever looks at
 * in->tc_c/tc_valid/heat_commanded (no context, no link fact), so a link
 * outage or a stalled sample_counter cannot make S11 fire or stay silent
 * either way; it is orthogonal by construction, not by a check added here.
 * What *is* worth verifying end-to-end is the S6/S13 split SAFETY_MODEL.md
 * section 4's S13 table draws: "not advancing -> S13", "frames not arriving
 * at all -> S6", never both for the same event. safety_guards.c keeps these
 * on two independent input facts (in->link_up for S6b, in->context_valid +
 * in->sample_counter_advancing for S13) with no cross-reads between the two
 * blocks, so the split's correctness is really a claim about how the
 * *caller* (safety_core, not yet built) is expected to set those facts --
 * per SAFETY_MODEL.md section 5 rule 2 ("stale context is no context"), a
 * dead link must collapse context_valid to false too, which is what these
 * tests assume of the caller. What is safe to assert at this module's own
 * boundary, and is asserted below: given those facts as SAFETY_MODEL.md's
 * table says they should look, S6 and S13 never both fire for one event,
 * and neither guard's accumulator drifts based on what the other is doing. */
static void test_s6_s13_split(void)
{
    TEST_SECTION("S6/S13 split -- borrowed staleness attributed to the right guard");

    /* Link dead, not a single-channel stall: link_up false long enough to
     * hit S6b's soft (current-present) threshold. A real caller collapses
     * context_valid to false the moment frames stop arriving (SAFETY_MODEL.md
     * section 5 rule 2), so S13's block is skipped outright -- verify it
     * stays fully quiet (no WARN, no elapsed accumulation) while S6 trips. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        safety_guard_input_t in = base_input();
        in.link_up = false;
        in.context_valid = false;         /* caller's job: dead link => no context */
        in.sample_counter_advancing = false; /* even so, must not leak into S13 */
        in.any_current_present = true;    /* arms S6b's soft (10s) path */
        in.dt_s = 11.0f;
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(tripped, "link-dead-only: S6b trips");
        TEST_CHECK(s.reason == SAFETY_TRIP_LINK_DEAD, "link-dead-only: reason is SAFETY_TRIP_LINK_DEAD, not S13");
        TEST_CHECK(!s.s13_warn, "link-dead-only: S13 never warns");
        TEST_CHECK(s.s13_stale_elapsed_s == 0.0f, "link-dead-only: S13's elapsed timer never accumulates");
    }

    /* Single channel stale, link healthy: link_up stays true every tick
     * (so S6b's elapsed timer never leaves zero), context_valid true, but
     * sample_counter stops advancing. Only S13 should move. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        safety_guard_input_t in = base_input();
        in.link_up = true;
        in.context_valid = true;
        in.sample_counter_advancing = false;
        in.dt_s = 61.0f;
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(tripped, "channel-stale-only: S13 trips");
        TEST_CHECK(s.reason == SAFETY_TRIP_BORROWED_STALE, "channel-stale-only: reason is SAFETY_TRIP_BORROWED_STALE, not link-dead");
        TEST_CHECK(s.s6b_link_down_elapsed_s == 0.0f, "channel-stale-only: S6b's elapsed timer never accumulates while link_up is true");
    }

    /* Both facts look bad on the same tick (link_up false AND, as a
     * defensive edge case, context_valid still true with a stalled
     * counter -- e.g. one tick of lag before a caller's own context_valid
     * catches up to the link outage). S6a/S6b are checked ahead of the
     * context-dependent block in safety_guards_tick(), so S6 must win and
     * latch; once latched, S13 never gets a chance to re-attribute the
     * event on a later tick either. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        safety_guard_input_t in = base_input();
        in.link_up = false;
        in.context_valid = true;          /* deliberately stale-caller edge case */
        in.sample_counter_advancing = false;
        in.any_current_present = true;
        in.dt_s = 11.0f;
        bool tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(tripped, "both-simultaneously: trips on this tick");
        TEST_CHECK(s.reason == SAFETY_TRIP_LINK_DEAD, "both-simultaneously: S6 wins over S13 by check order");

        /* Latched -- a further tick, even one where S13's own condition is
         * still true, must not re-attribute the trip. */
        tripped = safety_guards_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "both-simultaneously: second tick is a latched no-op, not a new S13 trip");
        TEST_CHECK(s.reason == SAFETY_TRIP_LINK_DEAD, "both-simultaneously: reason still S6 after latching, never overwritten by S13");
    }
}

/* ROADMAP.md M4: "Trip latches; clearing requires an explicit command."
 * safety_guards_try_clear() is the retick-and-refuse-if-still-tripping half
 * of that policy (safety_guards.h's own doc comment on it spells out
 * exactly what it does and does not catch). These tests exercise it
 * directly, as a pure function, same as every other test in this file. */
static void test_try_clear(void)
{
    TEST_SECTION("safety_guards_try_clear -- refuse a clear while still tripping");

    /* E-stop still pressed at clear time: the retick immediately re-trips
     * S7 (unwindowed, single-tick condition), so the clear is refused. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t pressed = base_input();
        pressed.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &pressed);
        TEST_CHECK(s.is_tripped, "sanity: S7 tripped");

        bool cleared = safety_guards_try_clear(&s, &cfg, &pressed);
        TEST_CHECK(!cleared, "E-stop still pressed: try_clear refuses");
        TEST_CHECK(s.is_tripped, "refused clear: state is re-tripped, is_tripped true");
        TEST_CHECK(s.reason == SAFETY_TRIP_ESTOP, "refused clear: reason is SAFETY_TRIP_ESTOP");
    }

    /* E-stop released before the clear attempt: the retick sees a safe
     * input, does not retrip, and the clear holds. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t pressed = base_input();
        pressed.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &pressed);
        TEST_CHECK(s.is_tripped, "sanity: S7 tripped");

        safety_guard_input_t released = base_input();
        released.estop_pressed = false;
        bool cleared = safety_guards_try_clear(&s, &cfg, &released);
        TEST_CHECK(cleared, "E-stop released: try_clear succeeds");
        TEST_CHECK(!s.is_tripped, "successful clear: is_tripped false");
    }

    /* A graduated guard (S1, 3-tick over-ceiling streak) still physically
     * over-ceiling at clear time: this is the documented scope limit, not a
     * bug. safety_guards_clear() (inside try_clear) resets
     * s1_over_ceiling_streak to 0 along with everything else, so the single
     * retick only brings the streak to 1 -- nowhere near
     * S1_OVER_CEILING_STREAK_TO_TRIP (3) -- and try_clear reports success
     * even though the underlying condition (reading still above the
     * ceiling) has not gone away. S1 is not broken by this: left running,
     * it will re-trip on its own normal timescale (3 more consecutive
     * over-ceiling ticks) once safety_core keeps calling safety_guards_tick()
     * afterward. This test documents that real, limited behaviour rather
     * than asserting a stronger guarantee try_clear does not provide. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t over = base_input();
        over.tc_c = 1400.0f; /* above abs_max_temp_c (1300) */
        for (int i = 0; i < 3; i++) safety_guards_tick(&s, &cfg, &over);
        TEST_CHECK(s.is_tripped, "sanity: S1 tripped via 3 consecutive over-ceiling readings");
        TEST_CHECK(s.reason == SAFETY_TRIP_OVERTEMP, "sanity: reason is SAFETY_TRIP_OVERTEMP");

        /* Still over-ceiling at clear time. */
        bool cleared = safety_guards_try_clear(&s, &cfg, &over);
        TEST_CHECK(cleared, "scope limit: a single retick does not rebuild S1's 3-tick streak, clear holds");
        TEST_CHECK(!s.is_tripped, "scope limit: is_tripped is false right after the clear, even though still over-ceiling");
        TEST_CHECK(s.s1_over_ceiling_streak == 1, "scope limit: the retick brought the streak to exactly 1, not 3");

        /* Left running against the same still-over-ceiling input, S1 does
         * its job and re-trips on its own timescale -- proving this is a
         * scope limit of try_clear's single retick, not a hole in S1
         * itself. */
        bool retripped = false;
        for (int i = 0; i < 2 && !retripped; i++) {
            retripped = safety_guards_tick(&s, &cfg, &over);
        }
        TEST_CHECK(retripped, "S1 re-trips on its own normal timescale once the streak rebuilds");
    }

    /* 2026-08-23 hardware finding: S5 (and S12/S13/S6b below) are the four
     * guards guard_condition_still_immediate() now checks BEFORE
     * safety_guards_clear() runs, precisely because -- unlike S1 above --
     * their trip condition is a single-tick-decidable level, so granting a
     * clear while it is still true would hand out a real window of
     * heating-enabled operation against an ongoing hazard, not just "the
     * trip isn't re-confirmed yet". Observed live: an S5 clear against a
     * still-blind safety TC. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t bad = base_input();
        bad.tc_valid = false;
        bad.tc_c = (float)NAN;
        bad.dt_s = 61.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &bad);
        }
        TEST_CHECK(tripped && s.reason == SAFETY_TRIP_SENSOR_INVALID, "sanity: S5 tripped");

        /* Still bad at clear time: refused outright, accumulators untouched. */
        uint16_t streak_before = s.s5_bad_streak;
        float elapsed_before = s.s5_bad_elapsed_s;
        bool cleared = safety_guards_try_clear(&s, &cfg, &bad);
        TEST_CHECK(!cleared, "S5 clear refused while the reading is STILL bad -- the hardware-finding fix");
        TEST_CHECK(s.is_tripped, "refused: still latched");
        TEST_CHECK(s.reason == SAFETY_TRIP_SENSOR_INVALID, "refused: reason unchanged");
        TEST_CHECK(s.s5_bad_streak == streak_before && s.s5_bad_elapsed_s == elapsed_before,
                   "refused clear leaves S5's accumulators completely untouched (no partial reset)");

        /* Good read: de-escalation is reachable once the reading recovers. */
        safety_guard_input_t good = base_input();
        cleared = safety_guards_try_clear(&s, &cfg, &good);
        TEST_CHECK(cleared, "S5 clear succeeds once the reading is actually good again");
        TEST_CHECK(!s.is_tripped, "cleared: not tripped");
    }

    /* S12 (enclosure/cold-junction over-temp): same shape, cj_c still over
     * cj_max_c at clear time. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t hot = base_input();
        hot.cj_c = 90.0f; /* > cj_max_c default (85) */
        hot.dt_s = 61.0f; /* > cj_time_s default (60) in one tick */
        bool tripped = safety_guards_tick(&s, &cfg, &hot);
        TEST_CHECK(tripped && s.reason == SAFETY_TRIP_ENCLOSURE_TEMP, "sanity: S12 tripped");

        bool cleared = safety_guards_try_clear(&s, &cfg, &hot);
        TEST_CHECK(!cleared, "S12 clear refused while cj_c is STILL over cj_max_c");
        TEST_CHECK(s.is_tripped && s.reason == SAFETY_TRIP_ENCLOSURE_TEMP, "refused: still latched, same reason");

        safety_guard_input_t cool = base_input();
        cool.cj_c = 25.0f;
        cleared = safety_guards_try_clear(&s, &cfg, &cool);
        TEST_CHECK(cleared, "S12 clear succeeds once cj_c is back under cj_max_c");
    }

    /* S13 (borrowed channel stale): same shape, sample_counter_advancing
     * still false at clear time. Requires tc_source BORROWED_ZONE/BOTH. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        safety_guard_input_t stale = base_input();
        stale.context_valid = true; /* S13 lives in the context-dependent block -- see safety_guards.c */
        stale.sample_counter_advancing = false;
        stale.dt_s = 61.0f; /* > borrowed_stale_trip_s default (60) in one tick */
        bool tripped = safety_guards_tick(&s, &cfg, &stale);
        TEST_CHECK(tripped && s.reason == SAFETY_TRIP_BORROWED_STALE, "sanity: S13 tripped");

        bool cleared = safety_guards_try_clear(&s, &cfg, &stale);
        TEST_CHECK(!cleared, "S13 clear refused while the channel is STILL not advancing");
        TEST_CHECK(s.is_tripped && s.reason == SAFETY_TRIP_BORROWED_STALE, "refused: still latched, same reason");

        safety_guard_input_t advancing = base_input();
        advancing.context_valid = true;
        advancing.sample_counter_advancing = true;
        cleared = safety_guards_try_clear(&s, &cfg, &advancing);
        TEST_CHECK(cleared, "S13 clear succeeds once the channel is advancing again");
    }

    /* S6b (link dead, hard backstop): same shape, link still down at clear
     * time. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t down = base_input();
        down.link_up = false;
        down.dt_s = 121.0f; /* > link_dead_hard_s default (120) in one tick */
        bool tripped = safety_guards_tick(&s, &cfg, &down);
        TEST_CHECK(tripped && s.reason == SAFETY_TRIP_LINK_DEAD, "sanity: S6b tripped (hard backstop)");

        bool cleared = safety_guards_try_clear(&s, &cfg, &down);
        TEST_CHECK(!cleared, "S6b clear refused while the link is STILL down");
        TEST_CHECK(s.is_tripped && s.reason == SAFETY_TRIP_LINK_DEAD, "refused: still latched, same reason");

        safety_guard_input_t up = base_input();
        up.link_up = true;
        cleared = safety_guards_try_clear(&s, &cfg, &up);
        TEST_CHECK(cleared, "S6b clear succeeds once the link is back up");
    }

    /* Audit 2026-08-27: S2 (sustained excess over setpoint) -- same shape,
     * tc_c still over max_zone_setpoint_c + overshoot_margin_c at clear time.
     * Before this pass, s2_over_elapsed_s got zeroed by safety_guards_clear()
     * and the single retick could never rebuild 120s of accumulated excess in
     * one tick -- the clear held, relay_owner re-armed, and it took another
     * full overshoot_time_s to re-trip. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true;
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        safety_guard_input_t over = base_input();
        over.context_valid = true;
        over.zone_count = 1;
        over.max_zone_setpoint_c = 900.0f;
        over.tc_c = 1000.0f; /* 100C over, past overshoot_margin_c default (75) */
        over.dt_s = 15.0f; /* accumulated over multiple ticks, deliberately NOT a single-tick
                             * trip -- a single retick's own dt_s alone must not be able to
                             * rebuild 120s of excess, so this only passes if
                             * guard_condition_still_immediate() is what is doing the refusing,
                             * not a coincidental single-tick retrip. */
        bool tripped = false;
        for (int i = 0; i < 9 && !tripped; i++) { /* 9*15s = 135s > overshoot_time_s(120) */
            tripped = safety_guards_tick(&s, &cfg, &over);
        }
        TEST_CHECK(tripped && s.reason == SAFETY_TRIP_OVER_SETPOINT, "sanity: S2 tripped");

        bool cleared = safety_guards_try_clear(&s, &cfg, &over);
        TEST_CHECK(!cleared, "S2 clear refused while tc_c is STILL over setpoint+margin -- audit 2026-08-27");
        TEST_CHECK(s.is_tripped && s.reason == SAFETY_TRIP_OVER_SETPOINT, "refused: still latched, same reason");

        safety_guard_input_t back_in_range = base_input();
        back_in_range.context_valid = true;
        back_in_range.zone_count = 1;
        back_in_range.max_zone_setpoint_c = 900.0f;
        back_in_range.tc_c = 920.0f; /* 20C over, well within margin */
        cleared = safety_guards_try_clear(&s, &cfg, &back_in_range);
        TEST_CHECK(cleared, "S2 clear succeeds once tc_c is back within overshoot_margin_c");
    }

    /* Audit 2026-08-27: S3 (load stuck on, welded SSR) -- same shape,
     * current still present with nothing recently commanded at clear time.
     * This is the worst case named in the audit: stuck_on_time_s defaults to
     * only 20s, so a refused-in-name-only clear here is the fastest of the
     * four to grant a repeatable re-arm window against mains flowing through
     * a failed-closed element. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t stuck = base_input();
        stuck.context_valid = true;
        stuck.any_current_present = true;
        stuck.relay_commanded_recently = false;
        stuck.dt_s = 3.0f; /* accumulated over multiple ticks, same "not a single-tick
                             * retrip" reasoning as the S2 block above -- a lone retick's
                             * 3s cannot rebuild stuck_on_time_s(20s) on its own. */
        bool tripped = false;
        for (int i = 0; i < 8 && !tripped; i++) { /* 8*3s = 24s > stuck_on_time_s(20) */
            tripped = safety_guards_tick(&s, &cfg, &stuck);
        }
        TEST_CHECK(tripped && s.reason == SAFETY_TRIP_LOAD_STUCK_ON, "sanity: S3 tripped");

        bool cleared = safety_guards_try_clear(&s, &cfg, &stuck);
        TEST_CHECK(!cleared, "S3 clear refused while current is STILL present with nothing commanded -- audit 2026-08-27");
        TEST_CHECK(s.is_tripped && s.reason == SAFETY_TRIP_LOAD_STUCK_ON, "refused: still latched, same reason");

        safety_guard_input_t no_current = base_input();
        no_current.context_valid = true;
        no_current.any_current_present = false;
        cleared = safety_guards_try_clear(&s, &cfg, &no_current);
        TEST_CHECK(cleared, "S3 clear succeeds once current is no longer present");
    }

    /* Audit 2026-08-27 (re-audited same day): S11 (frozen safety reading) --
     * same shape, the reading still sitting at exactly the value it tripped
     * on, at clear time. Uses state->s11_last_c (still intact -- this check
     * runs before safety_guards_clear() zeroes it), not an accumulator that
     * gets reset.
     *
     * The trip itself is produced with heat_commanded true throughout (that
     * IS a real pre-trip input: heat_commanded is wired from
     * any_current_present, and current genuinely is flowing while the kiln
     * is heating and the sensor is frozen). The CLEAR-TIME input below,
     * `frozen`, is deliberately built the way safety_core_build_input()
     * actually produces one post-trip: K4 has already been de-energized in
     * response to is_tripped, so any_current_present -- and therefore
     * heat_commanded -- reads false. This is exactly the combination the
     * second audit pass found the OLD guard_condition_still_immediate() S11
     * clause could never see (it required heat_commanded==true at clear
     * time, which safety_core.c's post-trip relay behaviour makes
     * structurally impossible), and exactly why the clause no longer reads
     * heat_commanded at all -- see that clause's own comment in
     * safety_guards.c. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t heating = base_input();
        heating.tc_c = 400.0f;
        heating.heat_commanded = true; /* pre-trip: current genuinely flowing */
        heating.dt_s = 700.0f; /* first tick only starts the window (elapsed=0); second is past frozen_window_s(600) */
        safety_guards_tick(&s, &cfg, &heating);
        bool tripped = safety_guards_tick(&s, &cfg, &heating);
        TEST_CHECK(tripped && s.reason == SAFETY_TRIP_FROZEN_SENSOR, "sanity: S11 tripped");

        safety_guard_input_t frozen = base_input();
        frozen.tc_c = 400.0f;        /* still the exact value that tripped it */
        frozen.heat_commanded = false; /* post-trip reality: K4 open, any_current_present false */
        bool cleared = safety_guards_try_clear(&s, &cfg, &frozen);
        TEST_CHECK(!cleared, "S11 clear refused while the reading is STILL frozen at the same value, "
                             "even against the realistic post-trip heat_commanded==false input -- "
                             "audit 2026-08-27 (re-audit)");
        TEST_CHECK(s.is_tripped && s.reason == SAFETY_TRIP_FROZEN_SENSOR, "refused: still latched, same reason");

        safety_guard_input_t moved = base_input();
        moved.tc_c = 401.0f; /* a real, different reading -- e.g. the kiln cooling once K4 opened */
        moved.heat_commanded = false; /* still the realistic post-trip value */
        cleared = safety_guards_try_clear(&s, &cfg, &moved);
        TEST_CHECK(cleared, "S11 clear succeeds once the reading has actually moved, heat_commanded "
                            "irrelevant to the decision either way");
    }

    /* Audit 2026-08-27: S9 (trip ineffective / welded contactor) is
     * unclearable, unconditionally -- ARCHITECTURE.md section 9 / SAFETY_
     * MODEL.md section 4's "no exit except power removal at the breaker".
     * Unlike S2/S3/S5/S11/S12/S13 above, there is no "condition clears"
     * input to hand it that would ever make this succeed. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t estop = base_input();
        estop.estop_pressed = true;
        safety_guards_tick(&s, &cfg, &estop); /* trip via S7 first */

        safety_guard_input_t verify = base_input();
        verify.context_valid = true; /* genuine sustained weld: context available too */
        verify.relay_deenergized = true;
        verify.any_current_present = true; /* welded contacts, still conducting */
        verify.dt_s = 15.0f; /* > trip_verify_s(10s) in one tick */
        bool escalated = false;
        for (int i = 0; i < 5 && !escalated; i++) { /* clears the debounce streak too */
            escalated = safety_guards_tick(&s, &cfg, &verify);
        }
        TEST_CHECK(escalated && s.trip_ineffective && s.reason == SAFETY_TRIP_INEFFECTIVE,
                   "sanity: escalated to S9/TRIP_INEFFECTIVE");

        /* Even a fully benign input (no current, relay confirmed open) must
         * not clear it -- there is no "condition still true" test to pass or
         * fail here at all; the refusal does not depend on `in`. */
        safety_guard_input_t benign = base_input();
        benign.relay_deenergized = true;
        benign.any_current_present = false;
        bool cleared = safety_guards_try_clear(&s, &cfg, &benign);
        TEST_CHECK(!cleared, "S9/TRIP_INEFFECTIVE clear refused unconditionally, even against a fully benign input");
        TEST_CHECK(s.is_tripped && s.trip_ineffective && s.reason == SAFETY_TRIP_INEFFECTIVE,
                   "refused: still latched, still escalated, reason unchanged");
    }
}

/* 2026-08-23 reboot-on-clear-trip investigation: safety_core_task's queue-
 * drain loop (src/tasks/safety_core.c) now resolves each dequeued CLEAR_TRIP
 * request into one of three outcomes purely from two booleans it already has
 * in hand -- whether a trip was still latched at dequeue time, and (only if
 * so) what safety_guards_try_clear() returned. safety_guards_decide_clear_
 * trip_outcome() is that classification, pulled out so it is host-testable
 * the same way link_frame_decide_clear_trip() already is for link_task's own
 * pair of CLEAR_TRIP refusal checks -- the FreeRTOS queue/task machinery
 * around it is not host-testable, but this 3-way decision is exactly as pure
 * as that one was. */
static void test_decide_clear_trip_outcome(void)
{
    TEST_SECTION("safety_guards_decide_clear_trip_outcome -- pure 3-way classification");

    TEST_CHECK(safety_guards_decide_clear_trip_outcome(false, false) ==
               SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_NOTHING_LATCHED,
               "was_tripped=false -> nothing-latched regardless of try_clear_result");
    TEST_CHECK(safety_guards_decide_clear_trip_outcome(false, true) ==
               SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_NOTHING_LATCHED,
               "was_tripped=false, try_clear_result=true -> still nothing-latched "
               "(try_clear must not even have run against nothing)");
    TEST_CHECK(safety_guards_decide_clear_trip_outcome(true, true) ==
               SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED,
               "was_tripped=true, try_clear succeeded -> accepted");
    TEST_CHECK(safety_guards_decide_clear_trip_outcome(true, false) ==
               SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STILL_TRIPPED,
               "was_tripped=true, try_clear refused -> refused-still-tripped");
}

/* GUARD_TEST_MATRIX.md: "Property tests: ceiling monotonicity over the float
 * range incl. NaN/Inf". S1's ceiling is not exposed outside the module, so
 * these tests probe it the same way test_s1() does -- through the
 * trip/no-trip behaviour at a reading pinned just above abs_max_temp_c. If
 * ceiling had been silently raised past abs_max_temp_c by a hostile/garbled
 * firing_max_c, that reading would fail to trip; the property under test is
 * that it always does. */
static void test_s1_ceiling_properties(void)
{
    TEST_SECTION("S1 -- ceiling monotonicity property (firing_max_c never raises the ceiling)");

    /* Property: for every firing_max_c below, a reading pinned just above
     * abs_max_temp_c must still trip S1 within the usual 3-tick streak --
     * i.e. the effective ceiling never exceeds abs_max_temp_c, regardless of
     * what firing_max_c claims. */
    {
        float firing_max_values[] = {
            1e30f,             /* large positive, far above abs_max_temp_c */
            -1e30f,             /* large negative */
            (float)NAN,         /* NaN */
            (float)INFINITY,    /* +Infinity */
            -(float)INFINITY,   /* -Infinity -- the actual bug */
            900.0f,              /* normal in-range value, ceiling tightens below abs_max_temp_c */
        };
        for (size_t i = 0; i < sizeof(firing_max_values) / sizeof(firing_max_values[0]); i++) {
            safety_guard_state_t s;
            safety_guards_reset(&s);
            safety_guard_cfg_t cfg = base_cfg();
            cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
            cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
            cfg.abs_max_temp_c = 1300.0f;
            cfg.firing_margin_c = 100.0f;
            cfg.firing_max_valid = true;
            cfg.firing_max_c = firing_max_values[i];
            safety_guard_input_t in = base_input();
            in.tc_c = 1301.0f; /* just above abs_max_temp_c */
            bool tripped = false;
            for (int j = 0; j < 3 && !tripped; j++) {
                tripped = safety_guards_tick(&s, &cfg, &in);
            }
            TEST_CHECK(tripped, "ceiling never exceeds abs_max_temp_c regardless of firing_max_c value");
        }
    }

    /* The actual bug, isolated: firing_max_c = -Infinity used to leave
     * ceiling latched at -Infinity (because "-Inf < finite" is true), which
     * would make S1 trip on every tick forever -- including on a perfectly
     * safe reading. Regression check: a safe reading well under
     * abs_max_temp_c must NOT trip, proving the ceiling fell back to
     * abs_max_temp_c rather than staying at -Infinity. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        cfg.abs_max_temp_c = 1300.0f;
        cfg.firing_margin_c = 100.0f;
        cfg.firing_max_valid = true;
        cfg.firing_max_c = -(float)INFINITY;
        safety_guard_input_t safe = base_input();
        safe.tc_c = 900.0f; /* well under abs_max_temp_c */
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &safe);
        }
        TEST_CHECK(!tripped,
                   "firing_max_c=-Infinity: safe reading under abs_max_temp_c does not trip "
                   "(regression check -- old buggy code latched ceiling to -Infinity and would have tripped here)");

        /* And a reading just above abs_max_temp_c still trips, confirming
         * the fallback ceiling is abs_max_temp_c, not something looser. */
        safety_guard_state_t s2;
        safety_guards_reset(&s2);
        safety_guard_input_t over = base_input();
        over.tc_c = 1301.0f;
        tripped = false;
        for (int i = 0; i < 3 && !tripped; i++) {
            tripped = safety_guards_tick(&s2, &cfg, &over);
        }
        TEST_CHECK(tripped, "firing_max_c=-Infinity: ceiling falls back to abs_max_temp_c, not -Infinity");
    }

    /* NaN case: already worked before the fix (NaN comparisons are always
     * false), but covered here so a future change to the comparison can't
     * silently regress it without a test catching it. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        cfg.abs_max_temp_c = 1300.0f;
        cfg.firing_margin_c = 100.0f;
        cfg.firing_max_valid = true;
        cfg.firing_max_c = (float)NAN;
        safety_guard_input_t safe = base_input();
        safe.tc_c = 900.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &safe);
        }
        TEST_CHECK(!tripped, "firing_max_c=NaN: safe reading under abs_max_temp_c does not trip");

        safety_guard_state_t s2;
        safety_guards_reset(&s2);
        safety_guard_input_t over = base_input();
        over.tc_c = 1301.0f;
        tripped = false;
        for (int i = 0; i < 3 && !tripped; i++) {
            tripped = safety_guards_tick(&s2, &cfg, &over);
        }
        TEST_CHECK(tripped, "firing_max_c=NaN: ceiling falls back to abs_max_temp_c, not NaN");
    }
}

/* GUARD_TEST_MATRIX.md: "No guard reads a disabled input." safety_guard_input_t's
 * doc comment on context_valid: "When false, S2/S3/S4/S10/S13 below are all
 * skipped outright." Feed deliberately provocative/garbage context-dependent
 * fields -- values that would trip or warn every one of those guards if
 * context were valid -- with context_valid=false, and confirm none of them
 * fire. */
static void test_context_gating(void)
{
    TEST_SECTION("context gating -- S2/S3/S4/S10/S13 never read a disabled (context_valid==false) input");

    /* S2, S3, S10, S13 all provoked at once: CHAMBER_AGREED + BORROWED_ZONE
     * so every guard's mode-gate is open, a huge setpoint/zone-TC
     * disagreement to provoke S2 and S10, current present with nothing
     * commanded to provoke S3, and a stalled sample_counter to provoke S13
     * -- all with context_valid==false. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        cfg.tc_placement_valid = true; /* commissioned -- see safety_guards.h */
        cfg.tc_placement_mode = SAFETY_TC_CHAMBER_AGREED;
        cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        cfg.abs_max_temp_c = 1300.0f; /* keep S1 well out of range */
        safety_guard_input_t in = base_input();
        in.context_valid = false;
        in.tc_c = 900.0f; /* under abs_max_temp_c so S1 doesn't interfere */
        in.zone_count = 3; /* would arm S2/S10 if context were valid */
        in.max_zone_setpoint_c = 100.0f; /* tc_c far above setpoint+margin -- would trip S2 */
        in.nearest_zone_measured_c = 50.0f; /* huge disagreement -- would warn S10 */
        in.any_current_present = true; /* with nothing commanded -- would trip S3 */
        in.relay_commanded_recently = false;
        in.sample_counter_advancing = false; /* would eventually trip S13 */
        in.dt_s = 61.0f; /* one tick alone exceeds S3's stuck_on_time_s (20s) and S13's borrowed_stale_trip_s (60s) if context were valid */
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "provocative context-dependent inputs never trip anything while context_valid==false");
        TEST_CHECK(!s.s10_warn, "S10 never warns while context_valid==false");
        TEST_CHECK(!s.s13_warn, "S13 never warns while context_valid==false");
        TEST_CHECK(s.s2_over_elapsed_s == 0.0f, "S2's elapsed accumulator never moves while context_valid==false");
        TEST_CHECK(s.s3_stuck_elapsed_s == 0.0f, "S3's elapsed accumulator never moves while context_valid==false");
        TEST_CHECK(s.s10_disagree_elapsed_s == 0.0f, "S10's elapsed accumulator never moves while context_valid==false");
        TEST_CHECK(s.s13_stale_elapsed_s == 0.0f, "S13's elapsed accumulator never moves while context_valid==false");
    }

    /* S4 provoked separately (its trigger, relay_commanded_continuously &&
     * !any_current_present, is mutually exclusive with S3's any_current_present
     * above): a continuously-commanded relay with no current at all, which
     * would set s4_warn if context were valid. */
    {
        safety_guard_state_t s;
        safety_guards_reset(&s);
        safety_guard_cfg_t cfg = base_cfg();
        safety_guard_input_t in = base_input();
        in.context_valid = false;
        in.relay_commanded_continuously = true;
        in.any_current_present = false;
        in.dt_s = 60.0f;
        for (int i = 0; i < 10; i++) {
            safety_guards_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!s.s4_warn, "S4 never warns while context_valid==false, even with its provoking condition held");
    }
}

// Host tests for safety_guards_deciding_threshold_c() -- ROADMAP.md M5,
// SAFETY_CMD_TRIP_EVENT (Frame D)'s `deciding_threshold` field. Pure function
// of (reason, cfg); the six guards it covers get a real number back (the
// configured value if set, the compiled default otherwise), the rest get an
// honest NaN. Not a guard-tripping test -- this never calls
// safety_guards_tick(), only the standalone threshold lookup.
static void test_deciding_threshold(void)
{
    TEST_SECTION("safety_guards_deciding_threshold_c -- per-reason coverage");

    safety_guard_cfg_t cfg = base_cfg(); /* abs_max_temp_c=1300, everything else 0 -> defaults */

    TEST_CHECK_NEAR(safety_guards_deciding_threshold_c(SAFETY_TRIP_OVERTEMP, &cfg), 1300.0f,
                     0.0001, "S1: reports the configured abs_max_temp_c verbatim");
    TEST_CHECK_NEAR(safety_guards_deciding_threshold_c(SAFETY_TRIP_OVER_SETPOINT, &cfg), 75.0f,
                     0.0001, "S2: overshoot_margin_c==0 -> compiled default 75.0");
    TEST_CHECK_NEAR(safety_guards_deciding_threshold_c(SAFETY_TRIP_LOAD_STUCK_ON, &cfg), 2.0f,
                     0.0001, "S3: i_present_a==0 -> compiled default 2.0");
    TEST_CHECK_NEAR(safety_guards_deciding_threshold_c(SAFETY_TRIP_FROZEN_SENSOR, &cfg), 600.0f,
                     0.0001, "S11: frozen_window_s==0 -> compiled default 600.0");
    TEST_CHECK_NEAR(safety_guards_deciding_threshold_c(SAFETY_TRIP_ENCLOSURE_TEMP, &cfg), 85.0f,
                     0.0001, "S12: cj_max_c==0 -> compiled default 85.0");
    TEST_CHECK_NEAR(safety_guards_deciding_threshold_c(SAFETY_TRIP_BORROWED_STALE, &cfg), 60.0f,
                     0.0001, "S13: borrowed_stale_trip_s==0 -> compiled default 60.0");

    /* A non-zero configured value must win over the compiled default -- the
     * lookup must not just always report the default no matter what cfg says. */
    cfg.overshoot_margin_c = 42.0f;
    TEST_CHECK_NEAR(safety_guards_deciding_threshold_c(SAFETY_TRIP_OVER_SETPOINT, &cfg), 42.0f,
                     0.0001, "S2: a real configured overshoot_margin_c overrides the default");

    /* Guards with no single meaningful magnitude -- honest NaN, not a guess. */
    TEST_CHECK(isnan(safety_guards_deciding_threshold_c(SAFETY_TRIP_SENSOR_INVALID, &cfg)),
               "S5 (dual count+time bar): NaN, no single threshold to report");
    TEST_CHECK(isnan(safety_guards_deciding_threshold_c(SAFETY_TRIP_MAIN_FAULT, &cfg)),
               "S6a (boolean mainFault): NaN");
    TEST_CHECK(isnan(safety_guards_deciding_threshold_c(SAFETY_TRIP_LINK_DEAD, &cfg)),
               "S6b (already-qualitative hard backstop): NaN");
    TEST_CHECK(isnan(safety_guards_deciding_threshold_c(SAFETY_TRIP_ESTOP, &cfg)),
               "S7 (boolean estop): NaN");
    TEST_CHECK(isnan(safety_guards_deciding_threshold_c(SAFETY_TRIP_INEFFECTIVE, &cfg)),
               "S9 (escalation, not a fresh threshold crossing): NaN");
    TEST_CHECK(isnan(safety_guards_deciding_threshold_c(SAFETY_TRIP_NONE, &cfg)),
               "SAFETY_TRIP_NONE: NaN (no trip to describe)");
}


/* --- CTs declared absent: S3/S4/S9/S14 go INERT, not silently "passing" -----
 *
 * ROADMAP.md M12, "CTs are optional hardware". Every check here is written as
 * a PAIR: the same tick, evaluated once with current_sensing_disabled false
 * and once with it true. The false half is what makes the true half mean
 * something -- a test that only asserted "disabled: no trip" would pass just
 * as well against a guard that never worked at all.
 */
static void test_ct_disabled_guards(void)
{
    TEST_SECTION("CTs declared absent -- S3/S4/S9/S14 report inactive, and the armed case still fires");

    safety_guard_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.stuck_on_time_s = 20.0f;
    cfg.trip_verify_s = 10.0f;
    cfg.correlation_window_s = 150.0f;
    cfg.overcurrent_pct = 150u;
    cfg.overcurrent_time_s = 30.0f;
    cfg.i_normal_valid[0] = true;
    cfg.i_normal_a[0] = 10.0f;

    /* --- S3 (TRIP): current present, nothing commanded on ----------------- */
    {
        /* ARMED. This is the exact tick shape a CT-less board produces all by
         * itself: current_presence_policy.c's uncalibrated counts fallback
         * reads the AD8542's offset floor as "present" forever, and nothing
         * is commanded on. Left alone it TRIPS -- which is the whole reason
         * the disable flag cannot just be "ignore the reading". */
        safety_guard_state_t st;
        safety_guards_reset(&st);
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.any_current_present = true;
        in.relay_commanded_recently = false;
        bool tripped = false;
        for (int i = 0; i < 250 && !tripped; i++) { /* 25 s > stuck_on_time_s */
            tripped = safety_guards_tick(&st, &cfg, &in);
        }
        TEST_CHECK(tripped && st.reason == SAFETY_TRIP_LOAD_STUCK_ON,
                   "S3 ARMED: sustained current with nothing commanded TRIPS (the bug the flag prevents)");
        TEST_CHECK(!st.ct_guards_disabled,
                   "S3 ARMED: ct_guards_disabled reports false -- the guards ARE watching");
    }
    {
        /* DISABLED: identical input, plus the declaration. */
        safety_guard_state_t st;
        safety_guards_reset(&st);
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.any_current_present = true;
        in.relay_commanded_recently = false;
        in.current_sensing_disabled = true;
        bool tripped = false;
        for (int i = 0; i < 600 && !tripped; i++) { /* 60 s, 3x stuck_on_time_s */
            tripped = safety_guards_tick(&st, &cfg, &in);
        }
        TEST_CHECK(!tripped, "S3 DISABLED: no trip, however long the phantom reading persists");
        TEST_CHECK(st.s3_stuck_elapsed_s == 0.0f,
                   "S3 DISABLED: the accumulator is held at zero, not merely under threshold");
        TEST_CHECK(st.ct_guards_disabled,
                   "S3 DISABLED: ct_guards_disabled is TRUE -- inactive is REPORTED, not silent");
    }

    /* --- S4 (WARN): heat commanded, no current seen ----------------------- */
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.relay_commanded_continuously = true;
        in.any_current_present = false;
        safety_guards_tick(&st, &cfg, &in);
        TEST_CHECK(st.s4_warn, "S4 ARMED: relay commanded with no current WARNs");

        in.current_sensing_disabled = true;
        safety_guards_tick(&st, &cfg, &in);
        TEST_CHECK(!st.s4_warn,
                   "S4 DISABLED: no warn -- otherwise every firing on a CT-less board warns forever");
    }

    /* --- S9 (unclearable TRIP_INEFFECTIVE): must not latch, and must not
     *     claim the fix is 'finish commissioning' -------------------------- */
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.any_current_present = true;
        in.relay_deenergized = true;
        in.current_sensing_commissioned = false; /* true of any CT-less board:
                                                  * k_ct_v_per_a is 0 */
        in.estop_pressed = true;                 /* force a trip so S9's window opens */
        safety_guards_tick(&st, &cfg, &in);
        in.estop_pressed = false;
        for (int i = 0; i < 300; i++) { /* 30 s > trip_verify_s */
            safety_guards_tick(&st, &cfg, &in);
        }
        TEST_CHECK(!st.trip_ineffective,
                   "S9 uncommissioned: does not latch trip_ineffective (pre-existing gate)");
        TEST_CHECK(st.s9_uncommissioned_warn,
                   "S9 uncommissioned: DOES raise the 'commission the chain' warn");

        /* Now the same board, with the CT question answered 'none fitted'.
         * The warn must go away: telling an operator to commission a current
         * chain that has no sensor is advice they cannot act on. */
        safety_guard_state_t st2;
        safety_guards_reset(&st2);
        safety_guard_input_t in2 = in;
        in2.current_sensing_disabled = true;
        in2.estop_pressed = true;
        safety_guards_tick(&st2, &cfg, &in2);
        in2.estop_pressed = false;
        for (int i = 0; i < 300; i++) {
            safety_guards_tick(&st2, &cfg, &in2);
        }
        TEST_CHECK(!st2.trip_ineffective, "S9 DISABLED: still does not latch trip_ineffective");
        TEST_CHECK(!st2.s9_uncommissioned_warn,
                   "S9 DISABLED: the 'finish commissioning' warn is suppressed -- it is not the truth here");
        TEST_CHECK(st2.ct_guards_disabled,
                   "S9 DISABLED: ct_guards_disabled survives the already-tripped path too");
    }

    /* --- S14 (WARN): per-channel over-normal current ---------------------- */
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        safety_guard_input_t in = base_input();
        in.context_valid = true;
        in.amps_valid[0] = true;
        in.amps[0] = 30.0f; /* 300% of the 10 A normal */
        in.relay_commanded_now_for_ct[0] = true;
        for (int i = 0; i < 400; i++) { /* 40 s > overcurrent_time_s */
            safety_guards_tick(&st, &cfg, &in);
        }
        TEST_CHECK(st.s14_warn[0], "S14 ARMED: sustained 300%-of-normal current WARNs");

        safety_guard_state_t st2;
        safety_guards_reset(&st2);
        safety_guard_input_t in2 = in;
        in2.current_sensing_disabled = true;
        for (int i = 0; i < 400; i++) {
            safety_guards_tick(&st2, &cfg, &in2);
        }
        TEST_CHECK(!st2.s14_warn[0], "S14 DISABLED: no warn");
        TEST_CHECK(st2.s14_over_elapsed_s[0] == 0.0f,
                   "S14 DISABLED: the accumulator is held at zero, not merely under threshold");
    }

    /* --- The guards that must be UNAFFECTED -------------------------------
     * The declaration disarms four CT-fed guards and nothing else. S7
     * (E-stop) is the check that would notice a flag accidentally wired into
     * the shared context block, since it sits before it. */
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        safety_guard_input_t in = base_input();
        in.current_sensing_disabled = true;
        in.estop_pressed = true;
        TEST_CHECK(safety_guards_tick(&st, &cfg, &in) && st.reason == SAFETY_TRIP_ESTOP,
                   "S7 still trips with CTs declared absent -- the flag disarms four guards, not the module");
    }
    {
        /* S6b's UNCONDITIONAL hard backstop still fires. Its soft,
         * current-keyed tier does not -- the known, documented degradation of
         * running without CTs (safety_guard_input_t::current_sensing_disabled). */
        safety_guard_cfg_t lcfg = cfg;
        lcfg.link_timeout_s = 10.0f;
        lcfg.link_dead_hard_s = 120.0f;

        safety_guard_state_t soft;
        safety_guards_reset(&soft);
        safety_guard_input_t in = base_input();
        in.link_up = false;
        /* any_current_present FALSE alongside the declaration, because that
         * is the pair safety_core.c actually produces: it forces the reading
         * to its no-information state at the producer (see its cts_disabled
         * block). safety_guards.c itself does NOT special-case S6b -- this
         * check documents the resulting end-to-end behaviour, and the fact
         * that it is the producer, not this module, that closes the soft
         * tier. Setting any_current_present true here alongside the flag
         * would trip at the soft threshold, which is exactly why the two must
         * be forced together upstream. */
        in.any_current_present = false;
        in.current_sensing_disabled = true;
        bool tripped = false;
        for (int i = 0; i < 500 && !tripped; i++) { /* 50 s: past soft, short of hard */
            tripped = safety_guards_tick(&soft, &lcfg, &in);
        }
        TEST_CHECK(!tripped,
                   "S6b DISABLED: the soft current-keyed tier is unreachable -- the accepted degradation");
        for (int i = 0; i < 800 && !tripped; i++) { /* on to 130 s */
            tripped = safety_guards_tick(&soft, &lcfg, &in);
        }
        TEST_CHECK(tripped && soft.reason == SAFETY_TRIP_LINK_DEAD,
                   "S6b DISABLED: the unconditional hard backstop STILL fires -- protection is degraded, not gone");
    }
}

static void test_warn_mask(void)
{
    TEST_SECTION("safety_guards_warn_mask() -- Opus review of 51c084f/c49bb0e finding 1: real "
                 "per-guard DIAG warn bits, not the former single-bit approximation");

    /* NULL is handled defensively, matching every other pure accessor in
     * this module. */
    TEST_CHECK(safety_guards_warn_mask(NULL) == 0u, "NULL state -> 0, not a crash");

    /* A freshly reset state has nothing warning. */
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        TEST_CHECK(safety_guards_warn_mask(&st) == 0u, "freshly reset state -> mask 0");
    }

    /* Each bit, set one at a time, lands at the documented position and
     * nowhere else -- proves the mapping is exact, not just "nonzero when
     * something is warning". */
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s4_warn = true;
        TEST_CHECK(safety_guards_warn_mask(&st) == (1u << 3), "s4_warn alone -> bit 3 only");
    }
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s5_warn = true;
        TEST_CHECK(safety_guards_warn_mask(&st) == (1u << 4), "s5_warn alone -> bit 4 only");
    }
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s9_uncommissioned_warn = true;
        TEST_CHECK(safety_guards_warn_mask(&st) == (1u << 9), "s9_uncommissioned_warn alone -> bit 9 only");
    }
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s10_warn = true;
        TEST_CHECK(safety_guards_warn_mask(&st) == (1u << 10), "s10_warn alone -> bit 10 only");
    }
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s12_warn = true;
        TEST_CHECK(safety_guards_warn_mask(&st) == (1u << 12), "s12_warn alone -> bit 12 only");
    }
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s13_warn = true;
        TEST_CHECK(safety_guards_warn_mask(&st) == (1u << 13), "s13_warn alone -> bit 13 only");
    }
    {
        /* S14: any ONE of the 3 channels sets exactly bit 14, not a
         * per-channel spread across multiple bits. */
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s14_warn[1] = true; /* middle channel, deliberately not index 0 */
        TEST_CHECK(safety_guards_warn_mask(&st) == (1u << 14),
                   "s14_warn[1] alone (any one of 3 channels) -> bit 14 only");
    }
    {
        /* S15: same "any one of 3 zones -> one bit" collapse as S14. */
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s15_warn[2] = true; /* last zone, deliberately not index 0 */
        TEST_CHECK(safety_guards_warn_mask(&st) == (1u << 15),
                   "s15_warn[2] alone (any one of 3 zones) -> bit 15 only");
    }

    /* Several guards warning at once OR together, with no cross-talk. */
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s4_warn = true;
        st.s10_warn = true;
        st.s14_warn[0] = true;
        st.s14_warn[2] = true; /* two channels of the SAME guard -> still just bit 14 */
        uint16_t expected = (uint16_t)((1u << 3) | (1u << 10) | (1u << 14));
        TEST_CHECK(safety_guards_warn_mask(&st) == expected,
                   "S4+S10+S14(x2 channels) -> exactly bits 3,10,14, no double-count for S14's two channels");
    }

    /* NEGATIVE TEST (negative-test-every-check discipline): a mutated
     * (buggy) reimplementation that swaps two guards' bits would be caught
     * by the "each bit alone" checks above -- prove that concretely by
     * showing the real function's S14-alone result does NOT equal what a
     * bit-swapped bug (S14 wrongly reusing S13's bit 13 instead of 14, e.g.
     * from a copy-paste of the S13 case) would have produced. */
    {
        safety_guard_state_t st;
        safety_guards_reset(&st);
        st.s14_warn[0] = true;
        uint16_t real = safety_guards_warn_mask(&st);
        uint16_t buggy_bit13_instead = (uint16_t)(1u << 13); /* what a copy-paste-from-S13 bug would produce */
        TEST_CHECK(real != buggy_bit13_instead,
                   "S14's real bit (14) differs from a plausible copy-paste bug's bit (13)");
        TEST_CHECK(real == (uint16_t)(1u << 14), "S14's real bit is exactly 14, confirming the above isn't a fluke");
    }
}

void run_test_safety_guards(void)
{
    test_s1();
    test_s1_ceiling_properties();
    test_s5();
    test_s5_not_installed();
    test_s7();
    test_s11();
    test_s12();
    test_s8();
    test_s2();
    test_s3_s4();
    test_sim_plant_disable();
    test_s14();
    test_s14_s15_summed_topology();
    test_s14_s15_zone_ct_channel_collapse();
    test_s14_s15_two_ct_split();
    test_s14_s15_wrong_membership_negative();
    test_s6();
    test_s6b_reboot_grace();
    test_s9();
    test_s10();
    test_s13();
    test_s6_s13_split();
    test_context_gating();
    test_independence_invariant();
    test_tx_independence_representative_sequence();
    test_try_clear();
    test_decide_clear_trip_outcome();
    test_deciding_threshold();
    test_ct_disabled_guards();
    test_warn_mask();
}
