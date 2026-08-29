#include "test_common.h"
#include "../drivers/heater_output.h"

void run_test_heater_output(void)
{
    TEST_SECTION("heater_output");

    /* Bang-bang: a pending want_on!=relay_on stays pending across multiple
     * calls (not just the single call where want_on first changed) and
     * applies the instant since_last_change_ms reaches min_ms. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 2000, .min_off_ms = 2000};
        heater_output_reset(&s);
        bool r = heater_output_bangbang(&s, &cfg, true, 0);
        TEST_CHECK(r == false, "off->on requested at t0: min_off_ms(2000) not yet elapsed, held off");
        r = heater_output_bangbang(&s, &cfg, true, 1000);
        TEST_CHECK(r == false, "still held off at 1000ms of 2000ms required -- want_on unchanged across calls");
        r = heater_output_bangbang(&s, &cfg, true, 1500);
        TEST_CHECK(r == true, "2500ms elapsed >= min_off_ms(2000): pending on is finally applied");
        TEST_CHECK(s.cycle_count == 1, "one transition counted so far");
        /* Flip back to off almost immediately -- inside min_on_ms(2000). */
        r = heater_output_bangbang(&s, &cfg, false, 200);
        TEST_CHECK(r == true, "flip-to-off inside min_on_ms is held at on");
        r = heater_output_bangbang(&s, &cfg, false, 1900);
        TEST_CHECK(r == false, "2100ms since the on-transition >= min_on_ms(2000): off is applied");
        TEST_CHECK(s.cycle_count == 2, "two transitions counted: on, then off");
    }

    /* Time-proportioned: 50% duty over a 60s window with negligible
     * min_on/min_off renders on for the first half, off for the second. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        bool r = heater_output_duty(&s, &cfg, 0.5f, 0);
        TEST_CHECK(r == true, "50% duty: window opens ON");
        TEST_CHECK(s.on_ms_this_window == 30000, "on-time computed as duty*window_ms");
        r = heater_output_duty(&s, &cfg, 0.5f, 29000);
        TEST_CHECK(r == true, "still within the 30000ms on-time");
        r = heater_output_duty(&s, &cfg, 0.5f, 2000);
        TEST_CHECK(r == false, "past the on-time within the same window -- now off");
    }

    /* Quantization: a duty whose computed on-time is below min_on_ms
     * renders OFF for the whole window, not rounded up to min_on_ms. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 2000, .min_off_ms = 2000};
        heater_output_reset(&s);
        /* duty=0.02 -> on_ms = 1200, below min_on_ms(2000). */
        bool r = heater_output_duty(&s, &cfg, 0.02f, 0);
        TEST_CHECK(r == false, "unachievably short on-time renders as OFF for the window, not clamped up");
        TEST_CHECK(s.on_ms_this_window == 0, "on_ms_this_window recorded as 0, confirming it wasn't rounded up");
    }

    /* Symmetric case: duty near 1 whose off-time would be below min_off_ms
     * renders ON for the whole window. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 2000, .min_off_ms = 2000};
        heater_output_reset(&s);
        /* duty=0.99 -> on_ms=59400, off would be 600ms < min_off_ms(2000). */
        bool r = heater_output_duty(&s, &cfg, 0.99f, 0);
        TEST_CHECK(r == true, "near-1 duty renders ON for the whole window when the off-time would be unachievable");
        TEST_CHECK(s.on_ms_this_window == cfg.window_ms, "on_ms_this_window snapped to the full window");
    }

    /* duty is clamped to [0,1] before any of the above logic. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        heater_output_duty(&s, &cfg, 5.0f, 0);
        TEST_CHECK(s.on_ms_this_window == cfg.window_ms, "duty > 1 clamps to 1.0 -> full window on");
        heater_output_reset(&s);
        heater_output_duty(&s, &cfg, -5.0f, 0);
        TEST_CHECK(s.on_ms_this_window == 0, "duty < 0 clamps to 0.0 -> full window off");
    }

    /* force_off drops the relay immediately and resets window/debounce
     * timing, but not the lifetime cycle_count. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        heater_output_duty(&s, &cfg, 1.0f, 0);
        TEST_CHECK(s.relay_on, "sanity: relay on before force_off");
        uint32_t cycles_before = s.cycle_count;
        heater_output_force_off(&s);
        TEST_CHECK(!s.relay_on, "force_off drops the relay immediately");
        TEST_CHECK(s.window_started == false, "force_off resets window_started for a clean next window");
        TEST_CHECK(s.cycle_count == cycles_before + 1, "force_off counts the on->off transition it caused");
    }

    /* heater_output_seed_phase: TODO.md 6A.5's load-staggering item. A
     * zone seeded with a 20000ms offset into a 60000ms window starts its
     * first (truncated) window OFF, then its window boundaries land 40000ms
     * later than an unphased zone's -- forever, not just for one window. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        heater_output_seed_phase(&s, cfg.window_ms, 20000);
        bool r = heater_output_duty(&s, &cfg, 0.5f, 0);
        TEST_CHECK(r == false, "seeded phase: first (truncated) window starts OFF regardless of duty");
        r = heater_output_duty(&s, &cfg, 0.5f, 39999);
        TEST_CHECK(r == false, "still inside the truncated first window (39999 < 40000ms remaining)");
        r = heater_output_duty(&s, &cfg, 0.5f, 2);
        TEST_CHECK(r == true, "40001ms in: first window boundary crossed, second (full, on-time) window begins");
        TEST_CHECK(s.on_ms_this_window == 30000, "second window computes on-time normally from duty (unaffected by phase)");
        r = heater_output_duty(&s, &cfg, 0.5f, 29999);
        TEST_CHECK(r == true, "still inside the second window's on-time (29999ms < 30000ms into it)");
        r = heater_output_duty(&s, &cfg, 0.5f, 2);
        TEST_CHECK(r == false, "30001ms into the second window -- past its 30000ms on-time");
        /* Crossing a window boundary resets window_elapsed_ms to exactly 0,
         * dropping (not carrying forward) whatever pushed it past the
         * boundary -- same behavior heater_output_duty() already has
         * without phase, confirmed above by the boundary crossing at
         * 40001ms landing exactly at window_elapsed_ms==0 for the second
         * window rather than ==1. An unphased zone's boundaries would be
         * at 60000/120000ms; this phased zone's are at 40000/100000ms -- a
         * permanent shift, not a one-time transient. */
    }
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        heater_output_seed_phase(&s, cfg.window_ms, 0);
        TEST_CHECK(s.window_started == false, "phase_offset_ms == 0 is a no-op -- first window unaffected");
        heater_output_reset(&s);
        heater_output_seed_phase(&s, 0, 20000);
        TEST_CHECK(s.window_started == false, "window_ms == 0 is a no-op (avoids a mod-by-zero)");
    }

    /* reset() preserves cycle_count (a lifetime/contact-life counter) but
     * clears everything else. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 60000, .min_on_ms = 0, .min_off_ms = 0};
        heater_output_reset(&s);
        heater_output_bangbang(&s, &cfg, true, 0);
        heater_output_bangbang(&s, &cfg, false, 0);
        uint32_t cycles = s.cycle_count;
        TEST_CHECK(cycles >= 2, "sanity: some cycles recorded");
        heater_output_reset(&s);
        TEST_CHECK(s.cycle_count == cycles, "reset() preserves the lifetime cycle_count");
        TEST_CHECK(s.relay_on == false, "reset() clears relay_on");
    }

    /* HEATER_MIN_ON_MS_FLOOR (10 s), owner request 2026-08-28. A duty whose
     * on-time is long enough to start but which then collapses must not
     * open the contacts before 10 s of continuous on-time. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 20000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        /* Window 1: duty 0.6 -> 12000ms on, above the floor, so it runs. */
        bool r = heater_output_duty(&s, &cfg, 0.6f, 0);
        TEST_CHECK(r == true, "min-on floor: window opens ON at duty 0.6");
        /* Demand collapses to 0 immediately -- but the window's on-time was
         * already fixed, so this exercises the running hold at the boundary
         * of the next window, below. First walk to just under 10 s. */
        r = heater_output_duty(&s, &cfg, 0.0f, 9999);
        TEST_CHECK(r == true, "still ON at 9999ms -- inside the 10s floor");
        r = heater_output_duty(&s, &cfg, 0.0f, 1);
        TEST_CHECK(r == true, "at exactly 10000ms the window's own 12000ms on-time still holds it ON");
    }

    /* The case the floor exists for: a window SHORTER than 10 s. A
     * full-window ON must be held across window boundaries until 10 s. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 4000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        bool r = heater_output_duty(&s, &cfg, 1.0f, 0);
        TEST_CHECK(r == true, "4s window at duty 1.0 still heats -- the floor does not clamp the zone dead");
        TEST_CHECK(s.on_ms_this_window == cfg.window_ms, "short window: on-time is the whole window");
        /* Demand drops to zero from here on. Window 2 (t=4000) computes an
         * on-time of 0, so without the running hold the relay would open at
         * 4000ms -- 6s short of the floor. */
        r = heater_output_duty(&s, &cfg, 0.0f, 4000);
        TEST_CHECK(r == true, "held ON across the window boundary at 4000ms -- only 4s of on-time so far");
        TEST_CHECK(s.on_ms_this_window == 0, "the new window did compute an on-time of 0 -- it is the hold keeping it on");
        r = heater_output_duty(&s, &cfg, 0.0f, 3000);
        TEST_CHECK(r == true, "still held at 7000ms of continuous on-time");
        r = heater_output_duty(&s, &cfg, 0.0f, 2999);
        TEST_CHECK(r == true, "still held at 9999ms -- one millisecond short of the floor");
        r = heater_output_duty(&s, &cfg, 0.0f, 1);
        TEST_CHECK(r == false, "10000ms of continuous on-time reached: the deferred OFF is applied, not earlier");
    }

    /* THE SAFETY TEST. A trip/halt/stop calls heater_output_force_off(),
     * which must de-energize on the tick it happens even with the 10s
     * min-on hold still pending. If this ever fails, the min-on logic has
     * been wired into the safety path and is delaying a real shutdown. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 4000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        bool r = heater_output_duty(&s, &cfg, 1.0f, 0);
        TEST_CHECK(r == true, "sanity: relay ON");
        r = heater_output_duty(&s, &cfg, 0.0f, 1500);
        TEST_CHECK(r == true, "sanity: 1500ms in, the min-on hold is pending");
        heater_output_force_off(&s); /* safety trip */
        TEST_CHECK(s.relay_on == false, "a safety force_off de-energizes IMMEDIATELY, 8.5s inside the min-on hold");
        TEST_CHECK(s.on_elapsed_ms == 0, "force_off clears the min-on accumulator -- the hold cannot resurrect the relay");
        /* And the very next control tick must not re-close the contacts
         * just because the hold was pending when the trip landed. */
        r = heater_output_duty(&s, &cfg, 0.0f, 10);
        TEST_CHECK(r == false, "the tick after a trip stays OFF at duty 0");
    }

    /* A configured min_on_ms below the floor is raised to it, not honoured
     * as-is: 0.2 duty of a 20s window is 4000ms, which cfg->min_on_ms=100
     * would allow but the 10s floor rejects for the window. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 20000, .min_on_ms = 100, .min_off_ms = 100};
        heater_output_reset(&s);
        bool r = heater_output_duty(&s, &cfg, 0.2f, 0);
        TEST_CHECK(r == false, "a 4000ms on-time is below the 10s floor even though cfg->min_on_ms is 100");
        TEST_CHECK(s.on_ms_this_window == 0, "sub-floor on-time renders as OFF for the window, not rounded up");
    }

    /* ------------------------------------------------------------------
     * The window-vs-min-on relationship (2026-08-29).
     *
     * This board's zone 0 was configured window_ms=2000 against the 10 s
     * floor. Both numbers were inside their own ranges; together they made
     * every fractional duty unrenderable, and an autotune step at duty 0.4
     * commanded heat for 40 minutes without the relay ever closing. The
     * config layer now refuses that pairing; these cases pin down both the
     * predicate it uses and the behaviour it protects.
     * ------------------------------------------------------------------ */

    /* The predicate. Negative-tested first -- a check nothing can fail is
     * not a check. */
    {
        TEST_CHECK(heater_output_required_window_ms(0) == 30000u,
                   "an unconfigured min_on uses the 10 s floor: required window is 30000 ms");
        TEST_CHECK(heater_output_required_window_ms(2000) == 30000u,
                   "a sub-floor min_on is raised before the multiple is applied, not multiplied as-is");
        TEST_CHECK(heater_output_required_window_ms(20000) == 60000u,
                   "the bound tracks a longer configured min_on -- it is not the constant 30000");

        heater_output_cfg_t bad = {.window_ms = 2000, .min_on_ms = 0, .min_off_ms = 100};
        TEST_CHECK(!heater_output_cfg_expressible(&bad),
                   "the bench's own 2000 ms window is reported unexpressible");
        heater_output_cfg_t edge_lo = {.window_ms = 29999, .min_on_ms = 0, .min_off_ms = 100};
        TEST_CHECK(!heater_output_cfg_expressible(&edge_lo),
                   "one millisecond under the bound still fails -- catches a > written for a >=");
        heater_output_cfg_t edge_ok = {.window_ms = 30000, .min_on_ms = 0, .min_off_ms = 100};
        TEST_CHECK(heater_output_cfg_expressible(&edge_ok), "exactly the bound passes: inclusive");
        heater_output_cfg_t deflt = {.window_ms = 0, .min_on_ms = 0, .min_off_ms = 0};
        TEST_CHECK(heater_output_cfg_expressible(&deflt),
                   "window_ms 0 means 'caller substitutes the 60 s default', which satisfies the rule");
        heater_output_cfg_t long_on = {.window_ms = 45000, .min_on_ms = 20000, .min_off_ms = 100};
        TEST_CHECK(!heater_output_cfg_expressible(&long_on),
                   "45 s against a 20 s min-on fails -- the bound is 3x THIS cfg's min-on");
    }

    /* The behaviour the rule protects: the bench's broken pairing renders a
     * real commanded duty as nothing at all. This is the bug, pinned. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 2000, .min_on_ms = 0, .min_off_ms = 100};
        heater_output_reset(&s);
        bool ever_on = false;
        for (int i = 0; i < 100; ++i) { /* 20 s of ticks, ten whole windows */
            ever_on |= heater_output_duty(&s, &cfg, 0.4f, 200);
        }
        TEST_CHECK(!ever_on,
                   "duty 0.4 in a 2000 ms window never closes the relay -- the zone-0 defect, reproduced");
        TEST_CHECK(s.cycle_count == 0, "not one relay transition in ten windows of commanded heat");
    }

    /* And a correctly sized window renders that same duty as a real
     * fractional on-time: ~12 s on, ~18 s off in a 30 s window. */
    {
        heater_output_state_t s = {0};
        heater_output_cfg_t cfg = {.window_ms = 30000, .min_on_ms = 0, .min_off_ms = 100};
        heater_output_reset(&s);
        TEST_CHECK(heater_output_cfg_expressible(&cfg), "sanity: this cfg satisfies the rule");

        uint32_t on_ticks = 0;
        for (int i = 0; i < 300; ++i) { /* 30 s at 100 ms, exactly one window */
            if (heater_output_duty(&s, &cfg, 0.4f, 100)) {
                on_ticks++;
            }
        }
        TEST_CHECK(s.on_ms_this_window == 12000u,
                   "0.4 * 30000 = 12000 ms of on-time is computed for the window, not quantized to 0");
        TEST_CHECK(on_ticks == 120u, "and 120 of the 300 ticks are actually energized: 12 s on, 18 s off");
        TEST_CHECK(on_ticks * 100u > HEATER_MIN_ON_MS_FLOOR,
                   "the rendered pulse clears the 10 s floor, which is the whole point of the 3x rule");
        TEST_CHECK(s.cycle_count == 2, "one ON and one OFF transition -- a real duty cycle, not bang-bang");
    }
}
