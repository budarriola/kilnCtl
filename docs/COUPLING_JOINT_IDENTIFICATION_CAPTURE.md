# Joint coupling-matrix identification — capture procedure

**Status: ready to execute. Bench heat path confirmed working. Not yet run.**
Design/analysis pass, 2026-09-09; revised 2026-09-10 per
`docs/audits/cplval75_coupling_verdict_2026-09-10.md` (`d2e570ad`); revised
again 2026-09-10 per `docs/audits/coupling_deficit_sim_hardware_identity_2026-09-10.md`
(`b60a3f85`), which added an eight-plateau per-row fit spanning `cplval45`
(`7c18a11e`), `cplval75` (`1efbdc0c`), and the simulation credibility gate's
own two captures. No control code was changed and nothing was flashed to
produce any version of this document.

**2026-09-10 revision (second pass) — read this before running anything.**
`b60a3f85`'s eight-plateau fit refutes the working assumption that the
matrix's deficit is a single scalar per row. z2 is a clean scale (log-log
exponent 1.040, an added offset does not survive leave-one-campaign-out). **z0
is not a scale** (exponent 1.366): its residual is `+3.91 °C` at ΔT 10.5,
crosses zero once, and reaches `−5.58 °C` at ΔT 45.9 — a shape no per-row
multiplier can produce. z1 is intermediate. This changes the procedure below
in four ways, each detailed in its own section further down:

1. **The column-by-column design is now confirmed necessary, not just
   sufficient — do not shorten it.** z0's own duty supplies under 20% of its
   rise in a joint hold (`cplval75` §2b), so z0's row is nearly unobservable
   there; that is exactly why the shape error survived three campaigns
   undetected. See "Why the column steps cannot be shortened" below.
2. **Plateaus now need a target ΔT, not a target temperature.** The
   affine-vs-superlinear ambiguity on z0 lives entirely below ΔT ≈ 23 °C
   (the measured crossing), and the existing 45 °C hold landing at ΔT 10.5
   was luck (ambient had drifted to 34.4 °C), not design. See "Step sizes"
   below for the specific ΔT targets.
3. **Ambient must be measured at the thermocouples, at the plateau**, not
   inferred from a start-of-run snapshot or a different campaign's
   convention — see "What is logged" below. Cross-campaign ambient
   referencing is the leading suspect for the residual ≈0.035 gap between
   the gate's own hardware estimate and `cplval75`'s published scalars
   (`b60a3f85` §3), and it is the same open item as `cplval75` §2d.
4. **Add one high-ΔT joint plateau in the same session as the column
   steps** (~45 minutes) to remove that cross-campaign ambient term at the
   source, rather than only bounding it after the fact.

The earlier (first) 2026-09-10 revision's findings all still stand and are
unchanged by this second pass:

1. The step-duration floor was computed from a stale preset file, not the
   board — it understated the real floor by ~60% (§"Step sizes" below).
2. Acceptance step 6 (reproduce the ~62 °C `ff_hold_infeasible` boundary) is
   backwards: that boundary is now known to be the *old* matrix's z2-row gain
   deficit announcing itself, not a plant power limit. A correctly identified
   matrix should push it out past the 80 °C ceiling, not reproduce it.
   Testing for the old boundary would reject a correct result.
3. The 62–75 °C acceptance span was too narrow (1.4×) to separate a per-row
   scale error from a constant offset. A 45 °C plateau is now included to
   widen it to ~2.9×. (Superseded in degree, not in kind, by point 2 above:
   the span needed to separate scale-only z2 from shape-error z0 is wider
   still — see "Acceptance test" below.)

It also found that part of the original plan is now unnecessary: the 70 °C and
75 °C validation holds already exist as genuinely held-out data
(`cplval75`, `logs/coupling/cplval75_20260910_settled_hold_points.tsv`,
commit `1efbdc0c`) and do not need to be re-run — see "Acceptance test" below,
which now pins the falsifying prediction from that data before any new
capture runs. This saves roughly two hours of kiln time. The three single-zone
column steps are unaffected and are, if anything, more clearly justified than
before (§2c of `d2e570ad`: three proportional three-zone holds are collinear,
`cond(U) = 730`, and cannot separate diagonal from off-diagonal no matter how
many are added — single-zone column steps are the only shape that resolves
this).

**Sequencing: re-identify the DC matrix on steady plateaus first, fit plant
order second.** `b60a3f85` §5 shows the eight-plateau fit is entirely
dynamics-free (settled means against a DC matrix) and that a second-order
plant fit constrained to the same DC gain cannot move any number in it — but
doing the two in the wrong order risks absorbing a DC gain error into a
dynamic parameter. This capture is the DC step; do not fold in a plant-order
re-fit at the same time. It also means the dwell-entry peak bar (±2 °C) is a
separate, later question: raising the row gain to the correct value makes
that bar's failures worse, not better, because the peaks it is meant to catch
were previously unmeasurable (the sim never reached target at the old,
deficient gain). Closing the DC gain question exposes the peak question; it
does not answer it. See `b60a3f85` §5 for the numbers.

## Why this capture exists

`docs/audits/dc_gain_factor_of_ten_2026-09-09.md` (`3605f278`) found that the
live coupling matrix mixed two experiments: the FOPDT single-zone diagonal
(`model_k_dc` = 31.96 / 23.48 / 21.74 °C/duty) and a separate coupling run's
off-diagonals, whose own diagonal (38.13 / 35.90 / 35.32 °C/duty) was
discarded. Neither half was jointly identified with the other. Solving the
production solver (`zone_coupling_gauss_solve_partial_pivot()`, not a
reimplementation) for the observed 70 °C three-zone hold (`ΔT = 44.5 °C`)
against each candidate:

| `G` | condition (2-norm) | solved `u` | observed `u` |
|---|---|---|---|
| shipped mixed (FOPDT diagonal) | 14.194 | `[0.076, -0.118, 2.085]` | `[0.145, 0.53, 0.90]` |
| self-consistent (coupling run's own diagonal) | 4.641 | `[0.207, 0.523, 1.027]` | `[0.145, 0.53, 0.90]` |

The mixed matrix demands a negative duty from zone 1 and 2.085 from zone 2 —
infeasible, and roughly triples the condition number.

**Correction (2026-09-10, `d2e570ad`):** the table above computes `model_k_dc`
from `tuned_baseline_20260831.json`, a preset file — not the board. The
board's live diagonal (confirmed again this pass via `GET /api/zones`) is
39.2459 / 31.9669 / 31.6810 °C/duty, not 31.96 / 23.48 / 21.74. On the live
diagonal the shipped mixed matrix's condition number is **5.508**, not 14.194,
and it solves to `u = [0.119, 0.532, 1.165]` at the same 70 °C hold — no
negative duty, and only zone 2 mildly infeasible. The provenance guard's
rationale is unaffected (mixing two experiments' halves is wrong regardless of
magnitude), but the *measured* harm is smaller than originally reported, and
the real justification for this capture is now the 12/19/31% per-row gain
deficit `d2e570ad` measured directly against `cplval75`, not this table. See
that audit for the corrected numbers throughout. `587a34ae` ("Refuse a
coupling matrix assembled from two different experiments") added
`coupling_matrix_provenance_ok()` in `zone_coupling_solve.c`, called from both
`zone_coupling_solve_hold()` and `zone_coupling_solve_climb()`: if the system
carries any measured off-diagonal, every member's diagonal must also come
from a measured, same-experiment fit (`coupling_diag_k_dc` with
`use_measured_diag_k_dc = true`), or the whole matrix is refused and the
caller falls back to the uncoupled per-zone diagonal feedforward
(`COUPLING_SOLVE_FALLBACK_MIXED_PROVENANCE`). **Today's board runs on that
fallback** — it has measured off-diagonals and no persisted
`coupling_diag_k_dc`, so `ff_hold`/`ff_climb` never assemble a coupled `G` at
all until a joint identification lands.

## The good news: no new firmware or tooling is needed

Verified directly against the firmware source, not assumed:

- `autotune_engine_step_identify.c`'s `autotune_finalize_fit()` already fits
  the **whole column** from one zone's step trace: the direct FOPDT fit for
  the stepped zone (its own diagonal cell) plus every neighbor zone's
  cross-gain response to that same step (their row, that column) — one
  trace, one persist. The comment there is explicit: *"cell[zone_index][j]
  ... is the FOPDT fit of ZONE j's own trace against the duty step commanded
  at zone_index's heater ... the AFFECTED zone owns the row, the zone under
  test is the column."*
- `autotune_engine_guard.c` (around the `coupling_diag_k_dc` persist,
  gated on `model_persisted`) writes that same run's direct-fit gain into
  `coupling_diag_k_dc` — same trace, same duty step, same run as the
  cross-gain cells just persisted above it. Diagonal and off-diagonals for a
  given zone's step are same-experiment by construction.
- The only reason this shape has never produced a self-consistent matrix on
  this board is that it has never been **run for all three zones** against
  the currently adopted off-diagonals — see "Residual gap" below.

So the capture is three ordinary single-zone **step-method autotune runs**
(`autotune_start(zone, "step", step_duty)` → `autotune_accept()`, both already
published through `kiln_call`/`kiln_find` on the `kilnctrl` MCP server), one
per zone, from a rested baseline, each followed by full cooldown before the
next. No firmware change, no new MCP tool.

## Why the column steps cannot be shortened to more joint plateaus

An earlier reading of this material suggested that running more joint
(all-three-zone) plateaus, rather than the three single-zone column steps,
might be a cheaper substitute. **`b60a3f85` shows the opposite: z0's row is
nearly unobservable in a joint hold, which is exactly why the shape error on
z0 hid for three separate campaigns before this fit caught it.** In a joint
hold, z0's own duty supplies under 20% of its rise (`cplval75` §2b) — over
80% is neighbour heat from z1/z2 — so a joint hold gives the solver almost no
leverage on z0's own diagonal or its cross-gains independent of the other two
rows. That is why `cplval75`'s inverse solve blamed z2 alone for the whole
matrix's deficit, when z0 in fact carries the larger, non-scalar error.
Stepping one column (one zone's duty) at a time, with the other two zones
undriven, is what isolates each row's response and gives each cross-gain
independent leverage. **Do not substitute additional joint plateaus for the
column-by-column design; more joint holds would reproduce the same blind
spot, not close it.**

## Hypothesis: z0's shape error is buoyant transport, not a bad fit

Recorded here as a **named hypothesis with a stated discriminator**, not a
finding — do not treat it as settled, and do not "fix" it with a better-
fitted linear matrix without checking the discriminator first.

Three separate observations single out z0, the physically **topmost** zone
(z2 is the bottom — `project_zone_physical_arrangement`): the residual sign
flip in the eight-plateau fit (§ above); the smallest apparent row scalar
when fitted at high ΔT only; and the simulation's dwell-entry overshoot
mismatch, which concentrates on z0 (`b60a3f85` §5). One mechanism explains
all three: **buoyant (convective) heat transport from the lower zones into
the top zone grows faster than linearly with ΔT.** The matrix models z0's
neighbour heat as a *linear* function of the lower zones' duty. At small ΔT
the convective flow is weak and the linear model over-credits z0 (the
observed positive residual at low ΔT); as ΔT grows the flow strengthens
superlinearly and the same linear model under-credits it (the observed
negative residual at high ΔT). z2, at the bottom, receives essentially none
of this and is well described by a single scale factor, which is what the
data shows.

**If this hypothesis is right, the fix is not a better-fitted linear
matrix — no re-identification of `G` alone can capture a superlinear term —
it is an additional term in the plant model** (a convective transport term,
plausibly with its own lag, contributing to why z0 shows extra dynamic order
beyond a single FOPDT lag). That is worth knowing before anyone concludes a
re-identified linear matrix has "solved" z0: it will fit the mean of the
measured range better, but it will still systematically mispredict at the
extremes, in the same sign pattern documented above.

**Named discriminator:** a per-row fit of `ΔT = s·(G·u) + c` against a
superlinear cross-gain model needs plateaus below ΔT ≈ 10 °C and above
ΔT ≈ 46 °C to separate an affine correction from a true superlinear one —
see "Step sizes" below, which sets the column-step and joint-plateau ΔT
targets accordingly. The hypothesis also predicts the z0 offset should
shrink when the lower zones are stepped **individually** (this capture's
column-by-column design) rather than held jointly, which this capture
measures directly as a side effect of its main purpose.

## Precondition 0 — bench heat path: CONFIRMED working

`docs/audits/cplval75_aborted_executor_panic_2026-09-09.md` recorded that an
earlier attempt at a heating capture on this same profile shape got zero
thermal response for 397 s while duty wound up to saturation on all three
zones. That gate is now satisfied: a 13-minute proof run took the bench board
from 36 °C to 53 °C, and `cplval75` subsequently completed a full 2h38m
heating capture (three settled plateaus at 62/70/75 °C — see
`docs/audits/cplval75_coupling_verdict_2026-09-10.md`, `d2e570ad`). The heat
path is real and working; this gate does not need to be re-checked before
running the steps below.

Also confirm before starting:
- `get_heap_status` — no unacknowledged crash banner.
- `safety_get_status` / `safety_get_diag` — link up, `trip_mask 0x0000`,
  `warn_mask 0x0000`, S1 (`abs_max_temp_c`) and S8 (`max_rate_c_per_min`)
  both ARMED (both confirmed armed as of `docs/audits/
  safety_commissioning_completion_2026-09-09.md`: S1 = 80 °C, S8 = 20 °C/min
  — the ramps below, ≤60 °C/hr = 1 °C/min, are well inside S8's ceiling).
- `profiles_get_exec_status` / `autotune_get_status` — both idle.
- E-stop verified per the bench procedure (readiness item `estop_verified`
  was `not_done` as of the same audit) — confirm separately; not re-litigated
  here.

## Preconditions per zone — rested baseline, verified not assumed

`project_autotune_needs_rested_baseline` (project memory) records that
residual heat biases a fitted gain low, and that **every** zone must be near
ambient before a step, not only the one under test — this board's cross-zone
coupling is 5–12 °C/duty, large enough that a warm neighbour measurably
distorts the stepped zone's own fit.

**The mechanical trap already found and worth avoiding here:**
`run_queue.py`'s `is_rested()` (default tolerance `DEFAULT_RESTED_TOL_C =
1.0 °C`, timeout `DEFAULT_RESTED_TIMEOUT_S = 3600 s`) checks each channel
against **its own cold-junction reading**, not a fixed absolute reference.
That is self-referential: in a back-to-back campaign the cold junction itself
drifts upward run over run, so the check keeps passing while the real
baseline ratchets. `1571de87` ("run_queue: enforce paired start temperatures
for A/B campaigns") fixed the specific case this caused for matched A/B pairs
by adding `pair_start_tol_c` (`DEFAULT_PAIR_START_TOL_C = 1.5 °C`), which
anchors the second arm to the **first arm's own actual recorded start
reading** instead of to its own CJ. That mechanism does not directly apply
here (this capture has three different zones' steps, not two arms of one
zone), but the lesson does: **do not rely on `is_rested()` alone across the
three sequential zone steps.**

Concretely, before each of the three zone steps:
1. Record `GET /api/status` (or `control_get_zones`) for all three zones and
   the enclosure/ambient reading at the start of the *whole* capture
   (session start), not per-zone — call this `T_amb0`.
2. Before starting zone *N*'s step, require **all three** zones' `actual_c`
   to be within 2 °C of `T_amb0` (not just within 1 °C of their own current
   cold junction) — this is the absolute check `is_rested()` does not do.
   `wait_until_rested()` can still be used to block on the mechanical
   per-channel-vs-own-CJ condition first (cheap, catches gross residual
   heat), but the absolute-vs-`T_amb0` check is the one that gates the step
   actually starting.
3. Log the confirmed rested state (all four thermocouples, ambient) before
   every step — this is also the sidecar data that would have separated
   "genuinely rested" from "reads flat because nothing is being logged."

## Step sizes, duration, and settling — confirmed not assumed

**2026-09-10 correction:** the durations below were originally derived from
`model_tau_s` = 166.9 / 129.1 / 114.8 s, read from
`tools/PcTools/config_presets/tuned_baseline_20260831.json` — a preset file,
not the board. Re-read live from the board this pass (`GET /api/zones`,
2026-09-10): **`model_tau_s` = 263.8 / 269.8 / 270.9 s** for zones 0/1/2 — all
three roughly 2.4× the preset values, and the worst case is now zone 2's
270.9 s, not zone 0. (Live `model_k_dc` is likewise 39.2459 / 31.9669 /
31.6810, not the preset's 31.96 / 23.48 / 21.74 — see the correction above.)
This matters because dwell truncation is exactly the failure mode this floor
exists to prevent: two earlier captures (`coupid6`, the aborted 2026-09-09
run) were made unusable by dwells too short to settle.

`docs/audits/dc_gain_factor_of_ten_2026-09-09.md` §5 specifies the
duration floor this capture must clear, based on the previous unusable
attempt: `coupid6`'s 70 °C dwell ran only ~2.3 τ (600 s) and zone 0's duty
was still visibly climbing (0.09 → 0.11 → 0.15 across the dwell, never
converged) — that capture answered nothing. The audit's own standard: **≥8 τ,
≥25 minutes**, with the last ~10 minutes reserved for averaging a settled
value.

**Target ΔT, not target temperature (2026-09-10, `b60a3f85`).** Every
plateau in this capture — the three column steps and the joint plateaus —
should be specified and logged by its **target rise above the
plateau's own measured ambient (ΔT)**, not by a fixed absolute temperature.
The existing 45 °C hold happened to land at ΔT ≈ 10.5 °C only because
ambient had drifted to 34.4 °C by the time it ran; that was luck, and the
eight-plateau fit shows the affine-vs-superlinear ambiguity on z0 lives
entirely below the measured crossing at ΔT ≈ 23 °C. Concretely:
- **Column steps:** aim for ΔT ≈ 19 °C at the stepped zone (the step-duty
  0.5 target below already predicts this) — mid-range is fine for the
  columns themselves, since their job is separating diagonal from
  off-diagonal, not resolving z0's shape.
- **New joint plateaus need to bracket the ΔT ≈ 23 °C crossing on both
  sides**, not sit inside it: one **below ΔT ≈ 10 °C** (a low-power joint
  hold, deliberately targeted rather than landed on by drift) and one
  **above ΔT ≈ 46 °C** (the high-ΔT plateau added below). The existing
  62/70/75 °C `cplval75` holds already supply ΔT ≈ 33/41/46 °C, so only the
  low-ΔT leg is a genuine gap once the high-ΔT addition below is run.

Apply this per zone step:
- **Step duty:** 0.5 (0–1 scale). At `K_diag ≈ 38 °C/duty` this predicts a
  single-zone rise of ~19 °C above ambient — well inside the 75 °C working
  ceiling from a ~25–32 °C start, and comfortably clear of the ceiling even
  accounting for the small cross-heat the other two (undriven, duty≈0)
  zones will show.
- **Minimum wall clock per step:** 8 × 270.9 s ≈ 2167 s ≈ **36.1 minutes**,
  rounded up to **37 minutes** minimum before the fit is even considered;
  do not shorten this to zone 0/1's smaller τ — one duration floor for all
  three keeps the three columns comparable. (This floor is worst-case-zone
  driven and is now 270.9 s's, not 166.9 s's — recompute it again from
  `GET /api/zones` if the board's fitted `model_tau_s` changes before this
  capture runs.)
- **Settling is confirmed by the firmware's own gate, not by a fixed timer.**
  `autotune_finalize_fit()`'s persist path only writes the coupling cells
  when `s_at.model.settled && s_at.model.extrapolation_converged &&
  s_at.model.tau_consistent_with_gain` are all true (the same STEP-method
  quality gate `autotune_accept()` checks via `model_settled` before
  allowing the plain gain/PID accept). Call `autotune_get_status()` and
  require `model_settled=True` before calling `autotune_accept()`. **Do not
  pass `ack_unsettled=True`** for this capture — an unsettled fit here would
  reintroduce exactly the truncation-bias problem
  (`MAX_EXTRAPOLATION_RATIO = 2.0` in `pid_autotune.c` bounds it, but the
  audit's own truncated-trace table shows −6% to −69% bias, which is not
  acceptable for a matrix meant to replace the current fallback). If
  `model_settled` is false after the 37-minute floor, let the run continue —
  the firmware's own 4-hour backstop is the real ceiling, not this
  document's estimate.

**New: one high-ΔT joint plateau, same session as the column steps
(2026-09-10, `b60a3f85`).** After the three column steps, and before the
board is allowed to cool down for the day, run one additional all-three-zone
joint hold targeting ΔT ≈ 46–51 °C (a setpoint near the 75 °C working
ceiling from whatever ambient prevails that session) for the same ≥8τ/37-minute
floor. Its purpose is narrower than the column steps': it pins each row's
scale under the **same session's ambient** as the columns, which removes the
cross-campaign ambient-referencing term that currently limits agreement
between the gate's own hardware estimate and `cplval75`'s published scalars
to ≈0.035 (`b60a3f85` §3) — a row-independent gap consistent with residual
ambient-reference error, the same open item as `cplval75` §2d. This plateau
costs about 45 minutes on top of the 6.5–8 hour total (see "Total kiln time"
below) and is the cheapest available accuracy win in this capture.

## What is logged, at what cadence

Match the existing `logs/coupling/*.jsonl` HTTP-poll format
(`tools/PcTools/src/kilnctrl/http_capture_log.py`) so this capture is
directly comparable to `coupid6`/`cpl_z*`/`cplval75`:

- One line per poll: `{"t": <unix>, "exec": <GET /api/profile_exec or
  equivalent autotune status body>, "status": <GET /api/status>, "control":
  <GET /api/control, for the bd_* feedforward breakdown>, "ct": <raw CT
  block>}`.
- Poll interval: 5 s (`DEFAULT_POLL_INTERVAL_S`), same as every other
  coupling capture on this rig.
- Required fields, per the `cplval75` post-mortem's own recommendation
  (`docs/audits/cplval75_aborted_executor_panic_2026-09-09.md`, "add
  `ct_counts` to the capture sidecar so the next run answers this in
  seconds" — implemented in `2c178fd2`, "Log raw ct_counts in the
  heating-capture sidecar; add no-heat diagnostic TSV"):
  - **Pre-PWM intended duty** per zone (`exec`/`status`'s `duty` field —
    confirmed by the earlier audit to be the same 0–1 intended value
    `pid_autotune.c`'s `duty_step` uses, not the chopped relay state).
  - **All four thermocouples** (three zone TCs + the safety processor's own,
    via `status`/`get_board_state.thermo`), with cold-junction readings, so
    "ambient" is never inferred from one channel alone.
  - **Ambient measured at the thermocouples, at the plateau itself
    (2026-09-10, `b60a3f85`)** — not just a start-of-run snapshot, and not a
    value carried over from a different campaign's convention. Sample the
    enclosure reading (or a spare unused channel) every poll throughout, and
    additionally record the mean ambient over the same final-10-minute
    averaging window used for settled duty, at every plateau (columns and
    joint holds alike). This is what the whole capture uses to convert a
    settled temperature into ΔT for the per-row fit; a cross-campaign or
    start-of-run ambient reference is the leading suspect for the ≈0.035
    row-independent gap `b60a3f85` §3 found between two otherwise-agreeing
    independent estimates, and is the same open item as `cplval75` §2d.
  - **Raw `ct_counts`**, every poll, both while relays are on and off. This
    is what would have distinguished "mains off" from "element break" in the
    aborted `cplval75` run and cost nothing extra now that `2c178fd2`'s
    sidecar carries it.
- Name files `coupling_col_z<N>_<YYYYMMDD>.jsonl` (one per zone step) plus a
  `..._ambient.tsv` 60 s sidecar per the `cplval75` convention, so the three
  captures sort together and match the naming this repo already uses.
  `.jsonl` raw captures stay local/uncommitted per `.gitignore:100`; commit
  only a derived observations TSV/MD per zone, same as
  `coupid6_dwell_observations.tsv` and `cplval75_20260909_observations.tsv`.

## Total kiln time, and splitting across sessions

Per zone: rested-wait (typically well under the 3600 s timeout once the
board has been sitting idle) + ≥37 min step + a cooldown-to-`T_amb0` wait
before the next zone's step (natural convective cooling; budget 30–60 min
based on the `cooldown_after_*` tails already in `logs/coupling/`). Call it
roughly **1.75–2.25 hours per zone**, **~5.5–6.5 hours total** for the three
columns.

Add one fresh low-ΔT (target ΔT ≈ 10 °C) three-zone hold for the widened
acceptance test below (rested-wait + ≥37 min hold + cooldown, same shape as a
column step): roughly **1–1.5 hours** more. (This is the same slot the
document previously called "one fresh 45 °C hold" — it is now specified by
target ΔT rather than target temperature; see "Step sizes" above.)

**The 70 °C and 75 °C validation holds do NOT need to be re-run** — they
already exist as genuinely held-out settled data (`cplval75`,
`docs/audits/cplval75_coupling_verdict_2026-09-10.md`, `d2e570ad`), saving
roughly two hours versus the original plan.

**New (2026-09-10, `b60a3f85`): add one high-ΔT (≈46–51 °C) joint plateau in
the same session as the column steps**, ~45 minutes, to pin each row's scale
under the same-session ambient and remove the cross-campaign ambient term —
see "Step sizes" above.

**Total new kiln time for this capture: roughly 7.25–8.75 hours** (three
columns ≈ 5.5–6.5 h + one new low-ΔT joint hold ≈ 1–1.5 h + one new high-ΔT
joint hold ≈ 45 min), against the first 2026-09-10 revision's estimate of
6.5–8 hours (three columns + one low-ΔT hold only) and the original estimate
of 4.5–6 hours that used an understated step duration and included a
validation hold that turned out to be unnecessary. The ~45-minute high-ΔT
addition is the only change to the time budget from the second 2026-09-10
revision.

**This can be split across sessions, zone by zone, without invalidating the
result** — each zone's step supplies its own diagonal and its own row of
off-diagonals from one self-contained trace; nothing about the
`coupling_matrix_provenance_ok()` guard cares whether zone 0's step ran
Tuesday and zone 2's ran Thursday. What must NOT happen: re-identifying only
one column of an otherwise-adopted matrix weeks or months later. The
audit's own "Residual gap this cannot close" section names exactly this: two
columns each internally complete but identified in different runs is
undetectable by the current guard, since there is no provenance stamp per
cell (`ZONES_CFG_VERSION` bump, explicitly the owner's call, not made in this
pass). Treat this capture as one campaign — run all three columns within the
same few-day window, note the run timestamps in the committed observations
doc, and do not "top up" one stale column later without re-running the whole
set.

## Acceptance test — pinned now, against data that already exists

Project memory: *"a matrix with 'coverage of 1' needs a prediction that
tests it"* — the standing lesson from `project_coupling_matrix_resolved` and
the negative-test discipline in `CLAUDE.md`. This is that prediction. As of
the 2026-09-10 revision, two of the three points it is tested against already
exist as genuinely held-out data and are pinned here **before** the new
capture runs, so nothing about the criterion can be tuned to fit afterward.

1. **Condition number.** Compute `cond(G)` (2-norm) for the freshly
   assembled self-consistent matrix using the real solver path (not a
   reimplementation). **Must be < 10.** For reference: the shipped mixed
   matrix measures **5.508** on the live diagonal (not 14.194 — that figure
   used the stale preset diagonal, see the correction above); the single
   coupling-run-diagonal candidate measured 4.641. A result at or above 10
   means the joint identification did not actually resolve the mismatch and
   should not be adopted.
2. **Falsifying prediction — pinned now, from existing held-out holds.**
   `cplval75` (`docs/audits/cplval75_coupling_verdict_2026-09-10.md`,
   `d2e570ad`) already contains two settled, full three-zone holds that were
   never used to fit any column step and remain valid held-out validation
   data for a jointly identified matrix:

   ```
   u(62 °C) = [0.168, 0.371, 0.591]   ambient 29.19 / 29.07 / 29.03 °C
   u(70 °C) = [0.176, 0.479, 0.759]   (+~2 °C drift by the 75 °C plateau)
   u(75 °C) = [0.176, 0.545, 0.853]
   ```

   The 70 °C plateau is settled at 6.8 τ (temperature std ≤ 0.27 °C, duty
   flat); the 75 °C plateau is stronger, 10.2 τ, std ≤ 0.33 °C. **This
   prediction still stands as the primary falsifier and is kept unchanged.**
   Add the two new plateaus below (a low-ΔT and a high-ΔT joint hold) to
   widen the span and supply a **second, independent criterion** that this
   single per-zone prediction cannot: whether the deficit is a per-row scale
   or has a z0-shaped offset term (criterion B below). Before running either new
   plateau, compute and record `u_pred = G⁻¹(T_sp·[1,1,1] − T_amb)` for it
   from the freshly assembled matrix, the same way `u_pred` was computed for
   62/70/75 °C — this is the only leg of the prediction not already pinned
   by existing data.
3. **Run the two new joint holds** (target ΔT ≈ 10 °C and ≈ 46–51 °C, per
   "Step sizes" above) for ≥8 τ each (≥37 min, same floor as the column
   steps — recompute from live `model_tau_s` if it has changed), averaging
   observed duty *and* ambient (measured at the thermocouples) over the
   final 10 minutes on all three zones, and confirming settlement the same
   way `coupid6` failed to: duty must be flat, not still monotonically
   drifting, at the end of the averaging window.
4. **Pass/fail, criterion A (point prediction, per plateau).** The fit is
   accepted only if every zone's observed settled duty is within **0.05
   duty (absolute) or 15% (relative), whichever is looser**, of `u_pred`, at
   **all five** plateaus — the new low-ΔT and high-ΔT joint holds plus
   62/70/75 °C (existing `cplval75` data, pinned above).
5. **Pass/fail, criterion B (scale vs. shape, per row) — new, 2026-09-10,
   `b60a3f85`.** Criterion A alone cannot tell a per-row scale error from a
   z0-shaped offset error apart if both happen to land inside the same
   tolerance band at the tested plateaus — which is exactly how the
   previous matrix's z0 deficit went undetected across three campaigns.
   Using the same five plateaus (plus the three column steps' own settled
   points, if logged with matching ΔT/duty pairs), fit per zone row
   `ΔT_observed = s·(G·u)_predicted_row + c` by least squares, the same
   method `b60a3f85` §4.1 used across the eight-plateau set. **Require, per
   row: either `c` is not significantly different from zero (its magnitude
   is smaller than the row's own residual rms under `c ≡ 0`), confirming a
   pure scale error remains and the matrix's fitted diagonal/off-diagonals
   already absorb it — or, if a nonzero `c` is needed to materially reduce
   rms (as z0 needed in the eight-plateau fit), the freshly identified
   matrix's OWN row must already show |c| under 1.5 °C** (i.e. the joint
   re-identification, not a post-hoc rescale, has actually captured the
   shape, not just re-centered the same shape error at a new scale). A
   freshly identified matrix that still needs a large offset correction on
   any row has not resolved that row's deficit, even if criterion A passes
   at every individual tested plateau.
6. **What would falsify it:** `u_pred` and the observed duty disagreeing
   outside criterion A's band on any zone at any of the five plateaus, OR
   criterion B finding a newly-fitted row still needs `|c| ≥ 1.5 °C`, OR the
   new holds' duty still trending (non-flat) at the end of the averaging
   window (the `coupid6` failure mode), OR the solve reporting
   `COUPLING_SOLVE_FALLBACK_MIXED_PROVENANCE` at any point (meaning the guard
   itself does not consider the persisted matrix self-consistent — check via
   the `control` capture's `bd_coupling_correction`/`ff_hold_used_matrix`
   fields, matching `cplval75`'s own diagnostic use of that flag).
7. **First-infeasible boundary — rewritten 2026-09-10, reversed from the
   original criterion.** `d2e570ad` §3 shows the previously documented
   ~62 °C `ff_hold_infeasible` boundary was the OLD matrix's z2-row gain
   deficit announcing itself (predicted `u=1` at ΔT=38.2 °C), not a plant
   power limit — `cplval75` measured z2 actually holding at duty 0.853 at
   ΔT=46 °C, well past that boundary. The freshly assembled matrix must
   therefore push its own solved first-infeasible ΔT (`1/`, on the diagonal
   of `G⁻¹`, per zone) out past **ΔT = 51 °C** (≈80 °C bench ceiling minus a
   ~29 °C ambient) on every zone. **Reproducing the old ~62 °C boundary is
   now a FAIL, not a pass** — it would mean the new matrix carries the same
   gain deficit as the old one.

## Bench limits — and what they mean, not just what they are

- Both processors' ceiling is 80 °C (`abs_max_temp_c` on the RP2040, S1
  ARMED per the 2026-09-09 commissioning check; `max_temp_c` = 80.0 on every
  zone in the ESP-side config, confirmed in
  `tools/PcTools/config_presets/bench_fixture.json`). The owner's standing
  margin rule is ~5 °C, so **75 °C is the working maximum** for anything in
  this capture, including the new low-ΔT and high-ΔT joint plateaus and the
  three column steps.
- **Corrected 2026-09-10 (`d2e570ad` §3).** The original text here claimed
  the documented `ff_hold` infeasibility above ~62 °C was "a direct,
  structural consequence of [the coupling run's own ~38 °C/duty diagonal],
  not a bug to be engineered around." **That is wrong as stated and is
  refuted by `cplval75`:** the ~62 °C boundary is the shipped matrix's z2-row
  gain deficit (§2–3 of `d2e570ad`) predicting saturation at ΔT=38.2 °C,
  while the plant itself was measured holding z2 at duty 0.853 at ΔT=46 °C —
  4 °C of ΔT and roughly 0.15 duty of headroom past where the old matrix
  claimed the plant ran out of power, with no sign of actually saturating.
  Extrapolating the three measured points puts the real `u₂=1` boundary near
  ΔT≈51–53 °C, i.e. a setpoint at or above the 80 °C ceiling — not 62 °C.
  **Do not treat a ~62 °C infeasibility boundary as expected or acceptable
  from a jointly identified matrix** (see acceptance step 7 above, reversed
  accordingly). Some infeasibility very close to the 75–80 °C ceiling on z2
  is still plausible and is not itself a failure of the acceptance test,
  which is about the *settled* duty at each of the five hold plateaus, not
  the transient into them.

## What this capture will and will not establish

**Will establish:** a self-consistent 3×3 coupling matrix identified from one
family of experiments (all three columns, same rig, same few-day window),
validated against held-out three-zone holds spanning ΔT ≈ 10–51 °C (roughly
35–80 °C absolute, depending on ambient) that were not used to fit the
columns, and — via acceptance criterion B — checked for whether any row's
deficit is a per-row scale or needs its own shape correction, not assumed to
be a scale by construction.

**Will not establish:** anything about kiln behavior at firing temperature.
Project memory (`project_iae_noise_floor_unknown`-adjacent findings and the
recorded FOPDT scaling) notes that both the identified gain `k` and time
constant `tau` fall by roughly the same factor — approximately 20× — between
this bench range and a real ~1200 °C kiln firing. A matrix identified here
describes the low-temperature radiative/conductive regime of this specific
bench rig (enclosure, element placement, insulation) and should be treated
as a **bench-only artifact**: useful for exercising and validating the
solver/guard logic end-to-end (which is otherwise untested against any
self-consistent matrix at all), not as a source of gains to trust at
production firing temperatures. Re-identification at real firing
temperatures, if ever undertaken, is a separate and much higher-stakes
capture with its own safety case — out of scope here.

## Defects noticed in this pass, not fixed here

- No provenance stamp exists per coupling cell (diagonal or off-diagonal) to
  catch a future "redo one column, leave the others stale" mistake — named
  above and in `3605f278`'s own "Residual gap" section; a `ZONES_CFG_VERSION`
  bump is the fix, deliberately left to the owner given the documented
  rollback-to-default-gains hazard that bump carries.
- `is_rested()` in `run_queue.py` remains self-referential for any campaign
  shape other than the paired-A/B case `1571de87` already covers. This
  capture's procedure works around that by hand (the absolute `T_amb0`
  check above); `run_queue.py` itself was not changed in this pass.
