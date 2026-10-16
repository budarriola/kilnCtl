// PN-02: stale CT data on a commissioned board must read as present, not absent.
#include "test_common.h"
#include <math.h>
#include <string.h>
#include "../src/ct_blind_policy.h"
#include "../src/snapshots.h"

void run_test_ct_blind_policy(void)
{
    TEST_SECTION("ct_blind_policy -- PN-02 stale CT fails safe");
    TEST_CHECK(!ct_blind_treat_as_present(true, false, true, 100000u, 99900u), "fresh -> normal handling");
    TEST_CHECK(!ct_blind_treat_as_present(true, false, false, 100000u, 95000u), "stale 5 s, inside grace -> not yet");
    TEST_CHECK(ct_blind_treat_as_present(true, false, false, 100000u, 80000u), "stale 20 s -> treat as present");
    TEST_CHECK(ct_blind_treat_as_present(true, false, false, 100000u, 0u), "never published, past grace -> present");
    TEST_CHECK(!ct_blind_treat_as_present(true, false, false, 5000u, 0u), "never published, inside boot grace -> not yet");
    TEST_CHECK(!ct_blind_treat_as_present(false, false, false, 100000u, 80000u), "CT not active -> unchanged");
    TEST_CHECK(!ct_blind_treat_as_present(true, true, false, 100000u, 80000u), "clock stalled -> not decided here");
    TEST_CHECK(ct_blind_treat_as_present(true, false, false, 5u, 4294967000u) == false,
               "timestamp ahead of now (wrap, age huge) handled without crash");

    // PN-01: borrowed zone reading feeds the guard temperature input.
    TEST_SECTION("snapshots -- PN-01 borrowed zone guard reading");
    context_snapshot_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.zone_count = 2;
    ctx.zones[0].zone_index = 0;
    ctx.zones[0].flags = CONTEXT_ZONE_FLAG_MEASURED_VALID;
    ctx.zones[0].measured_c = 111.0f;
    ctx.zones[1].zone_index = 2;
    ctx.zones[1].flags = CONTEXT_ZONE_FLAG_MEASURED_VALID;
    ctx.zones[1].measured_c = 777.5f;
    ctx.zones[1].tc_fault = 0x01u;
    float c = 0.0f;
    uint8_t fb = 0u;
    TEST_CHECK(context_borrowed_guard_reading(&ctx, true, true, 2, &c, &fb) && c == 777.5f && fb == 0x01u,
               "PN-01 borrowed zone 2 measured_c and fault bits reach the guard input");
    TEST_CHECK(!context_borrowed_guard_reading(&ctx, false, true, 2, &c, &fb), "PN-01 context invalid -> invalid");
    TEST_CHECK(!context_borrowed_guard_reading(&ctx, true, false, 2, &c, &fb), "PN-01 no index -> invalid");
    TEST_CHECK(!context_borrowed_guard_reading(&ctx, true, true, 1, &c, &fb), "PN-01 zone absent -> invalid");
    ctx.zones[1].flags = 0u;
    TEST_CHECK(!context_borrowed_guard_reading(&ctx, true, true, 2, &c, &fb), "PN-01 MEASURED_VALID clear -> invalid");
    ctx.zones[1].flags = CONTEXT_ZONE_FLAG_MEASURED_VALID;
    ctx.zones[1].measured_c = NAN;
    TEST_CHECK(!context_borrowed_guard_reading(&ctx, true, true, 2, &c, &fb), "PN-01 NaN -> invalid");
}
