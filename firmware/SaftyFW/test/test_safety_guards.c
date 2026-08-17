// Host tests for safety_guards.c. TODO.md Phase 4.
//
// ARCHITECTURE.md section 10: "the cases worth writing first are the
// *nuisance* cases, not the trip cases" -- each guard's section below leads
// with the inputs that must NOT trip it, then the input that must.
#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../src/safety_guards.h"

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
    in.dt_s = 0.1f; /* safety_core's real tick period, per ARCHITECTURE.md section 4 */
    return in;
}

static safety_guard_cfg_t base_cfg(void)
{
    safety_guard_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
    cfg.abs_max_temp_c = 1300.0f;
    /* Everything else left 0 -> firmware defaults (firing_margin_c=100,
     * bad_read_count_threshold=10, bad_read_time_s=5, blind_grace_s=60,
     * frozen_window_s=600, cj_warn_c=60, cj_max_c=85, cj_time_s=60). */
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
     * also the resolution of the context-free "heat commanded" ambiguity:
     * with nothing wiring heat_commanded to a real signal yet, callers pass
     * false, and false correctly keeps this guard dormant rather than
     * either false-tripping every idle period or silently dropping its own
     * qualifier. */
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
     * genuinely tracking sensor, however slowly, must never trip. */
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
}

void run_test_safety_guards(void)
{
    test_s1();
    test_s5();
    test_s7();
    test_s11();
    test_s12();
    test_independence_invariant();
}
