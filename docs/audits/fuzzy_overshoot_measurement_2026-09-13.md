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

---

# Review, 2026-09-13 — HEADLINE REFUTED: the "equivalent fixed retune" is not equivalent, and objective 2 is a band artifact

Adversarial review of `7ef487ff` (`firmware/KilnFW/App/test/sim_fuzzy_overshoot.c`
and this document). Everything below marked **[executed]** was produced by
running code on this machine during the review; everything marked **[read]**
is from reading source. The review touched no production file — in
particular `pid_fuzzy.c`, which is owned by another session and was already
dirty (+52 lines) from that session's band-derivation work, was **not
edited**; the negative test below was reproduced by compiling a *poisoned
copy* of that same production source into a scratch build instead.

## What reproduced cleanly

**[executed]** The harness was rebuilt from source by the reviewer in an
independent scratch build (own `cl` invocation, own object directory, no
reuse of `App/test/build`) and run. **Every number in every table above
reproduced exactly** — lag 183.46/183.53/198.79, settle 240/240/250,
overshoot 0.000/0.000/6.475, undershoot −6.313/−6.314/0.000 for the control,
and likewise for all three other arms. The tables in this document are a
faithful transcription of the harness output. Exit code 0, `PASS`.

**[executed]** The negative test is real, not print-only. A copy of
`pid_fuzzy.c` with the `strength_pct == 0` short-circuit broken
(`*out_kp = kp * 0.999f;`) was linked into the harness in place of the
production translation unit. Result: `FAIL: strength_pct=0 did NOT reproduce
base gains bit-for-bit` on both strength-0 arms, `=== sim_fuzzy_overshoot:
FAIL ===`, **exit code 1**. Assertion 1 genuinely gates the build. Caveat on
what this proves: it poisons a *copy* of the production source rather than
the production file, so it is not quite the same act the commit message
describes — but unlike a test-local mirror
(`project_negative_test_on_a_mirror_is_vacuous.md`), the harness code path
under test is untouched and it is the real `pid_fuzzy_adjust()` body being
broken, so it does establish that the assertion fires on a real regression in
that function.

**[executed]** Cross-harness consistency with `ba230bca` holds. This
harness's own IAE ranking (fuzzy_50 11388.2 vs retune 11771.3, retune 3.4%
worse) is directionally the same as `ba230bca`'s 7.8% IAE gap. The two
harnesses do **not** disagree on the control.

## Finding 1 (decisive) — the "equivalent fixed retune" is fuzzy's centre-cell MAXIMUM, not its time-average

**[read]** `pid_fuzzy.c`'s centre cell (`RULE_TABLE[1][1]`, error ZERO / rate
STEADY) is `{kp −1, ki +1, kd −1}`, and `scale = (strength_pct/100) ·
MAX_NUDGE_FRACTION = 0.50 · 0.5 = 0.25` at strength 50. So kp×0.75 /
ki×1.25 / kd×0.75 is **exactly and only** what fuzzy_50 applies when error
and rate are both at zero. It is the largest perturbation the layer can
produce at that strength, not a typical one.

**[executed]** Instrumenting fuzzy_50's actual per-tick multipliers over this
document's own scenario:

| | kp | ki | kd | n ticks |
|---|---|---|---|---|
| fuzzy_50, whole-scenario mean | ×0.7891 | ×1.2169 | ×0.7831 | 1546 |
| fuzzy_50, **ramp-phase-only** mean | **×0.8684** | **×1.1618** | **×0.8382** | 346 |
| this document's "equivalent" retune | ×0.7500 | ×1.2500 | ×0.7500 | — |

The ramp phase is the one that matters: dwell-entry overshoot/undershoot is
set by how the loop approaches the target, and during the ramp the error sits
around +6.3 °C (≈0.32 of the 19.62 °C error band) with rate ≈ −0.028 °C/s
(≈0.19 of the 0.1488 °C/s rate band), so the blend is nowhere near the centre
cell. **The retune arm applies roughly twice fuzzy_50's kp/kd perturbation
during the ramp.** It is mislabelled: it is not "the equivalent fixed
retune", it is "a fixed retune about twice as strong as fuzzy_50 where it
counts".

**[executed]** The consequence. Adding a fifth arm — a plain, static,
fuzzy-free gain rescale at fuzzy_50's own measured *ramp-phase average*
(kp×0.8684, ki×1.1618, kd×0.8382, `strength_pct=0` so `pid_fuzzy_adjust()`
is a pure no-op):

| arm | overshoot d0/d1 | undershoot d2 | steady RMS d0 | settle@2 °C d0 |
|---|---|---|---|---|
| fuzzy_off | 0.000 | +0.000 | 0.140 | 240 s |
| **fuzzy_50** | **0.148** | **−0.179** | **0.130** | **200 s** |
| flat retune @ fuzzy_50's ramp average | 0.164 | −0.253 | 0.119 | 195 s |
| flat retune @ fuzzy_50's whole-run average | 0.447 | −0.511 | 0.216 | 185 s |
| this document's retune (centre cell) | 0.603 | −0.667 | 0.264 | 180 s |

**A static gain rescale with no fuzzy inference anywhere in the path
reproduces fuzzy_50 on all four objectives to well inside the 0.5 °C / 30 s
materiality bars** (0.164 vs 0.148 °C overshoot; −0.253 vs −0.179 °C
undershoot; 0.119 vs 0.130 °C steady RMS; 195 vs 200 s settle). The
overshoot/undershoot penalty tracks the *magnitude* of the gain change
monotonically across all five arms — 0.000 → 0.148 → 0.164 → 0.447 → 0.603 —
and shows no discontinuity at the boundary between "inference" and "no
inference".

**Therefore the headline is refuted.** This document concludes that "the
fuzzy inference captures most of the settle-speed benefit without the
overshoot penalty a flat retune incurs." What the data actually show is that
a flat retune of the *same effective strength* incurs the *same* (negligible)
penalty. The +0.60 °C / −0.67 °C regression attributed to "a flat retune" is
an artifact of the comparison arm being roughly twice as strong, exactly as
suspected. On this evidence the fuzzy layer buys nothing on objective 4 that
a correctly-scaled constant multiplier does not also buy.

**[executed]** This is an inherited defect, not a new one: `ba230bca`'s own
commit message describes its retune arm as "a fixed always-on application of
the centre-cell's own multiplier triple". Both harnesses used the same
mislabelled arm. The new harness did not diverge from the old one — it
inherited the old one's confound and then built a headline on it.

## Finding 2 — objective 2's instrument is a band-crossing timer, and the band is 4× the materiality line

`SETTLE_BAND_C` is 2.0 °C on a plant whose materiality line is 0.5 °C. That
band declares a zone "settled" at four times the error the owner calls
material. **[executed]** Sweeping it:

settle time (s), dwell 0, at bands 2.0 / 1.0 / 0.5 / 0.25 °C:

| arm | 2.0 | 1.0 | 0.5 | 0.25 |
|---|---|---|---|---|
| fuzzy_off | 240 | 395 | 560 | 735 |
| fuzzy_25 | 220 | 340 | 445 | 530 |
| fuzzy_50 | 200 | 300 | 375 | **775** |
| retune (this doc's) | 180 | 250 | **765** | **985** |

**The reported ordering survives only at the chosen band.** At 0.5 °C — the
owner's own materiality scale — the retune arm settles **205 s *slower*** than
the control, inverting this document's "settles even faster (60–80 s)" claim
into a large regression. At 0.25 °C fuzzy_50 also inverts, settling 40 s
slower than the control. The reason is visible in this document's own
objective-3 column: the aggressive arms have roughly double the steady-state
RMS, so they cross a loose band early and then take far longer to actually
converge. **Objectives 2 and 3 are in direct conflict here, and a 2 °C band
is exactly wide enough to hide it.** At 100 °C/hr the approach error at dwell
entry is −6.3 °C, so "time to reach ±2 °C" is overwhelmingly a measure of
ramp-lag catch-up rate, not of settling.

**[executed]** A second failure of the same instrument: at a 25 °C/hr ramp,
every arm reports `settle = 0 s` — the measurement never leaves the 2 °C band
at all, so objective 2 has *zero* discriminating power and the harness's
assertion 4 does not catch it (it only rejects `settle_ticks < 0`, and 0
passes). Objective 2's entire result is therefore conditional on the one
ramp rate chosen.

Verdict on this instrument: **not defensible as written.** It should be
re-run at 0.5 °C, or relabelled "time to reach ±2 °C" and removed from the
objective-2 claim.

## Finding 3 — objective 4b's instrument is sound; objective 4a's dwell-2 numbers are mislabelled

**[read/executed]** The unwindowed signed undershoot definition is a genuine
improvement over `firing_score.c`'s clamped/windowed version and I found no
bias in it — it is `min(actual − target)` over the dwell, clamped at 0 only
in the "never dipped below" case, which is correctly documented. It does
capture things that are not really undershoot (the ~−6.2 to −6.3 °C figures
at dwells 0/1 are ramp-lag catch-up, not undershoot), but this document says
so explicitly and does not build a claim on them. **Accepted.**

The same artifact, however, is **not** flagged on objective 4a. Dwell 2's
"overshoot = 6.47 °C" is a ramp-*down* residual — the measurement is still
above target when the dwell begins — the exact mirror of the artifact
disclosed for 4b. This document then quotes dwell 2's deltas (−0.21 / −0.42 /
−0.34) as overshoot improvements, and the **0.42 °C figure the headline uses
as "fuzzy's maximum overshoot delta" comes entirely from that artifact**.
Fuzzy's real overshoot delta, at the two dwells where overshoot is actually
overshoot, is +0.15 °C. This does not change the pass/fail verdict on the bar,
but the number as labelled is wrong.

## Finding 4 — the 30 s bar is self-chosen, and disclosed

**[read]** `sim_fuzzy_overshoot.c`'s own comment states it plainly: "half of
one `DT_S*6` — a round, stated, conservative bar; not derived from any prior
finding". It is the reporting agent's own choice, derived from the
*simulation's tick rate*, not from the plant, the owner, or any prior
measurement — the `project_bound_relative_to_persisted_state.md` /
"bound justified against a test constant" hazard. To its credit it is
declared rather than smuggled in. It is moot in any case: Finding 2 shows the
settle numbers it is applied to are band artifacts, so no bar applied to them
means anything yet.

## Finding 5 — effective n is 2 conditions, not 3 dwells, and the plant is noiseless

**[executed]** Dwells 0 and 1 return effectively identical results on every
arm and every metric (lag 183.46 vs 183.53 s; settle 240/240; overshoot
0.148/0.148; steady RMS 0.130/0.130). The plant is linear and dwell 1 starts
from dwell 0's settled state, so dwell 1 is the same experiment run a second
time, not an independent replicate. **Effective coverage is two distinct
conditions — one ramp-up, one ramp-down — at one ramp rate, on one zone.**

Repetition would add nothing: `sim_plant.c` is deterministic with no noise,
no sensor quantisation and no actuator quantisation, so repeat runs are
bit-identical. The right question is not "how many runs" but "how much
scenario coverage", and the answer is: very little. **[executed]** Sweeping
ramp rate 25/50/100/200/300 °C/hr shows the result is rate-dependent —
at 25 and 50 °C/hr fuzzy_50 and the retune converge (overshoot 0.149 vs
0.182, and 0.230 vs 0.372, both arms under the bar, no separation to report);
the separation this document reports appears only at 100 °C/hr and above.
The qualitative "retune crosses the bar, fuzzy does not" does hold at
100–300 °C/hr — but that is the same confounded arm from Finding 1.

On generalisation: this is a bench FOPDT model topping out around
ambient + 39 °C, with no radiation term, no multi-zone coupling, no relay
PWM window, no thermocouple noise and no actuator saturation dynamics. A
real kiln at ~1200 °C is radiation-dominated with an order-of-magnitude
different `k_dc/tau`. **Nothing here transfers to kiln-scale magnitudes**, and
this document's own scope-limits section says so correctly.

## Finding 6 — the assertions are real but orthogonal to the headline

**[read]** All four assertions are vacuity guards (bit-exact contract, entry
window reached, steady portion reached, settled at all). None of them
constrains the reported finding. `sim_fuzzy_overshoot: PASS` is therefore
evidence that the harness ran, not evidence for any claim in this document —
which is the correct design for a measurement harness, but is worth stating
because the build tally above reads as corroboration and is not.

## Verdict

- **Headline: refuted.** The claim that fuzzy inference earns its place on
  objective 4 rests on a comparison arm roughly twice as strong as fuzzy_50
  during the ramp. A correctly-scaled flat retune matches fuzzy_50 on all
  four objectives inside materiality.
- **Objective 2 result: withdrawn pending re-measurement.** The 40–55 s and
  60–80 s settle improvements invert at a 0.5 °C band and vanish at a
  25 °C/hr ramp.
- **Objectives 1 and 3: stand.** Small, sub-materiality, reproduced.
- **Objective 4a/4b instruments: 4b accepted; 4a's dwell-2 numbers
  mislabelled** (ramp-down residual, not overshoot).
- **Reproducibility and negative test: both good.** No poisoned-binary or
  stale-artifact problem was found; the harness is honestly built and the
  assertions genuinely fail on a real regression.

The harness is worth keeping — the per-objective decomposition is the right
idea and the undershoot instrument is a real improvement. What it needs
before any number from it drives planning: a comparison arm matched to
fuzzy's measured ramp-phase average rather than its centre cell, a settle
band at or below 0.5 °C, and at least a second ramp rate.
