#ifndef UI_PAGE_HOME_RAIL_H
#define UI_PAGE_HOME_RAIL_H

/* Pure formatting/decision helpers for ui_page_home.c's right-quarter
 * dashboard rail (docs/UI_PLAN.md section 6.5: relays, zone temperatures,
 * zone power). Factored out host-testable, no LVGL/ESP-IDF -- same pattern
 * as ui_page_home_graph.h/.c.
 *
 * Two owner decisions from UI_PLAN.md section 6.8 are encoded here:
 *   1. The rail's power readout is per-zone duty % plus ONE kiln-total
 *      watts line, shown only when the snapshot's power reading is valid
 *      (zero height when hidden, never a fake "0 W").
 *   6. The dashboard's "Kiln: <name>" status-line suffix is shown only
 *      when the board holds two or more saved kiln configurations (owner
 *      rule 2026-09-19). With zero or one config the suffix is omitted
 *      entirely -- not "(none)", not the single name.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True only when the "  Kiln: ..." suffix should be appended to the home
 * page's status line at all -- config_count is what kiln_cfg_store_count()
 * returned (a plain count, 0..KILN_CFG_MAX_COUNT).
 * With 0 or 1 configs saved, the suffix is entirely redundant (there is
 * either nothing to name or exactly one board-wide default), so it is
 * dropped rather than shown as "(none)" or the lone name.
 *
 * The caller formats and appends the suffix text itself rather than
 * calling a helper for it: ui_home_refresh_cb() is the LVGL task's
 * deepest known dispatch target, and one extra call frame on that path
 * measured +80 B in check_all_task_stack_budgets.ps1 against a 4880 B
 * ceiling with no headroom left -- so only the DECISION lives here, and
 * it is `static inline` (not an out-of-line call into ui_page_home_rail.c)
 * for the same reason: this build has no LTO, so a cross-TU call to a
 * three-token predicate would cost ui_home_refresh_cb() another frame's
 * worth of spill on that same ceiling. The host test includes this header,
 * so the threshold is still covered by exactly one tested definition. */
static inline bool ui_page_home_kiln_suffix_visible(uint8_t config_count)
{
    return config_count >= 2;
}

/* Relay pill state (spare-relay WP-6). A pill is lit only when the relay
 * board read is good AND the relay is on -- a failed/absent read never lights
 * a pill from a stale shadow. */
bool ui_page_home_rail_pill_on(bool io_ok, bool relay_on);

/* The short caption an AUX-bound relay's pill carries ("A1".."A4", relay
 * 1-based like the profile editor's relay labels), or "" for a relay that is
 * not an enabled aux output (zone relays stay colour-only, as before).
 * aux_enabled_mask is dashboard_status_t.aux_enabled_mask (bit = relay-1);
 * relay_idx is 0-based. Never NULL; the pointer is static storage. The LCD
 * shows STATE only -- no control is attached to a pill. */
/* Number of relays the caption table covers; ui_page_home_refresh.c
 * _Static_asserts it against KILN_IO_RELAY_COUNT. */
#define UI_PAGE_HOME_RAIL_AUX_CAPTION_COUNT 4u

const char *ui_page_home_rail_aux_caption(uint8_t aux_enabled_mask, uint32_t relay_idx);

/* Clamps a duty fraction (0.0..1.0, but may arrive out of range from a
 * stale/degenerate snapshot) to an integer percent in [0, 100]. NaN maps to
 * 0 (never a garbage percent on screen). */
int ui_page_home_rail_duty_pct(float duty_fraction);

/* Formats a zone's temperature for the rail's compact per-zone block.
 * Mirrors main_page.html's style: an invalid reading renders as "--.-"
 * (never a fabricated "0.0"), and a stale-but-valid reading still renders
 * its real last-known number (staleness is surfaced by MEMORY.md's
 * "idealized test input" caution elsewhere on this page via colour, not by
 * blanking a real number here -- this helper only owns the digits). */
void ui_page_home_rail_format_zone_temp(bool valid, float temp_c, char *out, size_t out_cap);

/* Formats the rail's single kiln-total watts line into `out`, or writes an
 * empty string (out[0] = '\0') when `power_valid` is false -- the caller
 * must give the label zero height in that case, never show a fake "0 W". */
void ui_page_home_rail_format_kiln_watts(bool power_valid, float power_w, char *out, size_t out_cap);

/* True when `next` differs from the label's `current` text (or either is
 * NULL). Lets the rail write a label only on change (L30). */
bool ui_page_home_rail_text_changed(const char *current, const char *next);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_HOME_RAIL_H
