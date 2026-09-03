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

// ---- fake zones_config_get_coupling_diag_k_dc() -----------------------
// PID_EXPANSION_PLAN.md sec 3.2 ("the solver switch itself"): the matrix's
// own diagonal cell, kept in separate storage from s_fake_row above (whose
// diagonal is always 0, mirroring zones_config_get_coupling()'s real
// contract). Defaults to "not measured" (false) for every zone -- a test
// that wants use_measured_diag_k_dc to actually change the answer must
// call set_diag_k_dc() first.
static bool  s_fake_diag_present[MAX31856_CHANNEL_COUNT];
static float s_fake_diag[MAX31856_CHANNEL_COUNT];

bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !s_fake_diag_present[zone_index]) return false;
    *out_k_dc = s_fake_diag[zone_index];
    return true;
}

static void set_diag_k_dc(uint8_t zi, float k_dc)
{
    s_fake_diag_present[zi] = true;
    s_fake_diag[zi] = k_dc;
}

static void clear_diag_k_dc(void)
{
    memset(s_fake_diag_present, 0, sizeof(s_fake_diag_present));
    memset(s_fake_diag, 0, sizeof(s_fake_diag));
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
    clear_diag_k_dc();
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

    float u0 = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, false, zones, 3, 55.0f, 25.0f, &used_matrix,
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

    float u0 = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, false, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache0, &sig0);
    float u1 = zone_coupling_solve_hold(true, FF_K_DC_Z1, 1, false, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache1, &sig1);
    float u2 = zone_coupling_solve_hold(true, FF_K_DC_Z2, 2, false, zones, 3, 55.0f, 25.0f, &used_matrix,
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
    float before = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, false, zones, 3, 55.0f, 25.0f, &used_matrix,
                                            &infeasible, &reason, &changed, &cache, &prev_sig);
    TEST_CHECK(reason == COUPLING_SOLVE_FALLBACK_NO_NEIGHBORS, "no neighbours yet -- 1x1 fallback expected");
    TEST_CHECK_NEAR(before, 0.7644, 0.001, "1x1 fallback duty == dT/ff_k_dc");

    // z1 now qualifies -- membership changes on this same call.
    zones[1].qualifies = true;
    float after = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, false, zones, 3, 55.0f, 25.0f, &used_matrix,
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

// ---- test 4: use_measured_diag_k_dc=true switches the diagonal source ----
//
// PID_EXPANSION_PLAN.md sec 3.2's "solver switch itself": with the flag on
// AND coupling_diag_k_dc populated for both members, G[row][row] must be the
// matrix's OWN diagonal (38.13/35.90), not ff_k_dc -- pinned against the
// SAME own-diagonal answer (0.2632) test 1's header comment hand-solved and
// explicitly ruled out for the flag-off case. If G[row][row] silently stayed
// ff_k_dc regardless of the flag, this test would read 0.1614 (test 1's
// value) instead and fail.
static void test_hold_measured_diag_flag_switches_diagonal(void)
{
    TEST_SECTION("zone_coupling_solve: use_measured_diag_k_dc=true selects the matrix's own diagonal");
    setup_adopted_matrix();
    set_diag_k_dc(0, OWN_DIAG_Z0);
    set_diag_k_dc(1, OWN_DIAG_Z1);

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[2] = neighbor(false, FF_K_DC_Z2);

    zone_coupling_hold_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix = false, infeasible = false, changed = false;
    coupling_solve_reason_t reason;

    float u0 = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, true, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache, &prev_sig);

    TEST_CHECK(reason == COUPLING_SOLVE_OK, "flag-on 2-member hold solve should succeed");
    TEST_CHECK_NEAR(u0, 0.2632, 0.001, "flag on + measured diag present should give the own-diagonal solve");
    TEST_CHECK(fabs((double)u0 - 0.1614) > 0.05, "flag-on result should NOT match the ff_k_dc-diagonal hybrid solve");
}

// ---- test 5: flag on but coupling_diag_k_dc unmeasured -- falls back to
// ff_k_dc, does not crash or silently use zero/garbage --------------------
//
// The guarded-fallback half of the contract: `use_measured_diag_k_dc=true`
// is not itself sufficient to change behaviour if the per-zone value was
// never populated. This test models the getter reporting false outright
// (zone_index out of range, or a getter that has no data at all for this
// zone) -- a LEGAL outcome the guard must also survive, but NOT what a real
// board's every-zone-unmeasured state actually looks like: the real
// accessor (zones_config_accessors.c's zones_config_get_coupling_diag_k_dc())
// returns true for any in-range zone index regardless of whether the field
// was ever written, and an unwritten coupling_diag_k_dc default-initializes
// to 0.0f. So in production it is the getter reporting TRUE with `measured
// == 0.0f` (and negative/NaN/+-Inf are reachable too, e.g. via backup_http.c
// import of a corrupted/adversarial file) that represents "never measured
// on every zone" -- exercised by the five tests immediately below this one,
// which are the ones that actually pin the isfinite()/`> 0.0f` guard's
// real-world job. Must reproduce test 1's flag-off answer exactly.
static void test_hold_measured_diag_flag_falls_back_when_unmeasured(void)
{
    TEST_SECTION("zone_coupling_solve: use_measured_diag_k_dc=true falls back to ff_k_dc when unmeasured");
    setup_adopted_matrix(); // clears the diag stub -- nothing set for any zone

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[2] = neighbor(false, FF_K_DC_Z2);

    zone_coupling_hold_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix = false, infeasible = false, changed = false;
    coupling_solve_reason_t reason;

    float u0 = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, true, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache, &prev_sig);

    TEST_CHECK(reason == COUPLING_SOLVE_OK, "flag-on, unmeasured-diag hold solve should still succeed");
    TEST_CHECK_NEAR(u0, 0.1614, 0.001, "flag on but unmeasured diag should fall back to the ff_k_dc hybrid solve");
}

// ---- tests 6-10: flag on, getter reports TRUE, but the stored value is
// one of coupling_diagonal_k_dc()'s five documented "not usable" cases --
// each must still fall back to ff_k_dc, exactly like test 5, because this is
// what "never measured" actually looks like coming out of the real
// zones_config_get_coupling_diag_k_dc() (see the corrected comment above
// test 5, and coupling_diagonal_k_dc()'s own doc comment in
// zone_coupling_solve.c). Deleting `&& isfinite(measured) && measured >
// 0.0f` from that helper leaves test 1-5 green (they never populate the
// stub with a "present, unusable" value) but must turn these five red,
// because the stub now reports present=true with a value the un-guarded
// code would use directly as G[row][row] -- see this file's mutation-test
// notes in the report that shipped alongside this comment.
static void run_present_but_unusable_case(const char *label, float bad_value)
{
    setup_adopted_matrix();
    set_diag_k_dc(0, bad_value);
    set_diag_k_dc(1, OWN_DIAG_Z1); // z1 IS usable -- isolates the assertion to z0's own guard

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[2] = neighbor(false, FF_K_DC_Z2);

    zone_coupling_hold_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix = false, infeasible = false, changed = false;
    coupling_solve_reason_t reason;

    float u0 = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, true, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache, &prev_sig);

    TEST_CHECK(reason == COUPLING_SOLVE_OK, label);
    // z1's diagonal is the own-diagonal value (OWN_DIAG_Z1) in this call, not
    // FF_K_DC_Z1 -- so this is not test 1's exact hybrid answer (0.1614) or
    // test 4's exact own-diagonal answer (0.2632); it is a THIRD, distinct
    // number sitting between them, computed independently below so the
    // check is falsifiable rather than accidentally matching either pinned
    // constant.
    TEST_CHECK_NEAR(u0, 0.25279, 0.001, label);
}

static void test_hold_measured_diag_present_but_zero_falls_back(void)
{
    TEST_SECTION("zone_coupling_solve: measured present, value == 0.0f -- must fall back to ff_k_dc");
    run_present_but_unusable_case("present-but-zero must fall back to ff_k_dc-diagonal answer", 0.0f);
}

static void test_hold_measured_diag_present_but_negative_falls_back(void)
{
    TEST_SECTION("zone_coupling_solve: measured present, value < 0 -- must fall back to ff_k_dc");
    run_present_but_unusable_case("present-but-negative must fall back to ff_k_dc-diagonal answer", -5.0f);
}

static void test_hold_measured_diag_present_but_nan_falls_back(void)
{
    TEST_SECTION("zone_coupling_solve: measured present, value == NaN -- must fall back to ff_k_dc");
    run_present_but_unusable_case("present-but-NaN must fall back to ff_k_dc-diagonal answer", (float)NAN);
}

static void test_hold_measured_diag_present_but_posinf_falls_back(void)
{
    TEST_SECTION("zone_coupling_solve: measured present, value == +Inf -- must fall back to ff_k_dc");
    run_present_but_unusable_case("present-but-+Inf must fall back to ff_k_dc-diagonal answer", (float)INFINITY);
}

static void test_hold_measured_diag_present_but_neginf_falls_back(void)
{
    TEST_SECTION("zone_coupling_solve: measured present, value == -Inf -- must fall back to ff_k_dc");
    run_present_but_unusable_case("present-but--Inf must fall back to ff_k_dc-diagonal answer", (float)-INFINITY);
}

// ---- test 11: the singular-G consequence of a bad diagonal ---------------
//
// z1 here has NO measured coupling to anyone (its whole coupling_coeff row
// is 0 -- an isolated zone in this particular system, e.g. one whose
// off-diagonal cells were never commissioned) AND its diag_k_dc reads
// present-but-zero. With the guard intact, coupling_diagonal_k_dc() falls
// back to ff_k_dc for z1's diagonal, so z1's row is [0 (off-diag), ff_k_dc
// (diag)] -- a normal, solvable row (COUPLING_SOLVE_OK expected below).
// Delete the guard and z1's diagonal becomes the raw 0.0f the stub reports,
// making the WHOLE row (off-diagonal AND diagonal both 0) identically
// zero -- rank-deficient, unconditionally singular regardless of the other
// rows. This is the concrete "zero diagonal makes G singular" case the
// review called out; see this file's own mutation-test log for the actual
// COUPLING_SOLVE_FALLBACK_SINGULAR failure this produces once the guard is
// removed.
static void test_hold_zero_row_and_zero_diag_is_solvable_with_guard(void)
{
    TEST_SECTION("zone_coupling_solve: guard intact -- an unmeasured, zero-coupling-row zone still "
                 "produces a solvable (non-singular) system via the ff_k_dc fallback diagonal");
    setup_adopted_matrix();
    set_matrix_row(1, 0.0f, 0.0f, 0.0f); // z1: no measured coupling to anyone
    set_diag_k_dc(1, 0.0f);              // and its own diag_k_dc reads present-but-zero

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[2] = neighbor(false, FF_K_DC_Z2);

    zone_coupling_hold_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix = false, infeasible = false, changed = false;
    coupling_solve_reason_t reason;

    float u0 = zone_coupling_solve_hold(true, FF_K_DC_Z0, 0, true, zones, 3, 55.0f, 25.0f, &used_matrix,
                                        &infeasible, &reason, &changed, &cache, &prev_sig);
    (void)u0;

    TEST_CHECK(reason == COUPLING_SOLVE_OK, "z1's ff_k_dc-diagonal fallback keeps the system solvable "
              "even though z1's own coupling row and measured diag are both entirely unmeasured");
    TEST_CHECK(used_matrix, "a genuine (non-fallback) 2x2 solve must engage here");
}

// ---- tests 12-14: climb-term coverage of the same flag, mirroring the
// hold tests above -- PID_EXPANSION_PLAN.md's own documented history
// (project_feedforward_climb_uncoupled.md) is a hold term that solved the
// matrix while the climb term silently used the uncoupled formula; this
// flag shipped with ZERO climb coverage prior to this file's addition,
// which is exactly the shape of bug that history warns about. ------------

// z0/z1 tau values -- arbitrary but fixed, distinct per zone (never equal,
// so a row/column tau mixup would not accidentally cancel out).
#define TAU_Z0 120.0f
#define TAU_Z1 150.0f
#define TAU_Z2 90.0f
#define CLIMB_RATE_C_PER_S 0.5f

// ---- test 12: climb diagonal provenance is ff_k_dc (flag off), pinned
// against an independently hand-solved hybrid answer, with the own-diagonal
// alternative ruled out explicitly -- the climb-term counterpart of test 1.
static void test_climb_diagonal_is_ff_k_dc(void)
{
    TEST_SECTION("zone_coupling_solve_climb: diagonal provenance == ff_k_dc (flag off)");
    setup_adopted_matrix();

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[1].ff_tau_s = TAU_Z1; // neighbor() defaults ff_tau_s to 0 -- the climb RHS needs z1's real tau
    zones[2] = neighbor(false, FF_K_DC_Z2);

    zone_coupling_climb_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix = false, infeasible = false, changed = false;
    coupling_solve_reason_t reason;

    // numpy.linalg.solve([[39.2459,27.32],[14.30,31.9669]], [60,75]) ==
    // [-0.15162281, 2.41400343] (verified A@u reproduces b).
    float u0 = zone_coupling_solve_climb(true, FF_K_DC_Z0, TAU_Z0, 0, false, zones, 3, CLIMB_RATE_C_PER_S,
                                         &used_matrix, &infeasible, &reason, &changed, &cache, &prev_sig);

    TEST_CHECK(reason == COUPLING_SOLVE_OK, "2-member climb solve should succeed");
    TEST_CHECK(used_matrix, "2-member climb solve should report used_matrix");
    TEST_CHECK_NEAR(u0, -0.15162, 0.001, "z0 climb duty should match the ff_k_dc-diagonal hybrid solve");
}

// ---- test 13: use_measured_diag_k_dc=true switches the climb diagonal ----
// The climb-term counterpart of test 4.
static void test_climb_measured_diag_flag_switches_diagonal(void)
{
    TEST_SECTION("zone_coupling_solve_climb: use_measured_diag_k_dc=true selects the matrix's own "
                 "diagonal");
    setup_adopted_matrix();
    set_diag_k_dc(0, OWN_DIAG_Z0);
    set_diag_k_dc(1, OWN_DIAG_Z1);

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[1].ff_tau_s = TAU_Z1; // neighbor() defaults ff_tau_s to 0 -- the climb RHS needs z1's real tau
    zones[2] = neighbor(false, FF_K_DC_Z2);

    zone_coupling_climb_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix = false, infeasible = false, changed = false;
    coupling_solve_reason_t reason;

    // numpy.linalg.solve([[38.13,27.32],[14.30,35.90]], [60,75]) ==
    // [0.107341, 2.04637949].
    float u0 = zone_coupling_solve_climb(true, FF_K_DC_Z0, TAU_Z0, 0, true, zones, 3, CLIMB_RATE_C_PER_S,
                                         &used_matrix, &infeasible, &reason, &changed, &cache, &prev_sig);

    TEST_CHECK(reason == COUPLING_SOLVE_OK, "flag-on 2-member climb solve should succeed");
    TEST_CHECK_NEAR(u0, 0.10734, 0.001, "flag on + measured diag present should give the own-diagonal "
                    "climb solve");
    TEST_CHECK(fabs((double)u0 - (-0.15162)) > 0.05, "flag-on climb result should NOT match the "
              "ff_k_dc-diagonal hybrid solve");
}

// ---- test 14: flag on but coupling_diag_k_dc unmeasured -- climb falls
// back to ff_k_dc. The climb-term counterpart of test 5.
static void test_climb_measured_diag_flag_falls_back_when_unmeasured(void)
{
    TEST_SECTION("zone_coupling_solve_climb: use_measured_diag_k_dc=true falls back to ff_k_dc when "
                 "unmeasured");
    setup_adopted_matrix(); // clears the diag stub -- nothing set for any zone

    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    zones[0] = neighbor(true, FF_K_DC_Z0);
    zones[1] = neighbor(true, FF_K_DC_Z1);
    zones[1].ff_tau_s = TAU_Z1; // neighbor() defaults ff_tau_s to 0 -- the climb RHS needs z1's real tau
    zones[2] = neighbor(false, FF_K_DC_Z2);

    zone_coupling_climb_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    uint16_t prev_sig = 0;
    bool used_matrix = false, infeasible = false, changed = false;
    coupling_solve_reason_t reason;

    float u0 = zone_coupling_solve_climb(true, FF_K_DC_Z0, TAU_Z0, 0, true, zones, 3, CLIMB_RATE_C_PER_S,
                                         &used_matrix, &infeasible, &reason, &changed, &cache, &prev_sig);

    TEST_CHECK(reason == COUPLING_SOLVE_OK, "flag-on, unmeasured-diag climb solve should still succeed");
    TEST_CHECK_NEAR(u0, -0.15162, 0.001, "flag on but unmeasured diag should fall back to the ff_k_dc "
                    "hybrid climb solve");
}

int main(void)
{
    test_hold_diagonal_is_ff_k_dc();
    test_hold_full_system_hybrid_regression();
    test_hold_membership_transition_step();
    test_hold_measured_diag_flag_switches_diagonal();
    test_hold_measured_diag_flag_falls_back_when_unmeasured();
    test_hold_measured_diag_present_but_zero_falls_back();
    test_hold_measured_diag_present_but_negative_falls_back();
    test_hold_measured_diag_present_but_nan_falls_back();
    test_hold_measured_diag_present_but_posinf_falls_back();
    test_hold_measured_diag_present_but_neginf_falls_back();
    test_hold_zero_row_and_zero_diag_is_solvable_with_guard();
    test_climb_diagonal_is_ff_k_dc();
    test_climb_measured_diag_flag_switches_diagonal();
    test_climb_measured_diag_flag_falls_back_when_unmeasured();

    printf("zone_coupling_solve: %d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
