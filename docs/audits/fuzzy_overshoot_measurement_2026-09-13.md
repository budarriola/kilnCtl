# Fuzzy layer re-measured on the owner's four-part objective, not IAE/MAE (2026-09-13)

## Why this exists

Every prior measurement of the fuzzy layer (`sim_fuzzy_closedloop.c`'s tracking
scenario, every `iter_tune`/`firing_compare` comparator run) judged it on IAE and
MAE — integral/mean absolute error averaged over a whole run. The owner clarified
the fuzzy layer's actual purpose is narrower and more specific: reach temperature
at the correct rate, settle quickly, settle accurately, and minimize over/undershoot.
An aggregate error number can hide a transient concentrated at a setpoint
transition, and can hide one objective improving while another regresses.

Partway through this task, the objective itself was widened by the owner from
"overshoot only" to all four, and then the instrument was corrected again: an
opus pass (`2edbb6eb`, `docs/audits/reverted_control_decisions_reexamination_2026-09-13.md`)
established that `firing_score.c`'s own subscores are sound for objectives 1
(partially), 3, and 4a, but have **no instrument at all** for objective 2, and a
**clamped/windowed, accept-permissive gap** for objective 4b. This file's
measurements reflect that final, corrected instrument set.

## Scope limits — read before quoting any number below

1. **Single-zone only.** Single-column transport is measured LINEAR on hardware
   (`project_z0_coupling_is_shape_not_scale.md`); the multi-zone coupling model
   is refuted (`project_coupling_failure_is_joint_dwell_specific.md`) with two
   replacement attempts failed
   (`coupling_level_schedule_adjudication_2026-09-11.md`). Single-zone sim is
   the trustworthy scope; nothing here claims multi-zone coverage.
2. **Bench-scale magnitudes.** Zone 0's model tops out around ambient +
   k_dc·duty ≈ 24 + 39.2·1.0 ≈ 63 °C — this bench cannot exceed ~40 °C above
   ambient, while a real kiln firing reaches ~1200 °C. Every degC figure below
   ranks controller behaviour on THIS plant model only, never a kiln-scale
   overshoot claim.
3. **No synthetic disturbance injection.** Unlike `sim_fuzzy_closedloop.c`'s
   coverage scenario (which pokes `element_c`/`sensor_c` directly, bypassing
   `sensor_delay_s`, and is explicitly non-physical), every number here comes
   from ordinary ramp-to-dwell setpoint transitions — the case that matters for
   a real firing.

## What was built

A new, standalone host-test harness,
`firmware/KilnFW/App/test/sim_fuzzy_overshoot.c`, wired into
`firmware/KilnFW/App/test/build_host_tests.ps1` as its 38th
`Invoke-HostTestExe` call (`$totalExpected` bumped 37→38). It is a **separate
file**, not an edit to `sim_fuzzy_closedloop.c`, because that file was mid-edit
by another session throughout this task (the band-derivation pass,
`docs/audits/fuzzy_dimensionless_bands_2026-09-13.md`) — the two never
collided. It links the same real production code
`sim_fuzzy_closedloop.c` does — `pid_fuzzy_adjust()`/`pid_fuzzy_derive_bands()`
(`pid_fuzzy.c`, unmodified), `pid_update()`/`pid_rescale_integral_for_new_ki()`
(`pid.c`), and the single-zone FOPDT plant (`sim_plant.c`, zone 0's measured
constants) — with its own small orchestration loop reproducing the same
tick-wiring `sim_fuzzy_closedloop.c` documents.

## Scenario

Three ramp/dwell transitions on zone 0, alternating approach direction:

| # | dwell target | approach |
|---|---|---|
| 0 | ambient+15 = 39.0 °C | from below (ramp up) |
| 1 | ambient+30 = 54.0 °C | from below (ramp up) |
| 2 | ambient+12 = 36.0 °C | from above (ramp down) |

Ramp rate 100 °C/hr (0.0278 °C/s), the same "brisk but normal" figure
`sim_fuzzy_closedloop.c`'s own header comment uses — well under the derived
fuzzy rate band (0.1488 °C/s here). Each dwell holds for 400 ticks (2000 s);
zone 0's `entry_window_s = dead_time_s + 2·tau_s = 52.8 + 2·263.8 = 580.4 s`
(116 ticks), so every dwell has 284 ticks (23.7 min) of steady-state dwell
after its own entry window.

Four arms, each measured from the **same freshly rebuilt binary**
(`kilnctl_sim_fuzzy_overshoot.exe`, rebuilt via `build_host_tests.ps1` in this
pass — not reused from any earlier build):

- **fuzzy off** — `strength_pct=0`, base gains (kp=0.0318, ki=0.0001, kd=0.8401)
- **fuzzy_25** — `strength_pct=25`
- **fuzzy_50** — `strength_pct=50`
- **fixed retune** — `strength_pct=0` (fuzzy inference forced off, so
  `pid_fuzzy_adjust()` is a pure no-op) with gains multiplied by kp×0.75,
  ki×1.25, kd×0.75 — the equivalent-direction fixed retune the task asked
  for, so "fuzzy inference helps" and "these gains help" are answered
  separately.

## Definitions used, and how each relates to `firing_score.c`

`firing_score.c`'s three subscores
(`FIRING_SUBSCORE_LAG_S`/`ENTRY_PEAK_C`/`STEADY_RMS_C`) were read in full,
including the `22cf674b` revert from an EMA back to raw peak tracking for
`ENTRY_PEAK_C` — confirmed **correct** by the same `2edbb6eb` opus pass that
found the other two gaps, so this file keeps that definition unmodified. That
same pass found:

- **Objective 3 (settle accurately)** — `FIRING_SUBSCORE_STEADY_RMS_C` is
  **sound**. This file reuses it exactly: `sqrt(mean(err²))` over every tick
  after `entry_window_s` within the dwell segment.
- **Objective 4a (overshoot)** — `FIRING_SUBSCORE_ENTRY_PEAK_C` is **sound**.
  This file reuses it exactly: `max(actual − target, 0)` over
  `entry_window_s`, raw peak (not EMA).
- **Objective 1 (correct rate)** — `FIRING_SUBSCORE_LAG_S` is **present but
  flawed**: it is unsigned (a leading and a lagging tick score identically),
  and in `firing_score.c` itself it drops saturated-and-short ticks — exactly
  the ticks where objective 1 is failing hardest. This file reports the
  unsigned median exactly as `LAG_S` defines it (median of
  `abs(error_c)/commanded_rate`, over the ramp into each dwell), **with that
  caveat printed alongside every use**, and additionally reports a **signed**
  median of its own (`error_c/(ramp_dir·commanded_rate)`; positive = lagging
  behind the ramp, negative = leading ahead of it) so the direction is never
  lost. This file's own ramp loop applies no saturation filter and no
  minimum-tick threshold, so `LAG_S`'s second blind spot (dropping the worst
  ticks) does not carry over to either number reported here, even though it
  does affect `firing_score.c`'s own live subscore.
- **Objective 2 (settle quickly)** — **no instrument exists in
  `firing_score.c` at all** (a 60 s settle and a 400 s ring score identically
  given the same peak and steady RMS). `settle_ticks`/`settle_time_s` are this
  file's own addition: ticks from dwell entry to first-and-**lasting** arrival
  within a stated 2.0 °C band (a transient dip back out does not count).
- **Objective 4b (undershoot)** — `firing_score.c`'s own equivalent is
  `entry_peak_c`'s negative case, **deliberately clamped to 0.0** and
  **windowed** to `entry_window_s` — so an undershoot that recovers before
  `dead_time+2·tau` elapses leaves no trace at all, an accept-permissive gap.
  This file's `undershoot_signed_c` is its own, corrected definition: the
  **most negative** `(actual − target)` reached **anywhere in the whole dwell
  segment** (not windowed to entry), reported **signed**, **never suppressed**
  by a later recovery. (An earlier revision of this file made the same
  windowed/mirrored mistake `firing_score.c` does; it was corrected before
  this report was written.)

Materiality: the 0.5 °C rule is applied **per objective, to the quantity in
question** — degC for objectives 3/4a/4b, and a stated 30 s bar (half of six
ticks, a round conservative figure — this repo has no prior lag/settle-at-entry
measurement to derive it from) for the two time-valued objectives (1 and 2).
Fuzzy arms and the retune arm are compared **separately** against `fuzzy_off`,
never pooled, since they answer different questions.

## Results (bench-scale, single-zone, zone 0)

Per-dwell, per-arm (raw harness output, `kilnctl_sim_fuzzy_overshoot.exe`):

### Objective 1 — ramp lag into each dwell (unsigned median, seconds)

| dwell | off | f25 (Δ) | f50 (Δ) | retune (Δ) |
|---|---|---|---|---|
| 0 (39.0°C) | 183.5 | 186.1 (+2.6) | 188.7 (+5.3) | 193.5 (+10.0) |
| 1 (54.0°C) | 183.5 | 186.1 (+2.6) | 188.8 (+5.3) | 193.6 (+10.0) |
| 2 (36.0°C) | 198.8 | 199.7 (+0.9) | 200.6 (+1.8) | 208.9 (+10.2) |

Max |Δ|: fuzzy 5.3 s, retune 10.2 s — **neither exceeds the 30 s bar**. The
signed median equals the unsigned one on every arm/dwell here (no leading
observed anywhere in this scenario — the ramp is always lagged, never led,
on this plant). Caveat stands: this number, unsigned or signed, is the median
over an unfiltered ramp (no saturation exclusion), not `firing_score.c`'s own
live figure.

### Objective 2 — settle time from dwell entry (seconds, this file's own instrument)

| dwell | off | f25 (Δ) | f50 (Δ) | retune (Δ) |
|---|---|---|---|---|
| 0 | 240 | 220 (−20) | 200 (−40) | 180 (−60) |
| 1 | 240 | 220 (−20) | 200 (−40) | 180 (−60) |
| 2 | 250 | 220 (−30) | 195 (−55) | 170 (−80) |

Max |Δ|: fuzzy 55 s, retune 80 s — **both EXCEED the 30 s bar.** Both fuzzy
and the retune settle materially faster than the control on every dwell.

### Objective 3 — steady-state RMS error (°C)

| dwell | off | f25 (Δ) | f50 (Δ) | retune (Δ) |
|---|---|---|---|---|
| 0 | 0.14 | 0.04 (−0.10) | 0.13 (−0.01) | 0.26 (+0.12) |
| 1 | 0.14 | 0.04 (−0.10) | 0.13 (−0.01) | 0.26 (+0.12) |
| 2 | 0.15 | 0.04 (−0.10) | 0.09 (−0.05) | 0.28 (+0.13) |

Max |Δ|: fuzzy 0.10 °C, retune 0.13 °C — **neither exceeds 0.5 °C.**

### Objective 4a — overshoot, entry peak (°C, `FIRING_SUBSCORE_ENTRY_PEAK_C`)

| dwell | off | f25 (Δ) | f50 (Δ) | retune (Δ) |
|---|---|---|---|---|
| 0 | 0.00 | 0.00 (+0.00) | 0.15 (+0.15) | 0.60 (**+0.60**) |
| 1 | 0.00 | 0.00 (+0.00) | 0.15 (+0.15) | 0.60 (**+0.60**) |
| 2 | 6.47 | 6.27 (−0.21) | 6.06 (−0.42) | 6.13 (−0.34) |

Max |Δ|: **fuzzy 0.42 °C — does not exceed the 0.5 °C bar. Retune 0.60 °C —
EXCEEDS it**, and in the wrong direction at dwells 0/1 (introduces overshoot
where the control had none).

### Objective 4b — undershoot, signed whole-dwell peak (°C, this file's own instrument)

| dwell | off | f25 (Δ) | f50 (Δ) | retune (Δ) |
|---|---|---|---|---|
| 0 | −6.31 | −6.24 (+0.07) | −6.17 (+0.14) | −6.25 (+0.06) |
| 1 | −6.31 | −6.24 (+0.07) | −6.17 (+0.14) | −6.25 (+0.06) |
| 2 | 0.00 | −0.04 (−0.04) | −0.18 (−0.18) | −0.67 (**−0.67**) |

Max |Δ|: **fuzzy 0.18 °C — does not exceed the 0.5 °C bar. Retune 0.67 °C —
EXCEEDS it.** Note the large, near-constant ~6.2–6.3 °C undershoot at dwells
0/1 across every arm: this is the ramp's own dead-time/lag catch-up (the
measurement is still below target when the commanded ramp ends and the dwell
begins), essentially unaffected by strength_pct — it is not a controller
defect being scored here, and is reported for completeness since the
instruction was "not suppressed by recovery," but it should not be read as a
fuzzy-sensitive quantity.

## The trade, stated plainly

**Fuzzy inference itself (f25/f50) does not cross the 0.5 °C / 30 s materiality
line on three of five metrics (objective 1, objective 3, and objective 4b),
and does not cross it on objective 4a either (max 0.42 °C).** It DOES cross
the line on objective 2: fuzzy_50 settles 40–55 s faster than the control on
every dwell, a genuine, material improvement in the fuzzy layer's stated
purpose (albeit landing partly on settle speed rather than overshoot per se).
That faster settle comes with a small, sub-materiality increase in overshoot
at the ramp-up dwells (+0.15 °C) and in undershoot at the ramp-down dwell
(−0.18 °C) — a real trade, but one that stays under the line on both sides.

**The fixed-gain retune arm is a different, worse trade.** It settles even
faster (60–80 s) than fuzzy_50, but crosses the materiality line on BOTH
overshoot (+0.60 °C, at dwells where the control had zero) and undershoot
(−0.67 °C, at the dwell where the control had zero) — a genuine regression on
the fuzzy layer's stated purpose, not an improvement, bought for extra settle
speed. This is exactly the distinction the task asked this arm to expose:
**"fuzzy inference helps" and "these particular gains help" are different
claims, and on objective 4 specifically they point in opposite directions**
on this plant — fuzzy_50 stays under the bar, the equivalent fixed retune
does not, for a materially larger settle-time gain. The retune's `8a12521b`-
class hazard (a change compared on the wrong instrument looking safe) does not
apply here since this is a fresh, deliberate measurement, but the historical
parallel — a "just as good, and simpler" fixed-gain claim reading as strictly
worse once measured on the right axis — is exactly this arm's result.

**On this bench-scale single-zone plant, fuzzy inference (strength 25/50)
never crosses the 0.5 °C / 30 s materiality line except on objective 2
(settle time), where it is a genuine, favourable, sub-materiality-cost
improvement.** This is a materially different conclusion from every prior
IAE/MAE-based measurement in one respect (objective 2 was invisible to an
aggregate error number) and consistent with them in another (objectives 1,
3, 4a, 4b all stay under the line for fuzzy specifically, same as tracking
error always did).

## Negative test

`pid_fuzzy.c`'s `strength_pct == 0` short-circuit
(`if (strength_pct == 0) { *out_kp = kp; ... }`) was broken by hand
(`*out_kp = kp * 0.999f;`), confirmed to fail this file's own bit-exact
assertion with a non-zero exit
(`build_host_tests.ps1 -OutDir $env:TEMP\kilnctl_negtest` →
`=== sim_fuzzy_overshoot: FAIL ===`), then restored by hand line-for-line
(confirmed via `grep -n "0.999f|NEGATIVE-TEST" pid_fuzzy.c` returning no
match, i.e. an exact revert with zero residue in that concurrently-edited,
shared file), followed by a FULL rebuild
(`build_host_tests.ps1 -OutDir $env:TEMP\kilnctl_negtest2`) that re-confirmed
`=== sim_fuzzy_overshoot: PASS ===` from the freshly rebuilt binary — never
measured from a stale artifact (`project_green_build_in_dirty_tree_proves_
nothing_about_head.md`'s exact hazard, `8a12521b`).

`pid_fuzzy.c` was dirty from another session's concurrent work throughout
this task; the break/restore touched only the one targeted line via an exact
string replace, leaving that session's own in-progress changes elsewhere in
the file untouched (confirmed: the post-restore diff against the pre-negative-
test state showed no residual change in the touched hunk).

## Assertions (falsifiable, not print-only)

`sim_fuzzy_overshoot.c` fails its own exit code (not just prints a verdict) if:

1. `strength_pct=0` does not reproduce base gains bit-for-bit on any tick of
   any arm (the safety contract, verified negative-tested above).
2. Any dwell's entry window (`entry_seen`) is never reached — the measurement
   would be vacuous for that dwell.
3. Any dwell never reaches its post-entry steady portion (`steady_seen`) —
   a scenario-sizing bug, not a controller finding.
4. Any dwell never settles within `SETTLE_BAND_C` inside `DWELL_TICKS`
   (`settle_ticks < 0`) — refuses to report a sentinel as if it were a real
   settle time.

All four held on every arm/dwell in this run (`38/38` executables built,
`sim_fuzzy_overshoot: PASS`).

## Build/check tally

- `build_host_tests.ps1`: `sim_fuzzy_overshoot.c` wired as the 38th
  `Invoke-HostTestExe` call (`$totalExpected` 37→38). Multiple full rebuilds
  during this pass reported `Built: 38/38 executables` with
  `sim_fuzzy_overshoot: PASS` every time. Two OTHER, PRE-EXISTING build/run
  issues were observed during concurrent sessions' active edits this same
  day (`test_pid_fuzzy.c`'s "error band == k_dc * 0.5 exactly" assertion, and
  `test_adaptive_tune_model.c`'s `s_fake_zone_cfg` undeclared-identifier
  compile errors) — both are in files this task does not own
  (`pid_fuzzy.c`/`adaptive_tune_ki.c`'s test suites) and were mid-edit by
  other sessions; not investigated or fixed here per this task's own
  file-ownership boundary.
- `tools/run_all_checks.ps1`: **92 passed, 2 failed** in the run captured for
  this report. Both failures are pre-existing and unrelated to this task's
  files: `check_all_task_stack_budgets.ps1` (an `objdump` subprocess error
  against `firmware/KilnFW/build/KilnCtrl.elf`, a stale/missing build
  artifact, not a source change) and
  `check_fuzzy_gain_mirror_drift.ps1` (`test_closed_loop.c`'s `fuzzy_tick()`
  mirror is out of date against `profile_executor_pid_tick.c`'s new
  `zone_model_at()`/`pid_fuzzy_derive_bands()` call sequence — the other
  session's band-derivation change landing mid-task, owned by that session,
  not this one). `check_test_c_files_wired.ps1` and
  `check_no_orphaned_checks.ps1` both PASSED, confirming
  `sim_fuzzy_overshoot.c` is correctly wired into the build and not an
  orphan.
- `check_doc_hash_citations.ps1`: PASSED in the same run; every commit hash
  cited in this document (`2edbb6eb`, `22cf674b`, `8a12521b`, `fbdc5bd0`) was
  confirmed to resolve via `git cat-file -t <hash>` before this document was
  written.

## Files touched

- `firmware/KilnFW/App/test/sim_fuzzy_overshoot.c` — new, standalone harness.
- `firmware/KilnFW/App/test/build_host_tests.ps1` — added the build/run
  wiring for the new harness as call #38, bumped `$totalExpected` 37→38.
- `firmware/KilnFW/App/drivers/control/pid_fuzzy.c` — touched only
  transiently for the negative test above; restored byte-for-byte by hand,
  confirmed via `grep` finding no residue, and no diff is present in the
  final state of this file from this task.
- This document.

`firmware/KilnFW/App/test/sim_fuzzy_closedloop.c` was **not** touched by this
task (it was another session's WIP throughout); its own IAE/MAE tracking
scenario remains a separate, still-valid ranking tool for a different
purpose, unaffected by anything in this report.
