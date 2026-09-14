# Harness settle-band reconciliation, 2026-09-14

Scope: `firmware/KilnFW/App/test/sim_fuzzy_overshoot.c` only. Owner of this
pass: the sim_fuzzy_overshoot.c doc-comment/table fix described below.
`firing_score.{c,h}` and `firing_compare.{c,h}` were treated read-only for
this pass and are unchanged (verify with `git diff` against them -- empty).

## The problem, restated

Two agents moved in opposite directions on the same night:

- `1c54b237` pointed `sim_fuzzy_overshoot.c`'s own `SETTLE_BAND_C` at 0.5 degC,
  "to match production" -- because at the time production's own
  `FIRING_SCORE_SETTLE_BAND_C` (`firmware/KilnFW/App/drivers/control/firing_score.h`)
  really was 0.5 degC.
- `560cffe0`, landing immediately after, re-sized **production's own** band
  0.5 -> 2.0 degC, having measured (docs/audits/firing_score_subscore_enrolment_2026-09-14.md,
  and `firing_score.h`'s own "RE-SIZED FROM THE PLANT" comment) that +/-0.5 degC
  is held on only 0.02-0.31 of first-dwell ticks across 33 real dwell-zone
  instances in `logs/coupling/noise_floor_p7*`, while +/-2.0 degC is held on
  0.79-1.00 of all of them.

Net effect: `1c54b237`'s "match production" was correct when written and
false five minutes later, because it copied a *value*, not a *reference* to
production's value.

## What the code actually looked like at task start

Checked before touching anything, per this task's own instruction to verify
the mismatch exists rather than assume it:

```c
#include "../drivers/control/firing_score.h" /* FIRING_SCORE_SETTLE_BAND_C only */
...
#define SETTLE_BAND_C FIRING_SCORE_SETTLE_BAND_C
```

This is **not** a private literal -- it is a `#define` alias directly onto
production's constant. It was already structurally correct by the time this
task started; only the human-readable comment above it (and two label
strings later in the file) still said "0.5" and narrated the now-stale
rationale for that value, which would have misled the next reader into
thinking the number was frozen. The mismatch this task was asked to verify
was real, but it was a **stale comment describing an already-fixed value**,
not a live numeric divergence: the settle-time figures this file printed
before this pass were already being computed at production's current 2.0
degC band, automatically, via the `#include`.

## What changed here (item 1: eliminate the private copy)

The `#define` alias already eliminates the private copy of the *value*.
Linking the *algorithm* (calling `firing_score_seg_finish()` or similar
directly) was not attempted: this file's own top comment (`SCRATCH FILE, NOT
sim_fuzzy_closedloop.c`) documents a deliberate choice to duplicate a small
amount of orchestration rather than depend on `firing_score.c`'s per-segment
state machine, specifically so this harness has zero coupling to
`sim_fuzzy_closedloop.c`'s own concurrent edit. `firing_score_seg_*()`
expects to be driven from `profile_executor`'s own tick loop and segment
lifecycle, not from a standalone ramp-then-dwell simulation loop -- wiring it
in here would mean either reproducing that lifecycle by hand (defeating the
purpose) or taking a live dependency on `profile_executor.c`, which is a much
larger, less stable surface than a single `#define`. Given the constant
itself was already a direct alias with no way to drift, re-deriving the
whole algorithm was judged not worth the added coupling.

Changes made:

1. Rewrote the stale comment above `#define SETTLE_BAND_C FIRING_SCORE_SETTLE_BAND_C`
   to state plainly that it is a direct alias, not a literal, and to carry
   the corrected history (0.5 -> 2.0 degC on production's side, this file
   always follows).
2. Printed the live `SETTLE_BAND_C` value at the top of the program's own
   output (`Settle-time band (SETTLE_BAND_C): %.1fC`) so a reader never has
   to trust a comment to know what band a given run measured against.
3. Updated two label strings that still said "0.5C band" (the objective-2
   metric name and the top-of-file `[2] settle time` comment) to point at
   the live constant instead of a hardcoded string.

No assertion was added to detect future divergence because there is no
value left that *can* diverge -- `SETTLE_BAND_C` is textually
`FIRING_SCORE_SETTLE_BAND_C`, not a copy of it.

## Negative test (item 4)

1. Edited `firmware/KilnFW/App/drivers/control/firing_score.h` (read-only for
   this task's normal scope, edited here only as the negative test's
   required break of production code): changed
   `#define FIRING_SCORE_SETTLE_BAND_C 2.0f` to `5.0f`.
2. Deleted `firmware/KilnFW/App/test/build_host_tests` and ran
   `build_host_tests.ps1` fresh (foreground, `-ExecutionPolicy Bypass`).
   Result: `sim_fuzzy_overshoot.c`'s own printed band followed the change
   immediately (`Settle-time band (SETTLE_BAND_C): 5.0C`), proving live
   pickup with no local literal to go stale. Separately, and confirming the
   constant is genuinely load-bearing elsewhere too,
   `test_iter_tune.c` (owned by the `firing_score`/`firing_compare` work,
   not this pass) went from green to 6 failures at the 5.0 degC band --
   `RUN FAILURES (1): main`, `6 FAILURE(S)` at lines pinned to production's
   settle behavior (e.g. "ringing segment's settle time reflects the ~190s
   it spends outside band").
3. Restored `firing_score.h` BY HAND to `2.0f` (confirmed via
   `git diff firmware/KilnFW/App/drivers/control/firing_score.h` returning
   empty).
4. Deleted `firmware/KilnFW/App/test/build_host_tests` again and ran a full
   rebuild from the restored source. `sim_fuzzy_overshoot.c` printed
   `Settle-time band (SETTLE_BAND_C): 2.0C` again and reported
   `=== sim_fuzzy_overshoot: PASS ===`.

Note on the post-restore rebuild: it reported 4 unrelated failures in
`test_profile_executor_prestart.c` (per-zone `effective_target_c` vs.
`s_exec.target_c`, and a fuzzy-strength Ki-movement sanity check). These are
**not** caused by this pass -- `git status` shows
`profile_executor_pid_tick.c` and `test_profile_executor_prestart.c` both
modified in-place by another session's concurrent WIP (outside this task's
owned scope), and `firing_score.h`/`sim_fuzzy_overshoot.c` are the only files
this task's own diff touches. `sim_fuzzy_overshoot.c` itself passed in both
the pre- and post-negative-test runs.

## Re-measurement (item 2): does the 30.00s / 25.0s finding survive?

docs/audits/derived_bands_four_objective_score_2026-09-13.md reported the
derived-vs-absolute settle-time difference as exactly 30.00 s at 100 degC/hr
and 25.0 s at 300 degC/hr -- both measured at the (then believed current)
0.5 degC band. Re-running the same two arms (absolute_50 vs derived_50)
through the unchanged harness, now scoring at production's current 2.0 degC
band:

| Ramp rate | max\|diff\| DERIVED vs ABSOLUTE settle time |
|---|---|
| 100 degC/hr (primary sweep) | **10.00 s** (was reported as 30.00 s at 0.5 degC) |
| 300 degC/hr (rate-sensitivity pass) | **10.0 s** (was reported as 25.0 s at 0.5 degC) |

Both figures dropped by roughly a factor of 2.5-3x once measured at the band
production actually scores against. The **specific numbers** 30.00 s and
25.0 s do **not** survive -- they were an artifact of a 0.5 degC band
production has since abandoned, exactly as this task suspected.

The **qualitative conclusion** does survive, and arguably survives more
comfortably now: both re-measured figures (10.00 s / 10.0 s) sit well under
this file's own `TIME_MATERIALITY_S = 30.0f` bar, and are no longer near it
the way the old 30.00 s figure was (at, not under, the bar). The owner's
decision to keep the derived bands is unaffected either way -- this pass
does not revisit that decision, only the number that was cited in support
of "the derived-vs-absolute settle difference is immaterial." That
statement is still true, now on firmer numeric footing.

Also notable: the old finding described the settle gap as *rate-dependent*
(30.00 s at 100 degC/hr vs. 25.0 s at 300 degC/hr, i.e. some sensitivity to
ramp rate). At the 2.0 degC band, both rates now report the identical
10.00 s / 10.0 s -- if anything, the rate-dependence read into the old
numbers looks like more of that same 0.5 degC-band artifact rather than a
real effect, though this pass makes no strong claim about that beyond
noting it.

## Undershoot definition (item 3)

`560cffe0`'s `FIRING_COMPARE_VOTING_MASK` keeps `ENTRY_UNDERSHOOT_C` (the
windowed, `>=0`-clamped definition, same shape as overshoot) as a voting
axis; `sim_fuzzy_overshoot.c` already computed both definitions but reported
its own unwindowed, signed, whole-dwell `undershoot_signed_c` as the primary
"[4b]" row and the production-matching `undershoot_entry_windowed_c` as a
secondary "disagreement only" row -- backwards relative to what production
now actually scores with.

Swapped in this pass: `[4b]` is now the entry-window, production-matching
definition (`undershoot_entry_windowed_c`, labelled "PRIMARY" and cited
against `FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C` and `560cffe0` directly in the
metric name), and the unwindowed whole-dwell figure is now `[4b secondary]`,
kept only so a controller that trades entry-window undershoot for a slower
whole-dwell recovery is still visible. The disagreement-caveat text
immediately above the metrics table was rewritten to describe the rows in
their new order.

Re-run numbers for the primary ([4b]) row at 100 degC/hr: max|diff|
strength-50 vs off = 0.24C, strength-25 vs off = 0.12C, derived-vs-absolute =
0.13C -- all under the 0.5C materiality bar, same qualitative result as
before the swap (the two definitions track closely on this scenario; they
were never far apart numerically, only mislabelled as to which one
production actually votes with).

## Preserved caveats

Unchanged, still stated in the file's own comments and this pass's output:
arm separation between bands/strengths appears only at ramp rates >= 100
degC/hr (25-50 degC/hr converges all arms); effective n is 2 conditions
(absolute vs. derived), not 3 independent dwells, since the single-zone
plant is deterministic; dwell 2's ~6.47C "overshoot" is ramp-down residual
and stays excluded from every overshoot aggregation (`exclude_dwell = 2` in
the metrics table, unchanged).

## Check tally

`firmware/KilnFW/App/test/build_host_tests.ps1`: 38/38 executables built,
`sim_fuzzy_overshoot` reports PASS in every run in this pass (pre-change
baseline, post-negative-test-break, and post-restore). The post-restore run
carried 4 pre-existing failures in `test_profile_executor_prestart.c` caused
by another session's uncommitted, unowned WIP (`profile_executor_pid_tick.c`
/ `test_profile_executor_prestart.c`, confirmed via `git status`), not by
anything in this pass's diff.

`tools/run_all_checks.ps1`: **94 passed, 0 skipped, 0 failed** -- matches the
94/94 baseline this task started from. The earlier `test_profile_executor_
prestart.c` failures seen during the host-test rebuild above did not recur
here; they were transient, tied to another session's in-flight WIP on
`profile_executor_pid_tick.c`, not to anything in this pass's diff.
