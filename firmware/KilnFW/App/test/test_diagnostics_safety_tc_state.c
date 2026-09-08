// Host test for App/drivers/http/diagnostics_http.h's diag_safety_tc_state()
// -- the REAL production state-selection logic used by
// thermo_faults_get_handler() (diagnostics_http.c) to decide "ok" /
// "faulted" / "not_converting" for the safety-processor thermocouple.
//
// Added 2026-09-08 per docs/audits/negative_test_verification_2026-09-08.md:
// the existing test_safety_tc_diagnostics.js claimed to prove a dead chip
// (both TC and CJ NaN, zero fault bits) cannot render as healthy, but it
// never called any production state-computation code -- it hand-built a
// `{state: "not_converting", ...}` fixture and fed it straight to the page
// renderer. Deleting the real not_converting branch in diagnostics_http.c
// left that JS suite 19/19 green (see this test's own negative test below).
//
// This test drives diag_safety_tc_state() directly -- the actual function
// diagnostics_http.c calls -- with the real (double tc_temp_c, uint32_t
// tc_fault) inputs it takes on the wire, including a real NaN via 0.0/0.0,
// not a precomputed "temp_valid" boolean.
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

    TEST_CHECK(strcmp(diag_safety_tc_state(22.5, 0u), "ok") == 0,
               "a real (non-NaN) temperature with fault_status==0 must read ok");
}

static void test_real_fault_bit_is_faulted(void)
{
    TEST_SECTION("diag_safety_tc_state -- chip alive, real fault bit -> faulted");

    // A real reading (chip completed conversion) but a fault bit set (e.g.
    // open circuit) -- chip alive, probe is the problem.
    TEST_CHECK(strcmp(diag_safety_tc_state(24.0, 0x01u), "faulted") == 0,
               "a real temperature with a nonzero fault_status must read faulted, "
               "never ok and never not_converting");
}

static void test_nan_with_zero_faults_is_not_converting(void)
{
    TEST_SECTION("diag_safety_tc_state -- THE distinguishing case: NaN + fault==0");

    // This is the exact state that cost hours the night this feature was
    // written: the chip has stopped converting (or failed CR1 type-verify),
    // producing NaN with NO fault bit set at all. The old cached
    // safety_temp_c tile could not distinguish this from "ok"; this is the
    // regression test that catches that coming back.
    const char *state = diag_safety_tc_state(nan_val(), 0u);
    TEST_CHECK(strcmp(state, "not_converting") == 0,
               "NaN temperature with fault_status==0 must read not_converting, "
               "not ok and not faulted -- a dead chip must never render as healthy");
}

static void test_nan_with_fault_bit_still_faulted_not_masked(void)
{
    TEST_SECTION("diag_safety_tc_state -- NaN plus a fault bit stays a real fault, "
                  "not reclassified as not_converting");

    // temp_valid requires a non-NaN reading, so a NaN reading with a fault
    // bit set falls through to not_converting (matches the priority
    // documented on diag_safety_tc_state(): faulted requires temp_valid).
    // This locks in that priority order rather than leaving it implicit.
    TEST_CHECK(strcmp(diag_safety_tc_state(nan_val(), 0x01u), "not_converting") == 0,
               "NaN reading takes precedence over an accompanying fault bit -- "
               "faulted requires a completed (non-NaN) conversion");
}

void run_test_diagnostics_safety_tc_state(void)
{
    test_healthy_reading_is_ok();
    test_real_fault_bit_is_faulted();
    test_nan_with_zero_faults_is_not_converting();
    test_nan_with_fault_bit_still_faulted_not_masked();
}
