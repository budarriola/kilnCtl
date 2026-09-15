// Host tests for pid_fuzzy_confidence.c -- the confidence gate,
// docs/ADAPTIVE_FUZZY_EVALUATION_PLAN.md sec 3.
#include "test_common.h"
#include "../drivers/control/pid_fuzzy_confidence.h"

#include <math.h>

void run_test_pid_fuzzy_confidence(void)
{
    TEST_SECTION("pid_fuzzy_confidence");

    // --- cap_l: the anti-limit-cycle authority cap (N2) ---
    {
        // L/tau = 0 -> full authority (a zero-dead-time plant, e.g. the
        // factorial's 0 s row, which the plan measured stable).
        TEST_CHECK(pid_fuzzy_confidence_cap_l(0.0f, 100.0f) == 1.0f, "cap_l: L/tau=0 -> 1.0");
        // Exactly at the 0.10 breakpoint -- still full authority (plan:
        // "<= 0.10").
        TEST_CHECK(pid_fuzzy_confidence_cap_l(10.0f, 100.0f) == 1.0f, "cap_l: L/tau=0.10 -> 1.0 (boundary)");
        // Exactly at the 0.30 breakpoint -- fully closed (plan's linear taper
        // is "0.10 < L/tau <= 0.30", so 0.30 itself is the last point of the
        // taper, landing at 0.0).
        {
            float c30 = pid_fuzzy_confidence_cap_l(30.0f, 100.0f);
            TEST_CHECK(fabsf(c30 - 0.0f) < 1e-5f, "cap_l: L/tau=0.30 -> 0.0 (boundary)");
        }
        // Beyond 0.30 -- fully closed.
        TEST_CHECK(pid_fuzzy_confidence_cap_l(64.0f, 100.0f) == 0.0f, "cap_l: L/tau=0.64 -> 0.0");
        // Midpoint of the taper (L/tau = 0.20) -> ~0.5.
        {
            float mid = pid_fuzzy_confidence_cap_l(20.0f, 100.0f);
            TEST_CHECK(fabsf(mid - 0.5f) < 1e-5f, "cap_l: L/tau=0.20 -> ~0.5 (linear taper midpoint)");
        }
        // The plan's own worked kiln-scaled example: L/tau = 76.9/488 =
        // 0.1576 (inside the taper, not at either boundary) -- pinned so a
        // future edit to the breakpoints or the interpolation direction is
        // caught, not just the two endpoints.
        {
            float ratio = 76.9f / 488.0f;
            float cap = pid_fuzzy_confidence_cap_l(76.9f, 488.0f);
            float expected = 1.0f - (ratio - 0.10f) / (0.30f - 0.10f);
            TEST_CHECK(fabsf(cap - expected) < 1e-4f, "cap_l: plan's worked kiln-scaled L/tau example matches formula");
            TEST_CHECK(cap > 0.0f && cap < 1.0f, "cap_l: plan's worked example lands strictly inside the taper");
        }
        // Fail-safe on bad inputs: MUST read as "no authority," never as
        // "full authority" -- an unidentified plant (tau_s <= 0, the "never
        // autotuned" sentinel) must not accidentally grant cap_l=1.0.
        TEST_CHECK(pid_fuzzy_confidence_cap_l(0.0f, 0.0f) == 0.0f, "cap_l: tau_s=0 -> 0.0 (fail safe, not 1.0)");
        TEST_CHECK(pid_fuzzy_confidence_cap_l(0.0f, -5.0f) == 0.0f, "cap_l: negative tau_s -> 0.0");
        TEST_CHECK(pid_fuzzy_confidence_cap_l(-1.0f, 100.0f) == 0.0f, "cap_l: negative dead_time_s -> 0.0");
        TEST_CHECK(pid_fuzzy_confidence_cap_l(NAN, 100.0f) == 0.0f, "cap_l: NaN dead_time_s -> 0.0");
        TEST_CHECK(pid_fuzzy_confidence_cap_l(10.0f, NAN) == 0.0f, "cap_l: NaN tau_s -> 0.0");
        TEST_CHECK(pid_fuzzy_confidence_cap_l(INFINITY, 100.0f) == 0.0f, "cap_l: +inf dead_time_s -> 0.0");
    }

    // --- strength_pct: the c/4 * cap_l authority schedule ---
    {
        // c=0 -> exactly 0 regardless of cap_l -- "what happens when
        // confidence is LOW": bit-for-bit plain PID, no reduced-but-nonzero
        // mode to reason about.
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(0, 1.0f) == 0, "strength: c=0, cap_l=1.0 -> 0");
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(0, 0.0f) == 0, "strength: c=0, cap_l=0.0 -> 0");
        // cap_l=0 -> exactly 0 regardless of c -- the actual point of the
        // whole design (sec 1.4): a MATCHED model (high c) at a harmful
        // L/tau must NOT get raised authority.
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(4, 0.0f) == 0, "strength: c=4 (max), cap_l=0.0 -> 0 (the central design point)");
        // c=4, cap_l=1.0 -> full S_MAX.
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(4, 1.0f) == PID_FUZZY_CONFIDENCE_S_MAX_PCT,
                   "strength: c=4, cap_l=1.0 -> S_MAX (50)");
        // c=2 (half), cap_l=1.0 -> half of S_MAX (25).
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(2, 1.0f) == 25, "strength: c=2/4, cap_l=1.0 -> 25");
        // c=1, cap_l=0.5 -> 50 * 0.25 * 0.5 = 6.25 -> rounds to 6.
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(1, 0.5f) == 6, "strength: c=1/4, cap_l=0.5 -> round(6.25)=6");
        // c clamps above the documented max (a caller bug must not silently
        // grant MORE than S_MAX).
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(255, 1.0f) == PID_FUZZY_CONFIDENCE_S_MAX_PCT,
                   "strength: c way over max clamps to MAX_C, not overflow");
        // cap_l clamps above 1.0 / below 0.0 (caller rounding slop) rather
        // than pushing strength out of [0, S_MAX].
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(4, 1.5f) == PID_FUZZY_CONFIDENCE_S_MAX_PCT,
                   "strength: cap_l>1.0 clamps, does not exceed S_MAX");
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(4, -0.5f) == 0, "strength: cap_l<0.0 clamps to 0");
        TEST_CHECK(pid_fuzzy_confidence_strength_pct(4, NAN) == 0, "strength: NaN cap_l -> 0 (fail safe)");
    }

    // --- oscillation backstop (N3): reset semantics ---
    {
        pid_fuzzy_oscillation_state_t st;
        pid_fuzzy_oscillation_reset(&st);
        TEST_CHECK(st.tripped_this_firing == false, "osc: reset() clears tripped_this_firing");
        TEST_CHECK(st.have_prev_error == false, "osc: reset() clears have_prev_error");
        TEST_CHECK(st.crossings_in_window == 0, "osc: reset() clears crossing count");
    }

    // --- oscillation backstop: a converged approach (one crossing, then
    // settle) must NOT trip -- this is the "0 crossings in both stable
    // arms" case from the plan's own measurement, and it is the case a
    // sloppy implementation (e.g. counting every sign disagreement without
    // a real crossing, or counting settle-noise) would most likely get
    // wrong. ---
    {
        pid_fuzzy_oscillation_state_t st;
        pid_fuzzy_oscillation_reset(&st);
        bool any_trip = false;
        // Approach from +10 error down through 0 to a settled 0.05 error,
        // one tick per second: exactly one sign change (+ -> 0-neighborhood
        // positive the whole time actually settles positive, so let's cross
        // once deliberately: +10 .. +0.5 .. -0.2 .. settle at -0.05).
        float trace[] = {10.0f, 6.0f, 3.0f, 1.0f, 0.5f, -0.2f, -0.1f, -0.05f, -0.05f, -0.05f};
        for (size_t i = 0; i < sizeof(trace) / sizeof(trace[0]); i++) {
            if (pid_fuzzy_oscillation_tick(&st, trace[i], 1.0f)) any_trip = true;
        }
        TEST_CHECK(!any_trip, "osc: single converged approach (1 crossing) does not trip");
        TEST_CHECK(!st.tripped_this_firing, "osc: tripped_this_firing stays false after a converged approach");
    }

    // --- oscillation backstop: a real limit cycle (many crossings within
    // one window) DOES trip, and stays tripped for the rest of the firing
    // even after the caller stops feeding it further crossings. ---
    {
        pid_fuzzy_oscillation_state_t st;
        pid_fuzzy_oscillation_reset(&st);
        bool trip_edge_seen = false;
        int trip_edge_count = 0;
        // Alternate +1/-1 every second -- a textbook limit cycle. 12
        // crossings well inside one 600s window, comfortably below the
        // plan's measured 29 and well above the trip threshold, so this is
        // an unambiguous trip case, not a boundary probe.
        for (int i = 0; i < 12; i++) {
            float e = (i % 2 == 0) ? 1.0f : -1.0f;
            if (pid_fuzzy_oscillation_tick(&st, e, 1.0f)) {
                trip_edge_seen = true;
                trip_edge_count++;
            }
        }
        TEST_CHECK(trip_edge_seen, "osc: a real limit cycle trips within one window");
        TEST_CHECK(trip_edge_count == 1, "osc: the trip edge fires exactly once (level, not repeated)");
        TEST_CHECK(st.tripped_this_firing, "osc: tripped_this_firing is true after the trip");
        // Now feed it a perfectly quiet, converged signal -- sticky for the
        // rest of the firing, per plan sec 3.2 ("for the remainder of the
        // firing").
        for (int i = 0; i < 5; i++) {
            (void)pid_fuzzy_oscillation_tick(&st, 0.01f, 1.0f);
        }
        TEST_CHECK(st.tripped_this_firing, "osc: trip is sticky through quiet ticks (does not un-trip)");
    }

    // --- oscillation backstop: reset() at the next firing's start clears a
    // trip from the previous firing -- the "reset one side of a pair"
    // class this project has hit before, checked explicitly in the
    // direction that matters here (failing to clear, not failing to set). ---
    {
        pid_fuzzy_oscillation_state_t st;
        pid_fuzzy_oscillation_reset(&st);
        for (int i = 0; i < 12; i++) {
            float e = (i % 2 == 0) ? 1.0f : -1.0f;
            (void)pid_fuzzy_oscillation_tick(&st, e, 1.0f);
        }
        TEST_CHECK(st.tripped_this_firing, "osc: precondition -- tripped from a prior firing's limit cycle");
        pid_fuzzy_oscillation_reset(&st);
        TEST_CHECK(!st.tripped_this_firing, "osc: reset() at a new firing's start clears a stale trip");
    }

    // --- oscillation backstop: bad ticks are no-ops, not false crossings
    // and not silent window resets. ---
    {
        pid_fuzzy_oscillation_state_t st;
        pid_fuzzy_oscillation_reset(&st);
        TEST_CHECK(!pid_fuzzy_oscillation_tick(&st, 1.0f, 1.0f), "osc: first real tick establishes prev, no trip");
        TEST_CHECK(!pid_fuzzy_oscillation_tick(&st, NAN, 1.0f), "osc: NaN error_c tick is a no-op (no trip)");
        TEST_CHECK(!pid_fuzzy_oscillation_tick(&st, -1.0f, 0.0f), "osc: dt_s=0 tick is a no-op (no trip)");
        TEST_CHECK(!pid_fuzzy_oscillation_tick(&st, -1.0f, -1.0f), "osc: negative dt_s tick is a no-op (no trip)");
        // The bad ticks above must not have silently counted as a crossing
        // against the still-live prev_error_c=1.0f -- a real crossing tick
        // right after them must be the FIRST one counted.
        TEST_CHECK(st.crossings_in_window == 0, "osc: bad ticks left the crossing count untouched");
    }
}
