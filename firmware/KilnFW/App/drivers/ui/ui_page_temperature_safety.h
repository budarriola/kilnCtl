// UI_PLAN.md section 6.3: the LCD Temperature page also shows the safety
// processor's own relay (K4) state. Pure text-formatting seam, factored out
// of ui_page_temperature.c so it is host-testable (no LVGL) and cannot drift
// from main_page.html's renderSafetyCard() wording without both sides'
// tests noticing -- same discipline ui_page_home_graph.h documents.
#ifndef UI_PAGE_TEMPERATURE_SAFETY_H
#define UI_PAGE_TEMPERATURE_SAFETY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Renders dashboard_status_t's safety_relay_known/safety_relay_energized
 * pair (dashboard_http.h) as the LCD's own short label, following
 * main_page.html's renderSafetyCard() wording and this page's 0-based
 * relay-display convention: "Safety (K4): ON" / "Safety (K4): off" /
 * "Safety (K4): n/a".
 *
 * known == false (the link has never actually answered this poll -- see
 * dashboard_http.h's safety_relay_known doc comment) renders "n/a" here;
 * the web pill (main_page.html's renderSafetyCard()) renders the SAME
 * condition as "unknown" instead -- same data, deliberately different
 * wording per surface (UI_PLAN.md 6.3), not a drift to fix. Either way,
 * unknown and confirmed-de-energized are different states, and collapsing
 * them into "off" would silently misreport a dead safety link as a
 * de-energized relay. Deliberately does NOT take safety_heating_enabled --
 * see this file's .c header comment / ui_page_temperature.c's own comment on
 * why that flag means ARMED, not "relay energized".
 *
 * out_cap must be at least 20 to hold the longest case, "Safety (K4): off",
 * plus NUL (16 chars + NUL = 17, rounded up with margin) -- a smaller buffer
 * truncates safely (snprintf-backed) rather than overflowing. */
void ui_page_temperature_safety_text(bool known, bool energized, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_TEMPERATURE_SAFETY_H
