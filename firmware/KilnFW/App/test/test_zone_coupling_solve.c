// Host tests for zone_coupling_solve.c's diagonal-provenance question
// (PID_EXPANSION_PLAN.md sec 3.2, "CORRECTION 2026-09-02d"). No dedicated
// test file existed for this module before this one -- it was only ever
// exercised indirectly through test_profile_executor_prestart.c/
// test_adaptive_tune.c's larger fixtures, neither of which pins WHICH value
// G[row][row] actually uses. This file exists to make that provenance
// choice an explicit, falsifiable fact instead of an implicit one: today
// the diagonal is `ff_k_dc` (the step-identified per-zone gain), never the
// coupling matrix's own diagonal cell (contractually 0 in storage --
// zones_http.h's zones_config_get_coupling() doc comment). See sec 3.2 for
// why that choice was analysed and left unchanged: switching it needs
// persisted per-zone storage for the matrix's own diagonal that does not
// exist on the board today (zones_config_set_coupling() rejects a nonzero
// diagonal cell outright), and adding that storage is out of scope for the
// analysis pass that produced these numbers.
//
// Own executable (build_host_tests.ps1), same reason test_zones_http.c/
// test_adaptive_tune.c are: this file supplies its own tiny fake
// zones_config_get_coupling() (the only symbol zone_coupling_solve.c needs
// from zones_http.h) which would multiply-define against the main
// executable's or exe4's/exe17's own fakes of the same name if linked
// alongside them.
#include "test_common.h"

#include "zone_coupling_solve.h"

#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

// ---- fake zones_config_get_coupling() --------------------------------
// Row = affected zone, column = stepped zone, diagonal always 0 -- same
// contract zones_http.h documents for the real one.
static float s_fake_row[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];

bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_row[zone_index][j];
    return true;
}

static void set_matrix_row(uint8_t zi, float a, float b, float c)
{
    s_fake_row[zi][0] = a;
    s_fake_row[zi][1] = b;
    s_fake_row[zi][2] = c;
}

// PID_EXPANSION_PLAN.md sec 3.2's adopted 2026-09-02 matrix, [affected][stepped]:
//   [[38.13, 27.32, 21.72],
//    [14.30, 35.90, 22.15],
//    [ 8.33, 12.42, 35.32]]
// Diagonal cells (38.13/35.90/35.32) are the matrix's OWN diagonal -- not
// storable, never read by the production code, used here only as the
// alternative candidate to distinguish from ff_k_dc.
#define OWN_DIAG_Z0 38.13f
#define OWN_DIAG_Z1 35.90f
#define OWN_DIAG_Z2 35.32f

// z0/z1/z2 model_k_dc, read back off the board 2026-09-02 (sec 2) -- this is
// `ff_k_dc`, the value production code actually puts on the diagonal.
#define FF_K_DC_Z0 39.2459f
#define FF_K_DC_Z1 31.9669f
#define FF_K_DC_Z2 31.6810f

static void setup_adopted_matrix(void)
{
    memset(s_fake_row, 0, sizeof(s_fake_row));
    set_matrix_row(0, 0.0f, 27.32f, 21.72f);
    set_matrix_row(1, 14.30f, 0.0f, 22.15f);
    set_matrix_row(2, 8.33f, 12.42f, 0.0f);
}

// zi qualifies, is PID, unblocked, unfaulted -- the minimal true set every
// zone_coupling_neighbor_t below needs to be treated as a coupled member.
static zone_coupling_neighbor_t neighbor(bool qualifies, float ff_k_dc)
{
    zone_coupling_neighbor_t n;
    n.qualifies = qualifies;
    n.ff_k_dc = ff_k_dc;
    n.ff_tau_s = 0.0f;
    return n;
}

// ---- test 1: diagonal provenance is ff_k_dc, not the matrix's own cell ----
//
// Two-member system (z0 + z1 only, z2 unqualified), dT=30 -- representative
// of the bench dwell tests sec 3.2's figures were measured against
// (55C target, ~25C ambient). Two independently hand-solved 2x2 systems:
//
//   hybrid (production code, diag=ff_k_dc):    u0 = 0.1614
//   own-diagonal (the analysed alternative):   u0 = 0.2632
//
// The two solutions differ by ~0.10 duty -- comfortably outside float
// rounding, so this is a real fork in behaviour, not a tolerance question.
// Mutate G[row][row]'s assignment in zone_coupling_solve.c (e.g. swap
// `k_dc_s` for a hard-coded own-diagonal constant) and this test goes red
// against the wrong branch's number; that is the point of it.
static void test_hold_diagonal_is_ff_k_dc(void)
{
    TEST_SECTION("zone_coupling_solve: diagonal provenance == ff_k_dc");
    setup_adopted_matrix();

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[2] = neighbor(false, FF_K_DC_Z2); // z2 not in this system

    zone_coupling_hold_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix = false, infeasible = false, membership_changed = false;
    coupling_solve_reason_t reason;

    float u0 = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &membership_changed, &cache, &prev_sig);

    TEST_CHECK(reason == COUPLING_SOLVE_OK, "2-member hold solve should succeed");
    TEST_CHECK(used_matrix, "2-member hold solve should report used_matrix");
    // Pins the HYBRID answer (0.1614), not the own-diagonal answer (0.2632)
    // hand-solved in this file's header comment -- proves today's code path,
    // and flips red the moment G[row][row] stops being ff_k_dc.
    TEST_CHECK_NEAR(u0, 0.1614, 0.001, "z0 hold duty should match the ff_k_dc-diagonal hybrid solve");
    // The own-diagonal alternative is far enough away that a passing test
    // against 0.1614 also rules it out; check it explicitly anyway so the
    // "not this value" half of the claim is asserted, not just implied.
    TEST_CHECK(fabs((double)u0 - 0.2632) > 0.05, "z0 hold duty should NOT match the own-diagonal solve");
}

// ---- test 2: full 3-zone seam-step regression pin -----------------------
//
// All three zones qualify (steady dwell, no interlocks flapping) -- pins the
// full n=3 solve against the hybrid (ff_k_dc-diagonal) formula so any future
// change to the diagonal source, the matrix, or the elimination itself shows
// up here as a numeric drift rather than silently. See sec 3.2 for the
// derivation of the own-diagonal alternative (u0=0.1395/u1=0.3529/u2=0.6924)
// this is NOT pinned against.
static void test_hold_full_system_hybrid_regression(void)
{
    TEST_SECTION("zone_coupling_solve: full 3-zone hybrid solve regression pin");
    setup_adopted_matrix();

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[2] = neighbor(true, FF_K_DC_Z2);

    zone_coupling_hold_cache_t cache0, cache1, cache2;
    memset(&cache0, 0, sizeof(cache0));
    memset(&cache1, 0, sizeof(cache1));
    memset(&cache2, 0, sizeof(cache2));
    uint16_t sig0 = 0, sig1 = 0, sig2 = 0;
    bool used_matrix, infeasible, changed;
    coupling_solve_reason_t reason;

    float u0 = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache0, &sig0);
    float u1 = zone_coupling_solve_hold(true, FF_K_DC_Z1, 1, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache1, &sig1);
    float u2 = zone_coupling_solve_hold(true, FF_K_DC_Z2, 2, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache2, &sig2);

    TEST_CHECK_NEAR(u0, 0.0802, 0.001, "z0 full-system hybrid hold duty");
    TEST_CHECK_NEAR(u1, 0.3586, 0.001, "z1 full-system hybrid hold duty");
    TEST_CHECK_NEAR(u2, 0.7852, 0.001, "z2 full-system hybrid hold duty");
}

// ---- test 3: membership-transition seam step, z0-z1 pair -----------------
//
// Sizes what actually happens at the moment z1 joins z0's system: before,
// z0 is on the 1x1 fallback (duty = dT/ff_k_dc); after, it is on the 2x2
// hybrid solve from test 1. Both computed here so the transition step itself
// is pinned, independent of whether it is later judged acceptable.
static void test_hold_membership_transition_step(void)
{
    TEST_SECTION("zone_coupling_solve: 1x1->2x2 transition step size (z0, z1 joins)");
    setup_adopted_matrix();

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(false, FF_K_DC_Z1); // not yet qualifying
    zones[2] = neighbor(false, FF_K_DC_Z2);

    zone_coupling_hold_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix, infeasible, changed;
    coupling_solve_reason_t reason;

    // Before: no qualifying neighbours -- 1x1 fallback.
    float before = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, zones, 3, 55.0f, 25.0f, &used_matrix,
                                            &infeasible, &reason, &changed, &cache, &prev_sig);
    TEST_CHECK(reason == COUPLING_SOLVE_FALLBACK_NO_NEIGHBORS, "no neighbours yet -- 1x1 fallback expected");
    TEST_CHECK_NEAR(before, 0.7644, 0.001, "1x1 fallback duty == dT/ff_k_dc");

    // z1 now qualifies -- membership changes on this same call.
    zones[1].qualifies = true;
    float after = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, zones, 3, 55.0f, 25.0f, &used_matrix,
                                           &infeasible, &reason, &changed, &cache, &prev_sig);
    TEST_CHECK(reason == COUPLING_SOLVE_OK, "z1 joining should produce a genuine 2-member solve");
    TEST_CHECK(changed, "membership change must be reported on the joining tick");
    TEST_CHECK_NEAR(after, 0.1614, 0.001, "post-join duty matches test 1's hybrid 2x2 solve");

    double step = fabs((double)before - (double)after);
    // ~0.60 duty -- the coupling contribution itself, not a diagonal-choice
    // artifact (see this file's header comment / sec 3.2: the incremental
    // effect of switching diag source ALONE is ~0.10, an order of magnitude
    // smaller than this raw membership-transition step). Recorded here as a
    // sanity bound so a future change that makes this step implausibly large
    // or small is caught.
    TEST_CHECK(step > 0.3 && step < 0.9, "raw 1x1->2x2 transition step should be in the expected ballpark");
}

int main(void)
{
    test_hold_diagonal_is_ff_k_dc();
    test_hold_full_system_hybrid_regression();
    test_hold_membership_transition_step();

    printf("zone_coupling_solve: %d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
