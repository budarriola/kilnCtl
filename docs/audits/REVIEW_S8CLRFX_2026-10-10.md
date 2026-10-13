# Safety review: s8clrfx, the S8 rate-guard clear fix (2026-10-10)

Reviewer: Opus, read-only on firmware. No board access.

Scope: origin/dev commits 055d34833, 6e7470c95 and 652804cbf. They fix MED-1,
LOW-1 and LOW-2 of `docs/audits/REVIEW_SAFTYFX7_2026-10-10.md`, which concern the
S8 clear decision in `guard_condition_still_immediate()`, case `SAFETY_TRIP_RATE`
(`firmware/SaftyFW/src/safety_guards.c`).

Files read: `safety_guards.c` (reset/clear, the clear decision, the tripped-branch
post window, the S8 trip path), `safety_guards.h` (S8 state),
`tasks/safety_core.c` (task start reset, per-tick cfg reload, the CLEAR_TRIP call
site), `tasks/link_frame.c` (`link_frame_decide_clear_trip()`),
`config_params.c` (param 0x0205 `rate_window_s`), the new tests in
`test/test_safety_guards.c`, and the S1/S8 row of
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`.

## Verdict

The fix is a strict tightening. Every case that the pre-fix code refused is
still refused. The saftyfx7 MED-1 scenario (a partial post window of about 5 s
plus noise) can no longer grant: before the first full post window completes,
the frozen tripping window decides. Nothing in these commits weakens or blinds
a guard. The trip path is untouched.

This review found no HIGH or MED issue. Three LOW findings remain:

- LOW-1: one full window is still enough to grant, and it can be skewed by a
  single outlier sample. A probe test shows this.
- LOW-2: one refusal branch has no test.
- LOW-3: the post-window length follows a `rate_window_s` that has no bounds
  and can change mid-trip.

## Findings

### LOW-1: one outlier at a post-window roll can grant a clear on a fast rise

After the first full post window, the clear is decided by:

- the last full window rate, a two-sample estimate:
  `(tc_at_roll - tc_at_previous_roll) / window`;
- plus the partial window, but only once it has run for at least
  `S8_POST_MIN_S` (5 s).

So for the first 5 s after every roll, one full window decides alone. An error
of `e` °C in either anchor sample shifts that rate by `e` °C/min, because the
window is 60 s.

The trip side needs two consecutive over-limit windows
(`S8_OVER_RATE_STREAK_TO_TRIP`), so it tolerates one bad sample. The clear side
has no matching protection.

Scenario (probe test, see the negtest table):

1. S8 trips with `max_rate_c_per_min = 10`. The kiln keeps rising at 12 °C/min.
2. The sample that closes the first post window reads 2.5 °C high but is still
   valid. That window computes 14.5 °C/min and refuses.
3. The second window starts from that high anchor. At its roll it computes
   about 9.5 °C/min.
4. A CLEAR_TRIP inside the next 5 s is granted while the kiln is still rising
   at 12 °C/min.

The probe asserts that this clear is refused, and the assertion fails at
`test_safety_guards.c:3499` (CAUGHT).

Ordinary MAX31856 noise (about ±0.15 °C) moves the rate by only about
0.3 °C/min, so a realistic grant needs either a real outlier sample or a rise
within about 0.3 °C/min of the limit. With K4 open, a sustained fast rise is
also physically unlikely. S9 covers a welded K4. After a grant, S8 re-trips
once two fresh windows (120 s or more) are over the limit. For these reasons
this is LOW rather than MED.

Suggested fix (any one is enough):

- (a) Grant only after two consecutive full post windows are both at or under
  the limit, mirroring the trip streak. One outlier at a roll pushes the two
  adjacent windows in opposite directions, so one of them refuses.
- (b) During `[0, S8_POST_MIN_S)` after a roll, also check the span from the
  previous full window's start:
  `(tc - prev_start) / (win + elapsed)`.
- (c) Use a short median or mean of the last few samples as each anchor,
  instead of one sample.

### LOW-2: the partial-window refusal inside the full-window branch is untested

The second check in the `s8_post_rate_valid` branch refuses when the partial
window (5 s or more) is over the limit even though the last full window was
flat. No test reaches it.

- The LOW-1 test covers only two cases: a fast full window (refused by the
  first check), and a plateau that ends 2 s after a roll, where the partial
  window is under 5 s.
- Mutating the condition to `if (0 && state->s8_post_elapsed_s >= S8_POST_MIN_S)`
  makes the branch dead, and the change is **MISSED**.

The fix commit's negtests cover only these mutations: the restored partial
shortcut, the disabled last-full check, and EITHER changed to newest-only. None
of them removes this check.

The GUARD_TEST_MATRIX S1/S8 row says "afterwards it refuses if EITHER ... OR the
partial window now accumulating (>= 5 s) is over the rate" and cites
`test_try_clear`. The row describes the code correctly, but half of that claim
has no test.

Suggested fix: add a test that holds a plateau for at least 61 s (so a flat full
window completes), then rises at 30 °C/min for 6 s or more, and asserts that the
clear is refused. Negtest it with the mutation above.

### LOW-3: the post window follows a live, unbounded `rate_window_s`

The post window uses the current
`effective_f(cfg->rate_window_s, RATE_WINDOW_S_DEFAULT)` on every tick.

- `safety_core.c` reloads `s_guard_cfg` on every tick.
- `config_params.c` accepts any u16 for param 0x0205, with no floor or ceiling.
  0 falls back to 60.
- COMMIT_CONFIG is refused only while ARMED (`link_task.c`, around line 2824).

So the window length can be anything, and if a commit can land while the board
is tripped, it can change mid-trip.

Two consequences:

- **A short window (for example 1 to 10 s)** puts the clear back in the noise
  class of saftyfx7 MED-1, because each "full" window is only that long. With a
  window under 5 s, the partial check never runs at all. The trip side is just
  as noisy with such a window, so this is a commissioning hazard, not a
  regression from this change. The clear no longer has a 60 s floor of its own,
  though.
- **A long window set mid-trip** (for example 65535 s) means no full post window
  completes for about 18 h. Meanwhile the frozen 60 s trip window keeps refusing
  on any plateau above `trip_window_start + limit`. The latch is stuck for
  about 18 h. That is fail-safe, but it is a near-permanent latch that this fix
  makes reachable.

Suggested fix:

- Latch the post window length at trip time.
- Clamp it to a fixed band, for example `[60, 600]` s, independent of
  `rate_window_s`.
- Add a commissioning bound for `rate_window_s` in `config_params.c`, the way
  `max_rate_c_per_min` has a floor and a ceiling.

### INFO

- **I-1, wrong SHAs in the fix record.**
  `REVIEW_SAFTYFX7_2026-10-10.md` "Fix status" cites a2218f7ba/a988bb86e. Both
  exist as objects but neither is an ancestor of origin/dev. The landed commits
  are **055d34833** (code, matrix, first tests) and **6e7470c95** (test fix).
  Replace the citation.
- **I-2, a UTF-8 BOM was added.** 6e7470c95 put a UTF-8 BOM (`EF BB BF`) on
  line 1 of `firmware/SaftyFW/test/test_safety_guards.c`. It is the only BOM in
  any tracked SaftyFW `.c` file. It is harmless to MSVC and gcc, and
  `check_source_bytes.ps1` does not flag it. It most likely came from a
  PowerShell write. Strip it, and consider making `check_source_bytes` reject a
  BOM in C sources.
- **I-3, 055d34833's own LOW-1 "fast" case was not fast.** It added
  0.005 °C per 0.1 s tick, which is 3 °C/min, not the commented 30 °C/min. So
  that commit alone tested a plateau twice. 6e7470c95 corrected it to 0.05 per
  tick. The landed state is correct, and no action is needed.
- **I-4, resets: no stale state across a re-trip or a reboot.**
  - Every grant runs `safety_guards_clear()`, which is a `memset` that zeroes
    the frozen window and all `s8_post_*` fields.
  - `safety_core_task()` calls `safety_guards_reset()` at start, and guard
    state is never persisted across a Pico reboot.
  - A refused clear leaves the state untouched, so the post window keeps
    accumulating. That is correct.
  - The post block runs only while `reason == SAFETY_TRIP_RATE`. The reason can
    change mid-trip only to `SAFETY_TRIP_INEFFECTIVE` (S9), which is refused
    unconditionally both in `safety_guards_try_clear()` and in
    `link_frame_decide_clear_trip()`.
  - No other module holds a copy of, or a derived expectation from, the post
    window. The ESP sees only the clear outcome. So the reset-one-side bug
    class does not apply here.
- **I-5, edge cases.**
  - *The 60 s boundary.* The roll happens at `elapsed >= win`. A float sum of
    0.1 s steps may land on tick 600 or 601. At the roll tick `elapsed` resets
    to 0, so the just-completed window decides with no gap.
  - *Division.* The divisor `m` is at least 5/60 in the partial check, and at
    least `win/60` (greater than 0) at the roll.
  - *Invalid-TC ticks.* These pause `elapsed` but not the kiln. A rise during
    the gap inflates the rate, which refuses (conservative). A fall during the
    gap inflates the cooling, which can grant on a real measured drop.
  - *A clock stall.* If `dt_s` stays 0, the post window freezes, and the
    frozen trip window decides indefinitely. That is fail-safe.
  - *`max_rate_c_per_min` set to 0 mid-trip.* The clear is granted. This
    predates these commits and is by design: the guard is uncommissioned.
- **I-6, S9 / `trip_verify_s` (predates these commits, affects every guard).**
  `safety_guards_try_clear()` does not wait for the S9 verify period
  (`trip_verify_s`).
  - Through the frozen-window branch, an S8 clear can be granted within seconds
    of the trip. That happens only if the reading falls back under
    `start + limit`, which in practice means the tripping end sample was itself
    a high outlier.
  - The grant zeroes `s9_verify_elapsed_s`, so that trip never gets its
    K4-open proof.
  - The same applies to S1 and the other level clears. These commits do not
    change it. If wanted, refuse any clear while `relay_deenergized` and
    `s9_verify_elapsed_s < trip_verify_s`.
- **I-7, the GUARD_TEST_MATRIX row matches the code.** It describes the code
  accurately: frozen window until the first full post window completes, then
  EITHER last-full OR partial (5 s or more), and never a grant on a partial
  alone. The only gap is the test coverage noted in LOW-2.

## Testing

- SaftyFW host tests at 652804cbf, worktree `C:\wt\rvs8clr_kcspus`:
  `SAFTYFW HOST TESTS: all passed`.
- Negtests: `tools\negtest.ps1 -Preset saftyfw-host`, base 652804cbf,
  `-ExpectPattern "FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED"`. Baseline passed
  and the real tree was unchanged in both runs.

| Mutation / probe | Result |
|---|---|
| N1: partial-window check in the full-window branch made dead (`if (0 && ...)`) | MISSED (LOW-2) |
| P1: probe test. 12 °C/min rise, one +2.5 °C valid outlier on the sample that closes post window 1, clear just after the window 2 roll, asserts refusal | CAUGHT at `test_safety_guards.c:3499`, so the clear is granted (LOW-1). Its sanity checks (tripped, two rolls) passed. |

## Fix status

- LOW-1: fixed. A grant needs two consecutive full post windows at or under the limit; one full window alone refuses. Probe P1 is now a permanent test.
- LOW-2: fixed (test added, matrix row cites the real tests).
- LOW-3: fixed by latching the post window at trip time, clamped to [60, 600] s. No commissioning bound was added for param 0x0205 (clamp only; the trip-side window still follows the unbounded `rate_window_s`).
- I-1: fixed in REVIEW_SAFTYFX7. I-2: BOM stripped; `check_source_bytes` not extended.
