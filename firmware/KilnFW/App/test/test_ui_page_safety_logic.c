// Host tests for ui_page_safety_logic.c -- the decision logic behind the LCD
// Safety / Alarm page's trip indication and Clear Trip button.
//
// Two properties matter and each has a test that fails when it is broken:
//   1. AUTH GATE: Clear is only ever SEND for an admin session, and a fresh
//      "is a trip live" check is part of the verdict (no clearing a non-trip).
//   2. CLEAR-ELSEWHERE: the indication is a pure function of live DIAG. A trip
//      cleared by the web / MCP / UART (a later DIAG frame says ARMED) makes
//      the alarm go away with no LCD action, and a stale link never paints a
//      "trip" nor a "clear".
#include "test_common.h"
#include "../drivers/ui/ui_page_safety_logic.h"
#include "../drivers/safety/safety_link.h" // SAFETY_LINK_DIAG_STATE_*, SAFETY_LINK_STALE_MS

#include <string.h>

void run_test_ui_page_safety_logic(void)
{
    TEST_SECTION("ui_safety_view_derive: live trip needs fresh DIAG that says TRIPPED");
    {
        ui_safety_view_t v = ui_safety_view_derive(true, SAFETY_LINK_DIAG_STATE_TRIPPED, 100);
        TEST_CHECK(v.tripped_live && !v.diag_unknown, "fresh TRIPPED is a live trip");

        v = ui_safety_view_derive(true, SAFETY_LINK_DIAG_STATE_ARMED, 100);
        TEST_CHECK(!v.tripped_live && !v.diag_unknown, "fresh ARMED is not a trip and is known");

        v = ui_safety_view_derive(true, SAFETY_LINK_DIAG_STATE_TRIPPED, SAFETY_LINK_STALE_MS);
        TEST_CHECK(!v.tripped_live && v.diag_unknown, "stale TRIPPED is a silent link, not a live trip");

        v = ui_safety_view_derive(true, SAFETY_LINK_DIAG_STATE_TRIPPED, SAFETY_LINK_STALE_MS - 1);
        TEST_CHECK(v.tripped_live, "one ms inside the staleness bound is still live");

        v = ui_safety_view_derive(false, SAFETY_LINK_DIAG_STATE_TRIPPED, 0);
        TEST_CHECK(!v.tripped_live && v.diag_unknown, "never-received DIAG is unknown even if state byte reads TRIPPED");
    }

    TEST_SECTION("ui_safety_clear_verdict: auth gate");
    {
        ui_safety_view_t live = { .tripped_live = true, .diag_unknown = false };
        TEST_CHECK(ui_safety_clear_verdict(&live, false) == UI_SAFETY_CLEAR_NEEDS_ADMIN,
                   "live trip + non-admin session is refused, never sent");
        TEST_CHECK(ui_safety_clear_verdict(&live, true) == UI_SAFETY_CLEAR_SEND,
                   "live trip + admin session may send");
        TEST_CHECK(ui_safety_clear_verdict(NULL, true) == UI_SAFETY_CLEAR_NOT_TRIPPED,
                   "NULL view fails closed");

        ui_safety_view_t none = { .tripped_live = false, .diag_unknown = false };
        TEST_CHECK(ui_safety_clear_verdict(&none, true) == UI_SAFETY_CLEAR_NOT_TRIPPED,
                   "admin cannot clear a trip that is not live");
        TEST_CHECK(ui_safety_clear_verdict(&none, false) == UI_SAFETY_CLEAR_NOT_TRIPPED,
                   "non-admin, no trip -> not tripped (no login prompt reason)");

        ui_safety_view_t unk = { .tripped_live = false, .diag_unknown = true };
        TEST_CHECK(ui_safety_clear_verdict(&unk, true) == UI_SAFETY_CLEAR_NOT_TRIPPED,
                   "unknown (stale) link is never cleared blind");
    }

    TEST_SECTION("clear elsewhere: the LCD indication follows live state, no page latch");
    {
        // Poll sequence as the page's refresh timer sees it. The trip is
        // cleared by something other than the LCD between samples 2 and 3.
        // Nothing here ever tells the module "the LCD cleared it".
        struct { bool ever; uint8_t state; uint32_t age; bool expect_alarm; const char *what; } seq[] = {
            { true, SAFETY_LINK_DIAG_STATE_ARMED,   50, false, "armed: no alarm" },
            { true, SAFETY_LINK_DIAG_STATE_TRIPPED, 50, true,  "trip latches: alarm" },
            { true, SAFETY_LINK_DIAG_STATE_TRIPPED, 60, true,  "still tripped: alarm persists" },
            { true, SAFETY_LINK_DIAG_STATE_ARMED,   40, false, "cleared via web/MCP/UART: alarm gone" },
            { true, SAFETY_LINK_DIAG_STATE_TRIPPED, 40, true,  "tripped again: alarm returns" },
            { true, SAFETY_LINK_DIAG_STATE_WARN,    40, false, "state left TRIPPED: alarm gone" },
        };
        for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
            ui_safety_view_t v = ui_safety_view_derive(seq[i].ever, seq[i].state, seq[i].age);
            TEST_CHECK(v.tripped_live == seq[i].expect_alarm, seq[i].what);
        }

        // A Clear tap armed while tripped, with the trip cleared elsewhere
        // before the PIN was entered: the verdict re-derived at action time
        // must refuse, not send a clear for a trip that no longer exists.
        ui_safety_view_t at_tap = ui_safety_view_derive(true, SAFETY_LINK_DIAG_STATE_TRIPPED, 10);
        TEST_CHECK(ui_safety_clear_verdict(&at_tap, true) == UI_SAFETY_CLEAR_SEND, "tap time: send allowed");
        ui_safety_view_t at_action = ui_safety_view_derive(true, SAFETY_LINK_DIAG_STATE_ARMED, 10);
        TEST_CHECK(ui_safety_clear_verdict(&at_action, true) == UI_SAFETY_CLEAR_NOT_TRIPPED,
                   "cleared while the PIN prompt was open: action-time verdict refuses");
    }

    TEST_SECTION("ui_safety_format_age");
    {
        char b[24];
        ui_safety_format_age(12000, b, sizeof(b));
        TEST_CHECK(strcmp(b, "12s ago") == 0, "seconds");
        ui_safety_format_age(185000, b, sizeof(b));
        TEST_CHECK(strcmp(b, "3m05s ago") == 0, "minutes");
        ui_safety_format_age(7620000, b, sizeof(b));
        TEST_CHECK(strcmp(b, "2h07m ago") == 0, "hours");
    }
}
