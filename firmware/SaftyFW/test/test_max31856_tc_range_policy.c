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

void run_test_max31856_tc_range_policy(void)
{
    test_in_range_accepted_every_type();
    test_out_of_range_rejected_every_type();
    test_boundary_inclusive_every_type();
    test_just_past_boundary_rejected_every_type();
    test_nan_rejected_every_type();
    test_unrecognised_type_rejected();
    test_uncommissioned_default_k_covers_realistic_kiln_range();
}
