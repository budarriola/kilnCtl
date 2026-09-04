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

---

# Independent verification and corrections (2026-09-04, second pass)

All §1/§2 numbers recomputed from the raw captures with an independently
written transition-finder (same stated definitions: first `dwelling`
False→True in the final segment; ramp rate = z0 slope over the preceding
90 s; peak = max `actual_c − target_c` within 400 s after).

## W1. Reproduced exactly

§1's table comes back **identical to two decimals** — ramp rates
1.607/1.564/1.957/1.869, peaks 2.21/2.27/2.32/2.55, times to peak
+94/+89/+100/+107 s. §2a's fit reproduces to three decimals:

```
peak_overshoot_c = 0.434 + 1.006 × ramp_rate      n=10, r = 0.760
```

§2f's residuals reproduce exactly: mult 2.0 pool **+0.096 °C** (n=7),
mult 3.0 **−0.224 °C** (n=3). §2c's model figures match the preset. §2b's
row sums (49.04 / 36.45 / 20.75) and the synthesis's column sums (22.63 /
39.74 / 43.87) are both arithmetically correct off the same live matrix, and
**this report's row-sum framing is the correct one for this question** —
confirmed against `adaptive_tune_model.c:187`, `rise_obs[i] = Σ_j
coupling_coeff[i][j]·duty[j]`, so row = affected zone (heat received),
column = stepped zone (influence exerted).

## W2. Leverage and confound analysis of the r=0.76 fit

**The correlation is solid. The coefficients are not.**

- **Leave-one-out (10 refits):** r stays in **0.732–0.803** — no single
  capture drives it. Permutation test (200k shuffles): **two-sided
  p = 0.013**.
- **Ambient is not the confound.** corr(overshoot, ambient) = −0.07;
  corr(ramp rate, ambient) = +0.37; **partial corr(overshoot, ramp rate |
  ambient) = +0.85** — controlling for ambient *strengthens* it.
- **Campaign/profile variant is not the confound either**, despite the
  stock-p7 captures clustering high on both axes and the stabilised-p7
  ease-off captures low. Re-fitting on arm-mean-centred data (campaign
  effect removed entirely) gives **r = 0.768, slope 0.876** — essentially
  unchanged.
- **Target temperature is not a confound**: all ten points are 60 °C.
- **But slope and intercept are fragile.** Dropping the single lowest and
  single highest ramp rate (n=8) moves the fit to **slope 1.57, intercept
  −0.50**; dropping two at each end (n=6) gives **slope 0.79, intercept
  +0.88, r = 0.46**. Across those trims slope ranges **0.47–1.77** and
  intercept **−0.99 to +1.77**. **§2a's "near-1:1 slope" and the implied
  0.44 °C floor carry no weight and should not be quoted as mechanism
  evidence** — only the sign and the existence of the relationship survive.
- **Partly kinematic, which cuts both ways.** A slope of ~1 °C per °C/min is
  ~1.0 min of coasting, and z0's identified dead time is 52.8 s. A plant
  arriving with stored rate *must* show roughly this slope; the fit is
  therefore close to a restatement of §2c's dead time rather than
  independent evidence for a control-side remedy.

## W3. THE DESIGN OBJECTION — intervention 1 should not be run as written

Three problems, in descending order of seriousness.

1. **The one direct test of the proposed actuator is null.** Intervention 1
   assumes a longer ease-off window reduces the rate z0 arrives with. The
   dataset already contains that test: the 3.0× arm's mean pre-transition
   ramp rate is **1.580 °C/min** versus the 2.0× arm's **1.459 °C/min** —
   *higher*, not lower. Whatever the taper does, it did not lower arrival
   rate when it was lengthened by 50%. **Proposing 3.5× is extrapolating a
   null result one step further.** Either add a mechanism check that the
   window actually changes arrival rate, or drop the ramp-rate rationale and
   justify 3.5× on the (weaker, already-INDISTINGUISHABLE) residual in §2f.
2. **The power arithmetic mixes two profiles.** Within-config run-to-run SD
   of z0's final-segment overshoot is strongly profile-dependent:
   **0.055 (stock p7, `ab_new` n=3), 0.035 (stock p7, `noise_floor_p7d`
   n=3), 0.290 (stock p7, `ab_old` n=3)** versus **0.337 (`easeoff` 2.0×)
   and 0.535 (`easeoff` 3.0×)** on the stabilised variant. Pooled SD across
   all five triplets = **0.312 °C**. At that SD, **2 arms × 3 runs has ≈47%
   power** to detect the proposed 0.7 °C change at α=0.05 — a coin flip.
   Worse, the expected effect "2.2 → 1.4–1.6" takes its **baseline from the
   stock-p7 captures (2.18–2.25)** while the proposed campaign copies the
   **ease-off campaign's stabilised profile, whose 2.0× baseline is 1.93**;
   as designed the arms would be chasing a ~0.4 °C gap, not 0.7.
   **Fix: run both arms on the stock profile 7** (`ab_new` configuration,
   SD 0.04–0.06 — n=3/arm then gives >99% power), **or keep the stabilised
   profile and raise to n=5 per arm.** Do not run 3×3 on the stabilised
   profile.
3. **The mandatory pre-flight probe, as specified, will not prove
   anything.** Checked against the live `fuzzy_ab_20260904d_s50_run1.jsonl`
   capture (the first with `bd_*` fields): `bd_ff_rate_pretaper` and
   `bd_ff_rate_posttaper` are **equal on 96.5% of zone-samples** (1393 of
   1443) and both zero on 95%; they differ on only **50 samples**, in the
   short taper window at the end of a ramp. "Sample it a few times live once
   the final ramp segment is under way" will almost certainly land where
   pre == post and prove nothing either way. **Rewrite the proof as: capture
   continuously (`--capture-control-bd`), then compare the *onset time* of
   the first pre≠post sample relative to dwell entry between arms** — the
   3.5× arm must show it starting ~79 s earlier.

## W4. The ≥3-zone rule question is a misreading — the rule does apply

The proposal (and the high-temperature proposal) treat the confidence bar as
"≥3 zones must move the same direction," which a z0-only intervention could
never satisfy. **`CONSISTENT_PATTERN_MIN_KEYS = 3` in `pid_ab_compare.py`
counts distinguishable *keys*, where a key is (zone, metric, segment) — not
zones** (the docstring's rationale is only "so a two-zone coincidence cannot
pass as a pattern"). A z0-only change can and should clear it inside zone 0
alone: overshoot, offset and normalised IAE at the final segment are three
keys. **Correct decision rule for this campaign: z0's own ≥3 same-direction
keys as the primary endpoint, with z1 and z2 as pre-registered negative
controls that must NOT move** — which is a strictly stronger design than the
three-zone reading, because a change leaking into untreated zones would
falsify reachability.

## W5. Intervention 2 is already satisfied

§4.2 asks for `bd_coupling_correction` / `bd_ff_hold` on the next firing.
**The live `fuzzy_ab_20260904d_s50_run1.jsonl` already carries all twelve
`bd_*` fields per zone**, `bd_coupling_correction` populated on 479/479
control rows (z0 range −0.161…+0.279). Hypothesis (b) becomes answerable
from that campaign's completed captures at zero additional kiln time —
**re-scope §4.2 from "capture next time" to "analyse `fuzzy_ab_20260904d`
when it completes."**

## W6. Verdict

**Mechanism (a) is real** — the correlation survives leverage deletion,
ambient, and campaign-variant controls, at p=0.013. **The proposed A/B is
worth kiln time, but NOT as designed.** Before it runs: switch to the stock
profile 7 (or n=5/arm), rewrite the pre-flight proof as a taper-onset
comparison, adopt the key-based (not zone-based) decision rule with z1/z2 as
negative controls, and either justify 3.5× against the null arrival-rate
result in W3.1 or state plainly that the mechanism link is assumed, not
shown. §4.3 and §4.4's "do not run" verdicts are sound as written.

---

# Third pass: intervention 1 WITHDRAWN, not merely redesigned (2026-09-04)

W3.1 flagged the null actuator result but W6 still called the A/B "worth
kiln time... NOT as designed," leaving open whether a corrected design would
be worth running. This pass re-checks the actuator test directly against
the raw captures (not just the two arm means) and reads the taper
implementation to see whether the null result is noise or mechanism — the
distinction that decides withdrawal vs. redesign.

## X1. The null is not a fluke of the two-mean comparison

Recomputed ramp rate and peak overshoot per run, independently of the §2a/W1
pooled fit, from `easeoff_ab_20260904_2p0_run{1,2,3}.jsonl` and
`_3p0_run{1,2,3}.jsonl` directly:

| arm | run1 | run2 | run3 | mean | within-arm SD |
|---|---|---|---|---|---|
| 2.0× ramp rate (°C/min) | 0.88 | 1.77 | 1.78 | 1.48 | **0.52** |
| 3.0× ramp rate (°C/min) | 1.58 | 1.44 | 1.89 | 1.64 | **0.23** |

(Matches W3.1's arm means, 1.459/1.580, to within rounding from a
independently-chosen 90 s baseline sample.) **The 2.0×→3.0× arm-mean gap
(+0.16 to +0.18 °C/min, wrong sign) is smaller than the 2.0× arm's own
within-arm SD (0.52)** — run1 alone (0.88) spans nearly the entire gap
between the two arm means by itself. There is no reading of this n=3-per-arm
data, however it's sliced, that shows 3.0× lowering arrival ramp rate; the
noise on one arm is larger than the effect being chased on either.

## X2. Why the null is mechanistic, not just underpowered

`profile_executor_feedforward.c:247-271` (`zone_taper_climb_rate()`) shows
what `ease_off_window_mult` actually does: it scales the **feedforward
climb-rate command** (`rate_c_per_s`, the term derived from the segment's
programmed °C/s and used to size `ff_climb`) down linearly once the
remaining distance-to-target falls inside `mult × ff_dead_time_s`. It does
**not** touch the PID P/I/D terms, the coupling-correction term, or the
plant's actual thermal state — it only pre-emptively backs off *one input*
to the total commanded duty, earlier, the larger `mult` is.

§1 already established that z0's own **duty** (not just its feedforward
component) has crashed to 0.05–0.10 by the time the overshoot peaks, and is
already down to 0.23–0.27 at the transition itself. By the point the
"90 s-before-transition" window used to measure "ramp rate" is being
sampled, the feedforward climb term the taper acts on is already a small
fraction of a duty that is itself small and falling — there is little climb
duty left standing for a wider window to remove. The measured plant
temperature slope over that interval is dominated by stored heat and
neighbour-coupling injection (§2b: z2 duty 0.56–0.91 at the same instant),
neither of which `zone_taper_climb_rate()` touches at all. **A longer
`ease_off_window_mult` widens *when* the FF term starts tapering; it was
never mechanistically wired to reduce the arrival rate the plant actually
exhibits**, which is set by dynamics the knob doesn't reach. X1's null
result is therefore the expected outcome of reading the code, not an
underpowered fluke that a bigger n would flip.

## X3. Verdict: WITHDRAWN

**§3.6d's z0-only ease-off-window A/B (2.0× vs 3.5×) is withdrawn, not
redesigned.** The premise — "a wider taper window lowers z0's arrival ramp
rate, which lowers its overshoot" — fails at the first link: the knob does
not act on any of the quantities that set arrival rate (stored plant heat,
coupling injection, PID feedback), only on a feedforward term already mostly
spent by the time it would matter. Blockers 2 and 3 (power, pre-flight
probe) are moot for this specific campaign — there is nothing to power a
test of, since the one thing the campaign would need to show (a rate
reduction) has no mechanism to produce it. They are, however, kept below as
reference for the taper-onset metric and n/power figures, because both are
reusable for other work on this same knob (see the firmware note).

Because this was reached from a code-level read of the actuator plus
direct-per-run data, not from the pooled n=10 fit, it holds regardless of
which of W2's fragile slope/intercept readings of the ramp-rate↔overshoot
correlation is used — even a strong, well-powered version of that
correlation does not help an intervention that cannot move the independent
variable.

## X4. Blockers 2 and 3, resolved for the record (not being spent on this campaign)

**Power (blocker 2).** At the pooled within-config SD of 0.312 °C (stock and
stabilised p7 combined) and a two-sided α=0.05 test for a 0.7 °C shift:
n=3/arm ≈ 47% power (as W3.2 found); solving for 80% power at that SD needs
**n ≈ 6/arm** (two-sample t-test, δ=0.7, SD=0.312 → n≈4/arm at 80%, but the
review's own quoted 47%-at-n=3 implies the effective ratio is worse than a
clean two-sample calc — using the conservative reading, budget **n=5–6/arm**
at the stabilised-profile SD). At the stock-p7 SD (0.04–0.06), n=3/arm is
already >99% power. Kiln-hours: each z0 dwell-entry run in this pool takes
~35–45 min of firing time end-to-end (ramp segments + final dwell +
cooldown observation, per the existing `easeoff_ab_20260904` queue log); two
arms × 3 runs stock-p7 ≈ **3.5–4.5 hours total**; two arms × 5–6 runs
stabilised-p7 ≈ **12–18 hours total**. Stock p7 is cheaper AND better
powered — the review's preferred fix — but moot here since there's no
detectable ramp-rate delta for a well-powered n to detect.

**Reachability probe (blocker 3).** Correct metric: **taper onset time**,
defined as the first sample (per zone, per ramp segment approaching a
target) where `bd_ff_rate_pretaper != bd_ff_rate_posttaper` (float
inequality, not an epsilon — the firmware taper is off, i.e. `posttaper ==
pretaper` bit-for-bit, until `dist_s < window_s`), reported as elapsed time
into that segment. Verified computable from a real, on-disk capture:
`fuzzy_ab_20260904d_s50_run1.jsonl` (571 samples with `control.zones[]`
populated) shows zone 0's first pre≠post sample at **t=21.0 s** into segment
0 (`bd_ff_rate_pretaper=0.08333`, `bd_ff_rate_posttaper=0.07965`) — the
fields exist, differ when expected, and are cleanly diffable. **Comparison
between arms:** for a matched ramp segment (same target, same programmed
rate) run under each `ease_off_window_mult`, onset time should occur at
`segment_duration − window_s` where `window_s = mult × ff_dead_time_s`
(105.6 s for z0 at 2.0×, 184.8 s at 3.5×) — i.e. the 3.5× arm's onset should
land **79.2 s earlier** in the segment than the 2.0×arm's, a large,
easy-to-see difference against typical segment lengths of several hundred
seconds. This is a real, mechanistically-grounded pre-flight check (unlike
the rejected pretaper/posttaper *value* comparison) — reusable if the
per-zone `ease_off_window_mult` knob is ever used for something the taper
mechanism actually reaches.

## X5. Given the lever is dead, what's next for z0's overshoot — ranked shortlist

Working from the mechanism evidence already in hand (§1–§2, no new firings
needed for the top two):

1. **Slow z0's own commanded ramp rate on approach to a dwell — a
   profile-level fix, not a controller fix.** §2a's correlation, sign-robust
   under every control checked in W2 (ambient, leave-one-out, campaign
   centring; only the slope/intercept are fragile), says overshoot tracks
   the rate z0 is asked to arrive at. Unlike the taper knob, a slower
   profile ramp segment for z0's last approach to target directly reduces
   the quantity the correlation is about, with no dependency on any
   feedforward mechanism reaching it. Cost: a profile edit (no firmware
   change) plus a validation campaign — likely the same stock-p7, n=3-5/arm
   design already costed above (~4-6 hours), because the actuator here is
   the ramp-rate *input* itself, which is trivially and verifiably
   controllable, unlike the taper window. Best-supported, cheapest,
   least novel. Recommended first. **Revised cost, 2026-09-04 (item 2
   below run):** NOT a one-line config change — see item 2's finding. The
   ramp rate is a single scalar shared by every zone in the segment's
   `zone_mask`; there is no per-zone rate field to edit. Delivering a
   z0-only ramp rate requires firmware work of the same rough shape as
   the (already-shipped) `ease_off_window_mult` per-zone override:
   extend `profile_segment_t` (or add a per-zone multiplier next to it),
   thread it through `profiles_http.c`/`profiles_edit_http.c` JSON and
   `profile_feasibility.c`'s ceiling check, and — the larger piece —
   split the executor's single `s_exec.target_c` scalar into a per-zone
   value, since ramp-lock (`PROFILE_EXECUTOR_RAMP_LOCK_BAND_C`, 25 °C)
   currently compares every zone against that one shared setpoint
   (`profile_executor_internal.h:598`, `:922-933`). Cost: a firmware
   change touching the executor's core data model, not a preset edit —
   plan for it as new capability, not a quick win, and validate the
   *design* (does per-zone target_c break ramp-lock's "slowest zone sets
   the pace" semantics?) before costing a campaign.
2. **Zero-kiln-time check: does the profile even need to approach z0's dwell
   at the rate it currently does, or is the rate an artifact of a
   shared/unzoned ramp-rate parameter?** Before spending kiln time on (1),
   check whether the current profile already ties z0's ramp rate to
   z1/z2's for no physical reason (e.g. one shared `ramp_rate_c_per_min`
   across all zones) — if so, (1) may be a one-line profile change rather
   than new tooling. This is a documentation/config read, not a campaign.

   **Answered, 2026-09-04, zero kiln time spent — the premise is TRUE and
   item (1) is NOT a config change.** `profile_segment_t` (`docs/PROFILES.md`
   lines 32-36, mirrored in `firmware/KilnFW/App/drivers/profiles_http.h`)
   carries exactly one `ramp_c_per_hr` per segment, applied against
   "every zone in `zone_mask`" (`PROFILES.md:159-176`) — there is no
   per-zone rate array in the format. The executor confirms this is not
   just a format gap but the run-time model too: `s_exec.target_c` is a
   single `float`, documented in-line as "shared setpoint, stepped
   incrementally each tick" and "one ramp across every zone in a run,
   TODO.md 6A.5" (`profile_executor_internal.h:598,256`). Every active
   zone's PID setpoint is this one value each tick
   (`new_target = target_c ± ramp_c_per_hr * dt_s/3600`, `PROFILES.md:247`),
   and ramp-lock explicitly compares each zone's own reading against that
   *same* shared setpoint (`PROFILES.md:274-279`, "the slowest zone sets
   the pace"). The only per-zone differentiation that exists today is
   `zone_taper_climb_rate()`'s feedforward taper (§X6 above) — and that
   taper is scoped, by its own doc comment, to the feedforward term only,
   "never `s_exec.target_c`" (`profile_executor_pid_tick.c:62-64`); it does
   not touch the commanded setpoint trajectory this shortlist item is
   about, and it is the mechanism already shown (§X2-X3) not to reach
   arrival ramp rate. So z0's effective commanded trajectory is
   genuinely identical to its peers' — nothing today lets a profile or
   the executor give z0 a slower approach without a code change. Item
   (1)'s cost is revised above.

   **Overshoot reduction estimate for a slower z0 approach (range, not a
   point).** Using the working fit `peak_overshoot_c ≈ 0.44 + 1.00 ×
   ramp_rate_c_per_min` with z0's observed 1.56-2.32 °C/min approach
   rates (§1) and the W2 finding that slope/intercept are fragile
   (0.47-1.77 slope, -0.99 to +1.77 intercept under trimming) while the
   *direction* is robust: cutting the commanded approach rate by roughly
   1 °C/min (e.g. ~1.9 → ~0.9 °C/min, a plausible profile edit) predicts,
   at the central fit, an overshoot drop of ~1.0 °C (from ~2.2-2.5 °C
   toward ~1.2-1.5 °C) — clearing the 0.5 °C bar with margin similar to
   the withdrawn taper proposal's target. Under the fragile slope bounds
   alone, the same 1 °C/min cut spans roughly **0.5-1.8 °C** of reduction;
   either end is far above z0's 0.116 °C noise floor, so the effect
   direction and rough magnitude are trustworthy even though the exact
   number is not. A campaign is still needed to pin the real slope on
   this actuator (unlike the taper knob, this one is a direct, verifiable
   input — no reachability question), matching item (1)'s already-costed
   ~4-6 hour design.
3. **Coupling-floor campaign (matched ramp rate, deliberately varied peer
   duty)** — §2b's original open question, previously ranked #4 as "bigger,
   more novel kiln-time ask." Still true, and still worth deferring:
   revisit only if (1) under-delivers, since (1) is cheaper and already
   evidenced.
4. **Firmware-level per-zone feedforward compensation scaled by z0's own
   `model_k_dc`/`model_dead_time_s`** (§2c) — architecture-level, requires a
   firmware change (not a preset), and no firing would validate the
   *design* before it exists in code. Lowest priority; a note in the plan
   doc, not kiln time, until (1) and (3) are exhausted.

## X6. Is the per-zone `ease_off_window_mult` firmware work still worth keeping?

**Yes, as a general capability — its motivating experiment is withdrawn, not
the mechanism it implements.** The per-zone override (`ZONES_CFG_VERSION`
16→17, `zone_taper_climb_rate()` reading zone `zi`'s own value) is a clean,
narrowly-scoped piece of config plumbing that does exactly what it says:
lets one zone's taper window differ from its siblings'. That's independently
useful — e.g. shortlist item 1 above still benefits from being able to tune
z0 in isolation without touching z1/z2, and any future work on z0's dead
time/gain outlier status (§2c) will want the same isolation. What's
withdrawn is only the specific claim that *this* knob, at *this* window
size, fixes dwell-entry overshoot by lowering arrival rate — X2 shows the
knob was never wired to reach that quantity. The owner should know the
capability shipped for an experiment that turned out not to need it, not
that the capability itself was a mistake.
