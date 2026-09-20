// TODO.md 10.3's real main/status page -- replaces the pre-10.2 placeholder
// (see git history / TODO.md 10.1's status update for what used to live
// here). Per TODO.md 10.1a, every number on this page comes from the SAME
// plain-C getters dashboard_http.c's HTTP handlers call
// (dashboard_get_status(), profile_executor_get_status()) and every button
// calls the SAME action functions the web dashboard's POST handlers call
// (profile_executor_run()/_halt()) -- nothing here reimplements a read or
// write against kiln_io/MAX31856/profile_executor a second time.
//
// 2026-08-18, NO-SCROLL REWRITE (hard requirement: LCD pages must never
// require scrolling -- ILI9488.h's DISPLAY_WIDTH/HEIGHT plus Kconfig's
// default startup rotation (1) put this codebase's actual runtime canvas at
// 480x320 landscape, matching ui_theme.h's own "budgeted against the
// 480x320 landscape panel" comment; the physical panel is natively
// 320x480, but nothing on this page renders in that orientation with the
// shipped default). Everything that previously lived in one long
// scrollable flex column has been re-budgeted against a hard content height
// of roughly 320 - (2*UI_THEME_PADDING_PX outer pad) - UI_THEME_STATUS_BAR_HEIGHT_PX
// - (bar-to-content gap) =~ 264px, computed against real UI_THEME_PADDING_PX /
// UI_THEME_STATUS_BAR_HEIGHT_PX constants and LV_FONT_DEFAULT's real
// montserrat_14 line height (~20px with LVGL's default line spacing), not
// guessed. No real hardware was available to visually confirm the result --
// treat the numbers below as "computed to leave a real double-digit-pixel
// margin against a real budget," not as pixel-verified.
//
// What stayed on THIS page (the file's own prior framing: "kiln-process data
// the operator watches while firing"):
//   - Zone rows: name, temperature, heater on/off -- the core "glance while
//     firing" case.
//   - A single-line run-state summary (profile name + state) plus a
//     time/progress line and a slim progress bar -- trimmed from the
//     previous 4-widget profile card to fit the budget.
//   - A single merged Start/Stop action button, colour/label following run
//     state (2026-08-20: was Start+Stop+Menu; Menu moved to a gear icon in
//     the status bar and Start/Stop merged into one button -- see
//     ui_home_fire_btn_cb() and the status-bar build in ui_page_home_build()).
//
// What MOVED OFF this page, each reachable via kiln_ui_show() same as any
// other secondary page (TODO.md 10.1's page-manager pattern), because they
// could not fit the 264px budget alongside the zones/status/action content
// above without silently re-introducing scrolling:
//   - AP-join QR card (TODO.md 10.9) -- REMOVED outright, not moved.
//     ui_page_network.c already shows the identical QR
//     (WIFI:T:WPA;S:...;P:...;; payload, same gating) on its own AP-mode
//     section; this page's copy was a duplicate per 10.9's own audit note,
//     not unique content, so deleting it (rather than relocating it to a
//     third place) is the honest fix.
//   - "Safety Processor" card (ROADMAP.md M6, added 2026-08-18) -- moved to
//     the new ui_page_safety.c/.h, reachable from ui_page_config.c's
//     "Safety Processor" nav item.
//   - Desired-vs-actual temperature chart (TODO.md 10.3) -- moved to the new
//     ui_page_history.c/.h, reachable from ui_page_config.c's "Temperature
//     History" nav item. SUPERSEDED 2026-08-21 (see below): a compact,
//     actual-only chart came back to THIS page per an explicit user request
//     ("i always want to see the graph above the start/profile selection
//     even when not running"); the full actual+desired trend view with
//     legend stayed on ui_page_history.c for detail. SUPERSEDED AGAIN
//     2026-08-22: ui_page_history.c/"Temperature History" is REMOVED
//     outright (owner request, "tempiture history page can go away on the
//     lcd too") -- this page's chart is now the only trend chart on the LCD,
//     not a compact stand-in for a fuller one elsewhere. See "CHART FILLS
//     THE PAGE" below for what replaced it.
//
// 2026-08-21, CHART RETURNS (partial reversal of the no-scroll rewrite above):
// the operator wants the temperature trend visible on the home page at all
// times, not just via a Menu -> Temperature History detour, but a second,
// explicit follow-up narrowed the idle behaviour: "the current temps should
// just show as dots and should stay on the left side of the graph until the
// profile is started." So:
//   - IDLE with no history yet: a single dot (the representative zone's
//     current reading, same "first configured zone" convention
//     profile_executor.h's history ring already uses) pinned at chart index
//     0 -- LV_CHART_POINT_NONE fills every other index, so LVGL draws one
//     point marker and no connecting line, and it never marches across the
//     plot as time passes (no accumulation, no scrolling window) because it
//     is rewritten to the same index 0 every refresh tick, not appended.
//   - Once a profile is running (or has left history behind, same
//     `state==IDLE && history_count==0` gate ui_page_history.c already used):
//     normal trend rendering from profile_executor's history ring, windowed
//     into this page's own UI_PAGE_HOME_CHART_POINTS-point buffer exactly
//     the way ui_page_history.c windows into its own (separate, non-aliased)
//     buffer -- see s_chart_pts's own comment for why these arrays cannot be
//     shared between the two pages.
//   - Actual-only, not actual+desired: half the series of ui_page_history.c's
//     chart, a deliberate simplification to fit this page's height budget
//     (see the action-row/state_card arithmetic below for the fixed-height
//     accounting this trades against) -- the desired-vs-actual comparison
//     with its legend remains ui_page_history.c's job.
//   - Profile picker dropdown -- REMOVED outright, not moved. Start now
//     always uses the same fallback chain start_btn_cb() already had for
//     "picker untouched": current non-idle profile, else the last boot
//     record. An operator who wants to explicitly pick a *different* saved
//     profile before starting still has to use the web dashboard's picker
//     (main_page.html) -- a real, documented capability loss versus the
//     picker this page briefly had, traded for the hard no-scroll
//     requirement. See TODO.md 10.3's status note for this trade-off.
//
// 2026-08-21, DESIRED SERIES + PROGRESS BAR RETURN, part 1 (superseded by
// part 2 below the same day once the backend landed -- kept for history):
// two more user requests against this same compact chart -- "does the graph
// draw lines with the temps as the profile progresses, and also show the
// desired profile on the graph... it should also show a progress bar under
// the graph with the time elapsed and time left," followed by "i want both
// the web and the lcd to work this way". Landed first as a stopgap using
// only data that already existed (profile_executor_get_history()'s recorded
// desired_c for the chart, per-SEGMENT elapsed/remaining for the bar) because
// the whole-profile duration accessor a parallel pass was adding to
// profile_executor.c/profile_feasibility.c/dashboard_http.c was not yet
// present anywhere under firmware/ when this was first written.
//
// 2026-08-21, part 2 (the backend landed later the same day -- this is the
// CURRENT behaviour): profile_exec_status_t gained segments[]/run_start_c/
// total_elapsed_s, and profile_feasibility.h gained
// profile_feasibility_plan_curve() (the straight-line planned-setpoint
// polyline + total duration, or -1 if any segment's ramp duration is
// unknowable -- see that header's own HONESTY RULE comment). Both are called
// directly here (pure math, no zones_http/HTTP dependency, safe from an LVGL
// refresh timer) -- this file still does not own profile_executor.*/
// profile_feasibility.*/dashboard_http.* and did not have to.
//   - Chart: the SECOND series is now the PLANNED curve (ahead of the run),
//     not the trailing recorded desired_c ui_page_history.c still shows --
//     s_ui_home_chart_planned_series/s_ui_home_chart_planned_pts (still ACCENT_3, same color
//     the "desired" concept has always used on both pages). Per this page's
//     own effort note ("if both a planned and a recorded-desired line is too
//     noisy at this size, planned-ahead is the one that was asked for"), the
//     70px-tall home chart keeps ONLY actual + planned, not a third trailing-
//     desired line -- ui_page_history.c (110px, more room) keeps all three.
//     Both series are now plotted against a SHARED time axis spanning the
//     WHOLE run (0..the planned curve's own last point time, "horizon_s"),
//     not the old "last N ring-buffer samples" trailing window: planned is
//     evaluated at each of this page's UI_PAGE_HOME_CHART_POINTS bucket
//     times via piecewise-linear interpolation over the curve's own points
//     (ui_home_plan_lookup()); actual is looked up per-bucket from the history ring
//     by approximating the sample index from bucket time / HISTORY_SAMPLE_PERIOD_S
//     (samples are recorded at that fixed period, so index and elapsed time
//     are proportional) and left as LV_CHART_POINT_NONE for any bucket whose
//     time hasn't happened yet (real time > st.total_elapsed_s) -- this is
//     deliberate: the actual line stops at "now" and the planned line keeps
//     going, which is the whole point of drawing the schedule ahead of the
//     run. Recomputed EVERY refresh tick now (not gated on "did the ring
//     buffer gain a new sample"), since which buckets have already occurred
//     changes every second even between 30s samples; the per-tick cost is a
//     handful of single-entry profile_executor_get_history() reads (at most
//     UI_PAGE_HOME_CHART_POINTS of them), not a bulk copy of the ring buffer
//     (which can hold up to HISTORY_MAX_SAMPLES=640 entries (2026-09-02, sized to what the web/LCD graphs actually display) -- still too big
//     to stage as a local array on this board's DRAM budget).
//   - Progress bar: now shows the WHOLE-FIRING elapsed/remaining
//     (st.total_elapsed_s / profile_feasibility_plan_curve()'s total), not
//     the per-segment numbers part 1 above used as a stopgap. Same honesty
//     rules as before: an unknown total (-1) hides the bar and shows elapsed
//     only; a known total always labels the remaining figure "(estimate)"
//     because ramp-lock overrun is never corrected for (this run's total is
//     an estimate the instant it's known, not just when things go wrong).
//     The per-segment line part 1 added was DROPPED from the bar itself (no
//     room to show both a whole-firing line and a segment line in this
//     page's ~30px progress_row without re-growing the budget) -- segment
//     detail is still visible via s_state_label's summary line and the
//     per-zone rows above it.
//
// 2026-08-22, CHART FILLS THE PAGE (owner requests, same pass as
// ui_page_history.c's removal): "remove whatever it is between the graph and
// the start button" and "the graph ... should expand down to the start
// button". The run-state summary card (profile/state text, live safety-trip
// banner, active kiln-config name) and the progress bar (elapsed/remaining
// time) that used to occupy that space -- plus the bottom_spacer that used to
// keep the Start/Stop button pinned to the page's bottom edge while either of
// those could be hidden -- are all GONE, not hidden. The chart itself now
// carries flex_grow(1) (see ui_page_home_build()'s chart-build comment)
// instead of a fixed pixel height, so it does bottom_spacer's old job:
// whatever vertical space the fixed-height Start/Stop button below it does
// not need, the chart claims. The button is still the LAST child of
// `content` and still the lowest thing on the page -- that requirement is
// unchanged, only what sits above it changed. See ui_home_refresh_cb()'s tail comment
// for the safety-trip-visibility trade-off this removal carries (still
// visible on ui_page_safety.c, just no longer pre-empting this page).
//
// Content-container scrolling is explicitly disabled
// (LV_OBJ_FLAG_SCROLLABLE cleared on both `scr` and `content` in
// ui_page_home_build()) now that the content is sized to fit -- if a future
// change re-overflows this page, LVGL will clip the overflow instead of
// silently turning scrollable again, which is a visible bug report waiting
// to happen rather than a silent regression.


#include "ui_page_home_internal.h"
#include "ui_page_profile_picker.h" /* ui_page_profile_picker_set_pick_cb() -- UI_PLAN.md 6.1 */

const char *UI_HOME_TAG = "ui_page_home";

/* Refresh cadence for the live numbers on this page. 1 Hz matches
 * PROFILE_EXECUTOR_TICK_MS (profile_executor.h) -- no point refreshing
 * faster than the control loop that produces the numbers changes them. */

/* Home page's own chart -- 2026-08-21, see this file's header comment
 * ("CHART RETURNS"); ui_page_history.c (the separate, larger "Temperature
 * History" page this was once windowed smaller than) was removed 2026-08-22
 * per owner request, so this is now the ONLY trend chart on the LCD. Point
 * count stays modest (DRAM cost -- see s_ui_home_chart_actual_pts/s_ui_home_chart_planned_pts'
 * own comment) even though the chart's on-screen HEIGHT is no longer fixed:
 * see ui_page_home_build()'s chart-build comment for why height is now
 * flex_grow(1) instead of a compile-time pixel constant. */

/* 2026-08-21 owner request: "remove the zone a temp section ... also remove
 * the box that says no profile running". The per-zone ROW WIDGETS are gone
 * (no more build_zone_row()/zone_accent(), no more s_zone[] widget array) --
 * but s_ui_home_zone_count is NOT gone: ui_home_refresh_cb()'s idle branch still uses it to
 * gate the "representative zone" lookup that feeds the chart's single idle
 * dot (the "first configured zone" convention this file's header comment
 * documents, shared with ui_page_history.c's own idle fallback). Keeping the
 * count without the widgets means that gate still reads honestly (0
 * configured zones -> no dot, not a NULL-deref on a widget that was never
 * built) without reintroducing the removed UI. */
uint8_t s_ui_home_zone_count; /* zones_config_get_thermo_count() at build time */

/* Trip strip -- hidden unless a live trip is present; see its creation in
 * ui_page_home_build() for why it is hidden rather than absent. */
lv_obj_t *s_ui_home_trip_strip;

/* PID_EXPANSION_PLAN.md 7.4's LCD warning surface: "kiln is behind schedule"
 * -- INFORMATIONAL, not a fault, so deliberately NOT styled like
 * s_ui_home_trip_strip above (solid ACCENT_5 red, white text, "SAFETY TRIP"
 * wording). Same "built once, hidden = zero flex height" idiom as
 * s_ui_home_trip_strip/s_ui_home_progress_wrap, placed directly below the trip strip so a
 * real safety trip still reads above it. See build()'s creation site for
 * the exact styling choice and ui_home_refresh_cb() for the debounce this reads
 * from ui_page_home_graph.c's ui_page_home_lag_notice_tick()/
 * _should_show(). */
lv_obj_t *s_ui_home_lag_notice;
/* Consecutive-tick counter feeding the debounce -- see
 * ui_page_home_lag_notice_tick()'s header comment (ui_page_home_graph.h) for
 * why the counting stays pure/testable there while the storage lives here,
 * same split ui_page_home_axis_ratchet_should_reset() already uses. */
uint32_t s_ui_home_lag_notice_ticks;
lv_obj_t *s_ui_home_chart;                     /* home page's compact chart -- actual + planned-ahead */
lv_chart_series_t *s_ui_home_chart_actual_series;
lv_chart_series_t *s_ui_home_chart_planned_series;

/* UI_PLAN.md 6.5 -- the dashboard's right-quarter rail. graph_row wraps the
 * chart (now 74% width, was 100%) and this rail (25%, 4px gap between).
 * See ui_page_home_rail.h for the pure formatting helpers this rail's
 * refresh calls into, and ui_home_rail_refresh() (ui_page_home_refresh.c)
 * for the per-tick fill using the same ds/st snapshot the rest of
 * ui_home_refresh_cb() already fetched -- no new producer call. */
lv_obj_t *s_ui_home_rail;
lv_obj_t *s_ui_home_rail_relay_pill[KILN_IO_RELAY_COUNT];
lv_obj_t *s_ui_home_rail_zone_row[MAX31856_CHANNEL_COUNT];
lv_obj_t *s_ui_home_rail_zone_name[MAX31856_CHANNEL_COUNT];
lv_obj_t *s_ui_home_rail_zone_temp[MAX31856_CHANNEL_COUNT];
lv_obj_t *s_ui_home_rail_zone_bar[MAX31856_CHANNEL_COUNT];
lv_obj_t *s_ui_home_rail_watts_label;
/* These arrays ARE the chart's backing store (lv_chart_set_series_ext_y_array()),
 * not a scratch copy, so they must outlive the chart -- static, matching every
 * other widget on this page's "built once, page never torn down" lifetime
 * (kiln_ui.h's header comment). SEPARATE arrays from
 * ui_page_history.c's own chart arrays -- both pages exist for the app's
 * whole lifetime (never torn down), so aliasing one static array between two
 * live charts would have one page's lv_chart_set_series_ext_y_array() call
 * silently stomp the other's chart data every refresh. Size: 2 *
 * UI_PAGE_HOME_CHART_POINTS (30) * 4 bytes = 240 bytes total -- comfortably
 * small next to the ~4167-byte internal-DRAM headroom this codebase runs
 * under, not moved to PSRAM. 2026-08-21 part 2 (see this file's header
 * comment): this is now the PLANNED-ahead curve, not a trailing recorded
 * desired_c -- see ui_home_refresh_cb()'s ui_home_plan_lookup()/plan_pts usage. No size
 * change from part 1 (still one int32 per bucket). */
int32_t s_ui_home_chart_actual_pts[UI_PAGE_HOME_CHART_POINTS];
int32_t s_ui_home_chart_planned_pts[UI_PAGE_HOME_CHART_POINTS];

/* Temp/time scale overlay -- 2026-08-21 owner request ("make it so that i can
 * see a temp and time scale on the graph"). LVGL 9.5 removed
 * lv_chart_set_axis_tick() (no built-in chart axis widget any more -- grepped
 * this tree's own lv_chart.h to confirm before writing any of this). The two
 * remaining options were lv_scale (a full separate widget, src/widgets/scale/
 * lv_scale.h) or plain lv_label children positioned over the chart's own
 * corners. Chose plain labels: lv_scale's tick/label rendering is built
 * around a FIXED set of major ticks (lv_scale_set_total_tick_count() /
 * _set_major_tick_every()) and a static text_src[] array
 * (lv_scale_set_text_src()) -- reasonable for an axis that ticks at fixed
 * intervals, but this page's Y range is fully dynamic (ui_home_refresh_cb() rewrites
 * it from live data every tick, both branches below) and re-deriving a tick
 * spacing + a fresh text_src array every second just to show two numbers is
 * more moving parts than the two labels below, with more ways for the tick
 * labels to silently drift out of sync with the range that drives them. Two
 * labels directly overwritten with lv_label_set_text() every ui_home_refresh_cb()
 * call are trivially kept in sync BY CONSTRUCTION: they are written from the
 * exact same lo/hi (or v-10/v+10) values passed to
 * lv_chart_set_axis_range() in the same code path, not read back from the
 * chart afterward. Built as CHILDREN of s_ui_home_chart (not `content` siblings), so
 * they overlay the plot rather than consuming their own row height -- this
 * page has no spare height budget for a fourth chart-adjacent row (see the
 * action-row comment's arithmetic). Small semi-opaque background chips (not
 * fully transparent) so the digits stay legible against whichever part of
 * the actual/planned traces happens to be under that corner. */
/* 2026-08-30 owner decision (UI_PLAN.md 5.3, "option 3"): all 11 web-matching
 * Y-axis ticks, labelled, with a smaller font if needed -- NOT the previous
 * two-corner (hi/lo only) overlay, and NOT lv_chart_set_div_line_count()
 * (see that call site's own long comment for why div lines were reverted).
 * main_page.html's drawYAxis() draws tickCount=10 -> 11 evenly spaced
 * positions from axis min to axis max, each a numeric label plus a short
 * tick mark just outside the plot; this mirrors that count and spacing.
 * Static array, built ONCE (UI_PAGE_HOME_Y_TICK_COUNT lv_label children of
 * s_ui_home_chart -- 6, not the original 11, see the 2026-09-01 note below; never created
 * or destroyed per refresh -- see this file's own "watch memory" framing
 * elsewhere), and every ui_home_refresh_cb() tick only rewrites their text/position
 * from the SAME axis_lo/axis_hi just handed to lv_chart_set_axis_range()
 * (ui_home_chart_set_y_ticks() below), same "written from the same values, never
 * read back" discipline the old two-label version used -- see the header
 * comment above (near the static declarations) for why plain labels were
 * chosen over lv_scale in the first place; that reasoning is unchanged by
 * going from 2 to 11 of them, just repeated 11 times instead of 2. No
 * they use montserrat_10, enabled via CONFIG_LV_FONT_MONTSERRAT_10 in the
 * tracked sdkconfig.defaults -- see the build() comment for why that had to
 * go in sdkconfig.defaults and not sdkconfig, and why a render-time
 * transform_scale on montserrat_14 was tried first and rejected.
 *
 * 2026-09-01: was 11. Owner report ("vertical axis label spacing is too
 * wide -- the labels do not all fit on the graph") against a real device:
 * this chart's plot height is flex_grow(1) residual on a 480px page already
 * carrying a status row, trip strip, progress bar and action row above/below
 * it -- nowhere near the web GUI's much taller canvas the "all 11 ticks"
 * decision (UI_PLAN.md 5.3) was originally sized for. 11 montserrat_10
 * labels (~11px line height each) need >=110px of plot height with zero
 * overlap; on the real panel the residual is well under that, so the top and
 * bottom labels' text boxes were being pushed outside the chart's own
 * bounds. 6 ticks (0, 1/5, 2/5, 3/5, 4/5, top) keeps 0/peak/-endpoints
 * labelled and roughly doubles the per-label spacing without touching the
 * axis-RANGE math (ui_home_chart_set_y_ticks() below still derives every label from
 * axis_lo/axis_hi via the same frac-of-range formula, just over fewer k). */
lv_obj_t *s_ui_home_chart_y_tick_labels[UI_PAGE_HOME_Y_TICK_COUNT];
/* Current axis range, mirrored here so the draw-event tick-mark hook
 * (ui_home_chart_y_tick_draw_event_cb(), fires on every LVGL render pass, not just
 * ui_home_refresh_cb()'s 1 Hz tick) can recompute pixel positions without a second
 * per-tick pixel cache -- always in sync with the labels because both are
 * driven from the same lv_chart_set_axis_range() call. */
int32_t s_ui_home_chart_axis_lo, s_ui_home_chart_axis_hi;
bool s_ui_home_chart_y_ticks_visible; /* false hides labels AND tick marks */
/* Anti-jitter hold state for the active-firing Y axis (see
 * ui_page_home_active_y_axis_range()'s header comment in
 * ui_page_home_graph.h) -- the axis this function returned on the previous
 * tick of the CURRENT run, so it can only ever widen from here, never shrink
 * back in. s_ui_home_axis_hold_have is reset to false exactly once per run, at the
 * state_active false->true transition detected in ui_home_refresh_cb() -- see that
 * call site's own comment. */
bool s_ui_home_axis_hold_have;
int32_t s_ui_home_axis_hold_lo, s_ui_home_axis_hold_hi;
bool s_ui_home_axis_hold_prev_active;
/* total_elapsed_s observed on the previous "active" refresh tick -- fed to
 * ui_page_home_axis_ratchet_should_reset() alongside s_ui_home_axis_hold_prev_active
 * so the ratchet reset is keyed to RUN IDENTITY (a total_elapsed_s decrease)
 * rather than just the IDLE->active state edge, which misses a new run
 * started from DONE/FAULTED (no IDLE tick in between) and a stop/restart
 * that happens between two refreshes. See that function's header comment in
 * ui_page_home_graph.h for the full reasoning. Only meaningful/updated while
 * state_active; stale while idle, but never read in that state. */
uint32_t s_ui_home_axis_hold_prev_elapsed_s;
/* 2026-08-31 owner request ("the times for the lcd graph should be at the
 * bottom of the graph"): replaces the old single top-right "0:00-MM:SS" chip
 * with four individually positioned tick labels along the BOTTOM edge of the
 * plot, one per ui_page_home_x_ticks() value (0, h/3, 2h/3, h) -- the same
 * four-tick shape ui_page_home_build_x_label() already produced as one
 * pipe-joined string, now laid out spatially like s_ui_home_chart_y_tick_labels[]
 * rather than packed into a single corner string. Static array, built ONCE
 * in ui_page_home_build() (same "never allocate inside ui_home_refresh_cb()"
 * discipline as the Y ticks); ui_home_chart_set_x_ticks() below only ever rewrites
 * text/position on these four. */
lv_obj_t *s_ui_home_chart_x_tick_labels[UI_PAGE_HOME_X_TICK_COUNT];

/* 2026-09-01 owner request: "add a compact legend inside the graph, bottom
 * right corner" (plus, in the same pass, "remove the dots from the plot,
 * keep the lines" -- that half is a one-line lv_obj_set_style_size() on
 * LV_PART_INDICATOR at series-creation time in ui_page_home_build(), no
 * static state needed for it).
 *
 * Two rows, built ONCE as children of s_ui_home_chart (same "never allocate inside
 * ui_home_refresh_cb()" discipline as the Y/X tick labels above) -- row 0 is always
 * "Actual", row 1 is "Plan". Each row is its own small flex-row container
 * (a colour swatch + a label) so its on-screen width is LV_SIZE_CONTENT and
 * ui_home_chart_set_legend() never has to add up swatch+label widths by hand, the
 * same "let LVGL measure it" approach ui_home_chart_set_x_ticks() already uses for
 * its own label widths. ui_page_home_legend_visibility() (ui_page_home_graph.c)
 * is the host-tested pure logic for WHICH of these two rows should be visible
 * on a given tick; this file only ever shows/hides and repositions the two
 * already-built rows, never creates or destroys one. */
lv_obj_t *s_ui_home_chart_legend_row[UI_PAGE_HOME_LEGEND_ROWS];
lv_obj_t *s_ui_home_chart_legend_swatch[UI_PAGE_HOME_LEGEND_ROWS];
lv_obj_t *s_ui_home_chart_legend_label[UI_PAGE_HOME_LEGEND_ROWS];

/* 2026-08-23 owner request ("the LCD profile graph should look like the web
 * GUI's profile graph"): a small filled blue dot marking the current
 * position along the planned curve, matching main_page.html's own current-
 * position marker. A plain circular lv_obj (not a chart point-bullet style,
 * which would also mark every OTHER point on the series) positioned every
 * ui_home_refresh_cb() tick via lv_chart_get_point_pos_by_id() -- that call does the
 * axis-range-to-pixel mapping chart-internally, so this file does not
 * reimplement lv_chart's own y = f(value) math a second time. Hidden
 * whenever there is no live run to mark a position on (idle, or no plan
 * points), same discipline as the Y/X overlay labels above. */
lv_obj_t *s_ui_home_chart_now_dot;

/* Progress bar, under the chart -- owner request 2026-08-30: "add a
 * progress bar to the lcd under the graph like what exists on the web
 * page" (main_page.html's #progressWrap/renderProgress()). Reads the exact
 * same numbers via dashboard_plan_exec_fields() (dashboard_http.h,
 * TODO.md 10.1a shared-backend seam factored out of that function for this
 * purpose) rather than a second implementation of the elapsed/remaining
 * math. This re-introduces the vertical space the 2026-08-22 "chart fills
 * the page" pass reclaimed (see this file's header comment on that removal)
 * -- s_ui_home_progress_wrap has a fixed content height (no flex_grow), so it comes
 * back out of the chart's flex_grow(1) budget, not out of the Start/Stop
 * button's fixed row. Built HIDDEN and only shown for RUNNING/PAUSED/
 * FAULTED/DONE (same STOPPABLE-ish gate main_page.html's renderProgress()
 * uses), so it costs zero height in the far-more-common IDLE state, same
 * "hidden = zero flex height" discipline as s_ui_home_trip_strip above. */
lv_obj_t *s_ui_home_progress_wrap;
lv_obj_t *s_ui_home_progress_track;
lv_obj_t *s_ui_home_progress_fill;
lv_obj_t *s_ui_home_progress_label;

/* Web-match purple for the planned-profile curve -- main_page.html's own
 * planned-curve stroke is `ctx.strokeStyle = '#96c'` (CSS 3-digit shorthand,
 * i.e. #9966CC), read directly out of that file at ~line 1484. Kept as its
 * own named color here rather than reusing UI_THEME_ACCENT_2 (a similar but
 * NOT identical purple, 0xa15fd6) -- matching the reference image byte-for-
 * byte is the point of this task, an approximate purple would not be. */

/* Dash pattern for the planned curve, in pixels -- matches main_page.html's
 * `ctx.setLineDash([6, 4])` exactly (6 on, 4 off). See
 * ui_home_chart_draw_event_cb()'s own comment for how this gets applied: LVGL's
 * lv_chart has no built-in dashed-series style, so this is injected via a
 * LV_EVENT_DRAW_TASK_ADDED hook rather than a style property. */

lv_obj_t *s_ui_home_fire_btn;      /* merged Start/Stop button */
lv_obj_t *s_ui_home_fire_btn_label;
lv_obj_t *s_ui_home_pause_btn;      /* merged Pause/Resume button -- see ui_home_pause_resume_btn_cb() */
lv_obj_t *s_ui_home_pause_btn_label;
lv_obj_t *s_ui_home_profile_btn;    /* UI_PLAN.md 6.1 -- selected profile name, opens the picker */
lv_obj_t *s_ui_home_profile_label;
ui_topbar_t s_ui_home_topbar;       /* Menu gear icon, shared chrome -- see ui_topbar.h */

/* WiFi/IP/mDNS status readout, in the status bar. Text comes from
 * wifi_status_ui_get_text() (wifi_status_ui.c) -- see that module's header
 * comment for the underlying getters and the TODO.md 10.1a shared-backend
 * rule. */
lv_obj_t *s_ui_home_status_label;

/* mm:ss for anything under an hour (this page's numbers are segment-scale,
 * not multi-day), hh:mm:ss beyond that -- matches main_page.html's
 * fmtDuration() shape closely enough for web/LCD parity (TODO.md 10.5)
 * without pulling in the exact same JS-derived format. */

#include "ui_page_home_internal.h"

/* format_duration/freezing_point_disp were file-local utility helpers in
 * the pre-split ui_page_home.c; widened here only because
 * ui_page_home_refresh.c now calls both. See ui_page_home_internal.h. */

void ui_home_format_duration(uint32_t seconds, char *out, size_t out_cap)
{
    uint32_t h = seconds / 3600;
    uint32_t m = (seconds % 3600) / 60;
    uint32_t s = seconds % 60;
    if (h > 0) {
        snprintf(out, out_cap, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(out, out_cap, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
    }
}

/* 2026-08-21 owner request: "the charts should never show below freezing
 * temp." The clamp belongs on the AXIS, never on the DATA: a thermocouple
 * fault/disconnect/cold-workshop reading below freezing must still be
 * PLOTTED and still be visible as an out-of-range excursion -- silently
 * floor-clamping the value itself would hide exactly the fault this display
 * exists to surface. So every call site below computes its natural axis
 * bound from the real data first (unchanged), then raises axis_lo to this
 * floor ONLY when the real plotted minimum is itself still at-or-above the
 * floor (i.e. only the cosmetic padding dipped below freezing, not a real
 * reading) -- see the two call sites' comments for the exact guard. Freezing
 * is 0 in Celsius but 32 in Fahrenheit (unit_pref.h's own ABSOLUTE-vs-RATE
 * distinction: this is a fixed point on the Celsius scale, not a magnitude,
 * so it must be re-expressed per display unit, never just reused as "0"),
 * so this reads the same `unit_pref_t` every value on this page is already
 * converted through -- never a second, independent guess at the unit. */
float ui_home_freezing_point_disp(unit_pref_t unit)
{
    return (unit == UNIT_PREF_FAHRENHEIT) ? 32.0f : 0.0f;
}

/* Shared by ui_home_do_start() and the confirmation dialog builder below -- both need
 * the exact same fallback chain (whatever's currently known this boot
 * (non-idle profile_id), else the last boot record), and the dialog has to
 * name the SAME profile the button is actually about to start, not a second
 * guess at it. Returns false (out_id untouched) if neither source has one. */

lv_obj_t *ui_page_home_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    /* Hard no-scroll requirement -- see this file's header comment. Content
     * below is sized to fit; if it ever overflows again this clips instead
     * of silently becoming scrollable. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* docs/WEB_AUTH_PLAN.md item 10 -- the four transparent corner hit zones
     * for the physical credential-reset gesture. Built here, as the FIRST
     * children of `scr`, deliberately BEFORE the topbar/content/action_row
     * below: LVGL resolves an overlapping tap to the LAST-added (topmost)
     * child, so any real control built after this point -- the topbar's
     * gear icon, the Pause/Start buttons' extended touch halo -- always wins
     * an overlapping tap regardless of geometric overlap. This is the
     * primary safety mechanism; the explicit size shrinks below are
     * defense-in-depth on top of it, not a substitute for it.
     *
     * Bench check performed at code-review level (no live hardware access in
     * this pass -- see the commit message): grepping ui_topbar.c found the
     * home page's gear-icon box is a 36x36 FLOATING object flush at
     * LV_ALIGN_TOP_RIGHT (icon width UI_TOPBAR_ICON_W_PX plus
     * UI_THEME_STATUS_BAR_HEIGHT_PX/PADDING_PX math for one icon), directly
     * under a naive 40x40 top-right zone. Reading this file's own action_row
     * comment (immediately above where it is built, below) found the
     * Pause/Start buttons carry a documented ~24px touch-area halo beyond
     * their 36px drawn height, reaching both bottom corners. Top-left has no
     * competing control. Per the "shrink a zone rather than steal a tap"
     * instruction: top-left stays a full 40x40; top-right, bottom-left and
     * bottom-right are shrunk to 24x24, each commented with exactly which
     * real control it avoids stealing from -- on top of the z-order
     * precedence above, which is what actually guarantees the real controls
     * win. All four are fully transparent (bg_opa TRANSP, no border) --
     * this deliberately adds no new visible element and no new LCD color. */
    {
        const int32_t full_zone_px = 40;
        const int32_t shrunk_zone_px = 24; /* smaller on purpose -- see comment above */

        struct {
            auth_reset_gesture_corner_t corner;
            lv_align_t align;
            int32_t size_px;
        } zones[AUTH_RESET_CORNER_COUNT] = {
            { AUTH_RESET_CORNER_TOP_LEFT,     LV_ALIGN_TOP_LEFT,     full_zone_px },
            /* Shrunk: avoids stealing from ui_topbar.c's 36x36 gear-icon
             * FLOATING box, flush at LV_ALIGN_TOP_RIGHT (0,0). */
            { AUTH_RESET_CORNER_TOP_RIGHT,    LV_ALIGN_TOP_RIGHT,    shrunk_zone_px },
            /* Shrunk: avoids stealing from the Pause button's ~24px touch
             * halo below action_row's bottom-left corner. */
            { AUTH_RESET_CORNER_BOTTOM_LEFT,  LV_ALIGN_BOTTOM_LEFT,  shrunk_zone_px },
            /* Shrunk: avoids stealing from the Start/Stop button's ~24px
             * touch halo below action_row's bottom-right corner. */
            { AUTH_RESET_CORNER_BOTTOM_RIGHT, LV_ALIGN_BOTTOM_RIGHT, shrunk_zone_px },
        };

        for (size_t i = 0; i < AUTH_RESET_CORNER_COUNT; i++) {
            lv_obj_t *zone = lv_obj_create(scr);
            lv_obj_set_size(zone, zones[i].size_px, zones[i].size_px);
            lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(zone, 0, 0);
            lv_obj_set_style_pad_all(zone, 0, 0);
            lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
            /* FLOATING first, before alignment -- same ordering ui_topbar.c
             * uses for its icon box, and for the same reason (excludes it
             * from `scr`'s flex-column layout so it can be positioned
             * freely against the screen edge instead of stacking as a flow
             * child). */
            lv_obj_add_flag(zone, LV_OBJ_FLAG_FLOATING);
            lv_obj_align(zone, zones[i].align, 0, 0);
            lv_obj_add_event_cb(zone, ui_home_auth_reset_corner_tap_cb, LV_EVENT_CLICKED,
                                 (void *)(intptr_t)zones[i].corner);
        }
    }

    /* Top bar -- ui_topbar.c/.h owns both LVGL traps (hit-test-does-not-
     * escape-the-parent, and the flex trap on a FLOATING icon proxy); see
     * that header for the mechanism. No title here: this page shows the
     * live WiFi/IP status string instead (s_ui_home_status_label, built below), so
     * .title is NULL and the "kilnCtl" label that used to occupy the same
     * spot is gone -- it was the thing the user reported overlapping the
     * IP address. Home has no back/prev/next; the gear is Menu, same
     * destination as before (ui_home_menu_nav_cb() -> ui_page_config_reset_to_first_page()
     * + kiln_ui_show("config")). */
    /* .warning_icon reserves a second, non-clickable icon slot next to the
     * gear for RELAY_LIFE_BUDGET.md's relay-life indicator.
     * Starts hidden; ui_page_home_refresh.c's periodic tick calls
     * ui_topbar_set_warning() with relay_cycles_max_budget_tier() on every
     * refresh, the same cadence s_ui_home_lag_notice already uses -- no new
     * task or timer. */
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = NULL,
        .gear_cb = ui_home_menu_nav_cb,
        .warning_icon = true,
    }, &s_ui_home_topbar);
    lv_obj_t *bar = s_ui_home_topbar.bar;

    /* WiFi/IP/mDNS status readout -- right side of the status bar, just
     * left of the gear. Width capped so a long status string can't grow
     * under/behind the gear's (invisible, extended) hit area; the label
     * itself still just shows whatever wifi_status_ui_get_text() returns.
     *
     * WIDTH MUST BE SET EXPLICITLY, NOT INFERRED FROM align_to() --
     * confirmed on hardware (2026-08-20 tap-target dump) after the user
     * reported the WiFi text overlapping the "kilnCtl" title once
     * connected (long strings like "WiFi: 192.168.1.156 (kilnctl.local) --
     * Signal: -45 dBm"). lv_obj_align_to() only sets a POSITION (an anchor
     * point); it never constrains WIDTH, and this label's box was sized to
     * the full width of `bar` (its parent), so the fixed anchor point did
     * nothing to stop the box -- and therefore the rendered text -- from
     * running under whatever sat to its right. Fix: cap the label's own
     * width so its box can never reach the icon proxy, independent of
     * parent width, then use LV_LABEL_LONG_DOT so any string that still
     * doesn't fit ellipsises ("...") instead of overflowing.
     * LV_LABEL_LONG_SCROLL_CIRCULAR was considered and rejected: this is an
     * always-on kiln panel, and a perpetually scrolling label is a
     * needless distraction and redraw cost. Text stays left-aligned (the
     * label's default) so short strings look exactly as they did before.
     *
     * ui_topbar_t.icons_w (see ui_topbar.h) is the icon slots (gear +
     * warning) this page reserves, replacing the old hand-rolled
     * gear_hit_area_w literal. It is NOT the same number: the old
     * hand-rolled proxy was 80px wide (chosen generously, sideways headroom
     * with nothing else clickable in the bar); with this page's
     * `.warning_icon = true` (a second, non-clickable slot next to the
     * gear), ui_topbar_create()'s icons_w is
     * icon_count * UI_TOPBAR_ICON_W_PX + (icon_count-1) * UI_TOPBAR_ICON_GAP_PX
     * = 2 * 36 + 4 = 76px (ui_topbar.h's UI_TOPBAR_ICON_W_PX/GAP_PX). Width
     * derivation: cap = bar_width - icons_w - gap.
     *
     * PRE-topbar measurement, superseded: on hardware 2026-08-20 (before
     * this module existed, same physical 463px bar) the 80px-wide proxy
     * gave a ~379px cap. That 80px/~379px pair no longer describes what
     * this code computes -- with the topbar's 76px icons_w (gear + warning)
     * the same 463px bar now yields cap = 463 - 76 - 4 = ~383px. The
     * bar-width figure (463px) and the underlying bug this cap fixes
     * (align_to() sets a POSITION only and never constrains WIDTH, so an
     * uncapped label box still renders text under the gear) are still real
     * and still true; only the old proxy's specific width and resulting cap
     * are stale. Not re-measured on hardware since the topbar move --
     * s_ui_home_topbar.icons_w is logged below so a future boot can confirm
     * the ~383px figure. */
    lv_obj_update_layout(bar);
    int32_t bar_w = lv_obj_get_width(bar);
    int32_t status_label_max_w = bar_w - s_ui_home_topbar.icons_w - (UI_THEME_PADDING_PX / 2);
    /* Guard the subtraction. lv_obj_update_layout() above normally resolves
     * the bar's lv_pct(100) against the screen, but this page is BUILT
     * DETACHED (kiln_ui.c builds a page before it is ever shown), and a
     * width read before layout resolves is 0 -- which would make this
     * subtraction negative and the label either invisible or garbage. If
     * that ever happens, fall back to the full bar width: a label that
     * overlaps the gear is a cosmetic bug, a label that vanishes is a
     * functional one, and the log line says which case this boot took. */
    if (status_label_max_w <= 0) {
        ESP_LOGW(UI_HOME_TAG, "status label width fallback: bar_w=%ld resolved too small for the gear "
                      "reservation (%ld) -- label may overlap the gear this boot",
                 (long)bar_w, (long)s_ui_home_topbar.icons_w);
        status_label_max_w = (bar_w > 0) ? bar_w : LV_SIZE_CONTENT;
    } else {
        ESP_LOGI(UI_HOME_TAG, "status label width %ld of bar %ld (gear reserves %ld)",
                 (long)status_label_max_w, (long)bar_w, (long)s_ui_home_topbar.icons_w);
    }
    s_ui_home_status_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_ui_home_status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_width(s_ui_home_status_label, status_label_max_w);
    lv_label_set_long_mode(s_ui_home_status_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_ui_home_status_label, "WiFi: --");
    /* Anchor to s_ui_home_topbar.icons (the FLOATING proxy ui_topbar.c builds), not
     * any individual icon button -- the proxy is the wider box and the one
     * the label must actually clear. */
    lv_obj_align_to(s_ui_home_status_label, s_ui_home_topbar.icons, LV_ALIGN_OUT_LEFT_MID, -(UI_THEME_PADDING_PX / 2), 0);

    /* Content area -- deliberately NOT scrollable (see this file's header
     * comment for the ~264px budget this is sized against). */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* `content` is created after the topbar's icon proxy, so without this it
     * would sit above the proxy in z-order and win taps in the overlap
     * region (the padding gap between the bar and the first zone row).
     * ui_topbar_raise() forces the proxy back to the top so it keeps
     * winning the hit test there -- see ui_topbar.h's usage note for why
     * this must run AFTER content exists. */
    ui_topbar_raise(&s_ui_home_topbar);

    /* Actual+planned chart -- see this file's header comment ("CHART
     * RETURNS", 2026-08-21). Built directly into `content` (no card wrapper,
     * unlike ui_page_history.c's chart, which no longer exists -- see this
     * file's header comment on ui_page_history.c's removal) specifically to
     * skip a card's own pad_all/pad_gap overhead.
     *
     * 2026-08-22 owner request ("remove whatever it is between the graph and
     * the start button" / "the graph ... should expand down to the start
     * button"): the run-state card, progress bar, and bottom spacer that used
     * to fill this page's leftover vertical space are gone (see
     * ui_home_refresh_cb()'s tail comment and this function's own removed
     * state_card/progress_row/bottom_spacer). The chart itself now carries
     * flex_grow(1) instead of a fixed UI_PAGE_HOME_CHART_HEIGHT_PX, so it
     * claims however much of `content`'s main-axis space action_row (the
     * Start/Stop button, still the last child, still pinned to the bottom)
     * does not need -- the same job bottom_spacer used to do, but performed
     * by the chart growing into the space rather than an invisible filler
     * next to a fixed-height chart. No explicit height is set here; flex_grow
     * alone determines it. */
    /* Safety-trip strip. The run-state card that used to carry the trip
     * banner was removed on 2026-08-22 per the owner's "remove whatever it is
     * between the graph and the start button" -- but losing the trip callout
     * from the page an operator actually watches is not an acceptable side
     * effect of a layout request. This strip is the compromise: it is created
     * hidden and only ever shown while a trip is live, and LVGL SKIPS hidden
     * children when laying out a flex container, so while everything is
     * normal it occupies exactly zero pixels and the chart still grows all
     * the way down to the Start button, which is what was asked for.
     *
     * First child, above the chart, so a trip reads at the top of the screen
     * rather than displacing the button at the bottom. */
    s_ui_home_trip_strip = lv_label_create(content);
    lv_obj_add_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_ui_home_trip_strip, lv_pct(100));
    lv_label_set_long_mode(s_ui_home_trip_strip, LV_LABEL_LONG_DOT);
    lv_obj_set_style_bg_color(s_ui_home_trip_strip, UI_THEME_ACCENT_5, 0);
    lv_obj_set_style_bg_opa(s_ui_home_trip_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ui_home_trip_strip, lv_color_white(), 0);
    lv_obj_set_style_radius(s_ui_home_trip_strip, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_ui_home_trip_strip, 3, 0);
    lv_label_set_text(s_ui_home_trip_strip, "");

    /* PID_EXPANSION_PLAN.md 7.4's LCD lag notice. Same hidden-until-needed,
     * zero-height-while-hidden strip idiom as s_ui_home_trip_strip immediately
     * above (LVGL skips hidden children in flex layout -- this is what
     * keeps the chart still reaching the Start button when nothing is
     * lagging, exactly the reasoning s_ui_home_trip_strip's own comment gives).
     * Second child, so a real safety trip still reads first if somehow both
     * are live at once.
     *
     * STYLING, deliberately NOT s_ui_home_trip_strip's alarm treatment: this page
     * has no separate "informational" visual language anywhere else to
     * reuse (grepped every ui_page_*.c for one before adding this -- none
     * exists), so this follows the closest existing INFORMATIONAL (not
     * fault) precedent instead: s_ui_home_progress_label/s_ui_home_status_label's own
     * muted-secondary-text-on-card look (UI_THEME_COLOR_TEXT_SECONDARY on
     * UI_THEME_COLOR_CARD, no bg_opa COVER fill, no white-on-solid-color).
     * That is the opposite of s_ui_home_trip_strip's white-on-solid-ACCENT_5 by
     * every axis that distinguishes them (fill vs. card tone, alert vs.
     * secondary text color, "SAFETY TRIP"/all-caps wording vs. a plain
     * sentence) -- a glance must read this as a status readout, not a
     * fault banner. */
    s_ui_home_lag_notice = lv_label_create(content);
    lv_obj_add_flag(s_ui_home_lag_notice, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_ui_home_lag_notice, lv_pct(100));
    lv_label_set_long_mode(s_ui_home_lag_notice, LV_LABEL_LONG_DOT);
    lv_obj_set_style_bg_color(s_ui_home_lag_notice, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_ui_home_lag_notice, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ui_home_lag_notice, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_radius(s_ui_home_lag_notice, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_ui_home_lag_notice, 3, 0);
    lv_label_set_text(s_ui_home_lag_notice, "");

    /* UI_PLAN.md 6.5 -- graph_row replaces the chart as the flex_grow(1)
     * child of `content`: it takes the same residual vertical space the
     * chart used to take alone, and splits it 74/25 horizontally between
     * the chart and the new rail (4px gap between, see section 6.5's
     * arithmetic: 0.74*464=343, 0.25*464=116, +4 gap = 463 <= 464). Cross
     * axis STRETCH so both children fill graph_row's full height, matching
     * what the chart's own flex_grow(1)/lv_pct(100) height used to give it
     * directly. */
    lv_obj_t *graph_row = lv_obj_create(content);
    lv_obj_remove_flag(graph_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(graph_row, lv_pct(100));
    lv_obj_set_flex_grow(graph_row, 1);
    lv_obj_set_style_bg_opa(graph_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(graph_row, 0, 0);
    lv_obj_set_style_pad_all(graph_row, 0, 0);
    lv_obj_set_style_pad_gap(graph_row, UI_THEME_SPACE_1, 0);
    lv_obj_set_flex_flow(graph_row, LV_FLEX_FLOW_ROW);
    /* This LVGL build's lv_flex_align_t has no STRETCH value (cross axis
     * placement is only START/END/CENTER) -- both children set their own
     * height to lv_pct(100) explicitly instead (s_ui_home_chart above,
     * s_ui_home_rail below), so START here is a no-op given their explicit
     * full-height sizing, not a real behaviour choice. */
    lv_obj_set_flex_align(graph_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_ui_home_chart = lv_chart_create(graph_row);
    lv_obj_set_width(s_ui_home_chart, lv_pct(74));
    lv_obj_set_height(s_ui_home_chart, lv_pct(100));
    lv_obj_set_style_bg_color(s_ui_home_chart, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_ui_home_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui_home_chart, 0, 0);
    lv_obj_set_style_radius(s_ui_home_chart, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Phase 7 theme/style pass (TODO.md 1223-1225): pure-paint shadow, see
     * ui_theme.h -- costs no page-budget height (the flex_grow(1) chart
     * still claims exactly the same residual space it did before). */
    ui_theme_apply_card_shadow(s_ui_home_chart, 1);
    lv_obj_set_style_pad_all(s_ui_home_chart, 2, 0);
    lv_chart_set_type(s_ui_home_chart, LV_CHART_TYPE_LINE);
    /* Owner report 2026-08-22: "the LCD has 3 sections in the graph, it
     * should only be one", confirmed against a photo of the screen.
     *
     * Both counts were zeroed at the time. The culprit was the VERTICAL
     * count, which was 4: LVGL draws division lines INCLUDING one at each
     * edge of the plot area, so 4 vertical lines are drawn at 0, 1/3, 2/3
     * and 1 of the width -- two of them land in the middle and cut the plot
     * into exactly the three columns the report describes. It read as three
     * separate panels, not as gridlines, because the line color had as much
     * contrast against the card background as the trace itself did -- the
     * counts were not the only problem, the default LV_PART_MAIN line style
     * was too.
     *
     * (A first attempt zeroed only the HORIZONTAL count, on the assumption
     * that 2 horizontal lines making 3 stacked bands was the "3 sections".
     * It was not -- the photo shows the divisions running vertically. Noted
     * so the next person does not re-add either count reasoning that "the
     * other one was the problem".)
     *
     * 2026-08-30: an attempt to satisfy the owner's "show 10 vertical
     * markers like the web GUI" by setting hdiv=11/vdiv=4 here was made and
     * REVERTED the same day, because the premise was wrong. The web chart
     * has no gridlines at all. main_page.html's drawYAxis() strokes
     * moveTo(padL - 3, vy) -> lineTo(padL, vy): a 3px TICK MARK just
     * outside the plot, next to a numeric label at x=2. drawChartAxis()
     * does the same below the plot. The only full-width strokes inside the
     * web's plot area are its border rect and drawFreezingRef()'s dashed
     * 0 degC reference line. So "10 markers" means ten labelled ticks
     * outside the plot, and lv_chart div lines -- which are full-width
     * lines THROUGH it -- are the wrong primitive: hdiv=11/vdiv=4 puts back
     * the exact vdiv=4 geometry (lines at 0, 1/3, 2/3, 1) that produced the
     * 2026-08-22 "three sections" report, plus ten more cuts.
     *
     * Matching the web properly needs tick marks and labels OUTSIDE the plot
     * (lv_scale, or extending the s_chart_y_hi/lo_label overlay pattern to
     * more positions), not div lines. Note the density problem before
     * trying: this chart is flex_grow residual height on a 480px page --
     * order 120-160px -- so 11 numeric temperature labels will not fit
     * legibly the way they do on the web's much taller canvas. Deciding
     * how many labels this display can actually carry is an open question,
     * not something to guess at. */
    lv_chart_set_div_line_count(s_ui_home_chart, 0, 0);
    lv_chart_set_point_count(s_ui_home_chart, UI_PAGE_HOME_CHART_POINTS);
    /* Same "desired" accent color the now-removed ui_page_history.c used
     * (ACCENT_3), carrying the PLANNED-ahead curve instead of a trailing
     * recorded setpoint -- see this file's header comment, part 2. Still no
     * on-chart legend text (the two corner overlay labels below cover Y/X
     * scale; a colour-to-series legend was ui_page_history.c's job and was
     * never rebuilt here after that page's removal -- actual is
     * UI_THEME_ACCENT_1, planned is UI_THEME_ACCENT_3, same colours this
     * codebase has used for those two concepts everywhere else). */
    s_ui_home_chart_actual_series = lv_chart_add_series(s_ui_home_chart, UI_THEME_ACCENT_1, LV_CHART_AXIS_PRIMARY_Y);
    /* 2026-08-23: was UI_THEME_ACCENT_3 (teal) -- now the exact web-match
     * purple (see UI_PAGE_HOME_PLAN_COLOR_HEX's own comment). This color is
     * also how ui_home_chart_draw_event_cb() (registered just below) picks the
     * planned line's draw task out from the actual line's, so it must stay
     * in sync with that macro. */
    s_ui_home_chart_planned_series =
        lv_chart_add_series(s_ui_home_chart, lv_color_hex(UI_PAGE_HOME_PLAN_COLOR_HEX), LV_CHART_AXIS_PRIMARY_Y);
    /* Dashing hook -- see ui_home_chart_draw_event_cb()'s own comment for why this is
     * the only way to get a dashed lv_chart series line in LVGL 9.5. */
    lv_obj_add_event_cb(s_ui_home_chart, ui_home_chart_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    for (uint32_t i = 0; i < UI_PAGE_HOME_CHART_POINTS; i++) {
        s_ui_home_chart_actual_pts[i] = LV_CHART_POINT_NONE;
        s_ui_home_chart_planned_pts[i] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_ext_y_array(s_ui_home_chart, s_ui_home_chart_actual_series, s_ui_home_chart_actual_pts);
    lv_chart_set_series_ext_y_array(s_ui_home_chart, s_ui_home_chart_planned_series, s_ui_home_chart_planned_pts);
    lv_chart_set_axis_range(s_ui_home_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_remove_flag(s_ui_home_chart, LV_OBJ_FLAG_SCROLLABLE);
    /* 2026-09-01 owner request: "remove the dots from the plot, keep the
     * lines". lv_chart draws a per-point marker (LV_PART_INDICATOR) sized
     * from that part's width/height style props, halved to a circle radius
     * -- grepped this tree's own lv_chart.c (draw_series_point()) to confirm
     * before writing this: width/height 0 collapses that radius to 0, i.e.
     * no marker drawn, while leaving LV_PART_ITEMS (the line draw path
     * ui_home_chart_draw_event_cb() above already hooks for the dashed-planned-line
     * trick) completely untouched -- lines stay exactly as they were. Set on
     * s_ui_home_chart itself (not per-series) since LVGL has no per-series indicator
     * style selector, same "the widget doesn't expose it per-series"
     * situation ui_home_chart_draw_event_cb()'s own comment already documents for
     * dashing -- but here there is nothing to intercept: zero-size applies
     * uniformly to both series, which is exactly what "keep the lines,
     * remove the dots" asks for on both of them. */
    lv_obj_set_style_size(s_ui_home_chart, 0, 0, LV_PART_INDICATOR);

    /* Temp/time scale overlay widgets -- see s_ui_home_chart_y_tick_labels' own
     * comment (near the static declarations above) for why these are plain
     * labels overlaid on the chart's own plot rather than an lv_scale
     * widget, and for the UI_PLAN.md 5.3 "all 11 ticks" decision this array
     * originally implemented (now UI_PAGE_HOME_Y_TICK_COUNT == 6, see the
     * 2026-09-01 note near that macro's definition). No background chip on
     * the tick labels (unlike the old hi/lo pair, and unlike
     * s_ui_home_chart_x_tick_labels below) -- that many opaque chips
     * stacked down the left edge would themselves start to read as a solid
     * bar over the plot; a scaled-down, plain-text label in the muted
     * secondary color (matching main_page.html's g.muted) is legible enough
     * against this theme's dark card background without one. Built HIDDEN;
     * ui_home_refresh_cb() (called once at the bottom of this function, via
     * ui_home_chart_set_y_ticks()/ui_home_chart_hide_y_ticks()) un-hides whichever have real
     * data before the page is ever shown, so there is no visible flash of
     * an unset "0" label on first paint -- same discipline the old hi/lo
     * pair used.
     *
     * montserrat_10 is the "smaller font" the owner decision asks for
     * (UI_PLAN.md 5.3). A first pass instead faked it with
     * lv_obj_set_style_transform_scale(154/256) because only montserrat_14
     * was compiled in -- but that scales an already-rendered 14px bitmap,
     * which aliases badly at ~60% on a real panel, and UI_PAGE_HOME_Y_TICK_COUNT
     * stacked labels is exactly where that would show. CONFIG_LV_FONT_MONTSERRAT_10 is now
     * enabled in the tracked sdkconfig.defaults (NOT in sdkconfig, which is
     * gitignored -- a font left only there means a clean clone silently
     * falls back to the 14px face and the labels overlap). */
    for (int i = 0; i < UI_PAGE_HOME_Y_TICK_COUNT; i++) {
        lv_obj_t *label = lv_label_create(s_ui_home_chart);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        s_ui_home_chart_y_tick_labels[i] = label;
    }
    /* ui_home_chart_y_tick_draw_event_cb()'s own comment explains the short tick
     * marks this hook draws to go with the labels above -- DRAW_POST so they
     * paint over the finished plot (matching main_page.html drawing its
     * ticks after the trace), same ordering the dashed-planned-line hook
     * below uses relative to its own draw phase. */
    lv_obj_add_event_cb(s_ui_home_chart, ui_home_chart_y_tick_draw_event_cb, LV_EVENT_DRAW_POST, NULL);

    /* Bottom time-axis ticks -- see s_ui_home_chart_x_tick_labels' own comment for
     * why four individually positioned labels replaced the old single
     * top-right span chip. Same semi-opaque background chip style the old
     * label used (unlike the Y ticks' plain text) -- these sit low in the
     * plot where trace lines are often passing through. montserrat_10, same
     * as the Y ticks, for the same overlap-avoidance reason. Built HIDDEN;
     * ui_home_chart_set_x_ticks() (called from ui_home_refresh_cb() before the page is ever
     * shown) un-hides whichever have real data. */
    for (int i = 0; i < UI_PAGE_HOME_X_TICK_COUNT; i++) {
        lv_obj_t *label = lv_label_create(s_ui_home_chart);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_bg_color(label, UI_THEME_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
        lv_obj_set_style_pad_hor(label, 2, 0);
        lv_obj_set_style_radius(label, 3, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        s_ui_home_chart_x_tick_labels[i] = label;
    }

    /* Current-position dot -- see its own static declaration comment above.
     * 6px filled circle, blue, matching main_page.html's current-position
     * marker; positioned every ui_home_refresh_cb() tick via
     * lv_chart_get_point_pos_by_id(), built hidden like the labels above
     * (only shown once ui_home_refresh_cb() has a real "now" to mark). Zero border
     * (a plain filled dot, not a ring) so it reads as a single solid marker
     * against the dark card background at this size. */
    s_ui_home_chart_now_dot = lv_obj_create(s_ui_home_chart);
    lv_obj_remove_flag(s_ui_home_chart_now_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui_home_chart_now_dot, 6, 6);
    lv_obj_set_style_radius(s_ui_home_chart_now_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui_home_chart_now_dot, lv_color_hex(0x2196f3), 0); /* plain blue, matches the web dot */
    lv_obj_set_style_bg_opa(s_ui_home_chart_now_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui_home_chart_now_dot, 0, 0);
    lv_obj_set_style_pad_all(s_ui_home_chart_now_dot, 0, 0);
    lv_obj_add_flag(s_ui_home_chart_now_dot, LV_OBJ_FLAG_HIDDEN);

    /* Legend -- see s_ui_home_chart_legend_row's own static-declaration comment
     * above for the row-count logic. Built LAST among s_ui_home_chart's children (a
     * later lv_obj child paints on top of earlier ones in LVGL, same as
     * everything else in this z-stack) so it always sits above the trace
     * lines and the now-dot, never gets drawn under them. Colours reuse
     * the exact series-creation symbols above (UI_THEME_ACCENT_1,
     * UI_PAGE_HOME_PLAN_COLOR_HEX) rather than duplicating literals -- if
     * either series colour ever changes, the swatch changes with it. */
    for (int i = 0; i < UI_PAGE_HOME_LEGEND_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_ui_home_chart);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_gap(row, 3, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *swatch = lv_obj_create(row);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(swatch, 6, 6);
        lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(swatch, i == 0 ? UI_THEME_ACCENT_1 : lv_color_hex(UI_PAGE_HOME_PLAN_COLOR_HEX), 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(swatch, 0, 0);
        lv_obj_set_style_pad_all(swatch, 0, 0);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        lv_label_set_text(label, i == 0 ? "Actual" : "Plan");

        s_ui_home_chart_legend_row[i] = row;
        s_ui_home_chart_legend_swatch[i] = swatch;
        s_ui_home_chart_legend_label[i] = label;
    }

    /* UI_PLAN.md 6.5 -- the right-quarter rail. Sibling of s_ui_home_chart
     * inside graph_row (25% width, card-backed so its 3/4 split against
     * the chart is visible/sampleable per 6.5's numeric-verification
     * recipe), column flow, 4px inner padding + 4px gap between children,
     * matching the section's 108px-inner-width arithmetic (116 - 2*4). */
    s_ui_home_rail = lv_obj_create(graph_row);
    lv_obj_remove_flag(s_ui_home_rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_ui_home_rail, lv_pct(25));
    lv_obj_set_height(s_ui_home_rail, lv_pct(100));
    lv_obj_set_style_bg_color(s_ui_home_rail, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_ui_home_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui_home_rail, 0, 0);
    lv_obj_set_style_radius(s_ui_home_rail, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_ui_home_rail, UI_THEME_SPACE_1, 0);
    lv_obj_set_style_pad_gap(s_ui_home_rail, UI_THEME_SPACE_1, 0);
    lv_obj_set_flex_flow(s_ui_home_rail, LV_FLEX_FLOW_COLUMN);

    /* "Relays" caption, montserrat_10 -- same small-caption style the
     * chart's Y-tick labels use. */
    lv_obj_t *relay_caption = lv_label_create(s_ui_home_rail);
    lv_obj_set_style_text_color(relay_caption, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_bg_opa(relay_caption, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(relay_caption, &lv_font_montserrat_10, 0);
    lv_label_set_text(relay_caption, "Relays");

    /* 4 relay pills, 24px wide, 4px gaps -- KILN_IO_RELAY_COUNT fixed at
     * build time (4 today), so no runtime-count gate is needed the way
     * ui_page_temperature.c's per-zone relay rows need one (that page
     * loops over a per-ZONE relay count; this rail is one pill per
     * physical relay, always KILN_IO_RELAY_COUNT of them). Colour only
     * (no room for UI_RELAY_DISPLAY numbering text at 24x18) -- ON is
     * UI_THEME_ACCENT_4 (same green the safety-relay line on the
     * Temperature page uses), off is a plain card-toned pill so the two
     * states read apart without a new colour. */
    lv_obj_t *relay_row = lv_obj_create(s_ui_home_rail);
    lv_obj_remove_flag(relay_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(relay_row, lv_pct(100));
    lv_obj_set_height(relay_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(relay_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(relay_row, 0, 0);
    lv_obj_set_style_pad_all(relay_row, 0, 0);
    lv_obj_set_style_pad_gap(relay_row, UI_THEME_SPACE_1, 0);
    lv_obj_set_flex_flow(relay_row, LV_FLEX_FLOW_ROW);
    for (uint32_t i = 0; i < KILN_IO_RELAY_COUNT; i++) {
        lv_obj_t *pill = lv_obj_create(relay_row);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(pill, 24, 18);
        lv_obj_set_style_bg_color(pill, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        lv_obj_set_style_radius(pill, 4, 0);
        lv_obj_set_style_pad_all(pill, 0, 0);
        s_ui_home_rail_relay_pill[i] = pill;
    }

    /* Per-zone blocks: name / temperature / duty bar, up to MAX31856_
     * CHANNEL_COUNT (3) built at once -- ui_home_rail_refresh() hides
     * whichever are past s_ui_home_zone_count, same "built once, hide the
     * unused tail" idiom the chart's tick-label arrays already use. */
    for (uint32_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        lv_obj_t *zrow = lv_obj_create(s_ui_home_rail);
        lv_obj_remove_flag(zrow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(zrow, lv_pct(100));
        lv_obj_set_height(zrow, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(zrow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(zrow, 0, 0);
        lv_obj_set_style_pad_all(zrow, 0, 0);
        lv_obj_set_style_pad_gap(zrow, 2, 0);
        lv_obj_set_flex_flow(zrow, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *name = lv_label_create(zrow);
        lv_obj_set_width(name, lv_pct(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(name, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_obj_set_style_bg_opa(name, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_10, 0);

        lv_obj_t *temp = lv_label_create(zrow);
        lv_obj_set_width(temp, lv_pct(100));
        lv_label_set_long_mode(temp, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(temp, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_bg_opa(temp, LV_OPA_TRANSP, 0);

        lv_obj_t *bar = lv_bar_create(zrow);
        lv_obj_set_width(bar, lv_pct(100));
        lv_obj_set_height(bar, 8);
        lv_bar_set_range(bar, 0, 100);
        lv_obj_set_style_bg_color(bar, UI_THEME_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, UI_THEME_ACCENT_1, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

        s_ui_home_rail_zone_row[i] = zrow;
        s_ui_home_rail_zone_name[i] = name;
        s_ui_home_rail_zone_temp[i] = temp;
        s_ui_home_rail_zone_bar[i] = bar;
    }

    /* Kiln-total watts line -- owner decision 6.8 item 1: shown only when
     * ds.power_valid, zero height when hidden (same idiom as
     * s_ui_home_trip_strip/s_ui_home_lag_notice above). montserrat_10, same
     * as the other rail captions -- this is a secondary readout, not the
     * page's headline number. */
    s_ui_home_rail_watts_label = lv_label_create(s_ui_home_rail);
    lv_obj_set_width(s_ui_home_rail_watts_label, lv_pct(100));
    lv_label_set_long_mode(s_ui_home_rail_watts_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_ui_home_rail_watts_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_bg_opa(s_ui_home_rail_watts_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(s_ui_home_rail_watts_label, &lv_font_montserrat_10, 0);
    lv_label_set_text(s_ui_home_rail_watts_label, "");
    lv_obj_add_flag(s_ui_home_rail_watts_label, LV_OBJ_FLAG_HIDDEN);

    /* UI_PAGE_HOME_RAIL_WORST_CASE_HEIGHT_PX -- pinned by check_ui_budget_
     * asserts.ps1. Matches section 6.5's arithmetic: caption(14) + gap(4) +
     * relay row(22) + gap(4) + 3 zone blocks (46 each = 138) + 2 inter-block
     * gaps (4 each = 8) + gap(4) + watts line (14, zero when hidden) = 208,
     * against the row's own height (228, itself
     * UI_THEME_PAGE_CONTENT_BUDGET_PX(268) - action_row(36) - gap(4)). */
#define UI_PAGE_HOME_RAIL_WORST_CASE_HEIGHT_PX (14 + 4 + 22 + 4 + (3 * 46) + (2 * 4) + 4 + 14)
_Static_assert(UI_PAGE_HOME_RAIL_WORST_CASE_HEIGHT_PX <=
                   (UI_THEME_PAGE_CONTENT_BUDGET_PX - 36 - UI_THEME_SPACE_1),
               "ui_page_home.c: dashboard rail no longer fits its column of the "
               "action_row-height-reduced content budget -- split across more pages, don't scroll");
#undef UI_PAGE_HOME_RAIL_WORST_CASE_HEIGHT_PX

    /* Progress bar -- see s_ui_home_progress_wrap's own static-declaration comment.
     * Sits directly under the chart, above action_row (the Start/Stop
     * button, still the LAST child / still pinned to the bottom). Built
     * hidden; ui_home_refresh_cb() un-hides it once there's a real run to report on. */
    s_ui_home_progress_wrap = lv_obj_create(content);
    lv_obj_remove_flag(s_ui_home_progress_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_ui_home_progress_wrap, lv_pct(100));
    lv_obj_set_height(s_ui_home_progress_wrap, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_ui_home_progress_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui_home_progress_wrap, 0, 0);
    lv_obj_set_style_pad_all(s_ui_home_progress_wrap, 0, 0);
    lv_obj_set_style_pad_top(s_ui_home_progress_wrap, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(s_ui_home_progress_wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_ui_home_progress_wrap, 2, 0);
    lv_obj_add_flag(s_ui_home_progress_wrap, LV_OBJ_FLAG_HIDDEN);

    /* Track -- matches main_page.html's .progress-track (10px, rounded,
     * bordered). UI_THEME_COLOR_CARD (not a plain black) so it reads
     * against this page's dark background the same way the chart's own
     * card background does. */
    s_ui_home_progress_track = lv_obj_create(s_ui_home_progress_wrap);
    lv_obj_remove_flag(s_ui_home_progress_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_ui_home_progress_track, lv_pct(100));
    lv_obj_set_height(s_ui_home_progress_track, 8);
    lv_obj_set_style_bg_color(s_ui_home_progress_track, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_ui_home_progress_track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui_home_progress_track, 0, 0);
    lv_obj_set_style_radius(s_ui_home_progress_track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_ui_home_progress_track, 0, 0);

    /* Fill -- a child positioned at the track's left edge, width set every
     * ui_home_refresh_cb() tick as a fraction of the track's own width (0-100%, or
     * pinned to 100% in a distinct muted color for the "indeterminate"
     * case -- this page has no spare cycles for the web's animated stripe,
     * so "we don't know" is communicated by color instead of motion). */
    s_ui_home_progress_fill = lv_obj_create(s_ui_home_progress_track);
    lv_obj_remove_flag(s_ui_home_progress_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ui_home_progress_fill, 0, 0);
    lv_obj_set_height(s_ui_home_progress_fill, lv_pct(100));
    lv_obj_set_width(s_ui_home_progress_fill, 0);
    lv_obj_set_style_bg_color(s_ui_home_progress_fill, UI_THEME_ACCENT_1, 0);
    lv_obj_set_style_bg_opa(s_ui_home_progress_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui_home_progress_fill, 0, 0);
    lv_obj_set_style_radius(s_ui_home_progress_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_ui_home_progress_fill, 0, 0);

    /* Elapsed/remaining text -- one line ("Elapsed 12:34    3:45 left
     * (estimate)"), unlike main_page.html's two-span flex row: this page's
     * ~320px width has no room to spare for a second column, and one label
     * updated wholesale each tick is simpler than two kept in sync. */
    s_ui_home_progress_label = lv_label_create(s_ui_home_progress_wrap);
    lv_obj_set_width(s_ui_home_progress_label, lv_pct(100));
    lv_label_set_long_mode(s_ui_home_progress_label, LV_LABEL_LONG_DOT);
    /* No explicit font: LV_FONT_MONTSERRAT_12 is not confirmed enabled in
     * this build's lv_conf (only checked-in for CI/example configs, not this
     * app's), so this stays on LV_FONT_DEFAULT (montserrat_14, ui_theme.h)
     * like every other label on this page rather than risk a missing-glyph
     * fallback. */
    lv_obj_set_style_text_color(s_ui_home_progress_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_ui_home_progress_label, "");

    /* 2026-08-21 owner request: "remove the zone [] section" -- no zone row
     * widgets are built on this page any more (see s_ui_home_zone_count's own comment
     * near its declaration for what's KEPT: the count itself, still used by
     * ui_home_refresh_cb()'s idle branch to gate the chart's representative-zone
     * dot). s_ui_home_zone_count == 0 is handled sensibly by simply having nothing to
     * gate -- ui_home_refresh_cb() skips the representative-temp lookup and the
     * chart shows no idle dot, which is the honest "nothing configured"
     * state; there is no separate "No zones configured" label to build here
     * any more; each configured zone's live temperature/heater status is
     * still visible via the web dashboard (main_page.html), which was never
     * subject to this page's height budget. */
    s_ui_home_zone_count = zones_config_get_thermo_count();
    if (s_ui_home_zone_count > MAX31856_CHANNEL_COUNT) {
        s_ui_home_zone_count = MAX31856_CHANNEL_COUNT; /* defensive; should never trip */
    }

    /* Single merged Start/Stop button -- one user-visible request ("the
     * start stop button should be one button on the lcd"). Menu moved off
     * this row entirely into the status bar as a gear (see the status-bar
     * build above and ui_home_menu_nav_cb()) per the other request, so this row is
     * now just the one button. ui_home_build_button() still grows it across the
     * row's width via flex_grow(1).
     *
     * 2026-08-22: the run-state card, progress bar, and bottom spacer that
     * used to sit between the chart and this row are gone (see the chart's
     * own build comment above and ui_home_refresh_cb()'s tail comment) -- action_row
     * is now the chart's very next sibling. It still stays pinned to
     * `content`'s bottom edge: the chart above it carries flex_grow(1), so it
     * (not a dedicated spacer) claims whatever vertical space this button
     * does not need.
     *
     * Drawn vs effective button height: ui_home_build_button()'s
     * ui_theme_apply_touch_area(btn, false) call reads back the button's
     * real (post-layout) height, 36px here, which is BELOW
     * UI_THEME_MIN_TOUCH_TARGET_PX (72px) -- by ui_theme.c's own
     * non-compact-layout branch this is exactly the case it extends
     * further for: smaller_edge=36 < 72, so needed=(72-36)/2=18,
     * generous=UI_THEME_PADDING_PX*3=24, and ext_click_area is set to
     * max(18,24)=24px on every side (verified against ui_theme.c's actual
     * arithmetic, not assumed). Effective clickable height is therefore
     * 36 + 2*24 = 84px, comfortably clearing the 72px minimum this whole
     * mechanism exists to guarantee. The drawn box is what the operator sees
     * and taps confidently within; the extra 24px halo on every side (48px on
     * top, now reaching upward into the chart's own bottom padding) is there
     * so a slightly-off tap near the visual edge still registers -- the chart
     * is a plain lv_chart (not CLICKABLE by default in LVGL 9.5), so that
     * halo cannot steal a tap meant for anything inside it. */
    lv_obj_t *action_row = lv_obj_create(content);
    lv_obj_set_width(action_row, lv_pct(100));
    lv_obj_set_height(action_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(action_row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);
    /* Pause/Resume -- built BEFORE the fire button, matching app.js's sticky
     * bar ordering (pause/resume, then stop). Built hidden; ui_home_refresh_cb()
     * un-hides it for RUNNING/PAUSED only, at which point it shares
     * action_row's width evenly with the fire button (both flex_grow(1) via
     * ui_home_build_button()) -- hidden costs zero row width, same discipline as
     * s_ui_home_trip_strip/s_ui_home_progress_wrap elsewhere on this page, so a hidden pause
     * button never leaves the Start/Stop button looking off-center. */
    /* UI_PLAN.md 6.1: selected-profile name, first child so it absorbs the
     * row's leftover width (flex_grow(1)) while Pause/Start below are pinned
     * to their existing ~96px drawn size -- ui_home_build_button() always
     * grows its button to 1, so Pause/Start are overridden back to a fixed
     * width right after construction rather than adding a second growth
     * mode to that shared helper for its one remaining caller pair. Tap
     * opens the already-landed 6.2 picker page in pick mode
     * ("profile_picker" -- see kiln_ui_register_page() in kiln_ui.c). */
    s_ui_home_profile_btn = ui_home_build_button(action_row, "--", UI_THEME_ACCENT_2, ui_home_profile_btn_cb, 36,
                                &s_ui_home_profile_label);
    /* LONG_CLIP, not LONG_DOT: UI_PLAN.md 6.2 records that LONG_DOT's
     * lv_obj_get_self_height() -> lv_label_set_long_mode() ->
     * lv_obj_invalidate() -> lv_event_send() -> cleanup_event_list() ->
     * lv_malloc_core() chain is reachable from ui_home_refresh_cb() through
     * lv_obj_update_layout() and re-breaks the lvgl task's 4880 B stack
     * ceiling -- and this label is re-texted from that very callback every
     * tick, which is exactly the reachability that warning names. Nothing is
     * lost: PROFILE_NAME_MAX_LEN is 15, and the button stays 264px wide even
     * with Pause showing (about 33 montserrat_14 characters), so a stored
     * name can never reach the clip boundary in the first place. */
    lv_label_set_long_mode(s_ui_home_profile_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_ui_home_profile_label, lv_pct(100));
    lv_obj_set_style_text_align(s_ui_home_profile_label, LV_TEXT_ALIGN_CENTER, 0);
    ui_theme_apply_touch_area(s_ui_home_profile_btn, true);

    /* UI_PLAN.md 6.1 -- wire the picker's PICK-mode callback (the "a later
     * wave wires the caller" that ui_page_profile_picker.h's header comment
     * refers to). Without this, s_pick_cb stays NULL and
     * row_name_clicked_cb() does nothing at all in pick mode: the button
     * would open a list that cannot be picked from. Set once here, matching
     * this page's build-once lifetime. */
    ui_page_profile_picker_set_pick_cb(ui_home_profile_picked_cb);

    s_ui_home_pause_btn = ui_home_build_button(action_row, "Pause", UI_THEME_ACCENT_1, ui_home_pause_resume_btn_cb, 36,
                                &s_ui_home_pause_btn_label);
    lv_obj_add_flag(s_ui_home_pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_grow(s_ui_home_pause_btn, 0);
    lv_obj_set_width(s_ui_home_pause_btn, 96);

    s_ui_home_fire_btn = ui_home_build_button(action_row, "Start", UI_THEME_ACCENT_4, ui_home_fire_btn_cb, 36, &s_ui_home_fire_btn_label);
    lv_obj_set_flex_grow(s_ui_home_fire_btn, 0);
    lv_obj_set_width(s_ui_home_fire_btn, 96);

    /* Pages are never torn down (kiln_ui.h's header comment), so a timer
     * created once here and never deleted matches that lifetime. */
    lv_timer_create(ui_home_refresh_cb, UI_PAGE_HOME_REFRESH_MS, NULL);
    ui_home_refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
