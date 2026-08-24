#include "ui_page_home.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include <math.h>

#include "MAX31856.h"
#include "boot_button.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "kiln_ui.h"
#include "ui_confirm.h"
#include "watchdog_cfg.h"
#include "ui_page_config.h"
#include "safety_trip_words.h"
#include "profile_executor.h"
#include "profile_feasibility.h"
#include "profiles_http.h"
#include "ui_page_home_graph.h"
#include "run_state.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "wifi_status_ui.h"
#include "zones_http.h"

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
//     fire_btn_cb() and the status-bar build in ui_page_home_build()).
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
//     s_chart_planned_series/s_chart_planned_pts (still ACCENT_3, same color
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
//     (plan_lookup()); actual is looked up per-bucket from the history ring
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
//     (which can hold up to HISTORY_MAX_SAMPLES=2880 entries -- far too big
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
// unchanged, only what sits above it changed. See refresh_cb()'s tail comment
// for the safety-trip-visibility trade-off this removal carries (still
// visible on ui_page_safety.c, just no longer pre-empting this page).
//
// Content-container scrolling is explicitly disabled
// (LV_OBJ_FLAG_SCROLLABLE cleared on both `scr` and `content` in
// ui_page_home_build()) now that the content is sized to fit -- if a future
// change re-overflows this page, LVGL will clip the overflow instead of
// silently turning scrollable again, which is a visible bug report waiting
// to happen rather than a silent regression.

static const char *TAG = "ui_page_home";

/* Refresh cadence for the live numbers on this page. 1 Hz matches
 * PROFILE_EXECUTOR_TICK_MS (profile_executor.h) -- no point refreshing
 * faster than the control loop that produces the numbers changes them. */
#define UI_PAGE_HOME_REFRESH_MS 1000

/* Home page's own chart -- 2026-08-21, see this file's header comment
 * ("CHART RETURNS"); ui_page_history.c (the separate, larger "Temperature
 * History" page this was once windowed smaller than) was removed 2026-08-22
 * per owner request, so this is now the ONLY trend chart on the LCD. Point
 * count stays modest (DRAM cost -- see s_chart_actual_pts/s_chart_planned_pts'
 * own comment) even though the chart's on-screen HEIGHT is no longer fixed:
 * see ui_page_home_build()'s chart-build comment for why height is now
 * flex_grow(1) instead of a compile-time pixel constant. */
#define UI_PAGE_HOME_CHART_POINTS 30

/* 2026-08-21 owner request: "remove the zone a temp section ... also remove
 * the box that says no profile running". The per-zone ROW WIDGETS are gone
 * (no more build_zone_row()/zone_accent(), no more s_zone[] widget array) --
 * but s_zone_count is NOT gone: refresh_cb()'s idle branch still uses it to
 * gate the "representative zone" lookup that feeds the chart's single idle
 * dot (the "first configured zone" convention this file's header comment
 * documents, shared with ui_page_history.c's own idle fallback). Keeping the
 * count without the widgets means that gate still reads honestly (0
 * configured zones -> no dot, not a NULL-deref on a widget that was never
 * built) without reintroducing the removed UI. */
static uint8_t s_zone_count; /* zones_config_get_thermo_count() at build time */

/* Trip strip -- hidden unless a live trip is present; see its creation in
 * ui_page_home_build() for why it is hidden rather than absent. */
static lv_obj_t *s_trip_strip;
static lv_obj_t *s_chart;                     /* home page's compact chart -- actual + planned-ahead */
static lv_chart_series_t *s_chart_actual_series;
static lv_chart_series_t *s_chart_planned_series;
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
 * desired_c -- see refresh_cb()'s plan_lookup()/plan_pts usage. No size
 * change from part 1 (still one int32 per bucket). */
static int32_t s_chart_actual_pts[UI_PAGE_HOME_CHART_POINTS];
static int32_t s_chart_planned_pts[UI_PAGE_HOME_CHART_POINTS];

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
 * intervals, but this page's Y range is fully dynamic (refresh_cb() rewrites
 * it from live data every tick, both branches below) and re-deriving a tick
 * spacing + a fresh text_src array every second just to show two numbers is
 * more moving parts than the two labels below, with more ways for the tick
 * labels to silently drift out of sync with the range that drives them. Two
 * labels directly overwritten with lv_label_set_text() every refresh_cb()
 * call are trivially kept in sync BY CONSTRUCTION: they are written from the
 * exact same lo/hi (or v-10/v+10) values passed to
 * lv_chart_set_axis_range() in the same code path, not read back from the
 * chart afterward. Built as CHILDREN of s_chart (not `content` siblings), so
 * they overlay the plot rather than consuming their own row height -- this
 * page has no spare height budget for a fourth chart-adjacent row (see the
 * action-row comment's arithmetic). Small semi-opaque background chips (not
 * fully transparent) so the digits stay legible against whichever part of
 * the actual/planned traces happens to be under that corner. */
static lv_obj_t *s_chart_y_hi_label; /* top-left: current Y-axis max, in the user's unit_pref */
static lv_obj_t *s_chart_y_lo_label; /* bottom-left: current Y-axis min, in the user's unit_pref */
static lv_obj_t *s_chart_x_label;    /* top-right: the plotted window's time span, "0:00-MM:SS" */

/* 2026-08-23 owner request ("the LCD profile graph should look like the web
 * GUI's profile graph"): a small filled blue dot marking the current
 * position along the planned curve, matching main_page.html's own current-
 * position marker. A plain circular lv_obj (not a chart point-bullet style,
 * which would also mark every OTHER point on the series) positioned every
 * refresh_cb() tick via lv_chart_get_point_pos_by_id() -- that call does the
 * axis-range-to-pixel mapping chart-internally, so this file does not
 * reimplement lv_chart's own y = f(value) math a second time. Hidden
 * whenever there is no live run to mark a position on (idle, or no plan
 * points), same discipline as the Y/X overlay labels above. */
static lv_obj_t *s_chart_now_dot;

/* Web-match purple for the planned-profile curve -- main_page.html's own
 * planned-curve stroke is `ctx.strokeStyle = '#96c'` (CSS 3-digit shorthand,
 * i.e. #9966CC), read directly out of that file at ~line 1484. Kept as its
 * own named color here rather than reusing UI_THEME_ACCENT_2 (a similar but
 * NOT identical purple, 0xa15fd6) -- matching the reference image byte-for-
 * byte is the point of this task, an approximate purple would not be. */
#define UI_PAGE_HOME_PLAN_COLOR_HEX 0x9966cc

/* Dash pattern for the planned curve, in pixels -- matches main_page.html's
 * `ctx.setLineDash([6, 4])` exactly (6 on, 4 off). See
 * chart_draw_event_cb()'s own comment for how this gets applied: LVGL's
 * lv_chart has no built-in dashed-series style, so this is injected via a
 * LV_EVENT_DRAW_TASK_ADDED hook rather than a style property. */
#define UI_PAGE_HOME_PLAN_DASH_WIDTH_PX 6
#define UI_PAGE_HOME_PLAN_DASH_GAP_PX   4

static lv_obj_t *s_fire_btn;      /* merged Start/Stop button */
static lv_obj_t *s_fire_btn_label;
static ui_topbar_t s_topbar;       /* Menu gear icon, shared chrome -- see ui_topbar.h */

/* WiFi/IP/mDNS status readout, in the status bar. Text comes from
 * wifi_status_ui_get_text() (wifi_status_ui.c) -- see that module's header
 * comment for the underlying getters and the TODO.md 10.1a shared-backend
 * rule. */
static lv_obj_t *s_status_label;

/* mm:ss for anything under an hour (this page's numbers are segment-scale,
 * not multi-day), hh:mm:ss beyond that -- matches main_page.html's
 * fmtDuration() shape closely enough for web/LCD parity (TODO.md 10.5)
 * without pulling in the exact same JS-derived format. */
static void format_duration(uint32_t seconds, char *out, size_t out_cap)
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
static float freezing_point_disp(unit_pref_t unit)
{
    return (unit == UNIT_PREF_FAHRENHEIT) ? 32.0f : 0.0f;
}

/* Shared by do_start() and the confirmation dialog builder below -- both need
 * the exact same fallback chain (whatever's currently known this boot
 * (non-idle profile_id), else the last boot record), and the dialog has to
 * name the SAME profile the button is actually about to start, not a second
 * guess at it. Returns false (out_id untouched) if neither source has one. */
static bool resolve_start_profile_id(uint8_t *out_id)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    if (st.state != PROFILE_EXEC_IDLE) {
        *out_id = st.profile_id;
        return true;
    }
    run_state_record_t rec;
    if (run_state_get_boot_record(&rec)) {
        *out_id = rec.profile_id;
        return true;
    }
    return false;
}

static void do_start(void)
{
    /* Same action function dashboard_http.c's POST /api/profile_exec/start
     * handler calls (profile_exec_start_post_handler()) -- TODO.md 10.1a.
     *
     * No picker on this page anymore (see this file's header comment for
     * why) -- always the fallback chain: whatever's currently known this
     * boot (non-idle profile_id), else the last boot record. An operator
     * who wants a *different* profile than either of those has to use the
     * web dashboard's picker. */
    uint8_t id = 0;
    if (!resolve_start_profile_id(&id)) {
        ESP_LOGW(TAG, "Start pressed with no known profile id -- nothing has run this boot "
                      "and no picker on this page (TODO.md 10.3's no-scroll rewrite)");
        return;
    }

    char err_msg[64] = "";
    if (!profile_executor_run(id, err_msg, sizeof(err_msg))) {
        ESP_LOGW(TAG, "profile_executor_run(%u) refused: %s", id, err_msg);
    }
}

static void do_stop(void)
{
    /* Same action function dashboard_http.c's POST /api/profile_exec/stop
     * handler calls -- TODO.md 10.1a. profile_executor_halt() itself is the
     * only gate on a stop today -- no confirmation dialog exists anywhere in
     * this call path (see this file's header/report note: flagged back to
     * the requester rather than silently added here). */
    profile_executor_halt();
}

/* ---- Start/Stop confirmation overlay --------------------------------
 *
 * Both actions were firing immediately with no confirmation anywhere in this
 * call path (do_start()/do_stop()'s own header comments used to flag this
 * back to the requester rather than silently adding one). Starting energises
 * heaters for hours; stopping mid-firing aborts a load. Both now go through
 * a modal confirm/cancel step first, via the shared ui_confirm.c helper
 * (factored out of what used to be a hand-rolled lv_msgbox pair here, so the
 * new Profiles-hub detail page's START action -- see ui_page_profile_detail.c
 * -- reuses the exact same dialog instead of a second copy-pasted
 * implementation). See ui_confirm.h for the FLEX TRAP / cancel-safe-by-
 * default rationale that used to live in this comment. */

/* Second dialog's Yes -- the operator has now read the watchdog-panic-
 * disabled warning too. Same chaining pattern as ui_page_kiln_cfg_setup.c's
 * apply_no_safety_ack_cb()/apply_confirm_yes_cb() pair for its own
 * no-safety-processor warning. */
static void confirm_start_watchdog_ack_cb(void *user_data)
{
    (void)user_data;
    do_start();
}

static void confirm_start_yes_cb(void *user_data)
{
    (void)user_data;

    /* watchdog_cfg.h: a second, EXTRA confirmation on top of the one the
     * operator just answered, only when the task-watchdog panic is
     * currently disabled (dev/bench mode) -- same text on this dialog, the
     * web start flow (main_page.html), and watchdog_cfg.h's own macro
     * definition; App/test/lint_pages.js pins the web copy against the C
     * macro so they cannot drift. */
    if (watchdog_cfg_panic_disabled()) {
        ui_confirm_params_t warn = {
            .title = "Task-Watchdog Panic Disabled",
            .body = WATCHDOG_CFG_FIRING_WARNING,
            .confirm_label = "Start Anyway",
            .confirm_color = UI_THEME_ACCENT_5,
            .on_confirm = confirm_start_watchdog_ack_cb,
            .user_data = NULL,
        };
        ui_confirm_show(&warn);
        return;
    }

    do_start();
}

static void confirm_stop_yes_cb(void *user_data)
{
    (void)user_data;
    do_stop();
}

static void show_start_confirm(void)
{
    uint8_t id = 0;
    bool have_id = resolve_start_profile_id(&id);

    profile_t prof;
    bool have_prof = have_id && profiles_http_get(id, &prof);

    char body[256];
    if (have_prof) {
        /* Zones this profile drives -- cheap here (profiles_http_get() is a
         * plain NVS-backed struct copy, same call profile_executor.c itself
         * uses to run the profile, not a second read path), unlike trying to
         * derive it from profile_exec_status_t, which only carries a
         * meaningful zone_mask once the run has actually started. */
        char zones_buf[96];
        size_t zlen = 0;
        zones_buf[0] = '\0';
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && zlen < sizeof(zones_buf) - 1; zi++) {
            if (!(prof.zone_mask & (1u << zi))) {
                continue;
            }
            char name[16];
            const char *zname = (zones_config_get_name(zi, name, sizeof(name)) && name[0]) ? name : NULL;
            char piece[24];
            if (zname) {
                snprintf(piece, sizeof(piece), "%s%s", zlen ? ", " : "", zname);
            } else {
                snprintf(piece, sizeof(piece), "%sZone %u", zlen ? ", " : "", (unsigned)zi);
            }
            size_t piece_len = strlen(piece);
            if (zlen + piece_len < sizeof(zones_buf)) {
                memcpy(zones_buf + zlen, piece, piece_len + 1);
                zlen += piece_len;
            }
        }
        snprintf(body, sizeof(body), "Start \"%s\" now? This will energise %s for the duration of the "
                                      "firing, which can be hours.",
                 prof.name, zones_buf[0] ? zones_buf : "no zones");
    } else if (have_id) {
        /* Have an id but profiles_http_get() failed (slot no longer stored,
         * e.g. deleted between boot and now) -- name what we can rather than
         * a blank, and say plainly the rest could not be read. */
        snprintf(body, sizeof(body), "Start profile id %u now? Its saved details could not be read, "
                                      "but starting will energise heaters for the duration of the "
                                      "firing, which can be hours.",
                 (unsigned)id);
    } else {
        /* No non-idle profile this boot and no boot record either -- the
         * honest answer is "nothing to name," not a blank dialog. */
        snprintf(body, sizeof(body), "No profile can be identified to start (nothing has run yet this "
                                      "boot). Use the web dashboard's profile picker to choose one.");
    }

    ui_confirm_params_t params = {
        .title = "Confirm Start",
        .body = body,
        .confirm_label = "Start",
        .confirm_color = UI_THEME_ACCENT_4, /* start-green, matches the fire button */
        .on_confirm = confirm_start_yes_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&params);
}

static void show_stop_confirm(void)
{
    ui_confirm_params_t params = {
        .title = "Confirm Stop",
        .body = "Stop this firing now? This aborts the run in progress -- it cannot "
                "be resumed, and the load will not finish firing.",
        .confirm_label = "Stop",
        .confirm_color = UI_THEME_ACCENT_5, /* stop-red, matches the fire button */
        .on_confirm = confirm_stop_yes_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&params);
}

/* Merged Start/Stop button (single user-visible request: "the start stop
 * button should be one button on the lcd"). Idle/Done/Faulted -> "Start" +
 * confirm -> do_start(); Running/Paused -> "Stop" + confirm -> do_stop().
 * One callback reads current state at click time rather than two callbacks
 * each assuming a fixed action, so a state change between refresh_cb() ticks
 * and the actual tap can never fire the stale action -- the same reasoning
 * now also decides which of the two confirmation dialogs to show. */
static void fire_btn_cb(lv_event_t *e)
{
    (void)e;
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    if (st.state == PROFILE_EXEC_RUNNING || st.state == PROFILE_EXEC_PAUSED) {
        show_stop_confirm();
    } else {
        show_start_confirm();
    }
}

static void menu_nav_cb(lv_event_t *e)
{
    (void)e;
    /* Single "Menu" button replaces the old separate Configuration and
     * Temperature nav buttons -- see this file's header comment on the
     * budget this page is fit to. Temperature (manual relay control) is now
     * reached via ui_page_config.c's hub, one tap further than before.
     *
     * Rewind the hub first: it is built once and keeps its paging position,
     * so without this, Menu drops you on whichever hub page you were last on.
     * Back from a sub-page deliberately still returns to the page you left
     * from -- only Menu means "take me to the top of the menu". */
    ui_page_config_reset_to_first_page();
    kiln_ui_show("config");
}

/* out_label, if non-NULL, receives the button's label widget so a caller can
 * change its text/color later (the merged fire button's state-driven text --
 * see fire_btn_cb()/refresh_cb()).
 *
 * height_px, added 2026-08-21 (single user-visible request: "the start
 * button should sit at the bottom and be about half as tall so the graph can
 * be larger" -- see ui_page_home_build()'s action-row comment for why a
 * drawn height below UI_THEME_MIN_TOUCH_TARGET_PX is acceptable here and how
 * the effective touch area is kept whole regardless). Every other caller of
 * this helper used to get UI_THEME_MIN_TOUCH_TARGET_PX unconditionally; this
 * page has exactly one caller (the merged fire button), so rather than add a
 * second helper for "small button," the one call site now just says what
 * height it wants. */
static lv_obj_t *build_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb,
                               int32_t height_px, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, height_px);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (out_label) {
        *out_label = label;
    }

    /* TODO.md 10.4's touch hit-area helper (ui_theme.c) -- this is a sparse
     * button row, not a dense grid, so compact_layout=false. Forces an
     * immediate layout pass first: ui_theme_apply_touch_area() reads back
     * lv_obj_get_width/height(), which flex_grow leaves unresolved until
     * layout actually runs. */
    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, false);

    return btn;
}

/* Piecewise-linear sample of a profile_feasibility_plan_curve() point list at
 * time t (seconds from run start). pts[] is ordered by increasing t (the
 * order profile_feasibility_plan_curve() writes them in) -- t before the
 * first point holds the first point's value (there is no "before start"),
 * and t past the last point holds the LAST point's value (the chart's
 * horizon is derived from the last point's own time, so this only matters
 * for a t that lands exactly on it due to float rounding). A zero-width
 * step (t1<=t0, an unknown-duration ramp segment -- see
 * profile_feasibility.h's HONESTY RULE) holds the step's arrival value
 * rather than dividing by zero. Returns NAN if pts is empty. */
static float plan_lookup(const profile_plan_point_t *pts, size_t n, float t)
{
    if (n == 0) {
        return NAN;
    }
    if (t <= pts[0].t) {
        return pts[0].c;
    }
    for (size_t i = 1; i < n; i++) {
        if (t <= pts[i].t) {
            float t0 = pts[i - 1].t, t1 = pts[i].t;
            float c0 = pts[i - 1].c, c1 = pts[i].c;
            if (t1 <= t0) {
                return c1;
            }
            float f = (t - t0) / (t1 - t0);
            return c0 + f * (c1 - c0);
        }
    }
    return pts[n - 1].c;
}

/* LV_EVENT_DRAW_TASK_ADDED hook on s_chart -- the only way to get a dashed
 * series line out of LVGL 9.5's lv_chart (grepped this tree's own
 * lv_chart.c/.h first: draw_series_line() builds one lv_draw_line_dsc_t per
 * series from LV_PART_ITEMS style properties and calls lv_draw_line() with
 * it -- there is no per-series "dashed" flag or style selector to reach any
 * other way). lv_draw_line_dsc_t itself DOES support dashing natively
 * (dash_width/dash_gap fields, draw/lv_draw_line.h) -- LVGL's line drawing
 * primitive can dash, the chart widget just never exposes it -- so this
 * intercepts each line draw task after the chart builds it and, for the one
 * belonging to the planned-profile series (identified by its already-applied
 * color, set from s_chart_planned_series's own color at lv_chart_add_series()
 * time below), turns on the same 6-on/4-off dash main_page.html's
 * `ctx.setLineDash([6, 4])` uses. Every other draw task (the actual-series
 * line, any bullets, the card background/border) passes through untouched. */
static void chart_draw_event_cb(lv_event_t *e)
{
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    lv_draw_line_dsc_t *line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (line_dsc == NULL) {
        return; /* not a line task (rect/label/etc) -- nothing to dash */
    }
    lv_color_t plan_color = lv_color_hex(UI_PAGE_HOME_PLAN_COLOR_HEX);
    if (!lv_color_eq(line_dsc->color, plan_color)) {
        return; /* the actual-series line (or anything else), leave solid */
    }
    line_dsc->dash_width = UI_PAGE_HOME_PLAN_DASH_WIDTH_PX;
    line_dsc->dash_gap = UI_PAGE_HOME_PLAN_DASH_GAP_PX;
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* wifi_status_ui.h -- TODO.md 10.9 factored this formatter out of this
     * file into its own shared module so ui_page_network.c can call the
     * exact same "human-readable WiFi state" text instead of a second copy
     * of the switch statement that used to live here. */
    char status_buf[64];
    wifi_status_ui_get_text(status_buf, sizeof(status_buf));
    lv_label_set_text(s_status_label, status_buf);

    /* Same plain-C getters dashboard_http.c's GET /api/status and
     * GET /api/profile_exec handlers call -- TODO.md 10.1a's shared-backend
     * rule, not a reimplementation. */
    dashboard_status_t ds;
    dashboard_get_status(&ds);
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    /* Safety-trip strip. Same gate the removed state-card banner used, kept
     * verbatim on purpose: diag_age_ms < SAFETY_LINK_STALE_MS, because a
     * STALE diag_state == TRIPPED is a silent link, not a live trip, and
     * painting the two identically is exactly the "guard firing vs. dead
     * peer" confusion this codebase has been careful to avoid elsewhere.
     *
     * Hidden (not blanked) when clear: LVGL skips hidden children in flex
     * layout, so this costs zero height whenever nothing is wrong, which is
     * what lets the chart still reach all the way down to the Start button. */
    if (s_trip_strip != NULL) {
        bool safety_tripped = ds.diag_ever_received && ds.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED &&
                              ds.diag_age_ms < SAFETY_LINK_STALE_MS;
        // boot_button.h's OTA-auth bypass window takes PRIORITY over the
        // safety-trip text below: while it's open, anyone on the network can
        // flash firmware with no password check at all, which this project
        // treats as more urgent than an ordinary safety trip -- see
        // main_page.html's renderBootButtonBypass() for the same ordering on
        // the web side. Short on purpose (this strip's buffer is 96 chars
        // and this page must never scroll, 480x320) -- states the fact and
        // the remaining time, not the full recovery-hatch explanation.
        if (boot_button_ota_bypass_active()) {
            char bypass_buf[96];
            uint32_t rem_s = boot_button_bypass_remaining_ms() / 1000u;
            snprintf(bypass_buf, sizeof(bypass_buf),
                     "OTA PASSWORD BYPASSED -- closes in %lu:%02lu",
                     (unsigned long)(rem_s / 60u), (unsigned long)(rem_s % 60u));
            lv_label_set_text(s_trip_strip, bypass_buf);
            lv_obj_remove_flag(s_trip_strip, LV_OBJ_FLAG_HIDDEN);
        } else if (safety_tripped) {
            char trip_buf[96];
            snprintf(trip_buf, sizeof(trip_buf), "SAFETY TRIP -- %s",
                     safety_trip_words_short(ds.diag_trip_reason));
            lv_label_set_text(s_trip_strip, trip_buf);
            lv_obj_remove_flag(s_trip_strip, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_trip_strip, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Compact home chart -- see this file's header comment ("DESIRED SERIES
     * + PROGRESS BAR RETURN, part 2") for the idle-dot vs. whole-run-timeline
     * split. Same state==IDLE && history_count==0 gate ui_page_history.c
     * uses, so both pages flip from dot to timeline at the exact same
     * instant. profile_feasibility_plan_curve() is also used by the
     * progress-bar block further down -- computed once here and passed down
     * rather than called twice per tick. 2026-08-22: the progress bar itself
     * is gone (see this function's tail comment), but plan_pts/plan_n are
     * still needed for the chart's planned-ahead series and time-axis label,
     * so the call stays -- only its total-seconds return value is now
     * discarded. */
    profile_plan_point_t plan_pts[1 + 2 * PROFILE_MAX_SEGMENTS];
    size_t plan_n = 0;
    if (st.state == PROFILE_EXEC_IDLE && profile_executor_get_history_count() == 0) {
        for (uint32_t i = 1; i < UI_PAGE_HOME_CHART_POINTS; i++) {
            s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
            s_chart_planned_pts[i] = LV_CHART_POINT_NONE;
        }
        /* Representative zone -- same "first configured zone" convention
         * build_zone_row()'s loop and ui_page_history.c's idle-fallback
         * both use; zone_mask is meaningless before a profile has ever
         * started this boot, so there is no mask to read yet. */
        float val = NAN;
        if (s_zone_count > 0) {
            for (size_t i = 0; i < ds.channel_count; i++) {
                if (ds.channels[i].channel == 0) {
                    if (ds.channels[i].valid && !ds.channels[i].stale) {
                        val = unit_pref_convert(ds.channels[i].temp_c, ds.temp_unit, UNIT_PREF_KIND_ABSOLUTE);
                    }
                    break;
                }
            }
        }
        /* No planned curve without a running profile -- same rule
         * ui_page_history.c's idle branch documents. */
        s_chart_planned_pts[0] = LV_CHART_POINT_NONE;
        /* No time axis in the idle single-dot case -- there is nothing to
         * span yet (the dot never moves, see this file's header comment), so
         * a "0:00-0:00" label would be a confident lie rather than a scale.
         * Hidden here unconditionally; the running branch below is the only
         * place that ever un-hides it. */
        lv_obj_add_flag(s_chart_x_label, LV_OBJ_FLAG_HIDDEN);
        /* No planned curve, so no "current position along the curve" to mark
         * either -- same honesty rule as the x-label above. */
        lv_obj_add_flag(s_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
        if (!isnan(val)) {
            int32_t v = (int32_t)lroundf(val);
            s_chart_actual_pts[0] = v;
            /* Freezing floor (see freezing_point_disp()'s comment): only
             * raise the lower bound when the real point (v) is itself at or
             * above freezing -- i.e. only the fixed +/-10 padding dipped
             * below the floor, not a genuine sub-zero/fault reading. If v
             * itself is below freezing, axis_lo is left at v-10 unclamped so
             * the excursion stays visible instead of being clamped off the
             * bottom of the plot. */
            int32_t floor_i = (int32_t)lroundf(freezing_point_disp(ds.temp_unit));
            int32_t axis_lo = v - 10;
            int32_t axis_hi = v + 10;
            if (axis_lo < floor_i && v >= floor_i) {
                axis_lo = floor_i;
            }
            lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);
            char hi_buf[16], lo_buf[16];
            snprintf(hi_buf, sizeof(hi_buf), "%d%s", (int)axis_hi, unit_pref_suffix(ds.temp_unit));
            snprintf(lo_buf, sizeof(lo_buf), "%d%s", (int)axis_lo, unit_pref_suffix(ds.temp_unit));
            lv_label_set_text(s_chart_y_hi_label, hi_buf);
            lv_label_set_text(s_chart_y_lo_label, lo_buf);
            lv_obj_remove_flag(s_chart_y_hi_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_chart_y_lo_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            s_chart_actual_pts[0] = LV_CHART_POINT_NONE;
            /* No reading at all -- the axis range above is untouched (stays
             * whatever it last was), so a Y label here would describe a
             * range that's no longer being drawn. Hide rather than show a
             * stale number. */
            lv_obj_add_flag(s_chart_y_hi_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chart_y_lo_label, LV_OBJ_FLAG_HIDDEN);
        }
        lv_chart_refresh(s_chart);
    } else {
        /* This branch is entered whenever NOT (idle && history_count==0) --
         * that includes RUNNING/PAUSED/DONE/FAULTED, but ALSO plain IDLE with
         * leftover history from a run that already ended (nothing currently
         * active). state_active distinguishes the two: only the former has a
         * real schedule to show ahead of "now", so only it calls
         * profile_feasibility_plan_curve() -- calling it while IDLE would read
         * st.segments/run_start_c left over from whatever last ran and label
         * them as a live plan, which is exactly the "confident wrong number"
         * this task's owner warned against (2026-08-21: "the chart's time
         * scale must say the truth in both idle and running states -- idle
         * has no planned horizon, it's showing recent history"). */
        bool state_active = (st.state != PROFILE_EXEC_IDLE);
        size_t count = profile_executor_get_history_count();
        float horizon_s;
        if (state_active) {
            /* segments[]/run_start_c/total_elapsed_s are all meaningful once
             * state != IDLE (profile_executor.h's own field comments) --
             * profile_feasibility_plan_curve() is pure math over that copy,
             * safe to call from this refresh timer every tick. */
            (void)profile_feasibility_plan_curve(st.segments, st.segment_count, st.run_start_c,
                                                  plan_pts, sizeof(plan_pts) / sizeof(plan_pts[0]),
                                                  &plan_n);
            horizon_s = (plan_n > 0) ? plan_pts[plan_n - 1].t : 1.0f;
            if (horizon_s < 1.0f) {
                horizon_s = 1.0f; /* guard div-by-zero below; a real profile always has segments */
            }
        } else {
            /* IDLE with leftover history (count>0 is guaranteed here -- the
             * outer gate that chose this else-branch already ruled out
             * idle-with-zero-history). plan_n stays 0, so plan_lookup() below
             * returns NaN for every bucket and the planned series is simply
             * never drawn -- there is nothing planned right now, and drawing
             * one would be a lie. horizon_s instead spans the RECENT HISTORY
             * actually retained (oldest retained sample to the newest), so
             * the x-axis label below can honestly say "recent history", not
             * a run duration that does not exist. */
            plan_n = 0;
            horizon_s = (count > 1) ? (float)(count - 1) * (float)HISTORY_SAMPLE_PERIOD_S : 1.0f;
        }

        unit_pref_t unit = unit_pref_get();
        bool have_range = false;
        float lo = 0.0f, hi = 0.0f;
        for (uint32_t i = 0; i < UI_PAGE_HOME_CHART_POINTS; i++) {
            float t_i = (UI_PAGE_HOME_CHART_POINTS > 1)
                            ? (float)i * horizon_s / (float)(UI_PAGE_HOME_CHART_POINTS - 1)
                            : 0.0f;

            float planned_c = plan_lookup(plan_pts, plan_n, t_i);
            float planned_disp = unit_pref_convert(planned_c, unit, UNIT_PREF_KIND_ABSOLUTE);
            s_chart_planned_pts[i] = isnan(planned_disp) ? LV_CHART_POINT_NONE : (int32_t)lroundf(planned_disp);
            if (!isnan(planned_disp)) {
                if (!have_range) { lo = hi = planned_disp; have_range = true; }
                else { if (planned_disp < lo) lo = planned_disp; if (planned_disp > hi) hi = planned_disp; }
            }

            /* Actual stops at "now" -- a bucket time in the future (past
             * st.total_elapsed_s) has no recorded sample yet, and showing
             * one would fabricate data that hasn't happened. This gate only
             * makes sense while state_active (t_i is "seconds since run
             * start" there); the idle-with-history branch's t_i is "seconds
             * since the oldest RETAINED sample" instead (see horizon_s's
             * comment above) -- every bucket in that window already
             * happened, by construction, so there is nothing to gate. */
            bool have_actual = false;
            float actual_c = NAN;
            if (!state_active || t_i <= (float)st.total_elapsed_s + (float)HISTORY_SAMPLE_PERIOD_S / 2.0f) {
                if (count > 0) {
                    /* Samples are recorded every HISTORY_SAMPLE_PERIOD_S
                     * seconds of real time, so ring index and elapsed time
                     * are proportional -- this avoids paging the whole ring
                     * (up to HISTORY_MAX_SAMPLES=2880 entries, far too big
                     * for a local buffer here) for a single-entry lookup. */
                    size_t idx = (size_t)lroundf(t_i / (float)HISTORY_SAMPLE_PERIOD_S);
                    if (idx >= count) idx = count - 1;
                    profile_history_entry_t entry;
                    if (profile_executor_get_history(&entry, idx, 1) == 1) {
                        actual_c = entry.actual_c;
                        have_actual = true;
                    }
                } else if (i == 0) {
                    /* No samples recorded yet this tick, but the run's real
                     * starting temperature is known -- anchor bucket 0 to it
                     * rather than leaving even the start blank. */
                    actual_c = st.run_start_c;
                    have_actual = true;
                }
            }
            if (have_actual) {
                float disp = unit_pref_convert(actual_c, unit, UNIT_PREF_KIND_ABSOLUTE);
                s_chart_actual_pts[i] = isnan(disp) ? LV_CHART_POINT_NONE : (int32_t)lroundf(disp);
                if (!isnan(disp)) {
                    if (!have_range) { lo = hi = disp; have_range = true; }
                    else { if (disp < lo) lo = disp; if (disp > hi) hi = disp; }
                }
            } else {
                s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
            }
        }
        /* 2026-08-23 owner request ("the LCD profile graph should look like
         * the web GUI's profile graph"): main_page.html's Y axis is labelled
         * at exactly two points, 0 at the bottom and the planned curve's own
         * PEAK at the top -- not a padded min/max of whatever's plotted, the
         * way this chart's axis worked before. Only applied when there IS a
         * live plan to take a peak from (state_active && plan_n>0); every
         * other case (idle-with-history, or a running state whose plan came
         * back empty -- profile_feasibility_plan_curve()'s own -1/empty
         * path) falls back to the previous data-driven min/max-with-padding
         * behaviour below, since there is no "planned peak" to honestly
         * anchor a 0..peak axis to in those cases. */
        if (state_active && plan_n > 0) {
            float peak_c = ui_page_home_plan_peak_c(plan_pts, plan_n);
            if (!isnan(peak_c)) {
                float peak_disp = unit_pref_convert(peak_c, unit, UNIT_PREF_KIND_ABSOLUTE);
                int32_t axis_hi = (int32_t)lroundf(peak_disp);
                int32_t axis_lo = 0;
                if (axis_hi <= axis_lo) {
                    axis_hi = axis_lo + 1; /* guard a degenerate/zero-peak profile */
                }
                lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);
                char hi_buf[16], lo_buf[16];
                snprintf(hi_buf, sizeof(hi_buf), "%d%s", (int)axis_hi, unit_pref_suffix(unit));
                snprintf(lo_buf, sizeof(lo_buf), "%d%s", (int)axis_lo, unit_pref_suffix(unit));
                lv_label_set_text(s_chart_y_hi_label, hi_buf);
                lv_label_set_text(s_chart_y_lo_label, lo_buf);
                lv_obj_remove_flag(s_chart_y_hi_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_chart_y_lo_label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_chart_y_hi_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_chart_y_lo_label, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (have_range) {
            float range = hi - lo;
            if (range < 1.0f) range = 1.0f;
            float pad_c = range * 0.1f;
            int32_t axis_lo = (int32_t)lroundf(lo - pad_c);
            int32_t axis_hi = (int32_t)lroundf(hi + pad_c);
            /* Freezing floor -- same guard as the idle-dot branch above: only
             * raise axis_lo when the real data minimum (lo, pre-padding) is
             * itself at or above freezing. If `lo` itself is below freezing
             * (a genuine sub-zero actual/planned point), axis_lo is left
             * unclamped so that point stays plotted and visible rather than
             * being clipped off the bottom. */
            int32_t floor_i = (int32_t)lroundf(freezing_point_disp(unit));
            if (axis_lo < floor_i && lo >= floor_i) {
                axis_lo = floor_i;
            }
            lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);
            /* Y labels written from the SAME axis_lo/axis_hi just handed to
             * lv_chart_set_axis_range(), not read back from the chart --
             * that is what keeps them from ever drifting out of sync with a
             * range that changes every tick (see this file's header comment
             * on why lv_scale was rejected in favour of this). `unit` here is
             * the same unit_pref_get() result planned_disp/actual disp were
             * already converted through above, so the suffix can never
             * disagree with the plotted numbers. */
            char hi_buf[16], lo_buf[16];
            snprintf(hi_buf, sizeof(hi_buf), "%d%s", (int)axis_hi, unit_pref_suffix(unit));
            snprintf(lo_buf, sizeof(lo_buf), "%d%s", (int)axis_lo, unit_pref_suffix(unit));
            lv_label_set_text(s_chart_y_hi_label, hi_buf);
            lv_label_set_text(s_chart_y_lo_label, lo_buf);
            lv_obj_remove_flag(s_chart_y_hi_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_chart_y_lo_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* No actual and no planned point converted this tick -- nothing
             * to show a range for; leave the previous axis range alone (same
             * as before this change) but don't label it, same honesty rule
             * as the idle branch's "no reading" case above. */
            lv_obj_add_flag(s_chart_y_hi_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chart_y_lo_label, LV_OBJ_FLAG_HIDDEN);
        }
        /* X (time) label -- a real scale, not just a single span string
         * (2026-08-21 owner request: "I want a time scale on the LCD
         * chart"). Three points -- start, an intermediate tick at the
         * midpoint, and the end -- honest per branch:
         *   - state_active: the chart's horizontal axis is the WHOLE-RUN
         *     PLANNED horizon (0..horizon_s) from profile_feasibility_
         *     plan_curve(), NOT a trailing "last N samples" window (this
         *     file's header comment, part 2) -- "0:00 | <mid> | <end>" is the
         *     actual planned duration being plotted, not a guess. Gated on
         *     plan_n > 0: horizon_s falls back to a hardcoded 1.0f guard a
         *     few lines up specifically to avoid a div-by-zero when the plan
         *     curve came back empty (e.g. a malformed/zero-segment profile)
         *     -- that fallback is a guard, not a real duration, so labelling
         *     it would be exactly the "confident wrong number" this task's
         *     own instructions warn against.
         *   - !state_active (idle, leftover history): there is no planned
         *     run to span -- horizon_s here is the RECENT-HISTORY window
         *     actually being plotted (oldest retained sample to now, see
         *     its own comment above), so the label says exactly that instead
         *     of implying a schedule that does not exist. Gated on count > 1
         *     (need at least two samples for a non-zero span to be honest
         *     about); a single leftover sample has no span to show a scale
         *     for. */
        /* 48: worst case "hist -" (6) + up to 15 bytes of one duration + "|-"
         * (2) + up to 15 bytes of a second duration + "|now" (4) + NUL = 43
         * max -- rounded up with margin, same discipline the other
         * snprintf-into-fixed-buffer call sites in this file already use. */
        /* 64: worst case four "%lu:%02lu" ticks (up to 10 bytes each for a
         * uint32_t seconds count near UINT32_MAX / 60) + 3 "|" separators +
         * NUL -- rounded up with margin. 2026-08-23: state_active's four
         * ticks now use ui_page_home_x_ticks()/ui_page_home_format_mmss()
         * (0, h/3, 2h/3, h in total-elapsed M:SS, no hour rollover) to match
         * main_page.html's own four-tick x-axis exactly, instead of this
         * page's earlier three-point 0/mid/end using format_duration()'s
         * hh:mm:ss-beyond-an-hour shape. The idle-with-history branch below
         * is UNCHANGED (still three points, still format_duration()) --
         * main_page.html's four-tick spec is for the PLANNED-profile axis,
         * which idle-with-history has none of (see this branch's own
         * comment above). */
        char span_buf[64];
        if (state_active && plan_n > 0) {
            float ticks[4];
            ui_page_home_x_ticks(horizon_s, ticks);
            char t1_buf[16], t2_buf[16], t3_buf[16];
            ui_page_home_format_mmss((uint32_t)lroundf(ticks[1]), t1_buf, sizeof(t1_buf));
            ui_page_home_format_mmss((uint32_t)lroundf(ticks[2]), t2_buf, sizeof(t2_buf));
            ui_page_home_format_mmss((uint32_t)lroundf(ticks[3]), t3_buf, sizeof(t3_buf));
            snprintf(span_buf, sizeof(span_buf), "0:00|%s|%s|%s", t1_buf, t2_buf, t3_buf);
            lv_label_set_text(s_chart_x_label, span_buf);
            lv_obj_remove_flag(s_chart_x_label, LV_OBJ_FLAG_HIDDEN);

            /* Current-position dot -- see its own static declaration comment.
             * Only meaningful here (a live plan with a real horizon to place
             * a position along); positioned on the ACTUAL series when a real
             * sample already exists at that bucket, else on the PLANNED
             * series (covers the first tick or two of a run before any
             * actual sample has been recorded yet) so the dot never just
             * vanishes at the very start of a firing. */
            size_t now_idx =
                ui_page_home_now_bucket_index(horizon_s, (float)st.total_elapsed_s, UI_PAGE_HOME_CHART_POINTS);
            lv_point_t dot_pos;
            bool have_dot_pos = false;
            if (s_chart_actual_pts[now_idx] != LV_CHART_POINT_NONE) {
                lv_chart_get_point_pos_by_id(s_chart, s_chart_actual_series, (uint32_t)now_idx, &dot_pos);
                have_dot_pos = true;
            } else if (s_chart_planned_pts[now_idx] != LV_CHART_POINT_NONE) {
                lv_chart_get_point_pos_by_id(s_chart, s_chart_planned_series, (uint32_t)now_idx, &dot_pos);
                have_dot_pos = true;
            }
            if (have_dot_pos) {
                /* lv_chart_get_point_pos_by_id() returns a position relative
                 * to the chart's own top-left content origin, per its own
                 * doc comment (lv_chart.h) -- s_chart_now_dot is a CHILD of
                 * s_chart, so lv_obj_set_pos() (parent-relative) is the right
                 * call here, not lv_obj_align() or an absolute coordinate. */
                lv_obj_set_pos(s_chart_now_dot, dot_pos.x - 3, dot_pos.y - 3);
                lv_obj_remove_flag(s_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (!state_active && count > 1) {
            char mid_buf[16], end_buf[16];
            format_duration((uint32_t)lroundf(horizon_s / 2.0f), mid_buf, sizeof(mid_buf));
            format_duration((uint32_t)lroundf(horizon_s), end_buf, sizeof(end_buf));
            snprintf(span_buf, sizeof(span_buf), "hist -%s|-%s|now", end_buf, mid_buf);
            lv_label_set_text(s_chart_x_label, span_buf);
            lv_obj_remove_flag(s_chart_x_label, LV_OBJ_FLAG_HIDDEN);
            /* No live plan in this branch (idle with leftover history) --
             * nothing to mark a "current position along the curve" on. */
            lv_obj_add_flag(s_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_chart_x_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
        }
        lv_chart_refresh(s_chart);
    }

    /* 2026-08-22 owner request: "remove whatever it is between the graph and
     * the start button" -- the run-state summary card (profile/state text,
     * live safety-trip banner, active kiln-config name) and the progress bar
     * (elapsed/remaining time) that used to live in this space are both
     * GONE, not just hidden -- see this function's/ui_page_home_build()'s
     * removed state_card/progress_row for what used to be here.
     *
     * KNOWN TRADE-OFF, flagged rather than silently dropped: the safety-trip
     * banner this replaces was the one place a live safety trip pre-empted
     * this page's display (ROADMAP.md "safety processor faults should stop
     * firing and the GUI should reflect that, on both the LCD and web page").
     * profile_executor.c's watchdog still independently faults/latches any
     * RUNNING run on a trip -- the firing itself still stops -- but the LCD
     * home page no longer calls that out visually; ui_page_safety.c (Menu ->
     * Safety Processor) is now the only LCD surface that shows it. Confirm
     * this trade-off is intended before relying on the home page alone to
     * notice a trip. */

    /* Merged fire button -- label and color follow the same st.state this
     * function already polled above. Running/Paused reads "Stop" in the
     * danger accent; everything else (Idle/Done/Faulted) reads "Start" in
     * the start-ish accent. */
    if (st.state == PROFILE_EXEC_RUNNING || st.state == PROFILE_EXEC_PAUSED) {
        lv_label_set_text(s_fire_btn_label, "Stop");
        lv_obj_set_style_bg_color(s_fire_btn, UI_THEME_ACCENT_5, 0);
    } else {
        lv_label_set_text(s_fire_btn_label, "Start");
        lv_obj_set_style_bg_color(s_fire_btn, UI_THEME_ACCENT_4, 0);
    }
}

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

    /* Top bar -- ui_topbar.c/.h owns both LVGL traps (hit-test-does-not-
     * escape-the-parent, and the flex trap on a FLOATING icon proxy); see
     * that header for the mechanism. No title here: this page shows the
     * live WiFi/IP status string instead (s_status_label, built below), so
     * .title is NULL and the "kilnCtl" label that used to occupy the same
     * spot is gone -- it was the thing the user reported overlapping the
     * IP address. Home has no back/prev/next; the gear is Menu, same
     * destination as before (menu_nav_cb() -> ui_page_config_reset_to_first_page()
     * + kiln_ui_show("config")). */
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = NULL,
        .gear_cb = menu_nav_cb,
    }, &s_topbar);
    lv_obj_t *bar = s_topbar.bar;

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
     * ui_topbar_t.icons_w (see ui_topbar.h) is the single icon (the gear)
     * this page reserves, replacing the old hand-rolled gear_hit_area_w
     * literal. It is NOT the same number: the old hand-rolled proxy was
     * 80px wide (chosen generously, sideways headroom with nothing else
     * clickable in the bar); ui_topbar_create()'s icons_w for one icon is
     * icon_count * UI_TOPBAR_ICON_W_PX + (icon_count-1) * UI_TOPBAR_ICON_GAP_PX
     * = 1 * 36 + 0 = 36px (ui_topbar.h's UI_TOPBAR_ICON_W_PX/GAP_PX). Width
     * derivation: cap = bar_width - icons_w - gap.
     *
     * PRE-topbar measurement, superseded: on hardware 2026-08-20 (before
     * this module existed, same physical 463px bar) the 80px-wide proxy
     * gave a ~379px cap. That 80px/~379px pair no longer describes what
     * this code computes -- with the topbar's 36px icons_w the same 463px
     * bar now yields cap = 463 - 36 - 4 = ~423px. The bar-width figure
     * (463px) and the underlying bug this cap fixes (align_to() sets a
     * POSITION only and never constrains WIDTH, so an uncapped label box
     * still renders text under the gear) are still real and still true;
     * only the old proxy's specific width and resulting cap are stale.
     * Not re-measured on hardware since the topbar move -- s_topbar.icons_w
     * is logged below so a future boot can confirm the ~423px figure. */
    lv_obj_update_layout(bar);
    int32_t bar_w = lv_obj_get_width(bar);
    int32_t status_label_max_w = bar_w - s_topbar.icons_w - (UI_THEME_PADDING_PX / 2);
    /* Guard the subtraction. lv_obj_update_layout() above normally resolves
     * the bar's lv_pct(100) against the screen, but this page is BUILT
     * DETACHED (kiln_ui.c builds a page before it is ever shown), and a
     * width read before layout resolves is 0 -- which would make this
     * subtraction negative and the label either invisible or garbage. If
     * that ever happens, fall back to the full bar width: a label that
     * overlaps the gear is a cosmetic bug, a label that vanishes is a
     * functional one, and the log line says which case this boot took. */
    if (status_label_max_w <= 0) {
        ESP_LOGW(TAG, "status label width fallback: bar_w=%ld resolved too small for the gear "
                      "reservation (%ld) -- label may overlap the gear this boot",
                 (long)bar_w, (long)s_topbar.icons_w);
        status_label_max_w = (bar_w > 0) ? bar_w : LV_SIZE_CONTENT;
    } else {
        ESP_LOGI(TAG, "status label width %ld of bar %ld (gear reserves %ld)",
                 (long)status_label_max_w, (long)bar_w, (long)s_topbar.icons_w);
    }
    s_status_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_width(s_status_label, status_label_max_w);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_status_label, "WiFi: --");
    /* Anchor to s_topbar.icons (the FLOATING proxy ui_topbar.c builds), not
     * any individual icon button -- the proxy is the wider box and the one
     * the label must actually clear. */
    lv_obj_align_to(s_status_label, s_topbar.icons, LV_ALIGN_OUT_LEFT_MID, -(UI_THEME_PADDING_PX / 2), 0);

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
    ui_topbar_raise(&s_topbar);

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
     * refresh_cb()'s tail comment and this function's own removed
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
    s_trip_strip = lv_label_create(content);
    lv_obj_add_flag(s_trip_strip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_trip_strip, lv_pct(100));
    lv_label_set_long_mode(s_trip_strip, LV_LABEL_LONG_DOT);
    lv_obj_set_style_bg_color(s_trip_strip, UI_THEME_ACCENT_5, 0);
    lv_obj_set_style_bg_opa(s_trip_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_trip_strip, lv_color_white(), 0);
    lv_obj_set_style_radius(s_trip_strip, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_trip_strip, 3, 0);
    lv_label_set_text(s_trip_strip, "");

    s_chart = lv_chart_create(content);
    lv_obj_set_width(s_chart, lv_pct(100));
    lv_obj_set_flex_grow(s_chart, 1);
    lv_obj_set_style_bg_color(s_chart, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_chart, 2, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    /* Owner report 2026-08-22: "the LCD has 3 sections in the graph, it
     * should only be one", confirmed against a photo of the screen.
     *
     * Both counts are 0. The culprit was the VERTICAL count, which was 4:
     * LVGL draws vertical division lines INCLUDING one at each edge of the
     * plot area, so 4 lines are drawn at 0, 1/3, 2/3 and 1 of the width --
     * two of them land in the middle and cut the plot into exactly the three
     * columns the report describes. It reads as three separate panels, not
     * as gridlines, because the line color has as much contrast against the
     * card background as the trace itself does.
     *
     * (A first attempt zeroed only the HORIZONTAL count, on the assumption
     * that 2 horizontal lines making 3 stacked bands was the "3 sections".
     * It was not -- the photo shows the divisions running vertically. Noted
     * so the next person does not re-add either count reasoning that "the
     * other one was the problem".) */
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_chart_set_point_count(s_chart, UI_PAGE_HOME_CHART_POINTS);
    /* Same "desired" accent color the now-removed ui_page_history.c used
     * (ACCENT_3), carrying the PLANNED-ahead curve instead of a trailing
     * recorded setpoint -- see this file's header comment, part 2. Still no
     * on-chart legend text (the two corner overlay labels below cover Y/X
     * scale; a colour-to-series legend was ui_page_history.c's job and was
     * never rebuilt here after that page's removal -- actual is
     * UI_THEME_ACCENT_1, planned is UI_THEME_ACCENT_3, same colours this
     * codebase has used for those two concepts everywhere else). */
    s_chart_actual_series = lv_chart_add_series(s_chart, UI_THEME_ACCENT_1, LV_CHART_AXIS_PRIMARY_Y);
    /* 2026-08-23: was UI_THEME_ACCENT_3 (teal) -- now the exact web-match
     * purple (see UI_PAGE_HOME_PLAN_COLOR_HEX's own comment). This color is
     * also how chart_draw_event_cb() (registered just below) picks the
     * planned line's draw task out from the actual line's, so it must stay
     * in sync with that macro. */
    s_chart_planned_series =
        lv_chart_add_series(s_chart, lv_color_hex(UI_PAGE_HOME_PLAN_COLOR_HEX), LV_CHART_AXIS_PRIMARY_Y);
    /* Dashing hook -- see chart_draw_event_cb()'s own comment for why this is
     * the only way to get a dashed lv_chart series line in LVGL 9.5. */
    lv_obj_add_event_cb(s_chart, chart_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    for (uint32_t i = 0; i < UI_PAGE_HOME_CHART_POINTS; i++) {
        s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
        s_chart_planned_pts[i] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_ext_y_array(s_chart, s_chart_actual_series, s_chart_actual_pts);
    lv_chart_set_series_ext_y_array(s_chart, s_chart_planned_series, s_chart_planned_pts);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);

    /* Temp/time scale overlay widgets -- see s_chart_y_hi_label's own comment
     * (near the static declarations above) for why these are plain labels
     * overlaid on the chart's own corners rather than an lv_scale widget.
     * Small semi-opaque chips so digits stay legible over the plotted lines;
     * built HIDDEN, refresh_cb() (called once at the bottom of this function)
     * un-hides whichever ones have real data before the page is ever shown,
     * so there is no visible flash of an unset "0" label on first paint. */
    s_chart_y_hi_label = lv_label_create(s_chart);
    lv_obj_set_style_text_color(s_chart_y_hi_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_color(s_chart_y_hi_label, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_chart_y_hi_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_chart_y_hi_label, 3, 0);
    lv_obj_set_style_radius(s_chart_y_hi_label, 4, 0);
    lv_obj_align(s_chart_y_hi_label, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_add_flag(s_chart_y_hi_label, LV_OBJ_FLAG_HIDDEN);

    s_chart_y_lo_label = lv_label_create(s_chart);
    lv_obj_set_style_text_color(s_chart_y_lo_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_color(s_chart_y_lo_label, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_chart_y_lo_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_chart_y_lo_label, 3, 0);
    lv_obj_set_style_radius(s_chart_y_lo_label, 4, 0);
    lv_obj_align(s_chart_y_lo_label, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_obj_add_flag(s_chart_y_lo_label, LV_OBJ_FLAG_HIDDEN);

    s_chart_x_label = lv_label_create(s_chart);
    lv_obj_set_style_text_color(s_chart_x_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_color(s_chart_x_label, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_chart_x_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_chart_x_label, 3, 0);
    lv_obj_set_style_radius(s_chart_x_label, 4, 0);
    lv_obj_align(s_chart_x_label, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_add_flag(s_chart_x_label, LV_OBJ_FLAG_HIDDEN);

    /* Current-position dot -- see its own static declaration comment above.
     * 6px filled circle, blue, matching main_page.html's current-position
     * marker; positioned every refresh_cb() tick via
     * lv_chart_get_point_pos_by_id(), built hidden like the labels above
     * (only shown once refresh_cb() has a real "now" to mark). Zero border
     * (a plain filled dot, not a ring) so it reads as a single solid marker
     * against the dark card background at this size. */
    s_chart_now_dot = lv_obj_create(s_chart);
    lv_obj_remove_flag(s_chart_now_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_chart_now_dot, 6, 6);
    lv_obj_set_style_radius(s_chart_now_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_chart_now_dot, lv_color_hex(0x2196f3), 0); /* plain blue, matches the web dot */
    lv_obj_set_style_bg_opa(s_chart_now_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_chart_now_dot, 0, 0);
    lv_obj_set_style_pad_all(s_chart_now_dot, 0, 0);
    lv_obj_add_flag(s_chart_now_dot, LV_OBJ_FLAG_HIDDEN);

    /* 2026-08-21 owner request: "remove the zone [] section" -- no zone row
     * widgets are built on this page any more (see s_zone_count's own comment
     * near its declaration for what's KEPT: the count itself, still used by
     * refresh_cb()'s idle branch to gate the chart's representative-zone
     * dot). s_zone_count == 0 is handled sensibly by simply having nothing to
     * gate -- refresh_cb() skips the representative-temp lookup and the
     * chart shows no idle dot, which is the honest "nothing configured"
     * state; there is no separate "No zones configured" label to build here
     * any more; each configured zone's live temperature/heater status is
     * still visible via the web dashboard (main_page.html), which was never
     * subject to this page's height budget. */
    s_zone_count = zones_config_get_thermo_count();
    if (s_zone_count > MAX31856_CHANNEL_COUNT) {
        s_zone_count = MAX31856_CHANNEL_COUNT; /* defensive; should never trip */
    }

    /* Single merged Start/Stop button -- one user-visible request ("the
     * start stop button should be one button on the lcd"). Menu moved off
     * this row entirely into the status bar as a gear (see the status-bar
     * build above and menu_nav_cb()) per the other request, so this row is
     * now just the one button. build_button() still grows it across the
     * row's width via flex_grow(1).
     *
     * 2026-08-22: the run-state card, progress bar, and bottom spacer that
     * used to sit between the chart and this row are gone (see the chart's
     * own build comment above and refresh_cb()'s tail comment) -- action_row
     * is now the chart's very next sibling. It still stays pinned to
     * `content`'s bottom edge: the chart above it carries flex_grow(1), so it
     * (not a dedicated spacer) claims whatever vertical space this button
     * does not need.
     *
     * Drawn vs effective button height: build_button()'s
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
    s_fire_btn = build_button(action_row, "Start", UI_THEME_ACCENT_4, fire_btn_cb, 36, &s_fire_btn_label);

    /* Pages are never torn down (kiln_ui.h's header comment), so a timer
     * created once here and never deleted matches that lifetime. */
    lv_timer_create(refresh_cb, UI_PAGE_HOME_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
