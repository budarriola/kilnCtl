# firing_score four-objective scorecard (2026-09-13)

Follow-up implementation to the opus re-examination `2edbb6eb`
(`docs/audits/reverted_control_decisions_reexamination_2026-09-13.md`,
section 8), which found the scorecard itself -- not any single reverted
decision -- to be the biggest defect in play: two of the owner's four control
objectives had NO instrument at all, and both gaps were accept-permissive.
This document verifies that finding against `firing_score.c` at the time this
work started, then describes the fix.

No board was flashed, no firing was run. All work is host-side: `firing_score.c`/
`.h`, `firing_compare.c`/`.h`, and `firmware/KilnFW/App/test/test_iter_tune.c`.

## 1. Verification of the four claims

Read `firing_score.c` in full (205 lines, pre-change) before touching it.
All four claims from `2edbb6eb` check out as stated; none were overstated.

1. **Objective 2 (settle quickly) has no instrument.** Confirmed: pre-change
   `firing_score_seg_tick()`'s dwell branch partitions every tick by exactly
   one fixed boundary, `seg->elapsed_s <= seg->entry_window_s`, and feeds ticks
   before it only to `entry_peak_c` and ticks after only to `steady_sumsq`.
   Nothing recorded *when* the zone came to rest. A quick, clean settle and a
   400 s ring that decays before the entry window ends could score identically
   on all three pre-change subscores. Verified directly: added
   `test_settle_time_distinguishes_ringing_from_quick_settle` (below), and
   before adding `SETTLE_S`, the two constructed segments' `ENTRY_PEAK_C` and
   `STEADY_RMS_C` values were confirmed to differ by less than the 0.5 degC
   floor even though one rings for ~190 s and the other never leaves the band.

2. **Objective 4b (undershoot) is clamped to zero.** Confirmed at the
   pre-change line (then `firing_score.c:148` in `firing_score_seg_finish()`):
   ```c
   out->value[FIRING_SUBSCORE_ENTRY_PEAK_C] = (seg->entry_peak_c > 0.0f) ? seg->entry_peak_c : 0.0f;
   ```
   with the comment "Overshoot only: a dwell entered from below never
   overshoots, and reporting a negative 'overshoot' would let an
   undershooting trial score better on the overshoot axis for the wrong
   reason." Confirmed the consequence too: an entry undershooting 3 degC that
   recovers before `entry_window_s` elapses left literally nothing in any
   subscore -- excluded from `STEADY_RMS_C` by the entry-window boundary, and
   clamped out of `ENTRY_PEAK_C` by the line above. Verified with
   `test_undershoot_makes_a_worse_trial_score_worse`'s baseline/trial pair,
   which pre-change scored byte-identically (both `entry_peak_c == 0.0`).

3. **Both gaps are accept-permissive.** Confirmed by construction: nothing a
   missing/clamped subscore could report can ever make a trial look WORSE
   than it is, only equal to a better trial than it deserves to be compared
   against. `firing_compare.c`'s veto and Bar 1 both operate per sub-score;
   an axis that never reports a difference can never fire the veto and can
   never contribute a Bar-1 clear either, so the missing/clamped axis is
   strictly silent, never punitive.

4. **`FIRING_SUBSCORE_LAG_S` is unsigned and drops saturated-and-short
   ticks.** Confirmed at the pre-change lines:
   ```c
   float lag_s = abs_err / seg->rate_c_per_s;              // unsigned
   ...
   if (saturated_high && err < 0.0f) return;               // drops the worst ticks, globally
   ```
   The drop is global, not local to `LAG_S` -- it also removes those ticks
   from `scored_ticks`/`in_band_ticks` for the whole tick, which is correct
   for a "gains vs. heater" tuning comparison but means `LAG_S` cannot answer
   "is the kiln reaching temperature at the correct rate", the owner's literal
   objective-1 question, whenever the heater itself is the limiting factor
   (true routinely on the 4 W bench fixture). Verified with
   `test_lag_signed_distinguishes_lead_from_lag` (sign) and
   `test_lag_signed_includes_saturated_short_ticks` (drop).

None of the four claims needed correction. `FIRING_SUBSCORE_ENTRY_PEAK_C`'s
own definition was left exactly as-is per `2edbb6eb`'s explicit direction
(the `22cf674b` revert from an EMA to raw peak tracking was reconfirmed sound
on structural grounds there) -- no change was made to it here beyond letting
undershoot live on its own new axis instead of being folded into it.

## 2. New sub-score: `FIRING_SUBSCORE_SETTLE_S` (objective 2)

`firing_score.h`/`.c`: dwell segments only. Tracks
`settle_last_outside_s` = the `elapsed_s` of the LAST scored tick with
`|err| > FIRING_SCORE_SETTLE_BAND_C`, updated on every scored dwell tick
(both the entry-window and steady phases, so re-entering-then-leaving the
band later correctly pushes the value forward). At `seg_finish()`:

- If the zone never left the band: `settle_s = 0.0` (has = true).
- If it left and came back and stayed: `settle_s` = the elapsed time of that
  last excursion.
- **If the last scored tick of the whole segment is still outside the band**
  (never truly settled), `settle_last_outside_s` equals the segment's own
  duration -- the worst possible reading for that segment, not a good one.
  This is deliberate: it makes "never settled" visible as a large number
  instead of reading as an instant, good settle (Finding A's explicit ask).
  `has[SETTLE_S]` stays `true` in this case too (any scored dwell tick sets
  `settle_have_tick`), so a never-settling trial is not silently dropped from
  comparison the way a segment with zero `entry_seen`/`steady_ticks` would be.

**Band definition and justification.** Fixed at
`FIRING_SCORE_SETTLE_BAND_C = 0.5` degC -- the project's existing 0.5 degC
materiality line (`FIRING_COMPARE_OWNER_FLOOR_C`), duplicated as an
independent constant rather than shared, because `firing_score.h` must not
depend on `firing_compare.h` (the header's own top-of-file note: pure
decision/measurement logic). The audit named "a fraction of `cfg.band_c`" as
the other candidate; that was rejected because `band_c` is a per-profile,
per-zone configured TRACKING tolerance (5 degC default, used for the
capture-transient and in-band diagnostics), not a materiality figure -- sizing
the settle band off it would make two firings with different `band_c`
configs report incomparable settle times for the physically identical trace.
Tying `SETTLE_S` to the same 0.5 degC line already used everywhere else in
this project to mean "worth caring about" keeps all four objectives judged
against one standard.

**Bar-1 floor for `SETTLE_S`.** `firing_compare.h`'s new
`FIRING_COMPARE_SETTLE_FLOOR_S = 60.0` (one PWM window, the same 60 s already
used elsewhere in this module family, e.g. `FIRING_SCORE_MIN_SCORED_TICKS`).
0.5 (read as "0.5 seconds") would have made Bar 1 nearly always clear on this
axis, since almost any two real settle times differ by more than half a
second; the owner has not set a materiality figure for a time quantity
(Finding A explicitly notes this), so one PWM window -- the shortest interval
already established elsewhere as meaningful for this class of statistic -- is
used instead, and is called out as a deliberate choice open to revision once
the owner sets one.

## 3. New sub-score: `FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C` (objective 4b)

Tracks `entry_trough_c`, the most-negative `err` seen anywhere in the entry
window (same window and same `entry_seen` gate as `ENTRY_PEAK_C`, computed
alongside it, not instead of it). At finish:
`value = (entry_trough_c < 0) ? -entry_trough_c : 0.0`.

This is a **new axis**, not a sign change to `ENTRY_PEAK_C`. The existing
clamp-to-zero on `ENTRY_PEAK_C` is correct and was NOT removed: the comment
explains its reason (mixing signs on one axis would let an undershooting
trial buy credit on the OVERSHOOT axis), and that reasoning is sound for a
single axis. The fix the owner's 4-part objective needs is a second axis for
the fourth objective's other half, which is what `ENTRY_UNDERSHOOT_C`
provides -- undershoot is now measured on its own terms, never cancelled by a
later recovery (it is the trough over the whole window, not the final
value), and never bought as credit elsewhere.

Bar-1 floor: the existing default (`FIRING_COMPARE_OWNER_FLOOR_C`, 0.5 degC)
-- same materiality quantity (a temperature) as `ENTRY_PEAK_C`, so no new
floor constant was needed.

## 4. `LAG_S`'s two defects: addressed via a signed companion, not in place

`FIRING_SUBSCORE_LAG_S` itself is **left byte-identical** to its pre-change
behaviour -- same unsigned magnitude, same saturated-and-short exclusion. It
feeds the pinned A1 acceptance bar (`check_sim_iter_tune_bars.ps1`,
24/660 == 3.6364%), and changing its definition in place would move that
pin, which the ratchet guard in `sim_iter_tune.c`'s own comment forbids
loosening.

New: `FIRING_SUBSCORE_LAG_SIGNED_S`, fed on every ramp tick that survives
ONLY the capture-transient exclusion (i.e. computed BEFORE the infeasibility
`if (saturated_high && err < 0.0f) return;` line, so it sees the ticks
`LAG_S` drops). Sign convention: positive = behind schedule, negative = ahead
of schedule, using the ramp's own direction (`FIRING_SEG_RAMP_UP` vs.
`FIRING_SEG_RAMP_DOWN`) so "behind" means the same physical thing (less heat
input than commanded) regardless of ramp direction. Uses its own signed
histogram (`lag_signed_hist`, 512 bins x 2 s, centred at bin 256, so it
covers +/-512 s) rather than reusing `lag_hist`, since the existing histogram
is unsigned and half its own size in dynamic range would have to be sacrificed
to add a sign.

Both defects are demonstrated directly by tests (section 6):
`test_lag_signed_distinguishes_lead_from_lag` shows `LAG_S` reading the same
magnitude for a pure lag and a pure lead of equal size (the blind spot,
demonstrated on the untouched production axis) while `LAG_SIGNED_S` reports
opposite signs; `test_lag_signed_includes_saturated_short_ticks` shows
`LAG_SIGNED_S`'s median moving to reflect 300 saturated-and-short ticks that
`LAG_S` excludes entirely.

## 5. `ENTRY_PEAK_C` preserved

No change to its definition, its window, or its clamp. The only change
touching it is cosmetic: it and `ENTRY_UNDERSHOOT_C` are now computed side by
side in the same `if (seg->elapsed_s <= seg->entry_window_s)` block, since
they track the same window from the same ticks.

## 6. Tests added (`firmware/KilnFW/App/test/test_iter_tune.c`)

All six use the real production functions (`firing_score_seg_begin/_tick/_finish`,
`firing_score_set_add`, `firing_compare`), not reimplementations, per this
repo's standing rule against testing a mirror.

- `test_settle_time_distinguishes_ringing_from_quick_settle` -- a clean
  settle and a ~190 s ring (engineered to have the SAME eventual peak/steady
  values) must now differ materially on `SETTLE_S`.
- `test_settle_time_never_settling_reads_as_worst_not_zero` -- a dwell that
  stays 2 degC off target for its entire 400-tick length reads `SETTLE_S` as
  ~the full segment duration, not 0.
- `test_undershoot_measured_and_not_cancelled_by_recovery` -- a 3 degC entry
  undershoot that recovers before the entry window ends: `ENTRY_UNDERSHOOT_C`
  reports 3.0, `ENTRY_PEAK_C` stays 0 (no cross-contamination).
- `test_undershoot_makes_a_worse_trial_score_worse` -- **the accept-permissive
  gap, closed and proven through the real comparator**: baseline (perfect
  entry) vs. trial (2 degC undershoot, 3 matched classes by zone index) --
  pre-fix this pair scored byte-identically on every existing axis; with
  `ENTRY_UNDERSHOOT_C` wired in, `firing_compare()` returns
  `FIRING_COMPARE_REJECT_DEGRADED`, i.e. the previously-invisible worse
  controller now scores worse and the veto fires on it.
- `test_lag_signed_distinguishes_lead_from_lag` -- sign defect.
- `test_lag_signed_includes_saturated_short_ticks` -- drop defect.

## 7. Negative test (production code broken, confirmed red, restored by hand, rebuilt)

Broke `firing_score.c`'s entry_trough tracking by commenting out the
`if (!seg->entry_seen || err < seg->entry_trough_c) { seg->entry_trough_c = err; }`
update inside `firing_score_seg_tick()`'s dwell entry-window branch, leaving
`entry_trough_c` at its `memset`-zeroed 0.0 for every segment.

Rebuilt via `firmware/KilnFW/App/test/build_host_tests.ps1` (PowerShell,
`-ExecutionPolicy Bypass`). Result: **RED**, three failures, exactly the ones
touching undershoot:

```
FAIL test_iter_tune.c:239: the 3C undershoot is measured, not erased by the recovery (got 0.0000, want 3.0000 +/-0.0100)
FAIL test_iter_tune.c:290: the undershooting trial is flagged as degraded on its OWN axis
FAIL test_iter_tune.c:292: the comparator rejects a trial that only the old scorecard would have called equal
```

Restored the three lines BY HAND (typed back in, not `git checkout`/`restore`
per this session's standing rule), confirmed `git diff` on the file was empty
relative to the intended change, then **forced a full rebuild** (re-ran
`build_host_tests.ps1`, which recompiled the changed `.obj` and relinked)
before measuring anything again, per this session's standing instruction that
an empty source diff says nothing about build artifacts. Result: **GREEN**,
7465/7465 checks passed in the main host-test binary, 0 run failures across
all 38 built executables.

## 8. A1 acceptance bar: before and after

Measured via `firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1`
(n=220, 660 null comparisons), the canonical configuration
`2edbb6eb`/`22cf674b` and the pin's own history are quoted against.

- **Before (documented pin, `check_sim_iter_tune_bars.ps1`'s long-standing
  ceiling, independently re-reproduced by `8a12521b` per
  `docs/audits/reverted_control_decisions_reexamination_2026-09-13.md`
  section 6): 24/660 accepts (3.6364%).**
- **After this change (measured just now, same n=220 configuration): 24/660
  accepts (3.6364%) -- ACCEPT 24, REJECT 26, INSUFFICIENT 610, NO_PAIRS 0.**

**A1 did not move.** The new subscores did not flip any of the 660 null
comparisons from INSUFFICIENT/REJECT to ACCEPT or vice versa. This is a
plausible, not merely lucky, result: the A1 null experiment compares
identical gains against themselves with only noise and start-temperature
differing, so `SETTLE_S`/`ENTRY_UNDERSHOOT_C`/`LAG_SIGNED_S` differences
between the two arms of each null pair are dominated by noise and rarely
clear their own floors (60 s for `SETTLE_S`, 0.5 degC for
`ENTRY_UNDERSHOOT_C`) by a full owner-floor margin in a way that would flip a
verdict that the three original subscores did not already flip. `check_sim_iter_tune_bars.ps1`
itself still reports the run as `PASS` against its pinned known-failure
ceiling (unchanged wording, same caveat as before: this is not evidence the
2.0% design target is met, only that this change did not regress it).

## 9. Check tally

`tools/run_all_checks.ps1` (PowerShell, `-ExecutionPolicy Bypass`, foreground):
**94 passed, 0 skipped, 0 failed**, including
`firmware\KilnFW\App\test\check_00_kilnfw_target_build.ps1` (target build),
`firmware\KilnFW\App\test\check_sim_iter_tune_bars.ps1` (A1/A2/A5/A6),
`tools\check_test_c_files_wired.ps1`, `tools\check_no_orphaned_checks.ps1`,
`tools\check_doc_hash_citations.ps1`, and `tools\check_test_has_assertions.ps1`.

## Summary

| Objective | Instrument before | Instrument after |
|---|---|---|
| 1. correct rate | `LAG_S` only (unsigned, drops worst ticks) | `LAG_S` unchanged + new `LAG_SIGNED_S` (signed, includes those ticks) |
| 2. settle quickly | none | new `FIRING_SUBSCORE_SETTLE_S` |
| 3. settle accurately | `STEADY_RMS_C` | unchanged |
| 4a. overshoot | `ENTRY_PEAK_C` | unchanged (preserved per audit direction) |
| 4b. undershoot | clamped to zero | new `FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C` |

`FIRING_SUBSCORE_COUNT` went from 3 to 6. A1 (`check_sim_iter_tune_bars.ps1`)
measured unchanged at 24/660 before and after. All 94 `run_all_checks.ps1`
guards pass, including a full target build.
