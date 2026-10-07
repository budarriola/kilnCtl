// Pure decision logic for the LCD Safety / Alarm page (ui_page_safety.c) and
// for the home page's trip strip. No LVGL, no ESP-IDF: host-testable.
//
// THE RULE THIS FILE EXISTS TO ENFORCE ("reset one side of a pair", CLAUDE.md):
// whether a trip is shown, and whether Clear Trip is offered, is a pure
// function of the Pico's LIVE diagnostics (the same DIAG fields the web
// dashboard polls). There is deliberately NO latch, flag or timestamp in
// this module or in ui_page_safety.c that only the LCD's own button resets:
// a trip cleared from the web, MCP/HTTP, UART or the Pico itself changes the
// next DIAG frame, the next derive() call returns tripped_live == false, and
// the LCD indication disappears with no LCD action involved.
#ifndef UI_PAGE_SAFETY_LOGIC_H
#define UI_PAGE_SAFETY_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool tripped_live;  // fresh DIAG says TRIPPED right now -> alarm shown, Clear offered
    bool diag_unknown;  // never received, or stale: the trip state is UNKNOWN, not "clear"
} ui_safety_view_t;

// Same predicate the home trip strip has always used (diag_ever_received &&
// state == TRIPPED && age < SAFETY_LINK_STALE_MS), now shared so the strip and
// the Safety page cannot disagree. A STALE tripped state is a silent link, not
// a live trip: tripped_live is false and diag_unknown is true.
ui_safety_view_t ui_safety_view_derive(bool diag_ever_received, uint8_t diag_state, uint32_t diag_age_ms);

typedef enum {
    UI_SAFETY_CLEAR_SEND = 0,           // go ahead: hand the clear to the shared clear path
    UI_SAFETY_CLEAR_NOT_TRIPPED,        // nothing live to clear (cleared elsewhere, or stale link)
    UI_SAFETY_CLEAR_NEEDS_ADMIN,        // trip is live but the LCD session is not an admin
} ui_safety_clear_verdict_t;

// Decides what the Clear Trip button may do, from a FRESH derive() taken at
// the moment the action runs (not at the moment the button was drawn) and the
// LCD session's admin status. Never a bypass: SEND only means "call the same
// clear path the web uses"; that path still applies its own refusals.
ui_safety_clear_verdict_t ui_safety_clear_verdict(const ui_safety_view_t *view, bool has_admin);

// "12s ago" / "3m05s ago" / "2h07m ago".
void ui_safety_format_age(uint32_t age_ms, char *buf, unsigned long buf_len);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_SAFETY_LOGIC_H
