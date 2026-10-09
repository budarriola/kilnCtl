# Derived (autotune) fuzzy bands vs absolute defaults, on the owner's four objectives, not IAE/MAE (2026-09-13)

## 0. What this answers

`2c49465a` ("Derive fuzzy membership bands from each zone's own autotune
model") replaced `pid_fuzzy.c`'s absolute `ERROR_BAND_C_DEFAULT`/
`RATE_BAND_C_PER_S_DEFAULT` (20.0 degC / 0.5 degC-per-s, "sized by desk
reasoning about a mid-size kiln," never measured) with
`pid_fuzzy_derive_bands()`, computing both half-widths from a zone's own
Autotune FOPDT model: `rate_band_c_per_s = model_k_dc/model_tau_s`,
`error_band_c = model_k_dc*0.5`. The absolute constants remain in place as
the explicitly-logged fallback for a never-autotuned zone.

`docs/audits/fuzzy_dimensionless_bands_2026-09-13.md` measured the derived
bands only on IAE/MAE (via `sim_fuzzy_closedloop.c`'s tracking scenario) and
found them worse: MAE 3.0930 (derived, strength 50) vs 2.8356 (absolute,
strength 50), against a control of 3.3052. That comparison is the wrong
instrument for this decision: the owner's actual objective for the fuzzy
layer is four-part -- (1) reach temperature at the correct rate, (2) settle
quickly, (3) settle accurately, (4) minimize over/undershoot -- and an
aggregate error number lets all four trade against each other invisibly.
This document re-scores derived-vs-absolute bands on those four objectives
separately, using `sim_fuzzy_overshoot.c` (`7ef487ff`), the harness already
built for exactly this question, plus production's own subscores where they
now exist (`FIRING_SUBSCORE_SETTLE_S`/`ENTRY_UNDERSHOOT_C`/`LAG_SIGNED_S`,
`d41da85f`).

## 1. Instrument review honoured, and what changed in the harness

An opus review of `sim_fuzzy_overshoot.c`, appended to
`docs/audits/fuzzy_overshoot_measurement_2026-09-13.md` (`1570a65a`), found
two defects and confirmed two things sound. Both defects are fixed in this
pass, in the harness file this task owns (`sim_fuzzy_overshoot.c`); nothing
in `pid_fuzzy.c`, `profile_executor_pid_tick.c`, or `firing_score.c` was
touched (all three are read-only/owned-elsewhere for this task):

- **Settle-band defect (review Finding 2), FIXED.** The harness's
  `SETTLE_BAND_C` was `2.0` on a plant whose materiality line is `0.5`. The
  review swept the band and found the settle-time ordering **inverts** at
  0.5 degC (an arm that looked faster at 2.0 degC settles *slower* once
  measured at the owner's own materiality scale) and loses all
  discriminating power at a gentler ramp rate. **Fixed by switching to
  `FIRING_SCORE_SETTLE_BAND_C` (`firing_score.h`, 0.5 degC) directly** --
  this resolves the "2 C vs 0.5 C" question the task asked me to settle:
  **use 0.5 C, production's own band, not the harness's private 2.0 C
  one.** The settle-time *algorithm* itself was also rewritten to match
  `FIRING_SUBSCORE_SETTLE_S` exactly (elapsed time of the LAST tick seen
  outside the band, across the whole dwell, never reset by an earlier
  re-entry) rather than the harness's previous "first tick and lasting"
  framing -- the two are numerically equivalent for a monotonic approach but
  the production algorithm is now used verbatim rather than reproduced by a
  different (if equivalent) method.
- **Overshoot artifact at dwell 2 (review Finding 3), FIXED.** Dwell 2's
  "overshoot = 6.47 degC" is a ramp-*down* residual (the measurement is
  still above target when a ramp-down dwell begins), not real overshoot --
  the mirror image of the ramp-lag-catch-up artifact already disclosed for
  undershoot at dwells 0/1. The harness's previous revision's headline "0.42
  degC max fuzzy overshoot delta" came entirely from this artifact; the real
  delta at the two dwells where overshoot is actually overshoot was 0.15
  degC. **Fixed by excluding dwell 2 from objective 4a's materiality
  aggregation**, with the raw per-dwell number still printed and explicitly
  flagged `[EXCLUDED: ramp-down residual, not real overshoot]`.
- **Confirmed sound, kept as-is:** the undershoot instrument
  (`undershoot_signed_c` -- unwindowed, unclamped, never suppressed by
  recovery) and the negative test (a real regression in a real translation
  unit fails the build, not print-only).
- **Confounded comparison arm (review Finding 1), REMOVED, not fixed.** The
  harness's previous revision carried a "fixed_retune_equivalent" arm meant
  to separate "fuzzy inference helps" from "these particular gains help".
  The review proved that arm used fuzzy's rule-table **centre-cell
  multiplier** (kp x0.75/ki x1.25/kd x0.75 at error=0, rate=0) -- fuzzy_50's
  per-tick *maximum*, not its ramp-phase *average* (measured at kp
  x0.8684/ki x1.1618/kd x0.8382) -- so it applied roughly twice fuzzy_50's
  real strength during the ramp, and its "fuzzy loses to a flat retune"
  finding was an artifact of that mismatch, not a property of fuzzy
  inference. **This task's brief does not ask for a retune arm, and the
  question it exists to answer (derived bands vs absolute bands, both
  through real `pid_fuzzy_adjust()` inference) needs no retune arm to
  settle** -- both compared arms run identical strength and identical
  inference, differing only in which bands they resolve to, so the
  centre-cell-vs-average confound this review found does not apply here.
  The arm was removed rather than re-scaled, to avoid re-inheriting a
  comparison this task does not need.

## 2. Setup and sanity gate

Per-zone derived values, as supplied: z0 error_band_c=21.37,
rate_band_c_per_s=0.1672; z1 16.20/0.1251; z2 16.92/0.1370 -- against a
measured peak rate of 0.110 degC/s. **This harness is single-zone; zone 0's
own model was used throughout** (error_band_c=19.62, rate_band_c_per_s=0.1488
-- z0's Autotune fit as embedded in `sim_measured_zone_constants.h`, close
to but not identical to the 21.37/0.1672 figure quoted in the brief, which
was the live board's own separately-supplied reading; the harness uses its
own already-measured zone-0 constants, unchanged by this task, per the
concurrency boundary on `sim_plant.c`/`sim_measured_zone_constants.h`).

**Control sanity gate.** `sim_fuzzy_closedloop.c` (unmodified, owned by
another session, run read-only) at `strength_pct=0` reproduces the required
baseline exactly from a freshly rebuilt binary:

```
tracking scenario: IAE=13749.7 degC*s, MAE=3.3052 degC
```

confirming this build of `pid_fuzzy.c`/`pid.c`/`sim_plant.c` is not stale.
(`sim_fuzzy_overshoot.c`'s own scenario is a different, purpose-built
three-dwell measurement, not a repeat of that tracking scenario -- its IAE
figures below, ~11,000-12,000 degC*s over a much shorter run, are not
expected to match 13749.7 and are not compared to it.)

**Build/negative-test discipline.** `firmware/KilnFW/App/test/build` was
deleted and `build_host_tests.ps1` re-run from scratch three times during
this pass (before any edit, after the band-selection/settle-band/dwell-2
changes, and after the negative test's restore), per this project's own
"never measure from a stale binary" rule (`8a12521b`). All three rebuilds
report `Built: 38/38 executables`. The negative test (see \S6) also used a
full clean rebuild, not a re-run of a cached binary.

## 3. Arms and scenario

Three ramp/dwell transitions on zone 0 (same scenario `sim_fuzzy_overshoot.c`
ships with, at 100 degC/hr): dwell 0 = ambient+15 (ramp up), dwell 1 =
ambient+30 (ramp up), dwell 2 = ambient+12 (ramp down, undershoot risk).
Five arms, each from the **same freshly rebuilt binary**:

| arm | strength_pct | error_band_c | rate_band_c_per_s |
|---|---|---|---|
| fuzzy_off (control) | 0 | n/a (no-op) | n/a |
| absolute_50 | 50 | 20.00 | 0.5000 |
| derived_50 | 50 | 19.62 | 0.1488 |
| absolute_25 | 25 | 20.00 | 0.5000 |
| derived_25 | 25 | 19.62 | 0.1488 |

A second, compact pass reruns only `absolute_50`/`derived_50` at 300 degC/hr
(\S5) to check rate-dependence, per the review's Finding 5 (arm separation
was found to appear only at 100 degC/hr and above; a gentler ramp shows no
separation between any bands and would wrongly read as "no effect").

## 4. Results at 100 degC/hr (bench-scale, single-zone, zone 0)

All figures below are `max|diff|` across the three dwells (dwell 2 excluded
from objective 4a per \S1). "derived vs absolute (same strength)" is the
column that answers this task's actual question; "vs off" columns are
reported so a strength effect is not confused with a bands effect.

| objective | metric | bar | strength-50 vs off | strength-25 vs off | **derived vs absolute** |
|---|---|---|---|---|---|
| 1 (rate) | ramp-lag, unsigned median | 30 s | 5.29 s | 2.63 s | **0.85 s** |
| 1 (rate) | ramp-lag, signed median | 30 s | 5.29 s | 2.63 s | **0.85 s** |
| 2 (settle) | settle time, 0.5 C band | 30 s | 215.00 s (EXCEEDS) | 135.00 s (EXCEEDS) | **30.00 s** |
| 3 (accuracy) | steady-state RMS | 0.5 C | 0.05 C | 0.11 C | **0.04 C** |
| 4a (overshoot) | entry peak, dwells 0/1 only | 0.5 C | 0.20 C | 0.00 C | **0.05 C** |
| 4b (undershoot) | signed whole-dwell peak | 0.5 C | 0.26 C | 0.12 C | **0.10 C** |

None of the "derived vs absolute" figures exceeds its bar. The strength-50
settle-time gap vs off (215 s) does exceed the time bar -- fuzzy inference
itself materially speeds settling at 0.5 C, a real finding independent of
which band set it uses -- but derived and absolute bands differ from each
other by only 30.00 s on that same axis, landing exactly at (not over) the
stated 30 s line.

**Materiality stated per objective, to the quantity in question, not
pooled:** every "derived vs absolute" figure above is either a fraction of
its bar (rate 0.85/30 s = 2.8%; accuracy 0.04/0.5 C = 8%; overshoot
0.05/0.5 C = 10%; undershoot 0.10/0.5 C = 20%) or exactly at it (settle,
30.00/30 s = 100% of a bar this file itself states is "a round, stated,
conservative bar; not derived from any prior finding" -- see \S6 for why
this one line is treated as a caution rather than a clean pass).

**Objective 4a's disclosed artifact, quoted separately.** Dwell 2's raw
overshoot numbers (off=6.47, abs50=6.02, der50=6.06, d=-0.45/-0.42) are
ramp-down residual, not overshoot, and are excluded from the table above per
\S1. If included anyway, derived-vs-absolute would read 0.04 C at dwell 2 --
still sub-material, but the number is not a real overshoot measurement and
is not used for any conclusion in this document.

**Objective 4b, the disagreement this task asked to be surfaced.** Production's
`FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C` (`d41da85f`) is windowed to
`entry_window_s` and clamped to >=0, the same shape as overshoot --
`sim_fuzzy_overshoot.c` now reports both this file's own unwindowed
whole-dwell signed undershoot AND that production-matched entry-windowed
figure side by side. They disagree exactly where expected: dwells 0/1's
large (~6.1-6.3 C) undershoot is ramp-lag catch-up still present when the
dwell begins, so it shows up in BOTH definitions (the entry window is wide
enough, `entry_window_s`=580 s > most of the transient); dwell 2's small
(-0.04 to -0.26 C) undershoot from actual controller behaviour also shows up
in both, since it occurs early in the dwell. On this scenario the two
definitions do not diverge materially (max 0.13 C difference between them
across all arms), but they are structurally different instruments (windowed
vs. not) and a scenario with a later-recovering dip inside a longer dwell
could make them diverge more -- this document reports both rather than
picking one, per the task's instruction. The derived-vs-absolute verdict
above uses the unwindowed whole-dwell figure (0.10 C), the more permissive
(harder for a controller to hide a real undershoot from) of the two; using
the entry-windowed figure instead would give 0.13 C -- still sub-material.

## 5. Rate sensitivity: 300 degC/hr

Per the review's Finding 5, arm separation was found to appear only at
100 degC/hr and above; this section reruns `absolute_50`/`derived_50` at
300 degC/hr to check whether the derived-vs-absolute gap grows at a brisker
ramp:

| objective | metric | bar | derived vs absolute @300 degC/hr |
|---|---|---|---|
| 4a (overshoot) | entry peak, dwells 0/1 only | 0.5 C | 0.00 C |
| 4b (undershoot) | signed whole-dwell peak | 0.5 C | 0.05 C |
| 3 (accuracy) | steady-state RMS | 0.5 C | 0.00 C |
| 2 (settle) | settle time, 0.5 C band | 30 s | 25.0 s |

Every figure at 300 degC/hr is the same order of magnitude as, or smaller
than, its 100 degC/hr counterpart, and none exceeds its bar. **The
derived-vs-absolute conclusion is not rate-dependent over this range**: it
holds at both the primary 100 degC/hr ramp and the brisker 300 degC/hr ramp
tested. (Dwell 2's overshoot at 300 degC/hr, 13.04/13.04 C for both arms, is
the same ramp-down-residual artifact as \S4 and is excluded here too.)

## 6. Judgement -- do the derived bands cost anything material?

**On four of five metrics: no, cleanly.** Ramp-lag (0.85 s of a 30 s bar),
steady-state accuracy (0.04 C of 0.5 C), overshoot (0.05 C of 0.5 C, dwell-2
artifact excluded), and undershoot (0.10-0.13 C of 0.5 C, both instrument
definitions) all land well inside materiality, at both 100 and 300 degC/hr.

**On settle time: a genuine, narrow tension, reported rather than resolved.**
Derived and absolute bands differ from each other by exactly 30.00 s at
100 degC/hr on the settle-time axis -- landing precisely at, not under, the
bar this file states for itself. Two things temper how much this should
weigh:

1. The 30 s bar is this harness's own admitted, undedicated choice ("a
   round, stated, conservative bar; not derived from any prior finding" --
   this file's own comment, unchanged from before this pass), not an
   owner-set or production-derived threshold the way the 0.5 C figure is.
   Landing exactly on a self-chosen bar is a weaker signal than landing over
   or under an owner-set one.
2. Fuzzy inference *itself* (either band set, strength 50 vs off) moves
   settle time by 215 s -- seven times the derived-vs-absolute gap. The
   derived/absolute choice is a small perturbation on top of a much larger,
   already-accepted effect from turning fuzzy inference on at all.
3. At 300 degC/hr the same comparison narrows to 25.0 s, under the bar --
   the 100 degC/hr reading is the more marginal of the two tested rates, not
   a worsening trend.

**This is reported as a tension, not resolved here, per this task's own
instruction.** The owner's requirement that no shipped parameter be guessed
for, or trained on, a kiln other than the one it runs on is satisfied by the
derived bands on provenance grounds regardless of performance (unchanged
from `docs/audits/fuzzy_dimensionless_bands_2026-09-13.md`'s own conclusion).
The narrow question this task asked -- do the derived bands cost anything
material on the owner's four objectives -- answers **no on four of five
axes, and an exactly-at-the-line, self-chosen-bar result on the fifth
(settle time), which itself narrows to comfortably under the bar at a
brisker ramp rate and is small next to fuzzy inference's own much larger
settle-time effect.** Whether that fifth axis's exactly-at-bar reading is
enough to prefer the absolute bands' slightly faster settle over the
provenance argument for derived bands is the owner's call, not this
document's; the facts needed to make that call are stated above rather than
resolved into a single recommendation.

## 7. Negative test

Broke `sim_fuzzy_overshoot.c`'s own `strength_pct==0` bit-exact check (the
file this task owns; `pid_fuzzy.c` is read-only for this task and was left
untouched): changed `sim_tick()`'s comparison from
`(adj_kp == base_kp) && ...` to `(adj_kp == base_kp * 0.999f) && ...`.
Rebuilt (via `build_host_tests.ps1 -OutDir $env:TEMP\kilnctl_negtest`, a
separate output directory, not a reuse of the main `build/`) and confirmed:

```
FAIL: strength_pct=0 did NOT reproduce base gains bit-for-bit (the safety
=== sim_fuzzy_overshoot: FAIL ===
```

non-zero exit, `RUN FAILURES` reported by the wrapper script. Restored by
hand (reverted the one-line comparison back to the original), confirmed via
`grep -n "0.999f" sim_fuzzy_overshoot.c` finding no match (clean restore,
zero residue). **Then deleted `firmware/KilnFW/App/test/build` and
`$env:TEMP\kilnctl_negtest` and rebuilt fully from scratch a second time**
(not merely re-checked an empty `git diff`, per this project's own
`8a12521b` lesson on stale poisoned binaries), confirming
`Built: 38/38 executables` and `sim_fuzzy_overshoot: PASS`. Diffed that
final rebuild's harness output byte-for-byte against the output captured
immediately after the arm/instrument changes (before the negative test) --
identical, confirming the negative test and restore left no side effect on
the actual measurement.

## 8. Checks

- Host tests: `firmware/KilnFW/App/test/build_host_tests.ps1` -- three full
  clean rebuilds during this pass, all reporting `Built: 38/38 executables`,
  `sim_fuzzy_closedloop: PASS`, `sim_fuzzy_overshoot: PASS`. One pre-existing,
  unrelated failure was observed on every rebuild:
  `test_zones_http.c:4229` ("the whole-page POST (no autotune_baseline_k_dc
  key at all) must be accepted") -- this file is not touched by this task
  and the failure tracks the same `autotune_baseline_k_dc` field drift named
  below, owned by another session's concurrent work on `zones_http_get.c`
  (explicitly off-limits to this task per its own concurrency instructions).
  Not investigated or fixed here.
- `tools/run_all_checks.ps1`: **91 passed, 3 failed**. All three failures are
  pre-existing and in files this task does not own or touch:
  `tools/check_coil_power_w_sentinel_guard.ps1`,
  `tools/PcTools/check_zones_per_zone_field_drift.ps1` (the same
  `autotune_baseline_k_dc` field this task's own build already surfaced via
  `test_zones_http.c`), and `tools/PcTools/selfcheck.py`. None relate to
  `pid_fuzzy.c`, `sim_fuzzy_overshoot.c`, `firing_score.c`, or this document.
  `check_doc_hash_citations.ps1` and `check_test_c_files_wired.ps1` both
  PASSED.
- Every commit hash cited above (`2c49465a`, `d41da85f`, `1570a65a`,
  `8a12521b`, `7ef487ff`) was confirmed to resolve via
  `git cat-file -t <hash>` before this document was written.
- No board was flashed; no heating run was performed; read-only board
  access only, per this task's own instruction.

## Files touched

- `firmware/KilnFW/App/test/sim_fuzzy_overshoot.c` -- band-selection made a
  parameter (absolute vs derived, no longer always internally derived);
  `SETTLE_BAND_C` switched to `FIRING_SCORE_SETTLE_BAND_C` (0.5 C) and its
  algorithm rewritten to match `FIRING_SUBSCORE_SETTLE_S` exactly; added
  `undershoot_entry_windowed_c` (matches `FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C`)
  reported alongside the existing unwindowed figure; objective 4a's
  materiality aggregation now excludes dwell 2's ramp-down-residual artifact,
  flagged explicitly in the output; ramp rate made a parameter and a second,
  300 degC/hr pass added for the two headline arms; the previous
  "fixed_retune_equivalent" arm removed; arms reorganised to
  fuzzy_off/absolute_50/derived_50/absolute_25/derived_25.
- This document.
- No production file (`pid_fuzzy.c`, `pid_fuzzy.h`,
  `profile_executor_pid_tick.c`, `firing_score.c`, `firing_score.h`,
  `sim_fuzzy_closedloop.c`) was modified.
