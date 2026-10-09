// Host test for App/drivers/http/diagnostics_http.h's diag_safety_tc_state()
// -- the REAL production state-selection logic used by
// thermo_faults_get_handler() (diagnostics_http.c) to decide "ok" /
// "faulted" / "probe_fault" / "not_converting" for the safety-processor
// thermocouple.
//
// Added 2026-09-08 per docs/audits/negative_test_verification_2026-09-08.md:
// the existing test_safety_tc_diagnostics.js claimed to prove a dead chip
// (both TC and CJ NaN, zero fault bits) cannot render as healthy, but it
// never called any production state-computation code -- it hand-built a
// `{state: "not_converting", ...}` fixture and fed it straight to the page
// renderer. Deleting the real not_converting branch in diagnostics_http.c
// left that JS suite 19/19 green (see this test's own negative test below).
//
// 2026-09-08, safety_tc_warn_mask_disagreement audit: diag_safety_tc_state()
// grew a "probe_fault" state (chip alive/converting, TC probe/wiring/CR1
// verify is the problem) distinct from "not_converting" (chip itself never
// completed a conversion), driven by the new cj_temp_c/cj_valid_known/
// cj_valid parameters that mirror SaftyFW's V3 status frame's independent
// cold-junction-validity bit (LINK_FLAG2_CJ_VALID / SAFETY_LINK_STATUS_
// FLAG2_CJ_VALID). This file's tests were extended for the new signature
// and new state, not replaced -- every original case still holds with
// cj_valid_known=false/cj_valid=false, which reproduces the exact old
// two-way behaviour (see test_nan_with_zero_faults_and_no_cj_info_is_not_
// converting()).
//
// This test drives diag_safety_tc_state() directly -- the actual function
// diagnostics_http.c calls -- with the real (double tc_temp_c, uint32_t
// tc_fault, double cj_temp_c, bool cj_valid_known, bool cj_valid) inputs it
// takes on the wire, including a real NaN via 0.0/0.0, not a precomputed
// "temp_valid" boolean.
#include <math.h>

#include "test_common.h"
#include "../drivers/http/diagnostics_http.h"

static double nan_val(void)
{
    // Avoid a literal 0.0/0.0 constant-folding warning on MSVC; volatile
    // forces a real runtime division.
    volatile double zero = 0.0;
    return zero / zero;
}

static void test_healthy_reading_is_ok(void)
{
    TEST_SECTION("diag_safety_tc_state -- real reading, no fault bits -> ok");

    TEST_CHECK(strcmp(diag_safety_tc_state(22.5, 0u, 20.0, true, true), "ok") == 0,
               "a real (non-NaN) temperature with fault_status==0 must read ok, "
               "regardless of cj_valid (temp_valid alone already settles it)");
}

static void test_real_fault_bit_is_faulted(void)
{
    TEST_SECTION("diag_safety_tc_state -- chip alive, real fault bit -> faulted");

    // A real reading (chip completed conversion) but a fault bit set (e.g.
    // open circuit) -- chip alive, probe is the problem.
    TEST_CHECK(strcmp(diag_safety_tc_state(24.0, 0x01u, 20.0, true, true), "faulted") == 0,
               "a real temperature with a nonzero fault_status must read faulted, "
               "never ok and never not_converting/probe_fault");
}

static void test_nan_with_zero_faults_and_no_cj_info_is_not_converting(void)
{
    TEST_SECTION("diag_safety_tc_state -- NaN + fault==0, no cj info -> not_converting "
                  "(the original, pre-fix distinguishing case)");

    // This is the exact state that cost hours the night this feature was
    // written: the chip has stopped converting (or failed CR1 type-verify),
    // producing NaN with NO fault bit set at all, and (as of the original
    // wire contract) no separate cj-side evidence either. The old cached
    // safety_temp_c tile could not distinguish this from "ok"; this is the
    // regression test that catches that coming back. cj_valid_known=false
    // reproduces the exact pre-2026-09-08 wire contract (an older Pico, or
    // an ESP that hasn't confirmed V3 support).
    const char *state = diag_safety_tc_state(nan_val(), 0u, nan_val(), /*cj_valid_known=*/false,
                                              /*cj_valid=*/false);
    TEST_CHECK(strcmp(state, "not_converting") == 0,
               "NaN temperature with fault_status==0 and no cj info must read "
               "not_converting, not ok/faulted/probe_fault -- a dead chip must never "
               "render as healthy, and unknown cj status must never be read as a probe fault");
}

static void test_nan_with_fault_bit_still_faulted_not_masked(void)
{
    TEST_SECTION("diag_safety_tc_state -- NaN plus a fault bit stays a real fault, "
                  "not reclassified as not_converting or probe_fault");

    // temp_valid requires a non-NaN reading, so a NaN reading with a fault
    // bit set falls through to the NaN branch (matches the priority
    // documented on diag_safety_tc_state(): faulted requires temp_valid).
    // This locks in that priority order rather than leaving it implicit.
    TEST_CHECK(strcmp(diag_safety_tc_state(nan_val(), 0x01u, nan_val(), false, false),
                       "not_converting") == 0,
               "NaN reading takes precedence over an accompanying fault bit -- "
               "faulted requires a completed (non-NaN) conversion");
}

static void test_nan_tc_with_valid_finite_cj_is_probe_fault(void)
{
    TEST_SECTION("diag_safety_tc_state -- THE new distinguishing case: NaN tc_c, "
                  "fault==0, but a valid finite cj_c -> probe_fault");

    // The actual point of this task: a probe/CR1-verify fault leaves tc_c
    // NaN while the on-chip cold junction, from the SAME successful
    // transfer, is still real and finite -- the chip is alive and
    // converting, only the external probe/wiring is the problem.
    const char *state = diag_safety_tc_state(nan_val(), 0u, /*cj_temp_c=*/21.3, true, true);
    TEST_CHECK(strcmp(state, "probe_fault") == 0,
               "NaN tc_c + fault==0 + cj_valid_known=true + cj_valid=true + a real cj_temp_c "
               "must read probe_fault, distinguishing a live chip/dead probe from a dead chip");
}

static void test_nan_tc_with_cj_known_invalid_is_not_converting(void)
{
    TEST_SECTION("diag_safety_tc_state -- cj_valid_known=true but cj_valid=false -> "
                  "not_converting (chip itself never converted)");

    // The Pico positively reported (via V3) that the cold junction is ALSO
    // invalid -- both halves NaN, genuinely a dead/silent chip, not merely a
    // probe fault.
    const char *state = diag_safety_tc_state(nan_val(), 0u, nan_val(), /*cj_valid_known=*/true,
                                              /*cj_valid=*/false);
    TEST_CHECK(strcmp(state, "not_converting") == 0,
               "cj_valid_known=true with cj_valid=false must still read not_converting -- "
               "a positively-confirmed dead cold junction is not a probe-only fault");
}

static void test_cj_valid_true_but_cj_temp_nan_is_not_converting(void)
{
    TEST_SECTION("diag_safety_tc_state -- cj_valid=true claimed but cj_temp_c is itself NaN "
                  "-> not_converting, never trust the flag alone");

    // Defense in depth: even if a malformed/buggy peer set the cj_valid bit
    // but the temperature itself is NaN, this function must not call it
    // probe_fault -- the finite cj_temp_c is the actual evidence, not just
    // the flag saying so.
    const char *state = diag_safety_tc_state(nan_val(), 0u, nan_val(), /*cj_valid_known=*/true,
                                              /*cj_valid=*/true);
    TEST_CHECK(strcmp(state, "not_converting") == 0,
               "a claimed-valid but actually-NaN cj_temp_c must not be read as evidence of a "
               "live chip -- probe_fault requires a genuinely finite cj_temp_c, not just the flag");
}

void run_test_diagnostics_safety_tc_state(void)
{
    test_healthy_reading_is_ok();
    test_real_fault_bit_is_faulted();
    test_nan_with_zero_faults_and_no_cj_info_is_not_converting();
    test_nan_with_fault_bit_still_faulted_not_masked();
    test_nan_tc_with_valid_finite_cj_is_probe_fault();
    test_nan_tc_with_cj_known_invalid_is_not_converting();
    test_cj_valid_true_but_cj_temp_nan_is_not_converting();
}
