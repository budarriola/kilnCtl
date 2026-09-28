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

#include <stdbool.h>
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

/* 2026-08-30 owner request ("the LCD chart should always show the same
 * markers as the web page"): main_page.html's drawChartAxis() draws a
 * four-label tick row (tickCount=3: 0, 1/3, 2/3, full span) UNCONDITIONALLY
 * whenever it has a real [tMin, tMax] to span -- both the running chart
 * (drawHistoryChart, spanning recorded history + any planned preview) and
 * the idle-with-history case (same function, rows.length>0) get one; only
 * the idle-with-NO-history "single dot" state has no span to label (see that
 * state's own comment at ui_page_home.c's idle branch -- a static dot is a
 * single instant, not a series, and main_page.html's drawIdleDots() agrees:
 * it only calls drawChartAxis() when there's an active plan preview, which
 * this page has no equivalent data for while IDLE --
 * profile_executor_state_t's segments/run_start_c are only meaningful once
 * state != PROFILE_EXEC_IDLE, so there is nothing to preview).
 *
 * This is the shared tick-label builder for both spanned states (a live
 * plan's whole-run horizon, or idle's retained-history window) -- same four-
 * tick M:SS shape either way, since both really are "0 .. horizon_s of real,
 * already-computed span", never a fabricated one (see this file's other
 * functions' own honesty comments; horizon_s here must already be a genuine
 * span, not a div-by-zero guard value). has_span selects whether a label is
 * produced at all -- pass false (idle, no history) and the function writes
 * nothing and returns false, so the caller's existing "hide the label"
 * behaviour for that state stays a single, obvious branch instead of a
 * scattered "is this string still valid" check.
 *
 * out_cap must be at least 64: worst case four "%lu:%02lu" ticks (up to 10
 * bytes each for a uint32_t seconds count near UINT32_MAX/60) + 3 "|"
 * separators + NUL = 43 max, rounded up with margin, same discipline as this
 * file's other snprintf-into-fixed-buffer callers. */
bool ui_page_home_build_x_label(float horizon_s, bool has_span, char *out, size_t out_cap);

/* 2026-09-28 owner request: "the graph on the lcd and web should never
 * vertically span less than 5 degrees." Applied here in DISPLAY degrees --
 * lo/hi/floor_disp are already display-unit values by the time they reach
 * this function (see ui_page_home_refresh.c's call sites, which convert
 * through ui_home_freezing_point_disp()/the board's unit_pref_t before
 * calling in), so a caller displaying Fahrenheit gets a 5 F floor, not a
 * 5 C-converted-to-F one -- consistent with every other constant already
 * flowing through this same seam (UI_PAGE_HOME_AXIS_QUANT_STEP_DISP below is
 * the same convention: a "5" that means 5 of whatever unit is on screen).
 * The web side (main_page.html) makes the opposite, still-consistent-with-
 * itself choice: its own GRAPH_MIN_SPAN_C is applied in Celsius, since that
 * chart computes its whole range in Celsius internally and converts only at
 * label-draw time (kcUnit.toDisplay()) -- see that constant's own comment. */
#define UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP 5.0f

/* Padded Y-axis range from a real plotted data min/max (lo, hi) -- factored
 * out of refresh_cb()'s data-driven axis branch (the "have_range" case,
 * distinct from the idle single-dot branch's own fixed +/-10 padding, which
 * has no span to pad a PERCENTAGE of and is left inline) so this arithmetic
 * -- and its one load-bearing guard -- can be host tested without LVGL.
 *
 * Pads by 10% of (hi - lo) on both sides, same as main_page.html's own
 * padded-range convention this task's owner cited. The guard: if hi == lo
 * (a degenerate all-same-value span -- e.g. one sample recorded so far, or a
 * sensor stuck at one reading), range is floored to 1.0 BEFORE computing the
 * 10% pad. Without that floor, hi==lo produces pad_c == 0 and therefore
 * *out_axis_lo == *out_axis_hi -- lv_chart_set_axis_range() then has a
 * zero-height axis, and LVGL's own value-to-pixel mapping divides by
 * (axis_hi - axis_lo) when placing a point, i.e. a real div-by-zero one call
 * away from this function, not inside it. The floor is what keeps that call
 * safe; see test_ui_page_home_graph.c's "degenerate span" case, which is run
 * both with and without this guard to prove it is load-bearing.
 *
 * Before any of that: if the real data span (hi - lo) is under
 * UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP, lo/hi are widened symmetrically around
 * their own midpoint to exactly that span BEFORE the 10% pad and freezing
 * floor are applied -- so a flat or near-flat trace still gets the usual
 * padding/rounding on top of a real 5-degree floor, not instead of it. A
 * final integer-level check after the freezing-floor clamp (which can only
 * ever narrow axis_lo upward, never widen it) re-widens axis_hi if that
 * clamp alone pushed the final integer span back under
 * UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP, so the 5-degree floor holds
 * unconditionally on the returned integers, not just on the pre-clamp float
 * math.
 *
 * floor_disp is freezing_point_disp()'s return value (0 C or 32 F, ALREADY
 * converted to the caller's display unit) -- axis_lo is raised to it only
 * when the real data minimum (lo, pre-padding) is itself at or above the
 * floor, so a genuine sub-freezing reading still plots visibly instead of
 * being clipped off the bottom (same rule freezing_point_disp()'s own
 * comment in ui_page_home.c documents; this function just applies it). */
void ui_page_home_y_axis_range(float lo, float hi, float floor_disp, int32_t *out_axis_lo, int32_t *out_axis_hi);

/* Quantization step (display degrees, either C or F) the active-firing Y
 * axis snaps its padded bounds out to -- see
 * ui_page_home_active_y_axis_range()'s header comment below for why. */
#define UI_PAGE_HOME_AXIS_QUANT_STEP_DISP 5

/* 2026-09-01 defect fix (review of commit a50aa64, "the graph should
 * vertically autoscale to fit the plotted items"): during an active firing
 * (state_active && plan_n > 0) ui_page_home.c used to hard-pin the Y axis to
 * 0..plan_peak instead of the real accumulated lo/hi data range -- an
 * overshoot above the planned peak was clipped clean off the top of the
 * plot, and the whole lower part of the axis was dead space while the kiln
 * was still cold. That 0..plan_peak behaviour was itself a 2026-08-23 owner
 * request ("make it look like the web GUI"); this request is newer, so per
 * this repo's standing rule (newest instruction wins) it supersedes it --
 * the axis now autoscales to what is actually plotted, same as the idle/
 * fallback branch already did via ui_page_home_y_axis_range() above, which
 * this function wraps rather than duplicates.
 *
 * lo/hi here are the SAME accumulated range refresh_cb() already builds --
 * the loop that fills s_chart_planned_pts/s_chart_actual_pts folds every
 * finite planned point (across the WHOLE horizon, not just up to "now" --
 * plan_lookup() is called unconditionally for every bucket) and every
 * recorded actual point into lo/hi, so the planned curve's peak is already
 * part of the range fed in here: the axis anchors to where the firing is
 * going, not just where the trace happens to be standing right now, and an
 * actual sample above that planned peak (a real overshoot) widens the top
 * instead of being clipped by it.
 *
 * A range recomputed fresh from live data every ~1 Hz refresh would jitter
 * continuously as the trace grows by fractions of a degree tick to tick.
 * Two policies here fix that, applied on top of the padded range
 * ui_page_home_y_axis_range() already computes:
 *   1. Quantize axis_lo down and axis_hi up to the nearest
 *      UI_PAGE_HOME_AXIS_QUANT_STEP_DISP -- most single-tick drift in the
 *      raw padded bound does not cross a step boundary, so it quantizes to
 *      the exact same integer as the previous tick and the label never
 *      moves.
 *   2. Only-widen ratchet across a single run: have_held/held_lo/held_hi is
 *      the axis this function returned on the PREVIOUS tick of the SAME run
 *      (the caller resets have_held to false exactly once, at the
 *      state_active false->true transition -- see ui_page_home.c's call
 *      site). The returned bound is only ever moved outward (min of the two
 *      lows, max of the two highs), never inward, so the axis can grow to
 *      show a real overshoot but can never audibly "breathe" back in when a
 *      momentary dip in the accumulated data would otherwise narrow it.
 * Together these mean the axis changes only when the trace has genuinely
 * grown past the current view, and even then only in discrete, readable
 * steps -- not on every refresh tick. */
void ui_page_home_active_y_axis_range(float lo, float hi, float floor_disp, bool have_held,
                                       int32_t held_lo, int32_t held_hi, int32_t *out_axis_lo,
                                       int32_t *out_axis_hi);

/* 2026-09-01 owner request: "add a compact legend inside the graph, bottom
 * right corner". The chart carries exactly two possible series -- actual
 * (always attempted whenever the chart has a real span, see has_span in
 * ui_page_home.c's refresh_cb()) and planned-ahead (only meaningful, and
 * only ever drawn, when a live profile's plan curve came back non-empty --
 * the same `state_active && plan_n > 0` condition refresh_cb() already gates
 * the dashed planned series and the current-position dot on). Rather than
 * scatter that visibility decision inline at each of refresh_cb()'s legend
 * show/hide call sites, it lives here as one pure function so it is host
 * tested like this file's other seams.
 *
 * Row order is fixed: row 0 is "Actual", row 1 is "Plan" -- but unlike the
 * old count-only version of this function, the two rows are no longer
 * required to be a contiguous 0..N-1 prefix. 2026-09-01 housekeeping fix (
 * same review as ui_page_home_active_y_axis_range() above): with per-point
 * dot markers removed elsewhere on this chart, a run's very first tick plots
 * exactly one actual sample (the run_start_c anchor, before any real history
 * sample has been recorded) -- a lone point renders as nothing at all (a
 * line needs two points), yet the legend was still advertising an "Actual"
 * row with nothing visible to key. has_actual_multi is true only once at
 * least two actual points have been plotted this tick; when it is false the
 * Actual row is suppressed even though has_span may still be true (e.g. a
 * live plan curve IS being drawn), so the legend never claims a series is
 * on screen that isn't. has_span=false forces both outputs false regardless
 * of the other two, since neither series can legitimately be true without a
 * real horizon to plot it on -- this function does not trust the caller to
 * already enforce that and clamps defensively instead. */
void ui_page_home_legend_visibility(bool has_span, bool has_actual_multi, bool has_planned,
                                     bool *out_show_actual, bool *out_show_plan);

/* 2026-09-01 defect fix (review of commit 98c3278, reset-one-side class):
 * ui_page_home_active_y_axis_range()'s only-widen ratchet must be reset
 * exactly once per RUN, not once per IDLE->active state edge. Those are not
 * the same thing: profile_executor_start() only refuses RUNNING/PAUSED, so a
 * new firing started from DONE or FAULTED goes straight DONE->RUNNING or
 * FAULTED->RUNNING with no IDLE tick in between (`state_active` is `state !=
 * PROFILE_EXEC_IDLE`, so DONE and FAULTED both already count as "active") --
 * the edge never fires and the new run inherits the previous run's widened
 * axis. A stop/restart that both happen inside one ~1 Hz refresh interval has
 * the identical symptom: the sampled state is "active" on both the tick
 * before and the tick after, so there is no edge to see there either.
 *
 * profile_executor.h (owned by another task, not touched here) exposes no
 * run-id or run-start tick a caller can key off directly. What it does
 * document as a hard per-run invariant is `total_elapsed_s`: "real seconds
 * since this run started (profile_executor_run())... only freezes across a
 * PAUSE... otherwise keeps counting" -- i.e. it is set to 0 exactly once, at
 * profile_executor_run(), and is monotonically non-decreasing for the rest of
 * that run's life (flat during a pause, never lower than a prior tick's
 * value). That makes a DECREASE in total_elapsed_s, observed between two
 * ticks that are both "active", an unambiguous witness that a new run began
 * in between -- true whether the previous run ended in DONE, FAULTED, or was
 * still RUNNING/PAUSED when the operator stopped and immediately restarted
 * it. This is the best signal available from the UI layer alone; it needs no
 * change to profile_executor.c.
 *
 * active/prev_active still cover the ordinary IDLE->active start (prev_active
 * false means there is nothing to compare elapsed_s against, so that case
 * alone is sufficient and elapsed_s is not consulted). Returns false whenever
 * active is false -- there is no ratchet decision to make while idle; the
 * caller only calls this on ticks where state_active is true. */
bool ui_page_home_axis_ratchet_should_reset(bool active, bool prev_active, uint32_t elapsed_s,
                                             uint32_t prev_elapsed_s);

/* PID_EXPANSION_PLAN.md 7.4's LCD warning surface: an INFORMATIONAL (not
 * fault) notice that the shared setpoint has stopped advancing because at
 * least one zone is lagging behind it -- profile_exec_status_t's own
 * ramp_lock_held/ramp_lock_lagging_mask, which the executor already treats
 * as normal, healthy behaviour (the setpoint pauses so every zone reaches
 * the target; see profile_executor.h's header comment). Deliberately NOT
 * styled or worded like s_trip_strip's safety-trip banner in ui_page_home.c
 * -- that strip means a real fault; this one means "the plan is waiting on a
 * zone," which happens on essentially every normal ramp.
 *
 * DEBOUNCE: ramp_lock_held can flip on a single noisy tick (a momentary
 * excursion just past EXEC_RAMP_LOCK_BAND_C) without the kiln genuinely
 * being "behind" in any way an operator should be told about. This function
 * counts CONSECUTIVE held ticks (ui_page_home.c's refresh_cb() runs at
 * UI_PAGE_HOME_REFRESH_MS == 1000, so one tick == ~1s) and only reports
 * "show" once UI_PAGE_HOME_LAG_NOTICE_DEBOUNCE_TICKS have been held in a
 * row -- chosen as 5 (~5s): long enough that a single noisy sample or a
 * brief crossing right at the ramp-lock band edge cannot trip it, short
 * enough that a genuinely lagging zone is surfaced well within one
 * PROFILE_EXECUTOR_TICK_MS control cycle's neighbourhood, not held back for
 * tens of seconds. Clearing is NOT debounced -- ramp_lock_held going false
 * resets the counter to 0 and the caller hides the notice on the very next
 * tick, same "hidden = zero flicker" bias as s_trip_strip's own gating; a
 * lag that has genuinely resolved should disappear immediately, not linger.
 *
 * consecutive_ticks is the caller's own running counter (static state lives
 * in ui_page_home.c, same "counting stays with the pure function, storage
 * stays with the caller" split axis_ratchet_should_reset() above uses) --
 * this function only ever reads it to decide, the tick function below
 * updates it. */
#define UI_PAGE_HOME_LAG_NOTICE_DEBOUNCE_TICKS 5u

/* Advances the debounce counter for one refresh tick: held -> prev+1
 * (saturating so a firing that lags for hours cannot wrap), not held -> 0.
 * Pure counter arithmetic, no decision -- see
 * ui_page_home_lag_notice_should_show() for the threshold check. */
uint32_t ui_page_home_lag_notice_tick(bool ramp_lock_held, uint32_t prev_consecutive_ticks);

/* True once consecutive_ticks has reached the debounce threshold -- the
 * notice should be shown. False (including consecutive_ticks == 0, the
 * lock-clear case) means hidden. */
bool ui_page_home_lag_notice_should_show(uint32_t consecutive_ticks);

/* Commit 1e03448 added profile_exec_zone_status_t::ramp_lag_sustained -- a
 * per-zone signal that is ALREADY debounced in firmware (EXEC_SUSTAINED_LAG_S
 * == 30s continuous, profile_executor_internal.h), unlike ramp_lock_held
 * above (instantaneous, which is why ui_page_home_lag_notice_tick()'s own
 * ~5s debounce exists for it). Stacking this file's debounce on top of an
 * already-debounced field would make a real, sustained lag take ~35s to
 * reach the LCD for no safety benefit -- so this decides which signal to
 * trust and applies debounce ONLY to the one that needs it:
 *
 *   have_rich_zone_data true  -> report any_zone_sustained directly, no
 *                                 further debounce (the firmware-side one
 *                                 already did the job this file's debounce
 *                                 exists for).
 *   have_rich_zone_data false -> fall back to the existing
 *                                 ui_page_home_lag_notice_should_show()
 *                                 debounce over ramp_lock_held, for a board
 *                                 running firmware that predates commit
 *                                 1e03448 and has never heard of
 *                                 ramp_lag_sustained.
 *
 * debounced_ticks is still expected to be maintained every tick (the caller
 * keeps calling ui_page_home_lag_notice_tick() regardless of which path is
 * active) so a board that flips from rich to fallback mid-session (should
 * never happen outside a live reflash, but costs nothing to handle) does not
 * show a stale hidden state that never got the chance to accumulate. */
bool ui_page_home_lag_notice_active(bool have_rich_zone_data, bool any_zone_sustained,
                                     uint32_t debounced_ticks);

/* Extracts the set bits of a ramp_lock_lagging_mask (profile_exec_status_t's
 * uint8_t bitmask, one bit per zone index) into out_indices, lowest zone
 * index first, capped at max_zones bits and out_cap slots (whichever is
 * smaller) -- returns the count written. Pure bit-scan, factored out so the
 * "which zones does this mask name" question is host tested independently
 * of ui_page_home.c's zone-name lookup (zones_config_get_name(), which needs
 * NVS and cannot run at host-test level) -- this function only ever hands
 * back zone INDICES, never names or formatted text. */
size_t ui_page_home_lagging_zone_indices(uint8_t mask, uint8_t max_zones, uint8_t *out_indices,
                                          size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* UI_PAGE_HOME_GRAPH_H */
