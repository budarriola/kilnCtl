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
 * page's status line at all -- config_count is whatever
 * kiln_cfg_store_list() returned (a plain count, 0..KILN_CFG_MAX_COUNT).
 * With 0 or 1 configs saved, the suffix is entirely redundant (there is
 * either nothing to name or exactly one board-wide default), so it is
 * dropped rather than shown as "(none)" or the lone name. */
bool ui_page_home_kiln_suffix_visible(uint8_t config_count);

/* Appends the "  Kiln: <name>" (or "  Kiln: (none)" if config_count >= 2
 * but nothing is currently active) suffix onto `status_buf` in place, IFF
 * ui_page_home_kiln_suffix_visible(config_count) is true. `status_buf` must
 * already hold the base status text and be NUL-terminated; `status_buf_cap`
 * is its full buffer capacity. `has_active_name` is true when the caller
 * successfully resolved the active id to a name (kiln_cfg_store_get_name()
 * succeeded); `active_name` is only read when `has_active_name` is true.
 * When config_count < 2, `status_buf` is left completely unmodified --
 * this is the only function in this file that mutates a caller buffer,
 * matching ui_page_home_graph.c's precedent of keeping all other helpers
 * pure computations with no side effects. */
void ui_page_home_append_kiln_suffix(char *status_buf, size_t status_buf_cap, uint8_t config_count,
                                      bool has_active_name, const char *active_name);

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

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_HOME_RAIL_H
