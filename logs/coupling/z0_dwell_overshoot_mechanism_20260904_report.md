# Zone 0 dwell-entry overshoot: mechanism investigation (offline, 2026-09-04)

Follow-on to `tracking_synthesis_20260904_report.md`, which identified z0's
dwell-entry overshoot (2.18 °C on the current matrix, n=4, vs z1's 1.45 °C)
as the single largest remaining tracking defect and clears the owner's
0.5 °C actionable bar by ~4x. This pass diagnoses the mechanism from the
same 27-capture pool (no new firings — offline, from disk, per instructions)
and proposes a specific, board-time A/B. **Live campaign
`fuzzy_ab_20260904d` (arm B1 firing) was not touched — its
`fuzzy_ab_20260904d_s50_run1.jsonl` file is excluded from every number
below, matching the synthesis report's own exclusion.**

Tooling: a new offline script,
`tools/PcTools/scratch_z0_overshoot_analysis.py`-equivalent logic run from a
temp scratchpad (not checked in, same discipline the synthesis report used),
built on the existing `kilnctrl.http_capture_log`/`kilnctrl.log_analysis`
readers — no new capture format, no board access. It walks each capture for
the ramp→dwell transition on a given zone (`dwelling: False→True`), then
reports: the ramp rate in the 90 s before transition, the peak error and its
time-after-transition, the zone's own duty at transition/peak, and every
peer zone's duty at the same two instants.

## 1. Shape and timing of z0's dwell-entry overshoot (n=4, current matrix, target 60 °C)

| capture | ramp rate (°C/min) | peak error (°C) | peak time | own duty @trans / @peak | z1 duty @peak | z2 duty @peak |
|---|---|---|---|---|---|---|
| `ab_new_1` | 1.61 | 2.21 | +94 s | 0.27 / 0.10 | 0.375 | 0.727 |
| `ab_new_2` | 1.56 | 2.27 | +89 s | 0.25 / 0.08 | 0.342 | 0.646 |
| `ab_new_3` | 1.96 | 2.32 | +100 s | 0.23 / 0.06 | 0.300 | 0.558 |
| `p7_newmatrix_http` | 1.87 | 2.55 | +107 s | 0.26 / 0.05 | 0.284 | 0.579 |

(`p7_newmatrix2_http`, target 45 °C not 60, excluded from this table — see
§3 caveat.)

**Shape:** z0's own commanded duty has already crashed to near-zero
(0.05–0.10) by the time its temperature peaks, 89–107 s after the ramp→dwell
boundary — z0 is not over-driving itself at the moment of overshoot. The
error then persists 204–245 s before returning under 0.3 °C (n=4), roughly
matching z0's own identified thermal time constant (`model_tau_s` = 263.8 s,
current preset). At the same peak instant, its neighbours are still
delivering substantial duty — z2 (bottom zone) 0.56–0.73, z1 0.28–0.38 —
which the old-matrix pool (n=12, peer duties 0.6–0.75 for z2) shows even
higher, alongside the old matrix's larger overshoot (3.5–4.1 °C). This
parallels §3.1's original diagnosis for the (now largely fixed)
whole-project dwell-entry overshoot: the plant arrives with stored rate, not
that the zone is being over-driven after arrival.

**z0 vs z1 vs z2 at their own peaks** (current matrix, `ab_new_1..3` +
`p7_newmatrix_http`, n=4 each): z1's own duty at its peak is still moderate
(0.28–0.34, down from ~0.5), and z2's own duty barely eases by its own peak
(0.56–0.67, down from ~0.8–0.9) — z2 keeps driving hard through its entire
transition. z0 is the outlier: its own duty has crashed hardest and fastest
of the three by the time it overshoots.

## 2. Hypotheses tested against the data

### (a) Ramp rate into the transition — SUPPORTED, n=10, r≈0.76, near-1:1 slope

Pooling every current-matrix, target-60 °C z0 transition with a ramp-rate
figure — `ab_new_1..3`, `p7_newmatrix_http` (ease-off mult=2.0, board
default), and both ease-off campaign arms `easeoff_ab_20260904_2p0_run1..3`
/ `_3p0_run1..3` (n=10 total, all current matrix, all with the terminal
ease-off taper already active) — a linear fit of peak overshoot against the
90 s pre-transition ramp rate gives:

```
peak_overshoot_c ≈ 0.44 + 1.00 × ramp_rate_c_per_min      (n=10, r=0.76)
```

A near-1:1 slope, on data where the ease-off taper (§3.1) is *already
active in every single row* — this is the residual overshoot left over
after the fix that closed the original, larger, all-zone problem. z0 still
carries roughly 1 °C of overshoot per 1 °C/min of approach rate despite the
taper. This is the strongest single correlate found in this pass and the
best-supported mechanism.

### (b) Coupling injection from below (z1/z2 still driving after z0's duty crashes) — CONSISTENT ACROSS ERAS, NOT SUPPORTED AS THE VARIANCE DRIVER WITHIN AN ERA

Cross-era (old vs current matrix vs the one 45 °C data point), higher peer
duty at z0's transition tracks with larger z0 overshoot — old matrix
(z2 duty ~0.6-0.9, overshoot 3.5-4.1 °C) → current matrix (z2 duty ~0.74-0.91,
overshoot 2.2-2.5 °C) → the single 45 °C point (z2 duty ~0.17-0.23, overshoot
0.73 °C). That is a real, monotonic pattern, and it is independently
supported by the coupling matrix's own numbers (`tuned_baseline_20260831`/
`easeoff_ab_2p0_20260903.json` preset, `coupling_coeff` per zone): the
**row** sums that matter for heat arriving *at* z0 (not the column sums the
synthesis report quoted, which are z0's outgoing influence) are

| zone | heat received from neighbours (row sum, off-diagonal) |
|---|---|
| z0 | 27.32 (from z1) + 21.72 (from z2) = **49.04** |
| z1 | 14.30 (from z0) + 22.15 (from z2) = 36.45 |
| z2 | 8.33 (from z0) + 12.42 (from z1) = 20.75 |

**z0 receives more coupled heat from its neighbours than either of them
receives (49.04 vs 36.45 and 20.75)** — the opposite framing from the
synthesis report's column-sum statement (z0's influence *on* others is
lowest), but the same underlying asymmetric matrix, read the other way for
the question that matters here (what heats z0, not what z0 heats). This is
consistent with z0 being the physically top zone, receiving convected heat
from both zones below it, and fits the observation in §1 that z0 keeps
rising well after its own duty is near zero.

**However**, within the current-matrix, target-60 °C pool (n=10, same set as
2a), peer duty at transition varies only narrowly (z2: 0.74–0.91, z1:
0.47–0.56) and does **not** predict the run-to-run variance in z0's
overshoot: `corr(peak_error, z2_duty@transition) = -0.06`,
`corr(peak_error, z1_duty@transition) = -0.41` (wrong sign, weak). **The data
support coupling injection as a plausible background contributor to the
absolute size of z0's overshoot (via the cross-era/cross-target comparison),
but cannot show it drives the *variance* seen run-to-run at fixed
matrix/target** — that variance tracks ramp rate instead (2a). These two
mechanisms are not mutually exclusive (coupling could set a floor, ramp rate
sets the excursion above it) but this dataset cannot separate a
"floor-only" coupling contribution from "no marginal effect at all" because
the coupling term barely moves within any one era. **What would settle
it:** a controlled comparison at matched ramp rate with two different peer
duty levels — not present in this dataset, and not producible offline.

### (c) Plant-model mismatch specific to z0 — SUPPORTED as an amplifying factor, not the trigger

From the live per-zone identified model (`model_k_dc`/`model_tau_s`/
`model_dead_time_s`, `tuned_baseline_20260831.json` / `easeoff_ab_*.json`
presets, both current-matrix era):

| zone | `model_k_dc` | `model_tau_s` | `model_dead_time_s` |
|---|---|---|---|
| z0 | **39.2459** | 263.8 | **52.8** |
| z1 | 31.9669 | 269.8 | 43.5 |
| z2 | 31.6810 | 270.9 | 33.9 |

z0's identified DC gain is ~23% higher than z1/z2's (39.25 vs ~31.8), and
its dead time is the longest of the three (52.8 s vs 43.5/33.9 s — 21% and
56% longer respectively). `tau` is essentially identical across zones
(263.8–270.9 s), so this is not a slower/faster thermal-mass story — it is
specifically a **higher steady-state gain and longer dead time**. A higher
`k_dc` means the *same* residual disturbance (leftover stored rate, or
coupling heat arriving from below) produces a proportionally larger
temperature excursion in z0 than the identical disturbance would in z1/z2.
The longer dead time also means the terminal ease-off window
(`ease_off_window_mult × model_dead_time_s` = 105.6 s for z0 at the board
default 2.0×, vs 87.0 s for z1 and 67.8 s for z2) has more distance to
absorb the same stored rate before the plant's own delay lets it show up as
temperature. **This is architecture-level (the plant itself, not a control
bug) and cannot be "fixed" by re-tuning a shared constant — any intervention
has to either scale by `k_dc`/dead-time per zone or accept the asymmetry.**
Not directly measurable as a standalone effect size from tracking captures
alone (it is a property of the identified model, not something with its own
independent tracking-error signature) — flagged as a contributing/amplifying
factor consistent with (a) and (b), not a fourth independent mechanism with
its own separately measurable magnitude.

### (d) Feedforward climb-term decay at dwell entry — NOT RE-PROPOSED, per closed history

`PID_EXPANSION_PLAN.md` §4 already reverted this exact idea
(`b8b192d`+`a77db88`, reverted `f9d8445`): holding climb duty into the dwell
measured worse on every zone in both dwells and tripped thermal guard 2 on
the second dwell. Nothing in this pass's data contradicts that; the finding
here (z0's own duty is *already crashed* to near-zero by the time it
overshoots, §1) is if anything additional evidence against it — there is no
climb duty left on z0 to decay by the time the overshoot is visible, so
decaying it more slowly would not touch this mechanism at all. Not
re-proposed.

### (e) Integral floor / wind-up through the ramp — NOT IMPLICATED

The floor is `-ff_hold` (steady-state only, per the already-adopted,
measured-best-of-four design, §4/§1637). Nothing in the duty trajectories
here shows a floored-integral signature (a duty pinned at a hard floor
through the ramp, then a slow unwind) — z0's duty is falling smoothly
through the transition and crashes past floor toward zero, the opposite
shape. Not pursued further; no evidence pointed at it and the mechanism it
would predict (a *slow*, floor-release-shaped duty climb after the
transition) is not what's observed (a fast crash, §1).

### (f) Ease-off window sizing (`ease_off_window_mult`) — CONSISTENT WITH THE CLOSED "2.0 stays" VERDICT, not a basis to reopen it

Regressing out the ramp-rate effect from (a) and comparing residuals by arm
(`easeoff_ab_20260904_2p0_run1..3` vs `_3p0_run1..3`, n=3 each, nested
inside the n=10 pool above): mean residual at mult=2.0 (pooled with
`ab_new`/`p7_newmatrix_http`, n=7) is **+0.10 °C**, at mult=3.0 (n=3) is
**−0.22 °C** — a ~0.32 °C shift in the direction a bigger window should
produce, but on n=3 vs n=7 and well inside the noise this project already
called "INDISTINGUISHABLE" for this knob (`easeoff_ab_20260904_report.md`,
corroborated by the synthesis report). **This is not new evidence to reopen
`ease_off_window_mult` globally — it is fully consistent with the standing
verdict** (a small, real-but-unresolvable-at-this-n effect in the expected
direction). Recorded because the ranked proposal below is a *different*,
z0-only knob, and this result is the reason a naive "just bump mult
everywhere again" is explicitly not being proposed.

## 3. What this data cannot settle

- **Whether coupling injection (2b) sets a floor under z0's overshoot
  independent of ramp rate.** The within-era peer-duty range is too narrow
  (z2: 0.74–0.91) to separate from noise. Settling this needs two firings at
  matched ramp rate with a deliberately different z1/z2 hold-duty level at
  the transition — not obtainable from existing captures, and not something
  ramp-rate variance alone (which is what this dataset has) can produce.
- **Whether the >62 °C coupled-hold-infeasible regime changes any of this.**
  Per the synthesis report, no capture in `logs/coupling/` ever reaches
  above 60 °C — every number above, like every number in the synthesis
  report, is a ≤60 °C statement.
- **The z0-only ease-off-window residual (2f) at anything better than n=3
  vs n=7 with an uncontrolled ramp rate.** The regression-adjusted comparison
  above is suggestive, not conclusive.
- **Precise attribution of variance between (a) ramp rate and (c) the
  k_dc/dead-time amplification factor** — (c) is a property of the
  identified model, constant across all captures at a given zone, so it
  cannot show up as *within-zone* variance; it can only be argued from the
  cross-zone comparison (z0's k_dc/dead-time vs z1/z2's), which this report
  does, but that comparison has n=1 per zone (one identified model each, not
  a distribution).

## 4. Ranked interventions

Against the 0.5 °C actionable bar and the per-zone noise floors (z0 0.116,
z1 0.077, z2 0.147 °C):

1. **Rate-into-dwell-scaled ease-off, z0 only — investigate first.**
   Mechanism (a) is the best-supported finding here (r=0.76, n=10,
   near-1:1 slope) and is a *residual* left over after the existing
   fixed-window taper — i.e., a fixed `2.0 × dead_time` window does not fully
   absorb z0's stored rate across the range of ramp rates this profile
   naturally produces. This is **not** the already-closed global
   `ease_off_window_mult` question (§2f, §3.6b) — it proposes scaling the
   *z0-specific* taper by the zone's own approach rate (or, more simply,
   trialling a larger window on z0 alone while leaving z1/z2 at the board
   default 2.0×, since z1/z2 are not the problem and any change to their
   windows would be untested drift). Expected effect: if even half the
   ramp-rate-driven residual is absorbed, z0's overshoot would move from
   ~2.2 °C toward ~1.4–1.6 °C — at or below z1's 1.45 °C, clearing the
   0.5 °C bar with several times the margin and roughly an order of
   magnitude above every zone's noise floor. **Exact proposed A/B:**
   - **Parameter:** a new *per-zone* ease-off window override for z0 only
     (not the existing global `ease_off_window_mult`, which stays at its
     validated 2.0 for z1/z2) — concretely, trial applying
     `ease_off_window_mult = 3.5` to zone 0's own window
     (`105.6 s → 184.8 s`) while z1/z2 keep 2.0.  (3.5 chosen as a step
     past the already-tested 3.0 that did not clearly separate from noise —
     large enough to produce a bigger, more measurable effect if the
     mechanism is real, without requiring new firmware: this is the same
     `ease_off_window_mult` knob and mechanism, applied to one zone via
     preset, not a new code path.)
   - **Arms:** 2 (A: current board default per-zone, z0 at 2.0×; B: z0 at
     3.5×, z1/z2 unchanged at 2.0×), 3 runs per arm — matching the existing
     ease-off campaign's own n and matched-pair discipline
     (`easeoff_ab_20260904_report.md`), so the result is directly comparable
     to the already-closed global campaign.
   - **Pre-flight reachability proof (§3.6b, mandatory before kiln time):**
     before starting either arm's firing, apply the arm's preset, start the
     profile, and sample `GET /api/control`'s zone-0 `bd_ff_rate_posttaper`
     a few times live once the final ramp segment is under way, then switch
     arms and resample. **`bd_ff_rate_posttaper` is the field that proves
     the mechanism actually engaged**: it is the post-taper commanded rate
     zone_taper_climb_rate() produces, so a genuine window-size change must
     show a *later* onset of tapering (the post-taper value should still
     equal the pre-taper rate for longer into the ramp under the 3.5× arm
     before it starts differing from `bd_ff_rate_pretaper`) — reading back
     the preset's own `ease_off_window_mult`/override field is not enough,
     per the exact failure mode §3.6b named for the fuzzy campaign. Capture
     both `bd_ff_rate_pretaper` and `bd_ff_rate_posttaper` for the whole
     campaign (`--capture-control-bd`, the CLI default) so the proof survives
     in the log per §3.6b's standing requirement, and run
     `bd_reachability_check.py` against a matched pair before trusting any
     `pid_ab_compare` result from it.
2. **Capture `bd_coupling_correction` and `bd_ff_hold` for zone 0 on any
   future firing of this profile, whether or not a dedicated campaign is
   run.** This is not a campaign proposal by itself — it's the fix for §3's
   biggest limitation (hypothesis (b) cannot be measured from existing data
   because none of the 27 captures carry per-zone `bd_*` breakdown fields).
   The next time this profile fires for any reason, capturing these two
   fields turns "consistent across eras, inconclusive within an era" into an
   answerable question at zero extra kiln time.
3. **Do not investigate the z0 k_dc/dead-time model mismatch (2c) as a
   standalone campaign.** It is a property of the already-identified model,
   not something a new firing would re-measure differently, and there is no
   proposed fix cheaper or better-targeted than intervention 1 (a
   rate-scaled taper implicitly compensates for a longer dead time and
   higher gain, since both push toward "z0 needs a longer taper" already).
   Worth a one-line note in the plan doc, not kiln time on its own.
4. **Do not attempt a cross-era coupling-magnitude A/B (matched ramp rate,
   varied peer duty) as currently scoped.** It would answer §3's first open
   question cleanly, but designing a firing that holds z0's ramp rate fixed
   while deliberately varying z1/z2's duty is a materially bigger and more
   novel kiln-time ask than intervention 1, and intervention 1 is likely to
   move the number enough on its own to reduce how much this question
   matters in practice. Revisit only if intervention 1 under-performs its
   expected effect size.

## Files

- This report: `logs/coupling/z0_dwell_overshoot_mechanism_20260904_report.md`
- Linked from `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §3.6d.
- Builds on `logs/coupling/tracking_synthesis_20260904_report.md` (§3.6c) and
  `PID_EXPANSION_PLAN.md` §3.1 (ease-off), §3.2 (coupling matrix), §3.6b
  (reachability audit / `bd_*` pre-flight requirement), §4 (rejected climb-decay
  and integral-floor-at-total-ff approaches).
- Source captures: the same current-matrix/ease-off subset of
  `logs/coupling/*.jsonl` used by the synthesis report (`ab_new_1..3`,
  `p7_newmatrix_http`, `p7_newmatrix2_http`, `easeoff_ab_20260904_2p0/3p0_run1..3`)
  plus the old-matrix pool for the cross-era comparison in §2b — none
  modified, moved, or deleted. `fuzzy_ab_20260904d_s50_run1.jsonl` (live
  campaign) excluded throughout.
- Per-zone identified model figures (§2c) read from
  `tools/PcTools/config_presets/easeoff_ab_2p0_20260903.json` (current
  board's live zone config snapshot, 2026-09-03) — not modified.
- Scratch analysis script used to produce §1/§2 numbers was run from a temp
  scratchpad outside the repo and is not checked in, same discipline as the
  synthesis report; every number is reproducible from
  `kilnctrl.http_capture_log.parse_http_capture_jsonl()` /
  `kilnctrl.log_analysis.poll_row_from_exec_body()` on the files listed
  above (ramp→dwell transition = first `dwelling: False→True` in the final
  segment; ramp rate = slope over the 90 s preceding it; peak = max
  `actual_c - target_c` within 400 s after it).
