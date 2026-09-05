// Host tests for App/drivers/profile_feasibility.c -- TODO.md 5A.1, the
// "mark the schedule red if the tuning says it cannot be fired" check.
//
// The module is pure math (a first-order plant model, no I/O), so all of it
// is testable off-target; its only dependencies are three read-only getters
// from zones_http.h, stubbed below in the same style test_ota_interlock.c
// uses -- plain C definitions of the real symbols, with a small setter the
// tests drive, so the linker resolves them without any mocking machinery.
//
// The model used throughout is deliberately chosen so the arithmetic is
// checkable by hand:
//
//   tau_s = 3600 s  =>  a rate in C/s times 3600 is numerically the C/hr rate,
//                       so "max rate in C/hr" == "headroom in C". Every
//                       expected value below falls out of one subtraction.
//   k_dc  = 1000 C  =>  ceiling = T_amb + K = 20 + 1000 = 1020 C, and the
//                       UNREACHABLE threshold is ceiling - 5 = 1015 C.
#include <math.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/profile_feasibility.h"
#include "../drivers/zones_http.h"

// ---------------------------------------------------------------------------
// Stubs for the three zones_http.c getters profile_feasibility.c calls.
// ---------------------------------------------------------------------------

#define STUB_MAX_ZONES 3

typedef struct {
    bool model_getter_answers; /* false = the getter itself cannot answer */
    float k_dc;
    float tau_s;
    float dead_time_s;
    bool max_ramp_getter_answers;
    float max_ramp_c_per_hr;
} stub_zone_t;

static stub_zone_t s_zones[STUB_MAX_ZONES];
static uint8_t s_thermo_count;

/* zones_config_get_coupling()'s only definition in this binary lives in
 * test_backup_import.c (it stubs the whole zones_http.h surface for the import
 * validator); these two hooks reach that state, mirroring the way that file
 * reaches THIS one's thermo-count/max-ramp stubs. */
void test_stub_zones_set_coupling(uint8_t zone_index, bool answers,
                                  const float row[MAX31856_CHANNEL_COUNT]);

static void stub_zone_coupling_clear(uint8_t zi)
{
    const float zero[MAX31856_CHANNEL_COUNT] = { 0 };
    test_stub_zones_set_coupling(zi, true, zero);
}

/* A zone with the hand-checkable model above and a ceiling high enough that
 * the policy check never fires -- so a verdict is attributable to the physics
 * alone unless a test deliberately lowers it. */
static void stub_zone_tuned(uint8_t zi)
{
    s_zones[zi].model_getter_answers = true;
    s_zones[zi].k_dc = 1000.0f;
    s_zones[zi].tau_s = 3600.0f;
    s_zones[zi].dead_time_s = 30.0f;
    s_zones[zi].max_ramp_getter_answers = true;
    s_zones[zi].max_ramp_c_per_hr = 100000.0f;
    stub_zone_coupling_clear(zi);
}

/* The "never autotuned" state zones_http.c documents: zeros in all three
 * model fields, with the getter itself succeeding. */
static void stub_zone_untuned(uint8_t zi)
{
    s_zones[zi].model_getter_answers = true;
    s_zones[zi].k_dc = 0.0f;
    s_zones[zi].tau_s = 0.0f;
    s_zones[zi].dead_time_s = 0.0f;
    s_zones[zi].max_ramp_getter_answers = true;
    s_zones[zi].max_ramp_c_per_hr = 100000.0f;
}

static void stub_reset(void)
{
    memset(s_zones, 0, sizeof(s_zones));
    stub_zone_tuned(0);
    stub_zone_tuned(1);
    stub_zone_tuned(2);
    s_thermo_count = 1;
}

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s,
                            float *out_dead_time_s)
{
    if (zone_index >= STUB_MAX_ZONES || !s_zones[zone_index].model_getter_answers) {
        return false;
    }
    if (out_k_dc) *out_k_dc = s_zones[zone_index].k_dc;
    if (out_tau_s) *out_tau_s = s_zones[zone_index].tau_s;
    if (out_dead_time_s) *out_dead_time_s = s_zones[zone_index].dead_time_s;
    return true;
}

bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    if (zone_index >= STUB_MAX_ZONES || !s_zones[zone_index].max_ramp_getter_answers) {
        return false;
    }
    if (out_c_per_hr) *out_c_per_hr = s_zones[zone_index].max_ramp_c_per_hr;
    return true;
}

uint8_t zones_config_get_thermo_count(void)
{
    return s_thermo_count;
}

// thermo_owner_command_read_all() is thermo_owner.c's real production
// producer for profile_feasibility.c's live-ambient reading (see
// get_live_ambient_c() there); thermo_owner.c itself is not compiled into
// the host-test binary (it is a FreeRTOS task with a real SPI-bus owner
// loop), so this is the ONE definition of that symbol the linker sees here --
// same "plain C stand-in for the real symbol" shape as the zones_http.c
// getters above. Answering "not found, no readings" reproduces exactly the
// pre-live-ambient behaviour (fallback to FEASIBILITY_AMBIENT_C) for every
// existing test in this file that never calls
// profile_feasibility_test_set_ambient_c() -- see that hook below for the
// tests that exercise the live path instead.
esp_err_t thermo_owner_command_read_all(MAX31856Reading *out, size_t max_readings,
                                        size_t *out_count)
{
    (void)out;
    (void)max_readings;
    if (out_count) {
        *out_count = 0;
    }
    return ESP_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Test-only ambient hooks -- defined in profile_feasibility.c, not declared
// in profile_feasibility.h since production code never calls them (see that
// file's comment on profile_feasibility_test_set_ambient_c()).
// ---------------------------------------------------------------------------
void profile_feasibility_test_set_ambient_c(float ambient_c);
void profile_feasibility_test_clear_ambient_override(void);

// ---------------------------------------------------------------------------
// Cross-file control hooks for test_backup_import.c
// ---------------------------------------------------------------------------
// backup_http.c's import validation pass calls these same two zones_http.c
// getters (zones_config_get_thermo_count()/zones_config_get_max_ramp()), and
// this file's definitions above are the ONLY ones linked into the host test
// binary -- test_backup_import.c must not redefine them (multiple-definition
// link error), so it drives the same s_zones/s_thermo_count state through
// these two small setters instead. STUB_MAX_ZONES (3) already equals
// MAX31856_CHANNEL_COUNT, the same bound backup_http.c's zone entries are
// checked against, so no separate size to keep in sync.
void test_stub_zones_set_thermo_count(uint8_t n)
{
    s_thermo_count = n;
}

void test_stub_zones_set_max_ramp(uint8_t zone_index, bool answers, float c_per_hr)
{
    if (zone_index < STUB_MAX_ZONES) {
        s_zones[zone_index].max_ramp_getter_answers = answers;
        s_zones[zone_index].max_ramp_c_per_hr = c_per_hr;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static profile_segment_t seg_of(float target_c, float ramp_c_per_hr)
{
    profile_segment_t s;
    memset(&s, 0, sizeof(s));
    s.target_c = target_c;
    s.ramp_c_per_hr = ramp_c_per_hr;
    s.dwell_min = 10;
    return s;
}

static profile_t profile_of(uint8_t zone_mask, const profile_segment_t *segs, uint8_t n)
{
    profile_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.name, "test", sizeof("test"));
    p.zone_mask = zone_mask;
    p.segment_count = n;
    for (uint8_t i = 0; i < n && i < PROFILE_MAX_SEGMENTS; i++) {
        p.segments[i] = segs[i];
    }
    return p;
}

// ---------------------------------------------------------------------------
// 1. The honesty rule: no model means UNKNOWN -- never OK, never red.
// ---------------------------------------------------------------------------

static void test_no_model_is_never_ok_and_never_red(void)
{
    TEST_SECTION("no tuned model -> UNKNOWN, never OK and never red (the honesty rule)");

    stub_reset();
    stub_zone_untuned(0);

    // A spread of segments chosen so that WITH a model they would land on
    // every other verdict: gentle heat (OK), wild heat (TOO_FAST), far above
    // the ceiling (UNREACHABLE). All must be UNKNOWN when there is no model.
    const profile_segment_t probes[] = {
        seg_of(200.0f, 50.0f),
        seg_of(900.0f, 5000.0f),
        seg_of(5000.0f, 10.0f),
        seg_of(200.0f, 0.0f), /* even the "no rate limit" path must not shortcut to OK */
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        profile_seg_verdict_t v = profile_feasibility_segment(0, 20.0f, &probes[i]);
        TEST_CHECK(v == PROFILE_SEG_UNKNOWN,
                   "all-zero model (the documented 'no model' encoding) is UNKNOWN for every segment");
        TEST_CHECK(v != PROFILE_SEG_OK, "an untuned zone is never reported OK");
        TEST_CHECK(v != PROFILE_SEG_TOO_FAST && v != PROFILE_SEG_UNREACHABLE,
                   "an untuned zone is never reported red");
    }

    // The getter failing outright (out-of-range zone, NVS unreadable) is the
    // same situation: cannot answer, so do not guess.
    stub_reset();
    s_zones[0].model_getter_answers = false;
    profile_segment_t s = seg_of(200.0f, 50.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &s) == PROFILE_SEG_UNKNOWN,
               "zones_config_get_model() returning false is UNKNOWN too");

    // A NULL segment has nothing to judge -- same answer, not a crash.
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, NULL) == PROFILE_SEG_UNKNOWN,
               "NULL segment is UNKNOWN, not a crash");

    // Non-finite inputs are unanswerable, not infeasible.
    stub_reset();
    profile_segment_t nan_target = seg_of(NAN, 50.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &nan_target) == PROFILE_SEG_UNKNOWN,
               "a NaN target is UNKNOWN, not red");
}

// ---------------------------------------------------------------------------
// 2. Steady-state ceiling.
// ---------------------------------------------------------------------------

static void test_ceiling_unreachable(void)
{
    TEST_SECTION("target above the steady-state ceiling -> UNREACHABLE");

    stub_reset(); /* K=1000, tau=3600 -> ceiling 1020 C, threshold 1015 C */

    profile_segment_t above = seg_of(1100.0f, 50.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &above) == PROFILE_SEG_UNREACHABLE,
               "1100 C against a 1020 C ceiling is UNREACHABLE");

    // The 5 C margin: 1016 > 1015 is unreachable, 1014 is not.
    profile_segment_t inside_margin = seg_of(1016.0f, 1.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &inside_margin) == PROFILE_SEG_UNREACHABLE,
               "within the 5 C ceiling margin (1016 C of a 1020 C ceiling) is UNREACHABLE");

    profile_segment_t outside_margin = seg_of(1014.0f, 1.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &outside_margin) != PROFILE_SEG_UNREACHABLE,
               "just outside the margin (1014 C) is not UNREACHABLE");

    // UNREACHABLE wins over TOO_FAST when both would apply: 5000 C/hr is far
    // over any rate this kiln can hold, but the target being out of reach is
    // the more serious and more useful thing to say.
    profile_segment_t both = seg_of(1100.0f, 5000.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &both) == PROFILE_SEG_UNREACHABLE,
               "UNREACHABLE is reported in preference to TOO_FAST when both apply");

    // ...and that preference holds even against the POLICY ceiling, which is
    // checked after the steady-state one.
    s_zones[0].max_ramp_c_per_hr = 1.0f;
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &both) == PROFILE_SEG_UNREACHABLE,
               "UNREACHABLE also outranks the configured max-ramp ceiling");
}

// ---------------------------------------------------------------------------
// 3. Heating rate.
// ---------------------------------------------------------------------------

static void test_heating_rate(void)
{
    TEST_SECTION("heating segment vs (K - (T - T_amb))/tau * 3600");

    stub_reset();

    // Hand arithmetic at target 520 C:
    //   headroom = K - (T - T_amb) = 1000 - (520 - 20) = 500 C
    //   max rate = 500 / 3600 C/s * 3600 = 500.0 C/hr
    //   allowed  = 0.90 * 500.0        = 450.0 C/hr
    profile_segment_t too_fast = seg_of(520.0f, 600.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &too_fast) == PROFILE_SEG_TOO_FAST,
               "600 C/hr to 520 C, where the model allows 450 C/hr, is TOO_FAST");

    profile_segment_t comfortable = seg_of(520.0f, 100.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &comfortable) == PROFILE_SEG_OK,
               "100 C/hr to the same target is comfortably OK");

    // The worst point of a heating segment is its TOP end. A segment ending
    // at 950 C has headroom 1000 - 930 = 70 C, so only 63 C/hr is allowed --
    // a rate that would pass easily if the check were done at the 20 C start
    // (where 900 C/hr would be allowed).
    profile_segment_t high_end = seg_of(950.0f, 200.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &high_end) == PROFILE_SEG_TOO_FAST,
               "the rate is judged at the segment's top end (allowed 63 C/hr at 950 C), "
               "not at its cool start");
}

// ---------------------------------------------------------------------------
// 4. Cooling rate -- the case a heating-only check would miss.
// ---------------------------------------------------------------------------

static void test_cooling_rate(void)
{
    TEST_SECTION("cooling segment vs natural loss (T - T_amb)/tau * 3600");

    stub_reset();

    // Hand arithmetic at target 120 C, cooling from 800 C:
    //   above ambient = 120 - 20 = 100 C
    //   max rate      = 100 / 3600 C/s * 3600 = 100.0 C/hr
    //   allowed       = 0.90 * 100.0          = 90.0 C/hr
    profile_segment_t crash_cool = seg_of(120.0f, 200.0f);
    TEST_CHECK(profile_feasibility_segment(0, 800.0f, &crash_cool) == PROFILE_SEG_TOO_FAST,
               "200 C/hr down to 120 C, where natural loss only gives 90 C/hr, is TOO_FAST -- "
               "an electric kiln has no active cooling");

    profile_segment_t gentle_cool = seg_of(120.0f, 50.0f);
    TEST_CHECK(profile_feasibility_segment(0, 800.0f, &gentle_cool) == PROFILE_SEG_OK,
               "50 C/hr down to the same target is OK");

    // Worst point of a cooling segment is its BOTTOM end. Cooling 800 -> 700
    // allows 0.90 * 680 = 612 C/hr; cooling 800 -> 60 allows only 0.90 * 40
    // = 36 C/hr. Same start, same commanded rate, opposite verdicts.
    profile_segment_t shallow = seg_of(700.0f, 100.0f);
    TEST_CHECK(profile_feasibility_segment(0, 800.0f, &shallow) == PROFILE_SEG_OK,
               "100 C/hr from 800 to 700 C is within natural loss (allowed 612 C/hr)");
    profile_segment_t deep = seg_of(60.0f, 100.0f);
    TEST_CHECK(profile_feasibility_segment(0, 800.0f, &deep) == PROFILE_SEG_TOO_FAST,
               "the same 100 C/hr taken down to 60 C is not (allowed 36 C/hr) -- the check is "
               "at the bottom end");
}

// ---------------------------------------------------------------------------
// 5. Cooling to or below ambient.
// ---------------------------------------------------------------------------

static void test_cooling_to_ambient(void)
{
    TEST_SECTION("cooling target at or below ambient -> TOO_FAST");

    stub_reset();

    profile_segment_t at_ambient = seg_of(20.0f, 10.0f);
    TEST_CHECK(profile_feasibility_segment(0, 500.0f, &at_ambient) == PROFILE_SEG_TOO_FAST,
               "a commanded rate down to exactly ambient cannot be met -- the cooling rate "
               "goes to zero there");

    profile_segment_t below_ambient = seg_of(-40.0f, 10.0f);
    TEST_CHECK(profile_feasibility_segment(0, 500.0f, &below_ambient) == PROFILE_SEG_TOO_FAST,
               "below ambient is TOO_FAST too -- the kiln cannot be driven below the room");
}

// ---------------------------------------------------------------------------
// 6. rate <= 0 means "no rate limit".
// ---------------------------------------------------------------------------

static void test_no_rate_limit(void)
{
    TEST_SECTION("ramp_c_per_hr <= 0 is 'no rate limit'");

    stub_reset();

    profile_segment_t zero_rate = seg_of(500.0f, 0.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &zero_rate) == PROFILE_SEG_OK,
               "rate 0 with a reachable target is OK -- there is no commanded rate to be too "
               "fast for");

    profile_segment_t neg_rate = seg_of(500.0f, -25.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &neg_rate) == PROFILE_SEG_OK,
               "a negative rate is treated the same way");

    // The rate <= 0 shortcut sits BEFORE the policy-ceiling check, so even an
    // unconfigured (0) ceiling does not make an unlimited segment red.
    s_zones[0].max_ramp_c_per_hr = 0.0f;
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &zero_rate) == PROFILE_SEG_OK,
               "rate 0 is OK even against a 0 max-ramp ceiling");

    stub_reset();
    profile_segment_t unreachable = seg_of(1100.0f, 0.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &unreachable) == PROFILE_SEG_UNREACHABLE,
               "'no rate limit' does not excuse an unreachable target");
}

// ---------------------------------------------------------------------------
// 7. The configured max_ramp_c_per_hr policy ceiling.
// ---------------------------------------------------------------------------

static void test_policy_ceiling(void)
{
    TEST_SECTION("configured max_ramp_c_per_hr ceiling -> TOO_FAST even when the physics allow it");

    stub_reset();
    s_zones[0].max_ramp_c_per_hr = 100.0f;

    // Physics at 520 C allow 450 C/hr (see test_heating_rate), so 200 C/hr is
    // well inside the model -- but outside the operator's own ceiling.
    profile_segment_t over_policy = seg_of(520.0f, 200.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &over_policy) == PROFILE_SEG_TOO_FAST,
               "200 C/hr against a 100 C/hr configured ceiling is TOO_FAST though the model "
               "allows 450 C/hr");

    profile_segment_t at_policy = seg_of(520.0f, 100.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &at_policy) == PROFILE_SEG_OK,
               "exactly at the configured ceiling is OK (strictly-greater comparison)");

    // Ceiling 0 == "never configured", which profile_executor.c already treats
    // as refusing every nonzero rate. Verified on hardware (TODO.md 5A.1): a
    // 55 C/hr segment against a 0 ceiling really does refuse to start, so the
    // badge has to agree.
    stub_reset();
    s_zones[0].max_ramp_c_per_hr = 0.0f;
    profile_segment_t gentle = seg_of(200.0f, 55.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &gentle) == PROFILE_SEG_TOO_FAST,
               "55 C/hr against an unconfigured (0) ceiling is TOO_FAST -- matches what "
               "profiles_start() actually refuses");
    profile_segment_t tiny = seg_of(200.0f, 0.5f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &tiny) == PROFILE_SEG_TOO_FAST,
               "any positive rate is over a 0 ceiling");

    // If the ceiling getter cannot answer at all, the policy check is skipped
    // and the physics stand alone -- not a red mark on a missing answer.
    stub_reset();
    s_zones[0].max_ramp_getter_answers = false;
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &gentle) == PROFILE_SEG_OK,
               "an unanswerable max-ramp getter falls through to the physics check");
}

// ---------------------------------------------------------------------------
// 8. The 0.90 rate margin boundary.
// ---------------------------------------------------------------------------

static void test_rate_margin_boundary(void)
{
    TEST_SECTION("the 0.90 rate margin boundary");

    stub_reset();

    // At 520 C the model's own maximum is 500.0 C/hr, so the boundary sits at
    // 0.90 * 500.0 = 450.0 C/hr, and the comparison is strictly-greater.
    profile_segment_t under = seg_of(520.0f, 449.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &under) == PROFILE_SEG_OK,
               "449 C/hr, just under the 450 C/hr margin, is OK");

    profile_segment_t over = seg_of(520.0f, 451.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &over) == PROFILE_SEG_TOO_FAST,
               "451 C/hr, just over it, is TOO_FAST");

    // And the margin really is 0.90, not 1.00: a rate between the margin and
    // the raw model maximum must still be red.
    profile_segment_t between = seg_of(520.0f, 480.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &between) == PROFILE_SEG_TOO_FAST,
               "480 C/hr -- under the model's raw 500 C/hr but over the 90% margin -- is TOO_FAST");
}

// ---------------------------------------------------------------------------
// 9. The start-temperature carry across segments.
// ---------------------------------------------------------------------------

static void test_profile_carries_start_temperature(void)
{
    TEST_SECTION("profile walk carries the previous target as the next start temperature");

    stub_reset();

    // Segment 0: 20 -> 500 C at 100 C/hr. Heating, allowed 450 C/hr -> OK.
    // Segment 1: 500 -> 120 C at 200 C/hr.
    //   Correct carry (start 500) -> COOLING, allowed 0.90*100 = 90 C/hr
    //                                -> TOO_FAST.
    //   A bug that always started from ambient (20) would read this as
    //   HEATING to 120 C, allowed 0.90*900 = 810 C/hr -> OK. So the two
    //   readings give different verdicts and this test can tell them apart.
    const profile_segment_t segs[] = {
        seg_of(500.0f, 100.0f),
        seg_of(120.0f, 200.0f),
    };
    profile_t p = profile_of(0x01, segs, 2);

    profile_seg_verdict_t per_seg[PROFILE_MAX_SEGMENTS];
    for (size_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        per_seg[i] = PROFILE_SEG_UNKNOWN;
    }
    profile_seg_verdict_t rollup = profile_feasibility_profile(0, &p, per_seg, PROFILE_MAX_SEGMENTS);

    TEST_CHECK(per_seg[0] == PROFILE_SEG_OK, "segment 0 (20 -> 500 C at 100 C/hr) is OK");
    TEST_CHECK(per_seg[1] == PROFILE_SEG_TOO_FAST,
               "segment 1 is judged as a COOL from 500 C, not a heat from ambient -- the carry "
               "is what makes it TOO_FAST");
    TEST_CHECK(rollup == PROFILE_SEG_TOO_FAST, "the roll-up reports the worse of the two");

    // A NULL profile / empty profile has nothing to judge.
    TEST_CHECK(profile_feasibility_profile(0, NULL, NULL, 0) == PROFILE_SEG_UNKNOWN,
               "NULL profile is UNKNOWN");
    profile_t empty = profile_of(0x01, segs, 0);
    TEST_CHECK(profile_feasibility_profile(0, &empty, NULL, 0) == PROFILE_SEG_UNKNOWN,
               "a profile with no segments is UNKNOWN");
}

// ---------------------------------------------------------------------------
// 10. Roll-up ordering: UNREACHABLE > TOO_FAST > UNKNOWN > OK.
// ---------------------------------------------------------------------------

static void test_rollup_ordering(void)
{
    TEST_SECTION("roll-up ordering UNREACHABLE > TOO_FAST > UNKNOWN > OK");

    stub_reset();

    // One unanswerable segment (NaN target) among otherwise-fine ones must
    // stop the profile reporting a clean bill of health. It is put last so
    // the NaN start_c it carries forward cannot contaminate the others.
    const profile_segment_t ok_then_unknown[] = {
        seg_of(200.0f, 50.0f), /* OK */
        seg_of(NAN, 50.0f),    /* UNKNOWN */
    };
    profile_t p1 = profile_of(0x01, ok_then_unknown, 2);
    profile_seg_verdict_t v1[PROFILE_MAX_SEGMENTS];
    TEST_CHECK(profile_feasibility_profile(0, &p1, v1, PROFILE_MAX_SEGMENTS) == PROFILE_SEG_UNKNOWN,
               "one UNKNOWN segment prevents a whole-profile 'ok' verdict");
    TEST_CHECK(v1[0] == PROFILE_SEG_OK && v1[1] == PROFILE_SEG_UNKNOWN,
               "per-segment verdicts are still reported individually");

    // TOO_FAST outranks UNKNOWN -- a real problem is not masked by an
    // unanswerable neighbour.
    const profile_segment_t unknown_and_fast[] = {
        seg_of(950.0f, 200.0f), /* TOO_FAST (allowed 63 C/hr) */
        seg_of(NAN, 50.0f),     /* UNKNOWN */
    };
    profile_t p2 = profile_of(0x01, unknown_and_fast, 2);
    TEST_CHECK(profile_feasibility_profile(0, &p2, NULL, 0) == PROFILE_SEG_TOO_FAST,
               "TOO_FAST outranks UNKNOWN");

    // UNREACHABLE outranks TOO_FAST, whichever order they appear in.
    const profile_segment_t fast_then_unreachable[] = {
        seg_of(950.0f, 200.0f),  /* TOO_FAST */
        seg_of(1100.0f, 10.0f),  /* UNREACHABLE */
    };
    profile_t p3 = profile_of(0x01, fast_then_unreachable, 2);
    TEST_CHECK(profile_feasibility_profile(0, &p3, NULL, 0) == PROFILE_SEG_UNREACHABLE,
               "UNREACHABLE outranks TOO_FAST (worse verdict later in the profile)");

    const profile_segment_t unreachable_then_fast[] = {
        seg_of(1100.0f, 10.0f),  /* UNREACHABLE */
        seg_of(950.0f, 200.0f),  /* TOO_FAST */
    };
    profile_t p4 = profile_of(0x01, unreachable_then_fast, 2);
    TEST_CHECK(profile_feasibility_profile(0, &p4, NULL, 0) == PROFILE_SEG_UNREACHABLE,
               "...and worse verdict earlier in the profile");

    // All-OK really does report OK, so the ordering test above is not passing
    // for the trivial reason that nothing ever reports OK.
    const profile_segment_t all_ok[] = {
        seg_of(200.0f, 50.0f),
        seg_of(600.0f, 50.0f),
        seg_of(400.0f, 50.0f),
    };
    profile_t p5 = profile_of(0x01, all_ok, 3);
    TEST_CHECK(profile_feasibility_profile(0, &p5, NULL, 0) == PROFILE_SEG_OK,
               "a profile every segment of which is OK reports OK");

    // The string mapping the UI/JSON depends on.
    TEST_CHECK(strcmp(profile_feasibility_verdict_str(PROFILE_SEG_OK), "ok") == 0, "str(OK)");
    TEST_CHECK(strcmp(profile_feasibility_verdict_str(PROFILE_SEG_UNKNOWN), "unknown") == 0,
               "str(UNKNOWN)");
    TEST_CHECK(strcmp(profile_feasibility_verdict_str(PROFILE_SEG_TOO_FAST), "too_fast") == 0,
               "str(TOO_FAST)");
    TEST_CHECK(strcmp(profile_feasibility_verdict_str(PROFILE_SEG_UNREACHABLE), "unreachable") == 0,
               "str(UNREACHABLE)");
}

// ---------------------------------------------------------------------------
// 11. The zone-mask roll-up.
// ---------------------------------------------------------------------------

static void test_profile_mask(void)
{
    TEST_SECTION("profile_feasibility_profile_mask -- worst verdict across zones wins");

    const profile_segment_t segs[] = {
        seg_of(200.0f, 50.0f), /* OK on both zones below */
        seg_of(500.0f, 50.0f), /* OK on zone 0, UNREACHABLE on the weak zone 1 */
    };

    // Mask 0 with no zones configured: nothing to judge against.
    stub_reset();
    s_thermo_count = 0;
    profile_t p = profile_of(0x00, segs, 2);
    TEST_CHECK(profile_feasibility_profile_mask(0, &p, NULL, 0) == PROFILE_SEG_UNKNOWN,
               "mask 0 with zero configured zones is UNKNOWN -- nothing to judge against");

    // Two zones, the second much weaker: K=400 -> ceiling 420 C, threshold
    // 415 C, so the 500 C segment is out of its reach while zone 0 is fine.
    stub_reset();
    s_thermo_count = 2;
    s_zones[1].k_dc = 400.0f;

    profile_seg_verdict_t per_seg[PROFILE_MAX_SEGMENTS];
    profile_seg_verdict_t rollup =
        profile_feasibility_profile_mask(0, &p, per_seg, PROFILE_MAX_SEGMENTS);
    TEST_CHECK(rollup == PROFILE_SEG_UNREACHABLE,
               "mask 0 judges against every configured zone; the weak zone's UNREACHABLE wins");
    TEST_CHECK(per_seg[0] == PROFILE_SEG_OK,
               "segment 0 is within both zones, so it stays OK per-segment");
    TEST_CHECK(per_seg[1] == PROFILE_SEG_UNREACHABLE,
               "the worst verdict wins PER SEGMENT as well as overall");

    // An explicit mask naming only the strong zone must not inherit the weak
    // zone's verdict.
    TEST_CHECK(profile_feasibility_profile_mask(0x01, &p, NULL, 0) == PROFILE_SEG_OK,
               "an explicit mask of zone 0 alone is OK");
    TEST_CHECK(profile_feasibility_profile_mask(0x02, &p, NULL, 0) == PROFILE_SEG_UNREACHABLE,
               "an explicit mask of zone 1 alone is UNREACHABLE");

    // An untuned zone in the set drags the whole thing to UNKNOWN rather than
    // letting a tuned neighbour vouch for it.
    stub_reset();
    s_thermo_count = 2;
    stub_zone_untuned(1);
    TEST_CHECK(profile_feasibility_profile_mask(0, &p, NULL, 0) == PROFILE_SEG_UNKNOWN,
               "a tuned zone cannot vouch for an untuned one in the same mask");

    // ...but a real failure on the tuned zone still outranks that UNKNOWN.
    const profile_segment_t hot[] = { seg_of(1100.0f, 50.0f) };
    profile_t p_hot = profile_of(0x00, hot, 1);
    TEST_CHECK(profile_feasibility_profile_mask(0, &p_hot, NULL, 0) == PROFILE_SEG_UNREACHABLE,
               "a genuine failure is not masked by an untuned zone's UNKNOWN");
}

// ---------------------------------------------------------------------------
// Documented quirk -- TODO.md 5A.2. NOT an endorsement.
// ---------------------------------------------------------------------------

static void test_ceiling_test_gated_to_rising_segments(void)
{
    TEST_SECTION("FIXED (TODO.md 5A.2): the ceiling test is gated on target > start_c, so a "
                 "cooling segment near the ceiling is no longer misreported UNREACHABLE");

    // profile_feasibility_segment() used to apply the steady-state ceiling
    // test to EVERY segment, including descending ones -- but a cooling
    // target is arrived at by cooling, so it is reachable by construction.
    // The gate `target > start_c` confines the ceiling test to rising
    // segments, which is the only direction it can legitimately apply to.
    stub_reset(); /* ceiling 1020 C, UNREACHABLE threshold 1015 C */

    profile_segment_t cool_near_ceiling = seg_of(1016.0f, 50.0f);
    TEST_CHECK(profile_feasibility_segment(0, 1018.0f, &cool_near_ceiling) == PROFILE_SEG_OK,
               "cooling 1018 -> 1016 C is no longer UNREACHABLE now that the ceiling test is "
               "gated on target > start_c (rising only)");

    // A rising segment landing in the same margin band must still be caught.
    profile_segment_t heat_near_ceiling = seg_of(1016.0f, 50.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &heat_near_ceiling) == PROFILE_SEG_UNREACHABLE,
               "the same target approached by HEATING is still correctly UNREACHABLE -- the gate "
               "only excludes descending segments, not the check itself");

    // Immediately outside the margin the descending segment behaves sensibly
    // either way, confirming the gate does not merely widen a passing band.
    profile_segment_t cool_below_margin = seg_of(1010.0f, 50.0f);
    TEST_CHECK(profile_feasibility_segment(0, 1014.0f, &cool_below_margin) != PROFILE_SEG_UNREACHABLE,
               "a descending segment 5 C further down is not UNREACHABLE either");
}

static void test_flat_segment_above_ceiling_is_unreachable(void)
{
    TEST_SECTION("a standalone flat segment (target == start_c) above the ceiling is "
                 "UNREACHABLE -- the ceiling gate uses target >= start_c, not target > start_c, "
                 "so a dwell held above the ceiling cannot slip through as neither rising nor "
                 "falling");

    stub_reset(); /* ceiling 1020 C, UNREACHABLE threshold 1015 C */

    profile_segment_t flat_above_ceiling = seg_of(1016.0f, 50.0f);
    TEST_CHECK(profile_feasibility_segment(0, 1016.0f, &flat_above_ceiling) == PROFILE_SEG_UNREACHABLE,
               "a flat dwell at 1016 C, above the 1015 C threshold, is UNREACHABLE even though "
               "target == start_c is neither strictly heating nor cooling");
}


// ---------------------------------------------------------------------------
// 14. Coupling: a zone's ceiling rises when its neighbours fire alongside it.
// ---------------------------------------------------------------------------

static void test_coupling_raises_ceiling_in_mask(void)
{
    TEST_SECTION("multi-zone runs judge each zone with the coupling help of the other zones "
                 "in the mask, not on its solo k_dc alone");

    // Two zones, each solo K = 32 C -> solo ceiling 52 C, UNREACHABLE above
    // 47 C. Coupling 20 C each way, so with BOTH driven each zone's effective
    // gain is 32 + 20 = 52 C -> ceiling 72 C, threshold 67 C. A 60 C target
    // therefore flips from UNREACHABLE (solo) to OK (both zones firing) --
    // the real-hardware `cplval70` case in miniature.
    stub_reset();
    s_thermo_count = 2;
    for (uint8_t z = 0; z < 2; z++) {
        s_zones[z].k_dc = 32.0f;
    }
    const float row0[MAX31856_CHANNEL_COUNT] = { 0.0f, 20.0f, 0.0f };
    const float row1[MAX31856_CHANNEL_COUNT] = { 20.0f, 0.0f, 0.0f };
    test_stub_zones_set_coupling(0, true, row0);
    test_stub_zones_set_coupling(1, true, row1);

    const profile_segment_t segs[] = { seg_of(60.0f, 5.0f) };
    profile_t p = profile_of(0x03, segs, 1);

    // (a) single-zone verdict vs masked verdict.
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &segs[0]) == PROFILE_SEG_UNREACHABLE,
               "solo: 60 C is above zone 0's own 52 C ceiling, so UNREACHABLE");
    TEST_CHECK(profile_feasibility_segment(1, 20.0f, &segs[0]) == PROFILE_SEG_UNREACHABLE,
               "solo: 60 C is above zone 1's own 52 C ceiling too");
    TEST_CHECK(profile_feasibility_profile_mask(0x03, &p, NULL, 0) == PROFILE_SEG_OK,
               "with both zones in the mask each one's effective gain is 32 + 20 = 52 C, so "
               "60 C is comfortably under the 67 C threshold and the profile is OK");

    // The same conclusion through the segment-level entry point, so the fix
    // is not only visible via the roll-up.
    TEST_CHECK(profile_feasibility_segment_in_mask(0, 0x03, 20.0f, &segs[0]) == PROFILE_SEG_OK,
               "segment_in_mask with both zones driven is OK for zone 0");

    // The headroom rate test must use the same effective gain: at 60 C the
    // solo headroom is negative, while the coupled headroom is
    // 52 - (60 - 20) = 12 C, i.e. 12 C/hr max at tau = 3600 s. 20 C/hr is
    // over that, 5 C/hr is under it -- so the rate test is live and reading
    // k_eff, not merely passing because the ceiling test stopped failing.
    const profile_segment_t fast[] = { seg_of(60.0f, 20.0f) };
    profile_t p_fast = profile_of(0x03, fast, 1);
    TEST_CHECK(profile_feasibility_profile_mask(0x03, &p_fast, NULL, 0) == PROFILE_SEG_TOO_FAST,
               "20 C/hr exceeds 90% of the 12 C/hr the coupled headroom allows at 60 C");

    // (b) a mask naming only one of the two zones ignores coupling entirely.
    TEST_CHECK(profile_feasibility_profile_mask(0x01, &p, NULL, 0) == PROFILE_SEG_UNREACHABLE,
               "a mask of zone 0 alone gets no help from an idle zone 1 -- still UNREACHABLE");
    TEST_CHECK(profile_feasibility_profile_mask(0x02, &p, NULL, 0) == PROFILE_SEG_UNREACHABLE,
               "same for a mask of zone 1 alone");

    // (c) an all-zero coupling matrix leaves the verdict exactly as it was
    // before this feature existed.
    stub_reset();
    s_thermo_count = 2;
    for (uint8_t z = 0; z < 2; z++) {
        s_zones[z].k_dc = 32.0f;
    }
    TEST_CHECK(profile_feasibility_profile_mask(0x03, &p, NULL, 0) == PROFILE_SEG_UNREACHABLE,
               "zero coupling matrix: the multi-zone verdict is unchanged from the solo one");

    // ...and so does a coupling getter that cannot answer at all.
    test_stub_zones_set_coupling(0, false, NULL);
    test_stub_zones_set_coupling(1, false, NULL);
    TEST_CHECK(profile_feasibility_profile_mask(0x03, &p, NULL, 0) == PROFILE_SEG_UNREACHABLE,
               "a coupling getter that returns false contributes nothing, same as zeros");

    // Cooling is untouched: no zone helps another cool, so the coupled and
    // solo verdicts for a descending segment must agree.
    stub_reset();
    s_thermo_count = 2;
    const float hot0[MAX31856_CHANNEL_COUNT] = { 0.0f, 500.0f, 0.0f };
    const float hot1[MAX31856_CHANNEL_COUNT] = { 500.0f, 0.0f, 0.0f };
    test_stub_zones_set_coupling(0, true, hot0);
    test_stub_zones_set_coupling(1, true, hot1);
    profile_segment_t cool = seg_of(120.0f, 200.0f); /* from 220 C down */
    TEST_CHECK(profile_feasibility_segment_in_mask(0, 0x03, 220.0f, &cool) ==
                   profile_feasibility_segment(0, 220.0f, &cool),
               "a cooling segment's verdict is identical with and without coupling");
}

// ---------------------------------------------------------------------------
// 15. A zone-agnostic builtin (mask 0) must be judged SOLO per zone, not
// against the resolved all-zones coupling mask -- review finding on 96bcc43.
// ---------------------------------------------------------------------------

static void test_mask_zero_is_judged_coupled_across_configured_zones(void)
{
    TEST_SECTION("profile_feasibility_profile_mask(0, ...) judges every configured zone WITH "
                 "the coupling help of the others, same as an explicit full mask -- a builtin "
                 "(zone_mask 0) always runs on every configured zone together, per "
                 "profiles_http.c's resolution to (1u<<thermo_count)-1 before the executor runs "
                 "it, so it is never judged solo");

    // Same two-zone, k_dc=32, coupling 20-each-way setup as the coupled test
    // above: solo ceiling is 52 C (UNREACHABLE above 47 C), coupled ceiling
    // is 72 C (UNREACHABLE above 67 C). A 60 C target sits between the two,
    // so it is reachable only with coupling credited.
    stub_reset();
    s_thermo_count = 2;
    for (uint8_t z = 0; z < 2; z++) {
        s_zones[z].k_dc = 32.0f;
    }
    const float row0[MAX31856_CHANNEL_COUNT] = { 0.0f, 20.0f, 0.0f };
    const float row1[MAX31856_CHANNEL_COUNT] = { 20.0f, 0.0f, 0.0f };
    test_stub_zones_set_coupling(0, true, row0);
    test_stub_zones_set_coupling(1, true, row1);

    const profile_segment_t segs[] = { seg_of(60.0f, 5.0f) };
    profile_t p = profile_of(0, segs, 1); /* zone-agnostic: zone_mask field unused by _mask() */

    TEST_CHECK(profile_feasibility_profile_mask(0, &p, NULL, 0) == PROFILE_SEG_OK,
               "mask 0 resolves to both configured zones and each is judged against the "
               "coupled 72 C ceiling (60 C is comfortably under it), reachable only because "
               "coupling is credited -- a target that would be UNREACHABLE if judged solo");

    // An explicit nonzero mask naming both zones must reach the identical
    // verdict: mask 0 is just the resolved form of "every configured zone",
    // not a different, uncoupled judgement.
    TEST_CHECK(profile_feasibility_profile_mask(0x03, &p, NULL, 0) == PROFILE_SEG_OK,
               "an explicit mask of both zones gets the same coupled 72 C ceiling -> OK");
}

// ---------------------------------------------------------------------------
// 16. A zone_index outside zone_mask is UNKNOWN, not judged at all.
// ---------------------------------------------------------------------------

static void test_segment_in_mask_zone_not_in_mask_is_unknown(void)
{
    TEST_SECTION("profile_feasibility_segment_in_mask: zone_index absent from zone_mask -> "
                 "UNKNOWN, since that zone isn't being driven");

    stub_reset();
    const profile_segment_t seg = seg_of(60.0f, 5.0f);
    TEST_CHECK(profile_feasibility_segment_in_mask(2, 0x03, 20.0f, &seg) == PROFILE_SEG_UNKNOWN,
               "zone 2 is well-tuned but not named in mask 0x03 -- UNKNOWN, not judged solo "
               "and not silently skipped as OK");
}

// ---------------------------------------------------------------------------
// 17. Live ambient: a warmer bench raises the ceiling and can flip a verdict
// from UNREACHABLE to OK, matching the owner's bench observation (a 70 C
// target reported UNREACHABLE at the old hardcoded 20 C ambient, on a kiln
// that actually reaches 80 C).
// ---------------------------------------------------------------------------

static void test_live_ambient_raises_ceiling(void)
{
    TEST_SECTION("a live ambient of 30 C reaches a target that the 20 C fallback calls "
                 "UNREACHABLE -- ambient + k_eff is the ceiling, so raising ambient raises it");

    stub_reset();
    s_zones[0].k_dc = 50.0f;      // ceiling = t_amb + 50
    s_zones[0].tau_s = 3600.0f;   // so C/s * 3600 == C/hr, arithmetic stays hand-checkable
    profile_feasibility_test_clear_ambient_override();

    // At the 20 C fallback: ceiling 70, threshold 65 C -- a 70 C target sits
    // inside the margin (70 > 65) and is UNREACHABLE, same shape as the real
    // zone-2 bench case (72.4 C ceiling, 67.4 C threshold, 70 C target).
    profile_segment_t target70 = seg_of(70.0f, 1.0f); // slow rate -- only the ceiling matters
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &target70) == PROFILE_SEG_UNREACHABLE,
               "at the 20 C fallback ambient, 70 C is inside the 65 C threshold -> UNREACHABLE");

    // The SAME segment, judged with a live ambient of 30 C: ceiling 80,
    // threshold 75 C -- 70 C is now comfortably outside the margin.
    profile_feasibility_test_set_ambient_c(30.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &target70) == PROFILE_SEG_OK,
               "at a live 30 C ambient, the same 70 C target clears the 75 C threshold -> OK");

    profile_feasibility_test_clear_ambient_override();
}

// ---------------------------------------------------------------------------
// 18. No live reading (thermo_owner cannot answer, or the reading it gives
// is unusable) falls back to FEASIBILITY_AMBIENT_C (20 C), reproducing every
// verdict this module gave before a live ambient existed.
// ---------------------------------------------------------------------------

static void test_no_live_ambient_falls_back_to_20(void)
{
    TEST_SECTION("no usable live reading -> falls back to 20 C, old verdicts unchanged");

    stub_reset();
    profile_feasibility_test_clear_ambient_override();
    // thermo_owner_command_read_all() is stubbed above to always answer
    // ESP_ERR_NOT_FOUND/count==0 -- exactly "thermo_owner cannot answer" --
    // so every test in this file that never touches the ambient override
    // already exercises this path. This test pins the two boundary verdicts
    // from test_ceiling_unreachable() explicitly, so a regression that made
    // the fallback silently drift off 20 C would be caught here even if the
    // ceiling-specific test above changed its own stub setup first.
    profile_segment_t inside_margin = seg_of(1016.0f, 1.0f);  // ceiling 1020, threshold 1015
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &inside_margin) == PROFILE_SEG_UNREACHABLE,
               "with no live reading, ambient is still 20 C -- 1016 C stays inside the 1015 C "
               "threshold, UNREACHABLE exactly as before this feature existed");

    profile_segment_t outside_margin = seg_of(1014.0f, 1.0f);
    TEST_CHECK(profile_feasibility_segment(0, 20.0f, &outside_margin) != PROFILE_SEG_UNREACHABLE,
               "and 1014 C still clears it -> not UNREACHABLE, same boundary as the 20 C "
               "fallback always gave");
}

void run_test_profile_feasibility(void)
{
    test_no_model_is_never_ok_and_never_red();
    test_ceiling_unreachable();
    test_heating_rate();
    test_cooling_rate();
    test_cooling_to_ambient();
    test_no_rate_limit();
    test_policy_ceiling();
    test_rate_margin_boundary();
    test_profile_carries_start_temperature();
    test_rollup_ordering();
    test_profile_mask();
    test_ceiling_test_gated_to_rising_segments();
    test_flat_segment_above_ceiling_is_unreachable();
    test_coupling_raises_ceiling_in_mask();
    test_mask_zero_is_judged_coupled_across_configured_zones();
    test_segment_in_mask_zone_not_in_mask_is_unknown();
    test_live_ambient_raises_ceiling();
    test_no_live_ambient_falls_back_to_20();
}
