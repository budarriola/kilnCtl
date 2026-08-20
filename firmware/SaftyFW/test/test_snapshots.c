// Host tests for snapshots.h's pure reduction helpers -- context_reduce_zones()
// and current_any_present(). Added alongside the ROADMAP fix that wires
// safety_core_build_input() to actually read link_task's published context
// (S2/S3/S4/S10/S13 were structurally unreachable before this pass -- see
// docs/GUARD_TEST_MATRIX.md section 6). These two functions are the pure,
// host-testable half of that wiring: everything else safety_core_build_input()
// does needs FreeRTOS/pico-sdk (xTaskGetTickCount, current_task_get_snapshot,
// ...) and cannot be built on the host, so this is the free coverage that IS
// available, same "pull the pure logic out where the RTOS-shaped wrapper
// cannot be host-tested" reasoning link_frame.c's own functions already rely
// on.
#include <string.h>

#include "test_common.h"
#include "../src/snapshots.h"

static context_snapshot_t empty_context(void)
{
    context_snapshot_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    return ctx;
}

static context_zone_t make_zone(uint8_t zone_index, uint8_t flags, float setpoint_c,
                                  float measured_c)
{
    context_zone_t z;
    memset(&z, 0, sizeof(z));
    z.zone_index = zone_index;
    z.flags = flags;
    z.setpoint_c = setpoint_c;
    z.measured_c = measured_c;
    return z;
}

// --- context_reduce_zones ----------------------------------------------------

static void test_reduce_zones_invalid_context(void)
{
    TEST_SECTION("context_reduce_zones -- ctx->valid == false -> zone_count 0");

    context_snapshot_t ctx = empty_context();
    ctx.valid = false;
    ctx.zone_count = 2;
    ctx.zones[0] = make_zone(0, CONTEXT_ZONE_FLAG_ACTIVE | CONTEXT_ZONE_FLAG_MEASURED_VALID, 900.0f, 895.0f);

    uint8_t count = 0xFFu;
    float max_setpoint = -1.0f;
    float nearest = -1.0f;
    context_reduce_zones(&ctx, 900.0f, &count, &max_setpoint, &nearest);

    TEST_CHECK(count == 0u, "invalid context -> zone_count 0 regardless of wire zone_count");
    TEST_CHECK(max_setpoint == 0.0f, "invalid context -> max_setpoint_c left at 0.0f");
    TEST_CHECK(nearest == 0.0f, "invalid context -> nearest_measured_c left at 0.0f");
}

static void test_reduce_zones_null_context(void)
{
    TEST_SECTION("context_reduce_zones -- ctx == NULL -> zone_count 0, no crash");

    uint8_t count = 0xFFu;
    float max_setpoint = -1.0f;
    float nearest = -1.0f;
    context_reduce_zones(NULL, 500.0f, &count, &max_setpoint, &nearest);

    TEST_CHECK(count == 0u, "NULL ctx -> zone_count 0");
    TEST_CHECK(max_setpoint == 0.0f, "NULL ctx -> max_setpoint_c 0.0f");
    TEST_CHECK(nearest == 0.0f, "NULL ctx -> nearest_measured_c 0.0f");
}

static void test_reduce_zones_no_eligible_zones(void)
{
    TEST_SECTION("context_reduce_zones -- zones present but none eligible");

    context_snapshot_t ctx = empty_context();
    ctx.valid = true;
    ctx.zone_count = 2;
    // Zone 0: ACTIVE but not MEASURED_VALID -- ineligible.
    ctx.zones[0] = make_zone(0, CONTEXT_ZONE_FLAG_ACTIVE, 900.0f, 895.0f);
    // Zone 1: MEASURED_VALID but not ACTIVE -- ineligible.
    ctx.zones[1] = make_zone(1, CONTEXT_ZONE_FLAG_MEASURED_VALID, 800.0f, 795.0f);

    uint8_t count = 0xFFu;
    float max_setpoint = -1.0f;
    float nearest = -1.0f;
    context_reduce_zones(&ctx, 900.0f, &count, &max_setpoint, &nearest);

    TEST_CHECK(count == 0u, "neither zone has both flags -> zone_count 0 (matches "
                             "safety_guard_input_t's own 'no active zones' contract)");
    TEST_CHECK(max_setpoint == 0.0f, "no eligible zone -> max_setpoint_c 0.0f");
    TEST_CHECK(nearest == 0.0f, "no eligible zone -> nearest_measured_c 0.0f");
}

static void test_reduce_zones_max_setpoint(void)
{
    TEST_SECTION("context_reduce_zones -- max_setpoint_c is the HIGHEST eligible setpoint");

    context_snapshot_t ctx = empty_context();
    ctx.valid = true;
    ctx.zone_count = 3;
    uint8_t both = CONTEXT_ZONE_FLAG_ACTIVE | CONTEXT_ZONE_FLAG_MEASURED_VALID;
    ctx.zones[0] = make_zone(0, both, 850.0f, 840.0f);
    ctx.zones[1] = make_zone(1, both, 950.0f, 945.0f); // highest setpoint
    ctx.zones[2] = make_zone(2, both, 700.0f, 705.0f);

    uint8_t count = 0;
    float max_setpoint = 0.0f;
    float nearest = 0.0f;
    // tc_c = 943.0 -> zone 1 (945.0) is the nearest match too.
    context_reduce_zones(&ctx, 943.0f, &count, &max_setpoint, &nearest);

    TEST_CHECK(count == 3u, "all three zones eligible -> zone_count 3");
    TEST_CHECK(max_setpoint == 950.0f, "max_setpoint_c is zone 1's 950.0C, the highest of the three");
    TEST_CHECK(nearest == 945.0f, "nearest_measured_c is zone 1's 945.0C (|943-945|=2, smallest diff)");
}

static void test_reduce_zones_nearest_ignores_max_setpoint_zone(void)
{
    TEST_SECTION("context_reduce_zones -- nearest match is independent of which zone has the max setpoint");

    context_snapshot_t ctx = empty_context();
    ctx.valid = true;
    ctx.zone_count = 2;
    uint8_t both = CONTEXT_ZONE_FLAG_ACTIVE | CONTEXT_ZONE_FLAG_MEASURED_VALID;
    ctx.zones[0] = make_zone(0, both, 1200.0f, 400.0f); // highest setpoint, but far from tc_c
    ctx.zones[1] = make_zone(1, both, 300.0f, 401.5f);  // lower setpoint, strictly nearest to tc_c

    uint8_t count = 0;
    float max_setpoint = 0.0f;
    float nearest = 0.0f;
    context_reduce_zones(&ctx, 401.0f, &count, &max_setpoint, &nearest);

    TEST_CHECK(count == 2u, "both zones eligible");
    TEST_CHECK(max_setpoint == 1200.0f, "max_setpoint_c still tracks zone 0's higher setpoint");
    TEST_CHECK(nearest == 401.5f,
                "nearest_measured_c is zone 1's 401.5C (|401-401.5|=0.5, strictly closer than "
                "zone 0's |401-400|=1) even though zone 0 has the higher setpoint -- the two "
                "scalars are computed independently, S2 and S10 ask genuinely different "
                "questions of the same zone array");
}

static void test_reduce_zones_ineligible_zone_excluded_from_both(void)
{
    TEST_SECTION("context_reduce_zones -- an ineligible zone is excluded from both scalars even "
                  "when it would otherwise win");

    context_snapshot_t ctx = empty_context();
    ctx.valid = true;
    ctx.zone_count = 2;
    uint8_t both = CONTEXT_ZONE_FLAG_ACTIVE | CONTEXT_ZONE_FLAG_MEASURED_VALID;
    // Zone 0 would win both the max-setpoint and nearest-match comparisons,
    // but it is not ACTIVE (profile not running on it) -- must be excluded.
    ctx.zones[0] = make_zone(0, CONTEXT_ZONE_FLAG_MEASURED_VALID, 5000.0f, 500.0f);
    ctx.zones[1] = make_zone(1, both, 300.0f, 250.0f);

    uint8_t count = 0;
    float max_setpoint = 0.0f;
    float nearest = 0.0f;
    context_reduce_zones(&ctx, 500.0f, &count, &max_setpoint, &nearest);

    TEST_CHECK(count == 1u, "only zone 1 is eligible");
    TEST_CHECK(max_setpoint == 300.0f, "zone 0's 5000.0C setpoint must not leak through -- not ACTIVE");
    TEST_CHECK(nearest == 250.0f, "zone 0's exact-match 500.0C reading must not leak through -- not ACTIVE");
}

static void test_reduce_zones_null_outputs_no_crash(void)
{
    TEST_SECTION("context_reduce_zones -- NULL output pointers are tolerated");

    context_snapshot_t ctx = empty_context();
    ctx.valid = true;
    ctx.zone_count = 1;
    ctx.zones[0] =
        make_zone(0, CONTEXT_ZONE_FLAG_ACTIVE | CONTEXT_ZONE_FLAG_MEASURED_VALID, 900.0f, 895.0f);

    // Must not crash with any subset of NULL outputs.
    context_reduce_zones(&ctx, 900.0f, NULL, NULL, NULL);
    uint8_t count = 0;
    context_reduce_zones(&ctx, 900.0f, &count, NULL, NULL);
    TEST_CHECK(count == 1u, "count still written when the two float outputs are NULL");
}

static void test_reduce_zones_overrun_zone_count_clamped(void)
{
    TEST_SECTION("context_reduce_zones -- a wire zone_count above CONTEXT_SNAPSHOT_MAX_ZONES is "
                  "clamped, never read out of bounds");

    context_snapshot_t ctx = empty_context();
    ctx.valid = true;
    // Untrusted/corrupt input: zone_count claims more than the array holds.
    // link_frame_unpack_context() already rejects this on the wire, but
    // context_reduce_zones() must not assume that validation ran -- defense
    // in depth for a struct that could, in principle, reach here some other
    // way.
    ctx.zone_count = (uint8_t)(CONTEXT_SNAPSHOT_MAX_ZONES + 5u);
    for (uint8_t i = 0; i < CONTEXT_SNAPSHOT_MAX_ZONES; i++) {
        ctx.zones[i] =
            make_zone(i, CONTEXT_ZONE_FLAG_ACTIVE | CONTEXT_ZONE_FLAG_MEASURED_VALID, 100.0f * (i + 1),
                       100.0f * (i + 1));
    }

    uint8_t count = 0;
    float max_setpoint = 0.0f;
    float nearest = 0.0f;
    context_reduce_zones(&ctx, 999.0f, &count, &max_setpoint, &nearest);

    TEST_CHECK(count == CONTEXT_SNAPSHOT_MAX_ZONES,
                "clamped to CONTEXT_SNAPSHOT_MAX_ZONES, not the corrupt wire count");
}

// --- current_any_present ------------------------------------------------------

static void test_current_any_present_all_below_threshold(void)
{
    TEST_SECTION("current_any_present -- all three channels below threshold -> false");

    current_snapshot_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.amps[0] = 0.1f;
    cur.amps[1] = 0.05f;
    cur.amps[2] = 1.9f;

    TEST_CHECK(!current_any_present(&cur, 2.0f), "0.1/0.05/1.9A, all < 2.0A threshold -> not present");
}

static void test_current_any_present_one_channel_above(void)
{
    TEST_SECTION("current_any_present -- one channel above threshold -> true");

    current_snapshot_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.amps[0] = 0.0f;
    cur.amps[1] = 2.01f; // just above
    cur.amps[2] = 0.0f;

    TEST_CHECK(current_any_present(&cur, 2.0f), "channel 1 at 2.01A > 2.0A threshold -> present");
}

static void test_current_any_present_exactly_at_threshold(void)
{
    TEST_SECTION("current_any_present -- exactly at threshold -> false (strictly greater-than)");

    current_snapshot_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.amps[0] = 2.0f;
    cur.amps[1] = 2.0f;
    cur.amps[2] = 2.0f;

    TEST_CHECK(!current_any_present(&cur, 2.0f), "all three exactly at 2.0A -> not present (> not >=)");
}

static void test_current_any_present_null(void)
{
    TEST_SECTION("current_any_present -- NULL snapshot -> false (conservative default)");

    TEST_CHECK(!current_any_present(NULL, 2.0f), "NULL -> not present, never a crash");
}

void run_test_snapshots(void)
{
    test_reduce_zones_invalid_context();
    test_reduce_zones_null_context();
    test_reduce_zones_no_eligible_zones();
    test_reduce_zones_max_setpoint();
    test_reduce_zones_nearest_ignores_max_setpoint_zone();
    test_reduce_zones_ineligible_zone_excluded_from_both();
    test_reduce_zones_null_outputs_no_crash();
    test_reduce_zones_overrun_zone_count_clamped();

    test_current_any_present_all_below_threshold();
    test_current_any_present_one_channel_above();
    test_current_any_present_exactly_at_threshold();
    test_current_any_present_null();
}
