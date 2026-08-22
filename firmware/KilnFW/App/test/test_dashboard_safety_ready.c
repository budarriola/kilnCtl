// Host tests for App/drivers/dashboard_http.h's dashboard_safety_ready() --
// the pure predicate factored out of dashboard_get_status() and
// dashboard_http_get_hw_ready() (dashboard_http.c) after an owner-reported
// bench bug: "Safty link says linked even though the uart between them is
// disconnected." With the UART physically unplugged, the ESP web UI and the
// LCD both kept reporting the safety link as up.
//
// Root cause: both call sites computed "safety_ready" as
// `s_dash.safety != NULL` -- true from the moment the SafetyLinkClass driver
// object is constructed and forever after, regardless of whether the Pico
// has ever answered or has since gone silent. safety_link_get_status()'s
// link_up field is the real answer (safety_link.c's safety_link_up_locked(),
// gated at SAFETY_LINK_STALE_MS = 1500 ms of silence -- see
// test_safety_link.c for that half of the contract), but nothing read it for
// this bit.
//
// dashboard_safety_ready(have_safety_link, status_err, link_up) is the fixed
// logic, pulled out to a pure function so this "must equal link_up, never
// merely non-NULL" contract is host-testable without standing up the whole
// httpd/kiln_io/MAX31856/WiFi machinery dashboard_http.c otherwise needs.
#include "test_common.h"
#include "../drivers/dashboard_http.h"

static void test_no_driver_is_never_ready(void)
{
    TEST_SECTION("dashboard_safety_ready -- no driver object at all reads not-ready");

    TEST_CHECK(dashboard_safety_ready(false, ESP_OK, true) == false,
               "have_safety_link=false must read not-ready regardless of link_up "
               "(mirrors the pre-safety_link_start() / never-initialized case)");
}

static void test_driver_exists_but_link_down_reads_not_ready(void)
{
    TEST_SECTION("dashboard_safety_ready -- THE BUG: driver constructed, UART unplugged");

    // This is the exact bench scenario the owner reported: the SafetyLinkClass
    // object exists (safety_link_start() ran at boot), a status fetch still
    // succeeds (it only fails on a NULL pointer / uninitialized state -- see
    // safety_link_get_status()'s own contract), but link_up is false because
    // no frame has arrived within SAFETY_LINK_STALE_MS (safety_link_up_locked()
    // already gates this correctly on its own). The old code never looked at
    // link_up here at all -- it would have reported ready=true for this exact
    // input. This is the check that catches that regression coming back.
    TEST_CHECK(dashboard_safety_ready(true, ESP_OK, false) == false,
               "driver present + link_up=false (UART unplugged/stale) MUST read not-ready -- "
               "this is the owner-reported false positive");
}

static void test_driver_exists_and_link_up_reads_ready(void)
{
    TEST_SECTION("dashboard_safety_ready -- driver constructed, link genuinely alive");

    TEST_CHECK(dashboard_safety_ready(true, ESP_OK, true) == true,
               "driver present + link_up=true reads ready -- the only true-positive case");
}

static void test_status_fetch_error_reads_not_ready(void)
{
    TEST_SECTION("dashboard_safety_ready -- a failed status fetch is never ready, even if "
                  "the caller (wrongly) still had link_up=true lying around");

    // safety_link_get_status() itself fails only when the driver was never
    // initialized (ESP_ERR_INVALID_STATE) or the lock couldn't be taken
    // (ESP_FAIL) -- in either case there is no fresh `sl` to trust, so this
    // must not read ready no matter what stale/leftover link_up value a
    // caller happened to pass through.
    TEST_CHECK(dashboard_safety_ready(true, ESP_ERR_INVALID_STATE, true) == false,
               "status fetch error (ESP_ERR_INVALID_STATE) reads not-ready even with link_up=true");
    TEST_CHECK(dashboard_safety_ready(true, ESP_FAIL, true) == false,
               "status fetch error (ESP_FAIL) reads not-ready even with link_up=true");
}

void run_test_dashboard_safety_ready(void)
{
    test_no_driver_is_never_ready();
    test_driver_exists_but_link_down_reads_not_ready();
    test_driver_exists_and_link_up_reads_ready();
    test_status_fetch_error_reads_not_ready();
}
