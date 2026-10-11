# Safety review: s8clrfx2, the S8 clear follow-up fix (2026-10-10)

Reviewer: Opus. Read-only on firmware. No board access.

Scope: origin/dev commits d2ee0e50e and 6873410c1. They fix LOW-1..3, I-1 and
I-2 of `docs/audits/REVIEW_S8CLRFX_2026-10-10.md`. Code is in
`firmware/SaftyFW/src/safety_guards.c`/`.h`. Tests are in
`firmware/SaftyFW/test/test_safety_guards.c`.

What the fix claims:

- A clear needs two consecutive full post-trip windows, both at or under the limit.
- The post window is latched at trip time and clamped to 60-600 s.
- A cooled kiln is now refused roughly 60-120 s after the trip.

## Verdict

The fix tightens the clear. It does not blind the guard. The trip path is
untouched. Each claim holds as written, with two caveats:

- "60-120 s" is true only for the default 60 s window. For a latched window W,
  the refusal runs from W to 2W.
- "One outlier cannot grant" is true only after the first full post window. In
  the frozen-window phase `[0, W)` a single outlier sample still grants (LOW-1
  below).

No HIGH or MED findings. Three LOW findings. Six INFO notes.

Compared with the code before the fix (055d34833):

| Phase after the trip | Before | After |
|---|---|---|
| `[0, W)`, no full post window | the frozen trip window decides | unchanged |
| `[W, 2W)`, one full post window | last full window + partial (>= 5 s) | always refused |
| `>= 2W` | last full window + partial | last AND previous full window + partial |
| W | live `rate_window_s` (any value > 0, else 60) | latched at the first valid post-trip tick, clamped to [60, 600] |

Every pre-fix refusal is still a refusal, with one exception, which is
by design: a commissioned `rate_window_s` above 1200 s (see I-3).

## Findings

### LOW-1: in the frozen-window phase, one outlier sample still grants

Before the first full post window, `guard_condition_still_immediate()` grants
when `(tc_now - s8_window_start_c) / s8_window_elapsed_min <= max_rate`. That is
a single-sample test against the frozen tripping window. The fix made the
post-window side tolerate one outlier, but this branch is unchanged, so the
LOW-1 class is still open for the first W seconds.

Scenario (probe P2, CAUGHT, `test_safety_guards.c:3617` in the probe copy):

1. `max_rate_c_per_min = 10`, `rate_window_s = 60`. The kiln rises at
   12 °C/min (20 -> 32 -> 44 °C). S8 trips. The frozen window is 32 °C over 60 s.
2. 1 s later the kiln is at 44.2 °C and still rising at 12 °C/min.
3. A CLEAR_TRIP arrives on a tick whose sample reads 2.3 °C low (41.9 °C).
   `(41.9 - 32) / 1 = 9.9 <= 10`, so the clear is **granted**.

The outlier is the same size as the one in the original LOW-1 probe P1
(2.5 °C). The needed outlier grows by only about 0.2 °C per second after the
trip, so the hole is widest right after the trip.

This also makes the clear non-monotonic for a kiln that really cooled. It is
granted in `[0, W)`, refused in `[W, 2W)`, and granted again from 2W. An
operator who presses Clear at 30 s succeeds. One who waits until 90 s is told
the trip is still active.

Why LOW, not higher: this predates the fix. K4 is open after the trip, so a
sustained 12 °C/min rise is physically unlikely. S9 covers a welded K4. After
a wrong grant, S8 re-trips after two more over-limit windows.

Fix: refuse every S8 clear while `s8_post_windows_done < 2`. The frozen-window
branch then never grants, so it can be removed. The earliest clear becomes 2W
(120 s or more). That is longer than `trip_verify_s` (default 10 s), so it
also closes the S8 part of REVIEW_S8CLRFX I-6. The behaviour becomes
monotonic. Update the GUARD_TEST_MATRIX S1/S8 row, and add P2 as a permanent
test.

### LOW-2: the 60 s floor of the clamp is untested

Mutation M1 turns `lw < S8_POST_WIN_MIN_S ? S8_POST_WIN_MIN_S` into
`lw < 0.0f ? ...`, which removes the floor. It is **MISSED**.

- The LOW-3 test trips with `rate_window_s = 60`, so the latched window is 60
  either way. The mid-trip change to 2 s only proves the latch.
- The only clamp test is the 30000 s ceiling case (M2 CAUGHT).

Scenario the floor protects: `rate_window_s` is commissioned to 10 s, which
param 0x0205 allows. Without the floor, each post window is 10 s, two of them
take 20 s, and ±0.15 °C of noise moves the rate by ±0.9 °C/min. A 2.5 °C
outlier moves it by 15 °C/min. That puts the clear back in the saftyfx7 MED-1
noise class.

Fix: add a test that trips with `rate_window_s = 10`. Assert that
`s8_post_win_s` is 60, and that `s8_post_windows_done == 0` after 30 s of post
ticks. Negtest it with M1.

### LOW-3: the post block's `tc_valid` gate is untested, and NaN flagged valid grants

The post block runs only `if (state->reason == SAFETY_TRIP_RATE && in->tc_valid)`.

Mutation M7 drops the `in->tc_valid` term, and it is **MISSED**. No test feeds
invalid TC ticks during the post window. If invalid ticks entered the window,
their `tc_c` (NaN in production, `thermo_task.c:634`) would set
`s8_post_start_c` and both window rates to NaN. NaN comparisons are false, so
every refusal test passes and the clear is granted.

The module itself has the same NaN weakness wherever `tc_valid` is true. It
never asks `s5_bad_read_now()`, even though that is the module's own
definition of a bad read and includes `isnan(tc_c)`.

- P4 (CAUGHT): right after the trip, `tc_valid = true` and `tc_c = NAN`. The
  frozen-window branch computes `NaN > limit`, which is false, and the clear
  is granted.
- P3 (CAUGHT): NaN flagged valid for 121 s. Both post windows read NaN, and
  the clear is granted.

This is not reachable in production. `thermo_task.c:711-722` forces
`valid = false` for any NaN reading (plausibility check). So this is
defence-in-depth and a test gap, not a live hole. S1's clear
(`in->tc_c > abs_max`) has the same NaN shape.

Fix:

- In the `SAFETY_TRIP_RATE` case, refuse on `s5_bad_read_now(in)` instead of
  `!in->tc_valid`.
- In the post block, skip any tick where `s5_bad_read_now(in)` is true.
- Optionally refuse when either stored rate is not finite.
- Add P3/P4 and an invalid-TC-during-post test. Negtest it with M7.

### INFO

- **I-1, the comment is stale.** The block comment at `safety_guards.c:204-210`
  still says "Once a full window exists, refuse if EITHER the last full rate
  OR the partial window ... is over the limit". The code now needs two full
  windows and checks last, previous and partial. The inline comment at 211-216
  is correct. Reword the block comment.
- **I-2, one LOW-3 test is guarded by `if`.** The 30000 s ceiling test asserts
  only `if (safety_guards_tick(...) && s.reason == SAFETY_TRIP_RATE)`. If a
  future guard trips first on that setup (tc = 80000 °C), the test passes
  without asserting anything. It does trip today (M2 CAUGHT). Make the trip a
  `TEST_CHECK` sanity line, like the other blocks.
- **I-3, the only path that clears earlier than before.** For a commissioned
  `rate_window_s` above 1200 s, the old code waited one full window of that
  length, deciding with the frozen window until then. The new code decides
  from two 600 s windows at 1200 s. A kiln that rose a lot since the frozen
  window start, but whose last 20 minutes are under the limit, is now granted
  where the old code refused. That is a measured rate at or under the limit,
  which matches the guard's definition. Acceptable. It needs commissioning
  above 1200 s anyway.
- **I-4, the latch time is the first valid post-trip tick, not the trip tick.**
  If the TC is invalid right after the trip, a COMMIT_CONFIG landing before
  the first valid tick sets W. It is still clamped, so the effect is benign.
- **I-5, resets (the reset-one-side bug class).** No stale state found.
  - A grant runs `safety_guards_clear()`, a `memset`. A Pico reboot runs
    `safety_guards_reset()` (`safety_core.c:1437`).
  - A re-trip can only start from a cleared state, so `s8_post_active`,
    `s8_post_windows_done`, `s8_post_win_s` and both rates start at zero.
  - A refused clear leaves the state untouched.
  - A change to `rate_window_s` is now ignored mid-trip (latched).
    `max_rate_c_per_min` is compared live at clear time. The stored rates are
    in °C/min, so nothing derived needs a reset. Setting it to 0 grants,
    which is pre-existing and by design.
  - The S9 escalation changes the reason to INEFFECTIVE, which stops the post
    block. That trip is unclearable.
  - No other module holds a copy of the post state.
  - Overflow: `s8_post_windows_done` saturates at 2. `s8_post_elapsed_s`
    resets every window, so it stays under 600 + dt. One huge `dt_s` closes
    at most one window per tick. A 0 `dt_s` freezes the window, so the frozen
    branch decides, which is fail-safe apart from LOW-1.
- **I-6, interaction with TEST_TRIP.** WP2, the Pico-side latch, is not on
  origin/dev as of 48caa9a8a. Only WP1 (c2a866b23, wire format) and its review
  (37eb76be5) are. Per `docs/TEST_TRIP_PLAN.md` sec 4.1-4.3:
  - A test trip is evaluated only in the not-tripped branch, after every real
    guard.
  - It is refused while any trip is latched.
  - So it can neither overwrite a RATE reason nor inherit `s8_post_*` state.

  For WP2:
  - Keep the post block gated on `reason == SAFETY_TRIP_RATE`.
  - Put any new state field inside `safety_guard_state_t`, so the clear's
    `memset` covers it.
  - The planned `SAFETY_TRIP_TEST` clear waits `trip_verify_s`. S8's
    frozen-window grant does not. LOW-1's fix removes that asymmetry for S8.

Verified as fixed: I-1 (REVIEW_SAFTYFX7 now cites 055d34833/6e7470c95, and
055d34833 is an ancestor of origin/dev). I-2: `test_safety_guards.c` starts
with `//` and has no BOM. The REVIEW_S8CLRFX "Fix status" section names no fix
SHA. It is d2ee0e50e, plus 6873410c1 for the one-window cooled test. The
GUARD_TEST_MATRIX S1/S8 row matches the code.

## Testing

- SaftyFW host tests at 6873410c1, worktree `C:\wt\rvs8clrfx2_qzfdgk`:
  `SAFTYFW HOST TESTS: all passed`.
- `tools\negtest.ps1 -Preset saftyfw-host -ExpectPattern "FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED" -Mutations <12> -Parallel 4`,
  base 6873410c1. The baseline passed, the real tree was unchanged and the
  copies were removed.

| Mutation / probe | Result |
|---|---|
| M1: clamp floor removed (`lw < 0.0f`) | **MISSED** (LOW-2) |
| M2: clamp ceiling removed | CAUGHT, `:3573` LOW-3 huge window |
| M3: previous-window check dropped | CAUGHT, `:3509` P1 |
| M4: one-window refusal removed | CAUGHT, `:3596` one window when cooled |
| M5: partial check dead (old N1) | CAUGHT, `:3531` LOW-2 fast partial |
| M6: live `rate_window_s` instead of latched | CAUGHT, `:3553` LOW-3 mid-trip |
| M7: post block runs on invalid TC | **MISSED** (LOW-3) |
| M8: previous rate never shifted (stays 0) | CAUGHT, `:3509` P1 |
| M9: partial threshold 5 s -> 50 s | CAUGHT, `:3531` |
| P2 probe: 12 °C/min rise, a 2.3 °C low sample 1 s after the trip, asserts refusal | CAUGHT, so the clear is granted (LOW-1) |
| P3 probe: NaN with `tc_valid` for 121 s, asserts refusal | CAUGHT, so it is granted (LOW-3) |
| P4 probe: NaN with `tc_valid` right after the trip, asserts refusal | CAUGHT, so it is granted (LOW-3) |

Logs: `%TEMP%\negtest_logs\20261010_200647_kr8j`.
