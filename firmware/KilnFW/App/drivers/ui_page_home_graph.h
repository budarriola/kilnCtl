#ifndef UI_PAGE_HOME_GRAPH_H
#define UI_PAGE_HOME_GRAPH_H

/* Pure geometry/formatting helpers for ui_page_home.c's compact profile
 * chart, factored out so they are host-testable (no LVGL, no ESP-IDF --
 * watchdog_gate.c in the SaftyFW tree is this project's precedent for
 * pulling a decision out of untestable task/UI code into a testable seam).
 *
 * 2026-08-23 owner request: "the LCD profile graph should look like the web
 * GUI's profile graph" -- main_page.html's planned-profile curve draws a
 * dashed purple line with a Y axis labelled ONLY at 0 and the curve's peak,
 * and an X axis labelled at four evenly spaced elapsed-time ticks formatted
 * as total-minutes M:SS (NOT wall-clock hh:mm -- main_page.html's own
 * fmtDuration()-equivalent for that axis divides total seconds by 60 for the
 * minutes field with no hour rollover, confirmed against the screenshot's
 * "125:59" style ticks). These three concerns -- tick placement, peak
 * detection, and M:SS formatting -- are exactly the parts of "match the web
 * chart" that are pure math over already-computed data (profile_plan_point_t
 * from profile_feasibility_plan_curve()), so they live here instead of
 * inline in refresh_cb(), where they cannot be unit tested without dragging
 * in LVGL/esp_log stubs. */

#include <stddef.h>
#include <stdint.h>

#include "profile_feasibility.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Format `seconds` as M:SS -- minutes unbounded width (no hour rollover),
 * seconds always two digits, zero-padded. Matches main_page.html's planned-
 * profile x-axis tick format (screenshot: "0:00", "125:59", "251:58",
 * "377:57" -- total elapsed minutes, not hh:mm:ss). Distinct on purpose from
 * this file's own format_duration() (mm:ss under an hour, hh:mm:ss beyond),
 * which is used elsewhere on this page for a *duration* a human reads as
 * hours/minutes -- the web chart's ticks are a coordinate on an axis, and
 * the web page never rolls them over to hours.
 *
 * out_cap must be at least 12 to hold worst case "4294967:29" (a uint32_t
 * seconds count near UINT32_MAX) plus the NUL; a smaller buffer truncates
 * safely (snprintf-backed) rather than overflowing. */
void ui_page_home_format_mmss(uint32_t seconds, char *out, size_t out_cap);

/* Four evenly spaced elapsed-time ticks across [0, horizon_s]: 0, h/3, 2h/3,
 * h -- matches main_page.html's four x-axis labels (the screenshot's
 * "0:00 | 125:59 | 251:58 | 377:57" are 0, 1/3, 2/3, and the full horizon of
 * a ~378-minute profile). horizon_s < 0 is clamped to 0 (all four ticks
 * become 0.0f) rather than producing negative ticks -- refresh_cb()'s own
 * horizon_s is always guarded >= 1.0f before this is called, but this
 * function does not assume that guard was applied. */
void ui_page_home_x_ticks(float horizon_s, float out_ticks[4]);

/* Peak (maximum) setpoint, in degrees C, across a planned-curve point list --
 * feeds the Y axis's single top label ("0 at the bottom, the profile's peak
 * at the top", main_page.html's Y axis has no intermediate gridline labels).
 * Returns NAN if n == 0 (nothing to find a peak in -- caller must not label
 * an axis with a NaN peak). pts is NOT required to be sorted by value (only
 * profile_feasibility_plan_curve()'s own t-ordering is assumed elsewhere);
 * this is a plain linear scan, safe for the small PROFILE_MAX_SEGMENTS-sized
 * arrays this is always called with. */
float ui_page_home_plan_peak_c(const profile_plan_point_t *pts, size_t n);

/* Index (0-based, clamped to [0, point_count-1]) of the bucket nearest "now"
 * in a point_count-bucket series whose buckets are evenly spaced across
 * [0, horizon_s] -- i.e. the same bucket-time formula refresh_cb() already
 * uses for its actual/planned sampling loop (t_i = i * horizon_s /
 * (point_count-1)), inverted to find i given a target time. Feeds the
 * current-position dot's placement (lv_chart_get_point_pos_by_id() on the
 * actual series at this index). elapsed_s is clamped to [0, horizon_s]
 * first, so a stale/overrun elapsed reading still lands on a valid index
 * rather than being extrapolated off the plotted range. point_count < 2 or
 * horizon_s <= 0 returns 0 (the single valid index, or a safe default when
 * there is no meaningful span to place a dot along). */
size_t ui_page_home_now_bucket_index(float horizon_s, float elapsed_s, size_t point_count);

#ifdef __cplusplus
}
#endif

#endif /* UI_PAGE_HOME_GRAPH_H */
