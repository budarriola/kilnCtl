// Host tests for firmware/SaftyFW/src/max31856_tc_range_policy.c -- the
// per-tc_type plausibility band (TODO.md Phase 3, "Per-type plausibility
// ranges"). No pico-sdk/FreeRTOS dependency, same discipline as
// test_max31856_decode.c and test_max31856_tc_type_policy.c.
//
// Ranges are datasheet-verified (MAX31856.pdf page 12, Table 1, TEMP RANGE
// column) -- see max31856_tc_range_policy.c's own table for the citation.
// Bounds are INCLUSIVE at both ends (the header's own doc comment argues
// why: the datasheet clamps an out-of-range reading AT the limit, making the
// limit itself a reachable, meaningful value) -- every boundary check below
// exercises that explicitly, both at the exact bound (must pass) and one ULP
// of a real temperature (0.001 degC) past it (must fail).
#include "test_common.h"

#include "../src/max31856.h"
#include "../src/max31856_tc_range_policy.h"

#include <math.h>

typedef struct {
    uint8_t     tc_type;
    const char *name;
    float       min_c;
    float       max_c;
} tc_case_t;

// All eight real types -- TODO.md's own instruction is "every one of the
// eight real types, not a spot check".
static const tc_case_t CASES[8] = {
    {MAX31856_TC_TYPE_B, "B", 250.0f, 1820.0f},
    {MAX31856_TC_TYPE_E, "E", -200.0f, 1000.0f},
    {MAX31856_TC_TYPE_J, "J", -210.0f, 1200.0f},
    {MAX31856_TC_TYPE_K, "K", -200.0f, 1372.0f},
    {MAX31856_TC_TYPE_N, "N", -200.0f, 1300.0f},
    {MAX31856_TC_TYPE_R, "R", -50.0f, 1768.0f},
    {MAX31856_TC_TYPE_S, "S", -50.0f, 1768.0f},
    {MAX31856_TC_TYPE_T, "T", -200.0f, 400.0f},
};

static void test_in_range_accepted_every_type(void)
{
    TEST_SECTION("in-range reading accepted, every one of the 8 real types");

    for (size_t i = 0; i < 8; i++) {
        const tc_case_t *c = &CASES[i];
        float mid = c->min_c + (c->max_c - c->min_c) * 0.5f;
        TEST_CHECK(max31856_tc_range_is_plausible(c->tc_type, mid),
                   c->name); // midpoint of the type's own range must pass
    }
}

static void test_out_of_range_rejected_every_type(void)
{
    TEST_SECTION("out-of-range reading (both directions) rejected, every one of the 8 real types");

    for (size_t i = 0; i < 8; i++) {
        const tc_case_t *c = &CASES[i];
        TEST_CHECK(!max31856_tc_range_is_plausible(c->tc_type, c->min_c - 1.0f), c->name);
        TEST_CHECK(!max31856_tc_range_is_plausible(c->tc_type, c->max_c + 1.0f), c->name);
    }
}

static void test_boundary_inclusive_every_type(void)
{
    TEST_SECTION("boundary values are INCLUSIVE (exactly at min/max passes), every type");

    for (size_t i = 0; i < 8; i++) {
        const tc_case_t *c = &CASES[i];
        TEST_CHECK(max31856_tc_range_is_plausible(c->tc_type, c->min_c), c->name);
        TEST_CHECK(max31856_tc_range_is_plausible(c->tc_type, c->max_c), c->name);
    }
}

static void test_just_past_boundary_rejected_every_type(void)
{
    TEST_SECTION("one hundredth of a degree past either bound is rejected, every type -- "
                 "proves the inclusive bound is not accidentally a wide-open one");

    for (size_t i = 0; i < 8; i++) {
        const tc_case_t *c = &CASES[i];
        TEST_CHECK(!max31856_tc_range_is_plausible(c->tc_type, c->min_c - 0.01f), c->name);
        TEST_CHECK(!max31856_tc_range_is_plausible(c->tc_type, c->max_c + 0.01f), c->name);
    }
}

static void test_nan_rejected_every_type(void)
{
    TEST_SECTION("NaN is never plausible, regardless of type");

    float nan_val = NAN;
    for (size_t i = 0; i < 8; i++) {
        TEST_CHECK(!max31856_tc_range_is_plausible(CASES[i].tc_type, nan_val), CASES[i].name);
    }
}

static void test_unrecognised_type_rejected(void)
{
    TEST_SECTION("a tc_type past T (voltage mode / unrecognised) has no datasheet range -- "
                 "always implausible, regardless of the value offered");

    TEST_CHECK(!max31856_tc_range_is_plausible(0x08u, 25.0f),
               "0x08 (voltage mode gain=8) -- ambient-looking value still rejected");
    TEST_CHECK(!max31856_tc_range_is_plausible(0x0Fu, 1000.0f),
               "0x0F (voltage mode gain=32) -- plausible-for-K value still rejected, "
               "since 0x0F has no thermocouple range at all");
}

// The uncommissioned-board case (design decision argued fully in
// max31856_tc_range_policy.h and thermo_task.c's wiring comment): a board
// that has never been commissioned resolves tc_type to
// MAX31856_TC_TYPE_K == CONFIG_STORE_DEFAULT_TC_TYPE. This test proves the
// concrete claim the design argument rests on -- that K's own datasheet band
// is wide enough to accept every real kiln temperature this codebase's own
// docs discuss (SAFETY_MODEL.md/TODO.md: type K is "marginal above ~1150
// degC", not unusable), so the never-commissioned board's day-to-day
// behaviour does not regress.
static void test_uncommissioned_default_k_covers_realistic_kiln_range(void)
{
    TEST_SECTION("uncommissioned board (type K default) still accepts realistic kiln readings");

    TEST_CHECK(max31856_tc_range_is_plausible(MAX31856_TC_TYPE_K, 20.0f), "room temperature");
    TEST_CHECK(max31856_tc_range_is_plausible(MAX31856_TC_TYPE_K, 1000.0f), "mid firing, bisque range");
    TEST_CHECK(max31856_tc_range_is_plausible(MAX31856_TC_TYPE_K, 1150.0f),
               "the ~1150 degC TODO.md itself calls 'marginal' for type K -- still plausible, "
               "not yet outside the part's own linearized range");
    TEST_CHECK(max31856_tc_range_is_plausible(MAX31856_TC_TYPE_K, 1300.0f), "cone 10 territory");
}

// --- max31856_tc_range_is_plausible_uncommissioned() (2026-08-24, Part A) --
//
// The union band is [-210, 1820]: -210 is J's own low bound (the lowest of
// all eight), 1820 is B's own high bound (the highest). Every other type's
// bound sits strictly inside that span, so this test picks values that
// specifically exercise the union's OWN two extremes, not any single type's.
static void test_uncommissioned_union_band(void)
{
    TEST_SECTION("max31856_tc_range_is_plausible_uncommissioned -- widest-band garbage floor");

    TEST_CHECK(max31856_tc_range_is_plausible_uncommissioned(-210.0f), "union low bound (J's), inclusive");
    TEST_CHECK(max31856_tc_range_is_plausible_uncommissioned(1820.0f), "union high bound (B's), inclusive");
    TEST_CHECK(max31856_tc_range_is_plausible_uncommissioned(20.0f), "room temperature");
    TEST_CHECK(!max31856_tc_range_is_plausible_uncommissioned(NAN), "NaN is never plausible here either");

    // Just past the union's own bounds -- genuine garbage, rejected either way.
    TEST_CHECK(!max31856_tc_range_is_plausible_uncommissioned(-210.01f), "just below the union's own low bound");
    TEST_CHECK(!max31856_tc_range_is_plausible_uncommissioned(1820.01f), "just above the union's own high bound");
}

// The behavioural difference this pass exists to prove (task instructions:
// "one it accepts that a specific type's band would have rejected"). 1500
// degC sits inside the union [-210, 1820] but strictly above K's own band
// (-200..1372) -- an uncommissioned board must accept it, while a board
// genuinely commissioned as K must still reject it via the ORIGINAL
// function. Proving both halves in one test is what makes this a real
// behavioural-difference check, not two independent facts.
static void test_uncommissioned_accepts_what_a_specific_type_would_reject(void)
{
    TEST_SECTION("uncommissioned union band accepts a value K's own (or any single type's) band "
                 "would reject -- the actual behavioural difference this pass adds");

    float value = 1500.0f;
    TEST_CHECK(!max31856_tc_range_is_plausible(MAX31856_TC_TYPE_K, value),
               "K's own exact band rejects 1500 degC (above its 1372 ceiling)");
    TEST_CHECK(max31856_tc_range_is_plausible_uncommissioned(value),
               "the SAME 1500 degC is accepted by the uncommissioned union band "
               "(inside B's 1820 ceiling)");

    // And the union band still rejects a value no real type could ever
    // report -- it is a floor, not a rubber stamp.
    float garbage = 5000.0f;
    TEST_CHECK(!max31856_tc_range_is_plausible_uncommissioned(garbage),
               "5000 degC is rejected by the union band too -- it is a floor, not 'anything goes'");
}

// --- max31856_cr1_readback_check() (2026-08-24, Part B) --------------------

static void test_cr1_readback_match_every_real_type(void)
{
    TEST_SECTION("max31856_cr1_readback_check -- MATCH for every real type, "
                 "at the actual CR1 byte this driver writes (AVGSEL=4, 0x2X)");

    for (uint8_t tc_type = MAX31856_TC_TYPE_B; tc_type <= MAX31856_TC_TYPE_T; tc_type++) {
        uint8_t cr1 = (uint8_t)(0x20u | tc_type); // AVGSEL=4 samples, this driver's own fixed choice
        TEST_CHECK(max31856_cr1_readback_check(tc_type, cr1) == MAX31856_CR1_READBACK_MATCH,
                   "readback equal to the byte actually written matches");
    }
}

static void test_cr1_readback_mismatch(void)
{
    TEST_SECTION("max31856_cr1_readback_check -- MISMATCH when the readback nibble differs");

    // Asked for K (0x03), part reports back running J (0x02) -- e.g. its
    // failed CR1 write left it on whatever it held before.
    TEST_CHECK(max31856_cr1_readback_check(MAX31856_TC_TYPE_K, (uint8_t)(0x20u | MAX31856_TC_TYPE_J)) ==
                   MAX31856_CR1_READBACK_MISMATCH,
               "K asked for, J read back -- a real, different, real thermocouple type: MISMATCH");

    // Asked for T (0x07), part reports back its own power-on default K (0x03).
    TEST_CHECK(max31856_cr1_readback_check(MAX31856_TC_TYPE_T, (uint8_t)(0x20u | MAX31856_TC_TYPE_K)) ==
                   MAX31856_CR1_READBACK_MISMATCH,
               "T asked for, K (power-on default) read back: MISMATCH -- the exact "
               "'CR1 write failed, part stuck on its old type' scenario this check exists for");
}

static void test_cr1_readback_dead_bus(void)
{
    TEST_SECTION("max31856_cr1_readback_check -- DEAD_BUS for 0x00/0xFF, not MISMATCH");

    // 0x00 and 0xFF can never be a byte this driver itself wrote for ANY
    // real type (AVGSEL is always fixed at 4 samples, upper nibble 0x2),
    // regardless of what tc_type was asked for -- checked against every
    // real type to prove this classification does not depend on which type
    // happened to be intended.
    for (uint8_t tc_type = MAX31856_TC_TYPE_B; tc_type <= MAX31856_TC_TYPE_T; tc_type++) {
        TEST_CHECK(max31856_cr1_readback_check(tc_type, 0x00u) == MAX31856_CR1_READBACK_DEAD_BUS,
                   "0x00 readback is DEAD_BUS regardless of intended type");
        TEST_CHECK(max31856_cr1_readback_check(tc_type, 0xFFu) == MAX31856_CR1_READBACK_DEAD_BUS,
                   "0xFF readback is DEAD_BUS regardless of intended type");
    }

    // 0x00's low nibble (0x0) IS MAX31856_TC_TYPE_B -- proving DEAD_BUS is
    // checked BEFORE the nibble compare, not merely "falls through to
    // mismatch and happens to differ": asking for B itself must still
    // report DEAD_BUS on a 0x00 readback, not a spurious MATCH.
    TEST_CHECK(max31856_cr1_readback_check(MAX31856_TC_TYPE_B, 0x00u) == MAX31856_CR1_READBACK_DEAD_BUS,
               "0x00 readback is DEAD_BUS even when the intended type's own nibble is 0x0 (B)");
}

void run_test_max31856_tc_range_policy(void)
{
    test_in_range_accepted_every_type();
    test_out_of_range_rejected_every_type();
    test_boundary_inclusive_every_type();
    test_just_past_boundary_rejected_every_type();
    test_nan_rejected_every_type();
    test_unrecognised_type_rejected();
    test_uncommissioned_default_k_covers_realistic_kiln_range();
    test_uncommissioned_union_band();
    test_uncommissioned_accepts_what_a_specific_type_would_reject();
    test_cr1_readback_match_every_real_type();
    test_cr1_readback_mismatch();
    test_cr1_readback_dead_bus();
}
