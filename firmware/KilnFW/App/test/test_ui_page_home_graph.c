// Host tests for ui_page_home_graph.c -- the pure geometry/formatting seam
// factored out of ui_page_home.c's refresh_cb() so the LCD profile chart's
// match to main_page.html's web chart (M:SS x-axis ticks, 0..peak y-axis,
// current-position bucket index) can be checked without LVGL/esp_log stubs.
#include "test_common.h"
#include "../drivers/ui/ui_page_home_graph.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

void run_test_ui_page_home_graph(void)
{
    TEST_SECTION("ui_page_home_graph: format_mmss");
    {
        char buf[16];
        ui_page_home_format_mmss(0, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "0:00") == 0, "0s -> 0:00");

        ui_page_home_format_mmss(59, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "0:59") == 0, "59s -> 0:59");

        ui_page_home_format_mmss(60, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "1:00") == 0, "60s -> 1:00");

        // Screenshot's own tick values (owner-supplied reference): total
        // elapsed MINUTES, never rolled over to hours -- 125:59 is 125
        // minutes 59 seconds, not 2:05:59.
        ui_page_home_format_mmss(125u * 60u + 59u, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "125:59") == 0, "125m59s -> 125:59 (no hour rollover)");

        ui_page_home_format_mmss(377u * 60u + 57u, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "377:57") == 0, "377m57s -> 377:57");
    }

    TEST_SECTION("ui_page_home_graph: x_ticks");
    {
        float t[4];
        ui_page_home_x_ticks(0.0f, t);
        TEST_CHECK_NEAR(t[0], 0.0f, 0.001f, "horizon 0: tick0");
        TEST_CHECK_NEAR(t[3], 0.0f, 0.001f, "horizon 0: tick3");

        ui_page_home_x_ticks(300.0f, t);
        TEST_CHECK_NEAR(t[0], 0.0f, 0.001f, "horizon 300: tick0 == 0");
        TEST_CHECK_NEAR(t[1], 100.0f, 0.001f, "horizon 300: tick1 == h/3");
        TEST_CHECK_NEAR(t[2], 200.0f, 0.001f, "horizon 300: tick2 == 2h/3");
        TEST_CHECK_NEAR(t[3], 300.0f, 0.001f, "horizon 300: tick3 == h");

        // Negative horizon (should never happen post-guard, but the function
        // must not hand back negative tick times) clamps to all-zero.
        ui_page_home_x_ticks(-5.0f, t);
        TEST_CHECK_NEAR(t[3], 0.0f, 0.001f, "negative horizon clamps to 0");
    }

    TEST_SECTION("ui_page_home_graph: plan_peak_c");
    {
        TEST_CHECK(isnan(ui_page_home_plan_peak_c(NULL, 0)), "empty list -> NAN");

        profile_plan_point_t pts[4] = {
            {0.0f, 20.0f},
            {600.0f, 1170.0f}, // the screenshot's own peak value
            {1200.0f, 1170.0f},
            {1800.0f, 400.0f}, // controlled cool-down below peak
        };
        float peak = ui_page_home_plan_peak_c(pts, 4);
        TEST_CHECK_NEAR(peak, 1170.0f, 0.001f, "peak is the max c, not the last point");

        profile_plan_point_t single[1] = {{0.0f, 55.5f}};
        TEST_CHECK_NEAR(ui_page_home_plan_peak_c(single, 1), 55.5f, 0.001f, "single point is its own peak");
    }

    TEST_SECTION("ui_page_home_graph: now_bucket_index");
    {
        // 10 buckets over a 900s horizon (matches this page's own
        // UI_PAGE_HOME_CHART_POINTS-style bucket math, t_i = i*h/(n-1)).
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 0.0f, 10) == 0, "elapsed 0 -> bucket 0");
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 900.0f, 10) == 9, "elapsed == horizon -> last bucket");
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 450.0f, 10) == 4 ||
                       ui_page_home_now_bucket_index(900.0f, 450.0f, 10) == 5,
                   "elapsed == half horizon -> a middle bucket");
        // Overrun (elapsed past the plotted horizon, e.g. a stale read)
        // clamps to the last bucket rather than extrapolating past it.
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 5000.0f, 10) == 9, "overrun elapsed clamps to last bucket");
        // Degenerate inputs return the single safe index instead of
        // dividing by zero.
        TEST_CHECK(ui_page_home_now_bucket_index(0.0f, 10.0f, 10) == 0, "zero horizon -> 0");
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 10.0f, 1) == 0, "point_count<2 -> 0");
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 10.0f, 0) == 0, "point_count==0 -> 0");
    }

    TEST_SECTION("ui_page_home_graph: build_x_label");
    {
        char buf[64];

        // has_span == false (idle, no history / a plan that came back empty):
        // no label written, buffer left untouched, function reports "no
        // label" -- this is the case that must stay hidden on the LCD.
        memset(buf, 0x7A, sizeof(buf));
        bool wrote = ui_page_home_build_x_label(300.0f, false, buf, sizeof(buf));
        TEST_CHECK(!wrote, "has_span=false -> no label produced");
        TEST_CHECK(buf[0] == (char)0x7A, "has_span=false -> out buffer untouched");

        // has_span == true, matches the running-profile case (0..horizon_s,
        // four M:SS ticks) -- same numbers ui_page_home_x_ticks() itself
        // returns for horizon 300.
        wrote = ui_page_home_build_x_label(300.0f, true, buf, sizeof(buf));
        TEST_CHECK(wrote, "has_span=true -> label produced");
        TEST_CHECK(strcmp(buf, "0:00|1:40|3:20|5:00") == 0, "horizon 300 -> 0:00|1:40|3:20|5:00");

        // has_span == true also covers idle-with-history: same shape, driven
        // by whatever the caller's own (different) horizon_s span is --
        // proves the function does not special-case a "running" value.
        wrote = ui_page_home_build_x_label(90.0f, true, buf, sizeof(buf));
        TEST_CHECK(wrote, "history span -> label produced");
        TEST_CHECK(strcmp(buf, "0:00|0:30|1:00|1:30") == 0, "horizon 90 -> 0:00|0:30|1:00|1:30");

        // A too-small out buffer must never overflow -- snprintf-backed
        // truncation, same discipline as ui_page_home_format_mmss().
        char tiny[6];
        wrote = ui_page_home_build_x_label(300.0f, true, tiny, sizeof(tiny));
        TEST_CHECK(wrote, "truncated buffer still reports a label was produced");
        TEST_CHECK(strlen(tiny) == sizeof(tiny) - 1, "truncated buffer is NUL-terminated within its capacity");
    }

    TEST_SECTION("ui_page_home_graph: x_ticks/format_mmss over real-run horizons");
    {
        // Tonight's 3-zone firing: ~27 minutes total. Ticks must land at
        // 0, 9:00, 18:00, 27:00 and each format cleanly as mm:ss (no hour
        // rollover -- see ui_page_home_format_mmss()'s own header comment).
        float t[4];
        float horizon_27min = 27.0f * 60.0f;
        ui_page_home_x_ticks(horizon_27min, t);
        TEST_CHECK_NEAR(t[1], 9.0f * 60.0f, 0.01f, "27min horizon: tick1 == 9:00");
        TEST_CHECK_NEAR(t[2], 18.0f * 60.0f, 0.01f, "27min horizon: tick2 == 18:00");
        TEST_CHECK_NEAR(t[3], 27.0f * 60.0f, 0.01f, "27min horizon: tick3 == 27:00");
        char buf[16];
        ui_page_home_format_mmss((uint32_t)t[3], buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "27:00") == 0, "27min horizon: full-span tick formats as 27:00");

        // Autotune's worst-case 4-hour budget: ticks at 0, 80:00, 160:00,
        // 240:00 -- large enough that a naive hh:mm choice would need to
        // differ from the 27-minute case's mm:ss, which is exactly why this
        // page's ticks stay unit-of-minutes (M:SS) at every horizon rather
        // than switching formats and forcing the label-width/tick-count
        // logic to handle two shapes.
        float horizon_4h = 4.0f * 3600.0f;
        ui_page_home_x_ticks(horizon_4h, t);
        TEST_CHECK_NEAR(t[3], 240.0f * 60.0f, 0.01f, "4h horizon: full span == 240 minutes");
        ui_page_home_format_mmss((uint32_t)t[3], buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "240:00") == 0, "4h horizon: full-span tick formats as 240:00");
    }

    TEST_SECTION("ui_page_home_graph: y_axis_range");
    {
        int32_t lo, hi;

        // Tonight's firing spans ~36-63 C -- a real, few-tens-of-degrees
        // range. 10% pad on each side, freezing floor (0 C) does not apply
        // (36 C is well above it).
        ui_page_home_y_axis_range(36.0f, 63.0f, 0.0f, &lo, &hi);
        TEST_CHECK(lo < 36 && hi > 63, "36-63C range: padded on both sides");
        TEST_CHECK_NEAR((float)lo, 36.0f - (63.0f - 36.0f) * 0.1f, 1.0f, "36-63C range: ~10% pad on the low side");
        TEST_CHECK_NEAR((float)hi, 63.0f + (63.0f - 36.0f) * 0.1f, 1.0f, "36-63C range: ~10% pad on the high side");

        // A span of a few degrees (e.g. an idle bench sitting at 31-33C) --
        // still must pad sensibly and never collapse lo==hi.
        ui_page_home_y_axis_range(31.0f, 33.0f, 0.0f, &lo, &hi);
        TEST_CHECK(hi > lo, "small few-degree range: axis_hi > axis_lo");
        TEST_CHECK(lo <= 31 && hi >= 33, "small few-degree range: still spans the real data");

        // Degenerate all-same-value span (lo == hi) -- e.g. exactly one
        // sample recorded so far, or a stuck sensor. THE load-bearing
        // assertion: axis_hi must come back strictly greater than axis_lo,
        // or lv_chart_set_axis_range()'s zero-height axis divides by zero
        // one call downstream (see ui_page_home_y_axis_range()'s own header
        // comment in ui_page_home_graph.h for exactly where). Verified this
        // is not vacuous by removing the `if (range < 1.0f) range = 1.0f;`
        // guard in ui_page_home_graph.c and re-running: with the guard
        // removed, this exact call returns axis_lo == axis_hi == 45 and this
        // assertion fails -- restored afterward.
        ui_page_home_y_axis_range(45.0f, 45.0f, 0.0f, &lo, &hi);
        TEST_CHECK(hi > lo, "degenerate all-same-value span (lo==hi==45): axis_hi > axis_lo, guard held");

        // Freezing floor: a real sub-zero low must NOT be clamped up (the
        // excursion has to stay visible), but padding that merely DIPS below
        // freezing on an otherwise-above-freezing range must be raised to
        // the floor.
        ui_page_home_y_axis_range(-5.0f, 10.0f, 0.0f, &lo, &hi);
        TEST_CHECK(lo < 0, "genuine sub-zero low (-5C): axis_lo stays unclamped below freezing");

        ui_page_home_y_axis_range(1.0f, 5.0f, 0.0f, &lo, &hi);
        TEST_CHECK(lo >= 0, "above-freezing data whose padding alone dips below 0: axis_lo floored at freezing");
    }

    TEST_SECTION("ui_page_home_graph: legend_visibility");
    {
        bool show_actual, show_plan;

        // Idle single-dot state: no span at all, so no legend regardless of
        // what has_actual_multi/has_planned claim -- a stray true here would
        // be exactly the "confident wrong number" this codebase's other
        // honesty guards warn against (has_planned should never legitimately
        // be true without has_span, but the function must not trust that).
        ui_page_home_legend_visibility(false, false, false, &show_actual, &show_plan);
        TEST_CHECK(!show_actual && !show_plan, "no span, no plan -> nothing shown");
        ui_page_home_legend_visibility(false, true, true, &show_actual, &show_plan);
        TEST_CHECK(!show_actual && !show_plan, "no span (even if actual/plan claimed) -> nothing shown");

        // Idle-with-history or a running plan that came back empty: actual
        // line only, and it has at least two real points.
        ui_page_home_legend_visibility(true, true, false, &show_actual, &show_plan);
        TEST_CHECK(show_actual && !show_plan, "span, multi-point actual, no plan -> actual only");

        // A live running profile with a real plan curve: both series drawn.
        ui_page_home_legend_visibility(true, true, true, &show_actual, &show_plan);
        TEST_CHECK(show_actual && show_plan, "span + multi-point actual + plan -> both shown");

        // The defect this housekeeping fix targets: the very first tick of a
        // run plots exactly ONE actual point (the run_start_c anchor) --
        // has_actual_multi is false even though has_span is true (a live
        // plan curve is being drawn). The Actual row must be suppressed
        // (nothing visible to key -- a lone point is not a line, and
        // per-point markers are gone), while the Plan row -- which IS a real
        // multi-point dashed line at this tick -- still shows.
        ui_page_home_legend_visibility(true, false, true, &show_actual, &show_plan);
        TEST_CHECK(!show_actual && show_plan, "single-sample actual, span+plan -> Actual suppressed, Plan shown");

        // Same single-sample case but no plan either (e.g. a run whose plan
        // curve came back empty) -- nothing to show at all.
        ui_page_home_legend_visibility(true, false, false, &show_actual, &show_plan);
        TEST_CHECK(!show_actual && !show_plan, "single-sample actual, no plan -> nothing shown");
    }

    TEST_SECTION("ui_page_home_graph: active_y_axis_range (autoscale-during-firing defect fix)");
    {
        int32_t lo, hi;

        // THE clipping bug this task is about: plan peaks at 60C, kiln
        // starts at 25C, actual has overshot to 62C -- ABOVE the planned
        // peak. The old 0..plan_peak behaviour would have hard-clipped this
        // at 60. The fixed axis must cover the overshoot.
        ui_page_home_active_y_axis_range(25.0f, 62.0f, 0.0f, false, 0, 0, &lo, &hi);
        TEST_CHECK((float)hi > 62.0f, "overshoot above plan peak (62C, plan peak 60C folded into hi): axis_hi covers it, not clipped at 60");
        TEST_CHECK((float)lo <= 25, "starting temp (25C) still within axis_lo");

        // Degenerate span guard still holds through this wrapper (same
        // load-bearing lroundf(44.9)==lroundf(45.1)==45 case as the
        // underlying ui_page_home_y_axis_range()).
        ui_page_home_active_y_axis_range(45.0f, 45.0f, 0.0f, false, 0, 0, &lo, &hi);
        TEST_CHECK(hi > lo, "degenerate all-same-value span: axis_hi > axis_lo, guard held through the wrapper");

        // Anti-jitter, quantization: two raw ranges that differ by a
        // fraction of a degree (simulating tick-to-tick sensor noise) but do
        // not cross a UI_PAGE_HOME_AXIS_QUANT_STEP_DISP-unit step boundary
        // must quantize to the EXACT SAME integer pair -- this is what stops
        // the label from flickering every refresh.
        // These two raw padded highs straddle an INTEGER rounding boundary
        // (hi=59.5 -> 10%-padded raw 61.9 -> lroundf 62; hi=59.6 -> raw
        // 62.56 -> lroundf 63) -- exactly the kind of one-tick sensor drift
        // that would flip the displayed integer bound every refresh without
        // quantization. Both land in the SAME UI_PAGE_HOME_AXIS_QUANT_STEP_
        // DISP=5 step (60..65), so the quantized, jitter-resistant result
        // must be identical.
        int32_t lo_a, hi_a, lo_b, hi_b;
        ui_page_home_active_y_axis_range(30.0f, 59.5f, 0.0f, false, 0, 0, &lo_a, &hi_a);
        ui_page_home_active_y_axis_range(30.0f, 59.6f, 0.0f, false, 0, 0, &lo_b, &hi_b);
        TEST_CHECK(lo_a == lo_b && hi_a == hi_b,
                   "sub-degree tick-to-tick drift that doesn't cross a quant step: identical quantized axis");

        // Anti-jitter, only-widen ratchet: a held range from a "previous
        // tick" must never be narrowed by a new tick whose own raw range is
        // smaller (e.g. the accumulated actual/planned lo/hi dipped a hair).
        // The returned range must still cover the held one.
        ui_page_home_active_y_axis_range(25.0f, 60.0f, 0.0f, false, 0, 0, &lo_a, &hi_a);
        // Next tick: a narrower raw range (as if the accumulated hi dropped)
        // fed in WITH the previous tick's own result held.
        ui_page_home_active_y_axis_range(25.0f, 40.0f, 0.0f, true, lo_a, hi_a, &lo_b, &hi_b);
        TEST_CHECK(lo_b <= lo_a && hi_b >= hi_a,
                   "only-widen ratchet: a narrower raw range this tick never shrinks the held axis");

        // And it DOES widen when the new data genuinely exceeds the held
        // range (a real overshoot arriving on a later tick).
        int32_t lo_c, hi_c;
        ui_page_home_active_y_axis_range(25.0f, 70.0f, 0.0f, true, lo_a, hi_a, &lo_c, &hi_c);
        TEST_CHECK(hi_c > hi_a, "genuine growth beyond the held range widens the axis");

        // Quantize never collapses: sweep lo/hi pairs across every possible
        // alignment against the UI_PAGE_HOME_AXIS_QUANT_STEP_DISP=5 grid
        // (raw_lo/raw_hi landing exactly on a step boundary, just past one,
        // and just before one) and confirm axis_hi > axis_lo always holds --
        // this is the case the removed `if (q_hi <= q_lo)` guard in
        // ui_page_home_active_y_axis_range() used to cover defensively.
        // ui_page_home_y_axis_range()'s own axis_hi>axis_lo contract makes
        // that guard provably unreachable (see this function's own header
        // comment in ui_page_home_graph.c), so this sweep stands in as the
        // proof instead of an untested guard.
        for (int base = 0; base < 50; base += 1) {
            int32_t lo_s, hi_s;
            // hi is always at least 1 above lo, matching
            // ui_page_home_y_axis_range()'s own guarantee.
            ui_page_home_active_y_axis_range((float)base, (float)base + 1.0f, 0.0f, false, 0, 0, &lo_s, &hi_s);
            TEST_CHECK(hi_s > lo_s, "quantize sweep: axis_hi > axis_lo at every grid alignment");
        }
    }

    TEST_SECTION("ui_page_home_graph: axis_ratchet_should_reset (run-identity defect fix, review of 98c3278)");
    {
        // Ordinary IDLE -> active start: prev_active false, always resets
        // regardless of elapsed_s values (nothing meaningful to compare).
        TEST_CHECK(ui_page_home_axis_ratchet_should_reset(true, false, 0, 0),
                   "IDLE -> active start: ratchet resets");

        // Not active this tick: no reset decision to make (caller does not
        // even consult this while idle, but the function must not claim a
        // reset here either).
        TEST_CHECK(!ui_page_home_axis_ratchet_should_reset(false, true, 0, 100),
                   "not active this tick: no reset");

        // THE BUG: a new run started from DONE. The old ui_page_home.c edge
        // check used `state != PROFILE_EXEC_IDLE` for "active", so DONE
        // already counted as active and profile_executor_start() allows a
        // new run to begin straight from DONE with no IDLE tick in between
        // -- the old code's IDLE->active edge never fired and the axis hold
        // was never reset. Here: previous tick was DONE (active=true,
        // elapsed=1620, a finished ~27 minute run), this tick is the new
        // run's first RUNNING sample (active=true, elapsed=0 -- total_elapsed_s
        // resets to 0 exactly once, at profile_executor_run()).
        TEST_CHECK(ui_page_home_axis_ratchet_should_reset(true, true, 0, 1620),
                   "new run started from DONE: ratchet resets (elapsed_s dropped)");

        // Same bug, FAULTED source instead of DONE.
        TEST_CHECK(ui_page_home_axis_ratchet_should_reset(true, true, 0, 340),
                   "new run started from FAULTED: ratchet resets (elapsed_s dropped)");

        // Stop/restart both happening inside one ~1 Hz refresh interval:
        // sampled state is "active" on the tick before (RUNNING, elapsed=50)
        // AND the tick after (the new run's RUNNING, elapsed=2) -- no IDLE
        // tick was ever observed, yet this is still a different run and must
        // still reset.
        TEST_CHECK(ui_page_home_axis_ratchet_should_reset(true, true, 2, 50),
                   "stop/restart within one refresh interval: ratchet resets (elapsed_s dropped)");

        // Regression guard: within a SINGLE run, elapsed_s only ever
        // increases (or holds flat across a PAUSE) -- the ratchet must NOT
        // reset on every ordinary tick, or the only-widen behaviour this
        // whole mechanism exists for would never accumulate.
        TEST_CHECK(!ui_page_home_axis_ratchet_should_reset(true, true, 51, 50),
                   "same run, elapsed_s advanced by one tick: no reset");
        TEST_CHECK(!ui_page_home_axis_ratchet_should_reset(true, true, 50, 50),
                   "same run, elapsed_s flat across a PAUSE tick: no reset");
        TEST_CHECK(!ui_page_home_axis_ratchet_should_reset(true, true, 1600, 10),
                   "same run, elapsed_s far advanced from an early tick: no reset");
    }

    TEST_SECTION("ui_page_home_graph: lag_notice debounce (PID_EXPANSION_PLAN.md 7.4)");
    {
        // Counter climbs one per held tick, resets instantly the moment the
        // lock clears -- no debounce on the way down.
        uint32_t ticks = 0;
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        TEST_CHECK(ticks == 1, "1st held tick -> counter 1");
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        TEST_CHECK(ticks == 4, "4 consecutive held ticks -> counter 4");
        TEST_CHECK(!ui_page_home_lag_notice_should_show(ticks),
                   "below UI_PAGE_HOME_LAG_NOTICE_DEBOUNCE_TICKS (5): not shown yet -- "
                   "a momentary lock must not flicker the notice on");

        ticks = ui_page_home_lag_notice_tick(true, ticks);
        TEST_CHECK(ticks == UI_PAGE_HOME_LAG_NOTICE_DEBOUNCE_TICKS,
                   "5th consecutive held tick reaches the threshold");
        TEST_CHECK(ui_page_home_lag_notice_should_show(ticks),
                   "threshold reached: notice shows");

        // Clearing is immediate, not debounced -- one non-held tick zeroes
        // the counter and the notice hides on the very next refresh.
        ticks = ui_page_home_lag_notice_tick(false, ticks);
        TEST_CHECK(ticks == 0, "lock clears: counter resets to 0 immediately");
        TEST_CHECK(!ui_page_home_lag_notice_should_show(ticks),
                   "counter 0: notice hidden the instant the lock clears");

        // A single noisy tick that flickers the lock momentarily (held, then
        // clear, then held again) must never accumulate across the gap --
        // each held run starts counting from 0 again.
        ticks = ui_page_home_lag_notice_tick(true, 0);
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        ticks = ui_page_home_lag_notice_tick(false, ticks); // momentary clear
        TEST_CHECK(ticks == 0, "momentary clear mid-run resets the count, not just held-back one tick");
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        ticks = ui_page_home_lag_notice_tick(true, ticks);
        TEST_CHECK(!ui_page_home_lag_notice_should_show(ticks),
                   "re-accumulating from the reset: still below threshold after only 4 more held ticks");

        // Saturation: never wraps back to a small value after a very long
        // sustained lag (a multi-day firing kept behind schedule).
        TEST_CHECK(ui_page_home_lag_notice_tick(true, UINT32_MAX) == UINT32_MAX,
                   "counter saturates at UINT32_MAX rather than wrapping");
    }

    TEST_SECTION("ui_page_home_graph: lag_notice_active (commit 1e03448 rich-field wiring)");
    {
        // Rich data present: reported immediately, no extra debounce beyond
        // what firmware's own EXEC_SUSTAINED_LAG_S already applied -- a
        // debounced_ticks of 0 (first ever tick) still shows if the zone is
        // already sustained.
        TEST_CHECK(ui_page_home_lag_notice_active(true, true, 0),
                   "rich data, zone sustained, tick 0: shows immediately (no client debounce)");
        TEST_CHECK(!ui_page_home_lag_notice_active(true, false, 0),
                   "rich data, no zone sustained: hidden");
        TEST_CHECK(!ui_page_home_lag_notice_active(true, false, UI_PAGE_HOME_LAG_NOTICE_DEBOUNCE_TICKS),
                   "rich data path ignores debounced_ticks entirely -- a stale nonzero "
                   "counter from a prior fallback-mode tick must not force it on");

        // Old-firmware fallback: behaves exactly like
        // ui_page_home_lag_notice_should_show() did before this pass.
        TEST_CHECK(!ui_page_home_lag_notice_active(false, false, UI_PAGE_HOME_LAG_NOTICE_DEBOUNCE_TICKS - 1u),
                   "fallback path, below threshold: hidden");
        TEST_CHECK(ui_page_home_lag_notice_active(false, false, UI_PAGE_HOME_LAG_NOTICE_DEBOUNCE_TICKS),
                   "fallback path, at threshold: shows");
        TEST_CHECK(!ui_page_home_lag_notice_active(false, true, 0),
                   "fallback path ignores any_zone_sustained entirely (older board never sets it)");
    }

    TEST_SECTION("ui_page_home_graph: lagging_zone_indices");
    {
        uint8_t idx[8];
        // No zones lagging -- mask 0.
        TEST_CHECK(ui_page_home_lagging_zone_indices(0x00, 8, idx, 8) == 0,
                   "empty mask -> 0 zones");

        // Zone 0 only.
        size_t n = ui_page_home_lagging_zone_indices(0x01, 8, idx, 8);
        TEST_CHECK(n == 1 && idx[0] == 0, "mask 0x01 -> zone 0 only");

        // Zones 1 and 3, lowest-index-first.
        n = ui_page_home_lagging_zone_indices(0x0Au /* 0b1010 */, 8, idx, 8);
        TEST_CHECK(n == 2 && idx[0] == 1 && idx[1] == 3, "mask 0x0A -> zones 1,3 in index order");

        // All 5 real MAX31856 channels lagging (max_zones caps the scan --
        // this codebase's channel count, not the full 8 mask bits).
        n = ui_page_home_lagging_zone_indices(0xFFu, 5, idx, 8);
        TEST_CHECK(n == 5 && idx[0] == 0 && idx[4] == 4,
                   "max_zones=5 caps the scan even though the mask has bits set above it");

        // out_cap smaller than the number of set bits truncates rather than
        // overflowing the caller's buffer.
        uint8_t small[2];
        n = ui_page_home_lagging_zone_indices(0xFFu, 8, small, 2);
        TEST_CHECK(n == 2 && small[0] == 0 && small[1] == 1,
                   "out_cap=2 truncates to the first 2 zones, does not overflow");
    }
}
