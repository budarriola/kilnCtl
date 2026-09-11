# firing_score.c dwell-entry EMA: review, revert, and the A1 pin it left behind (2026-09-10)

## Summary

`8b96b591` added a 60 s EMA smoothing `firing_score.c`'s dwell-entry peak
before taking the max, to fix `sim_iter_tune.exe`'s A1 null-experiment
false-accept rate (0.00% -> 3.64% after `d63a5591` changed `sim_plant.c`'s
coupling model class). An opus review of that commit was verified claim by
claim and found substantially correct. The EMA has been **reverted**
(`firing_score.c`/`.h` restored to byte-identical parity with the version
before `8b96b591` -- confirmed with `git diff 8b96b591^:...firing_score.c
firing_score.c`, empty). The resulting honest A1 rate, 24/660 (3.64%), is
now **pinned as a known-failure ceiling** in `sim_iter_tune.c` and enforced
by `check_sim_iter_tune_bars.ps1`, rather than left to silently fail forever
(the pre-existing state) or silently smoothed to look clear (`8b96b591`'s
approach).

## Why the EMA was reverted

Four claims against `8b96b591`, each independently verified:

1. **Unlinked constant.** `FIRING_SCORE_ENTRY_SMOOTH_TAU_S` (60 s) was a
   bare literal with no compile-time or runtime link to the actual per-zone
   `heater_window_ms` (per-zone, runtime-settable via
   `zones_config_set_heater_cfg` and the HTTP surface). `firing_score.h`
   deliberately has no dependency on `heater_output.h` to stay pure
   decision/measurement logic (`#include <stdbool.h>/<stddef.h>/<stdint.h>`
   only) -- confirmed by grep. A zone on a different window would silently
   decouple the filter from the ripple it exists to remove.

2. **Signal attenuation, worse on unfitted zones.** `firing_score_seg_begin`
   falls back to `entry_window_s = 60.0f` (exactly one EMA time constant)
   when `dead_time_s + 2*tau_s == 0`, i.e. on a zone with no fitted model.
   Measured with a standalone synthetic probe (a peaked error trajectory
   fed through the real `firing_score_seg_tick()`, not committed to the
   tree) comparing a baseline peak of 2.00 degC against a trial peak of
   2.55 degC (a genuine 0.55 degC degradation, just over
   `FIRING_COMPARE_OWNER_FLOOR_C` = 0.5 degC):
   - Fallback window (dead_time=0, tau=0, i.e. unfitted zone): smoothed
     delta measured **0.22 degC** (retention ~40%).
   - Fitted window (dead_time=56 s, tau=130 s -> window=316 s): smoothed
     delta measured **0.47 degC** (retention ~85%).
   - Unsmoothed (post-revert): delta measures the full **0.55 degC**
     (100% retention) in both cases (the fallback-window number is
     window-truncation-limited in the synthetic probe, not smoothing --
     irrelevant post-revert since there is no smoothing left to blunt).

3. **Veto weakened in the accept-permissive direction.** Both numbers in
   (2) are *under* the 0.5 degC floor `firing_compare.c:135` uses for its
   one-sided degradation veto (`median_normalised >= 1.0`, i.e. exactly one
   owner floor). A genuine 0.55 degC degradation on either a fitted or
   unfitted zone would, with the EMA in place, measure below that floor and
   **fail to trip the veto** -- the exact failure mode the review flagged.
   Post-revert, the same degradation measures the full 0.55 degC and
   correctly trips it.

4. **One-tick bypass.** `firing_score.c`'s tick handler (pre-revert) seeded
   `entry_err_ema_c` with the raw, unsmoothed sample on the segment's first
   entry-window tick, and could set `entry_peak_c` directly from that raw
   value in the same branch -- exactly the single-tick PWM-edge case the
   EMA was meant to filter out.

## Why it was not corroborated by real hardware

The only real-hardware measurement of this statistic (`49bb1123`'s
capture-pair verification, `entry_peak_c` n=6, median delta **-0.05 degC**
against the 0.5 degC floor) sits roughly 10x inside the floor -- it does not
show the problem the EMA was introduced to fix existing outside the
simulator's additive coupling model. Weighed against (2)/(3) above, a
production accept/reject veto was measurably weakened to satisfy a bar in a
simulator whose coupling-model class had just changed, on the day it
changed.

## Before/after numbers (n=220, i.e. mc_runs=220 -> 660 A1 comparisons)

| Metric | With EMA (8b96b591, byte-identical re-check) | Reverted (current) |
|---|---|---|
| A1 false-accept | 13/660 = 1.97% PASS (design target 2.0%) | 24/660 = 3.64% FAIL (design target 2.0%) |
| A2 never-worse | PASS | PASS (unaffected) |
| A5 termination | PASS | PASS (unaffected) |
| A6 cage violations | PASS (0) | PASS (0, unaffected) |
| Veto sensitivity (synthetic 0.55 degC true degradation) | 0.22-0.47 degC measured (fails to trip 0.5 degC veto) | 0.55 degC measured (correctly trips veto) |

24/660 matches `8b96b591`'s own commit message exactly (it quotes this as
the pre-fix number it was fixing).

## Enforcement: from silent to loud

Before this pass, `sim_iter_tune.c`'s `main()` printed PASS/FAIL per bar but
always `return 0`, and `build_host_tests.ps1` links it as a "data-generating
harness, not run automatically" -- no `check_*.ps1` or CI path ever ran it.
That is exactly how the A1 rate moved from 0.00% to 3.64% and sat
undetected until a manual re-run. Fixed: `main()` now returns 1 if any of
A1/A2/A5/A6 fails, and `check_sim_iter_tune_bars.ps1` builds and runs it at
n=220, failing loud (naming the failed bar) on a non-zero exit.
Negative-tested by temporarily forcing `main()` back to an unconditional
`return 0` in the production file: the check went GREEN despite A1 sitting
at 3.64% and the harness's own printed "OVERALL: FAIL" -- proving the old
code's vacuity was real, and that the new check depends on the process exit
code rather than being fooled by stale printed text. Restored by hand;
confirmed with an empty `git diff` on the restored file.

## The A1 pin: known-failure, not a cleared bar

A1's honest rate (3.64%) fails against the 2.0% design target, and the
target is **not** widened to accommodate it (per standing instruction).
But `d63a5591`'s coupling-model change is the actual upstream cause -- the
fix is a re-identified coupling matrix (capture in progress as of this
writing), not anything in `firing_score.c`/`firing_compare.c`/this harness.
A standing red check on `main` is actively harmful in a shared tree (it has
already caused at least one agent to mis-attribute a real
`-Werror=format-truncation` regression to "someone else's known-red build"
today) -- so A1 is enforced against a **pinned known-failure ceiling**
instead of the design target:

```c
// sim_iter_tune.c
const double A1_DESIGN_TARGET_PCT = 2.0;   // the real bar -- NOT enforced below
const int A1_PINNED_MAX_ACCEPTS = 24;      // pinned ceiling: measured 2026-09-10
const int A1_PINNED_TOTAL = 660;           // at mc_runs=220, immediately after this revert
```

Both defences the pin needs are in place:

1. **Auditable provenance, not self-reference.** The constants carry a
   comment (in `sim_iter_tune.c`, reproduced in full above the table in
   this doc) giving what produced 24/660, on what date, at what `n`, and
   naming `d63a5591` as the upstream cause -- so raising this pin later
   requires a new measurement and a new comment, not just editing a number.
2. **Ratchet guard.** The comparison is `accepts * A1_PINNED_TOTAL <=
   A1_PINNED_MAX_ACCEPTS * total` (cross-multiplied, exact regardless of
   `mc_runs`) -- it fails the moment the measured rate gets *worse* than
   24/660, and raising the pin is only ever a deliberate, commented, by-hand
   edit to the two literals above. It is never something a run auto-adjusts
   or a check auto-widens.

A green `check_sim_iter_tune_bars.ps1` run states explicitly, in its own
output, that it is running against this pinned known-failure baseline, that
the real target is 2.0%, and that the blocker is the coupling
re-identification -- so a reader of the green line cannot conclude the
2.0% bar is met.

## Exit condition

Once the re-identified coupling matrix lands: re-measure A1 at n=220
(`sim_iter_tune.exe 220`, 660 comparisons) and either tighten
`A1_PINNED_MAX_ACCEPTS`/`A1_PINNED_TOTAL` toward `A1_DESIGN_TARGET_PCT`
(2.0%) or remove the pin entirely in favour of enforcing the design target
directly. This is also stated in `sim_iter_tune.c`'s own comment and in
`check_sim_iter_tune_bars.ps1`'s header, so whoever lands that matrix has
the pointer in both places they are likely to be looking.

## What this does NOT do

- Does not touch `firing_score.c`'s or `firing_compare.c`'s logic beyond
  the EMA revert -- both are otherwise unchanged.
- Does not widen, soften, or delete the 2.0% design target -- it stays in
  the code (`A1_DESIGN_TARGET_PCT`) as the documented, unmet goal.
- Does not change A2/A5/A6, which were unaffected by the EMA either way.
