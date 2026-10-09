// test_dashboard_protocol_version.c -- proves dashboard_http.c's
// GET /api/status field `self_protocol_version` reports this firmware's
// ESP<->Pico kilnlink version (KILNLINK_PROTOCOL_VERSION, CommonFW's
// kilnlink_version.h, currently 13), not the PC<->ESP benchproto UART
// version (UART_PROTOCOL_VERSION, uart_task_ids.h, currently 11).
//
// dashboard_http.h documents the field as paired with `peer_protocol_version`
// (whatever the Pico's last FW_VERSION frame announced over the safety
// link) so a GUI/tool can say "ESP N / Pico M, mismatch" -- every real
// consumer (safety_page.html, ui_page_diagnostics.c, PcTools's
// capability_preflight.py) compares self_protocol_version against
// peer_protocol_version on that basis. Before this fix, dashboard_http.c
// assigned UART_PROTOCOL_VERSION (11) into a field documented and consumed
// as the kilnlink version (12) -- comparing to a Pico honestly reporting
// KILNLINK_PROTOCOL_VERSION 12 always read as a version mismatch.
//
// dashboard_http.c is not host-compilable (pulls in lvgl_port.h's LCD/touch
// driver stack, ILI9488.h's __attribute__((format(...))) does not compile
// on MSVC -- see dashboard_json.h's own doc comment, and this codebase's
// test_display_power_wiring.c precedent for the same constraint). This test
// therefore source-text-scans the real .c file rather than compiling it,
// same convention as that file and SaftyFW's test_safety_core_s8_wiring.c:
// fails CLOSED if the file or the expected assignment cannot be located at
// all, rather than silently reporting nothing wrong.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

static char *read_file_any(const char *const *candidates, size_t count)
{
    return test_read_source_anchored(__FILE__, candidates[0], candidates, count);
}

static const char *DASHBOARD_HTTP_C_CANDIDATES[] = {
    "../drivers/http/dashboard_http.c",
    "App/drivers/http/dashboard_http.c",
    "firmware/KilnFW/App/drivers/http/dashboard_http.c",
};

static void run_section1_self_protocol_version_is_kilnlink(void)
{
    TEST_SECTION("dashboard_http.c assigns self_protocol_version from "
                 "KILNLINK_PROTOCOL_VERSION, never UART_PROTOCOL_VERSION");

    char *text = read_file_any(DASHBOARD_HTTP_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/http/dashboard_http.c from the host "
                           "test's working directory -- update the candidate paths in this "
                           "test if the build layout moved");
        return;
    }

    /* Every assignment site into the field must use KILNLINK_PROTOCOL_VERSION. */
    const char *good = "out->self_protocol_version = (uint16_t)KILNLINK_PROTOCOL_VERSION;";
    int good_count = 0;
    const char *p = text;
    while ((p = strstr(p, good)) != NULL) {
        good_count++;
        p += strlen(good);
    }
    TEST_CHECK(good_count >= 2,
               "expected at least 2 occurrences of "
               "'out->self_protocol_version = (uint16_t)KILNLINK_PROTOCOL_VERSION;' "
               "(the safety-link-up and safety-link-down assignment sites) -- got fewer, "
               "check dashboard_http.c hasn't regressed to the UART link version or lost "
               "an assignment site entirely");

    /* No assignment site may use the wrong (UART/benchproto) version. */
    const char *bad = "out->self_protocol_version = (uint16_t)UART_PROTOCOL_VERSION;";
    TEST_CHECK(strstr(text, bad) == NULL,
               "found 'out->self_protocol_version = (uint16_t)UART_PROTOCOL_VERSION;' -- "
               "this field is documented (dashboard_http.h) and consumed (safety_page.html, "
               "ui_page_diagnostics.c, PcTools capability_preflight.py) as this firmware's "
               "ESP<->Pico KILNLINK_PROTOCOL_VERSION, paired against peer_protocol_version "
               "from the Pico's own kilnlink announcement -- reporting the PC<->ESP "
               "UART_PROTOCOL_VERSION here makes every such comparison a false mismatch");

    /* The file must actually include kilnlink_version.h to have the symbol
     * at all, not merely have it transitively/accidentally visible. */
    TEST_CHECK(strstr(text, "kilnlink/kilnlink_version.h") != NULL,
               "dashboard_http.c no longer #includes \"kilnlink/kilnlink_version.h\" -- "
               "KILNLINK_PROTOCOL_VERSION would not resolve");

    free(text);
}

void run_test_dashboard_protocol_version(void)
{
    run_section1_self_protocol_version_is_kilnlink();
}
