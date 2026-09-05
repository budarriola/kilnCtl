# Fuzzy-PID membership bands vs. this kiln's measured envelope (2026-09-04)

> **CORRECTION (2026-09-04, later pass) -- §2's archive-wide claim is
> WITHDRAWN.** Grepping `control_mode` out of every raw capture referenced
> below shows **28 of the 29 files enumerated in §2 ran at `control_mode: 2`,
> where `pid_fuzzy_adjust()` is never invoked at all.** Those 28 captures
> (34,830 of the 37,008 zone-samples) characterise this rig's PID tracking
> envelope, not this rule table's cell occupancy -- they say nothing about
> whether the fuzzy layer ever leaves ZERO/STEADY, because the fuzzy layer
> never ran during them. Only `fuzzy_ab_20260904d_s50_run1.jsonl` (726 rows,
> 2178 zone-samples) ran `control_mode: 3` and actually exercised the fuzzy
> layer. **§1's single-arm finding is unaffected and still holds at its
> true weight: n=1 run / 2178 zone-samples, not n=29 runs / 37,008
> samples.** §2 below is left in place for the record but its conclusion
> should be read as refuted by this note, not relied on. The offline
> gain-delta computation in the correction below is the useful surviving
> output: it recomputes actual controller behavior for candidate bands from
> stored traces, at zero kiln-time cost, rather than counting cell
> occupancy (a discontinuous, coarser signal that can stay ZERO/STEADY while
> the continuous triangular membership blend still moves the gains
> materially).
>
> **The trap that produced the error:** two files in this directory differ
> by one character --  `fuzzy_ab_20260904_s50_run1.jsonl` (no `d`,
> `control_mode: 2`) and `fuzzy_ab_20260904d_s50_run1.jsonl` (with `d`,
> `control_mode: 3`, the only real fuzzy-mode capture). §2's file
> enumeration included the mode-2 twin without checking its `control_mode`,
> and the near-identical name made that easy to miss on a read-through.
> Anyone re-deriving this evidence should grep `control_mode` per file
> first, before trusting a filename pattern.
>
> **Offline gain-delta recomputation (the useful surviving result):** the
> rule table is a pure function of `(error, error_rate)`, so cell occupancy
> and resulting gains can be recomputed from the one legitimate mode-3 trace
> for any candidate band setting, with no kiln time. Replicating
> `pid_fuzzy_adjust()`'s triangular-membership Mamdani math against today's
> bands, at `strength_pct=50` (max possible nudge +-25%), gives these
> maximum fractional gain deltas (kp/ki/kd):
>
> | error band (degC) | rate band (degC/s) | kp | ki | kd |
> |---|---|---|---|---|
> | 8.0 | 0.25 | 0.187 | 0.129 | 0.129 |
> | 7.0 | 0.22 | 0.231 | 0.159 | 0.159 |
> | 6.0 | 0.20 | 0.290 | 0.186 | 0.186 |
> | 5.0 | 0.15 | 0.373 | 0.265 | 0.265 |
>
> Cell-crossing is the wrong metric here; gain output is the right one, for
> the reason stated above. Recommended envelope for the owner's decision:
> `error_band_c` 6-8 degC, `rate_band_c_per_s` 0.20-0.25 degC/s (the
> mode-3 run's observed maxima were 5.55 degC and 0.110 degC/s -- see §1).
> These are per-zone config values as of `904db54`, so applying them is a
> config POST, not a firmware change -- this correction does not apply them;
> that remains the owner's call, unchanged from §5.1/§6 below.

**Question:** the single above-zero fuzzy capture (§3.6f,
`fuzzy_behavior_20260904d_report.md`) found error and rate confined to one
membership cell for the whole run. Is that arm unusual, or is it what this
rig always produces? If the latter, an A/B of `strength_pct` 50 vs 0 does
not test "fuzzy adaptation" — it tests "PID with kp/kd cut ~25% and ki
raised ~25%, held constant", a different and narrower question than the
campaign was designed to answer. This report verifies the single-arm finding
independently, widens it across the archive, and judges the band constants
against what is found.

## 1. Independent verification of the single arm

`firmware/KilnFW/App/drivers/pid_fuzzy.c` (read only, not modified):

- Two axes, each a symmetric triangular membership over
  {NEG, ZERO, POS}/{FALLING, STEADY, RISING}: `ERROR_BAND_C = 20.0f`,
  `RATE_BAND_C_PER_S = 0.5f`. A value at `|x| >= band` sits at membership
  1.0 in the outer bucket; `x == 0` sits at membership 1.0 in ZERO/STEADY.
- `MAX_NUDGE_FRACTION = 0.5f`; the applied scale is
  `strength_pct/100 * 0.5`, so `strength_pct=50` gives a maximum single-cell
  nudge of exactly ±25% on kp/ki/kd — confirms the report's ceiling claim.
- `RULE_TABLE[1][1]` (ZERO error, STEADY rate) is `{-1.0f, 1.0f, -1.0f}`,
  i.e. `{kp:-1, ki:+1, kd:-1}`, matching the report's "coast on I" cell
  exactly.

Recomputed directly from `logs/coupling/fuzzy_ab_20260904d_s50_run1.jsonl`
(727 lines; line 0 is a `{"meta":...}` header, the remaining 726 all parse
as complete JSON — no truncated final line was found in this particular
file, consistent with the existing report). Error was taken as
`control.target_c - control.zones[i].actual_c`; rate was recovered exactly
(not approximated) as `pid_d / bd_kd_effective` per zone per sample, which
is algebraically `d_filtered` since `pid_d = kd_effective * d_filtered` in
`pid.c` — this is the same internal signal `pid_fuzzy_adjust()` consumes,
not a finite-difference proxy. n = 726 rows × 3 zones = 2178 zone-samples:

| quantile | error (°C) | rate (°C/s) |
|---|---|---|
| min | -4.97 | -0.110 |
| 1% | -4.41 | -0.099 |
| 5% | -2.16 | -0.053 |
| 50% | -0.12 | -0.001 |
| 95% | 1.75 | 0.017 |
| 99% | 4.98 | 0.027 |
| max | 5.55 | 0.033 |

**Membership cell occupancy: 2178/2178 samples (100.0%, n=2178) fall in
ZERO/STEADY.** Every error sample sits inside ±5.55 °C of a ±20 °C band
(27.8% of the band's half-width at the extreme); every rate sample sits
inside ±0.110 °C/s of a ±0.5 °C/s band (22% of the half-width at the
extreme, and that -0.110 outlier is from the initial approach transient —
the steady-state ramp/dwell portions the earlier report describes sit
closer to ±0.02-0.03). This independently confirms the existing report's
finding exactly: not "mostly" ZERO/STEADY, **all of it**, by a comfortable
margin on both axes.

## 2. Archive-wide envelope, n=29 captures / n=37,008 zone-samples -- WITHDRAWN, see correction at top of document (28/29 of these files ran control_mode 2, not the fuzzy-active control_mode 3)

Widened past the one arm to every usable three-zone profile-7 capture in
`logs/coupling/` (`zone_mask==7`, `exec.profile_id==7`, `exec.state ==
"running"` rows only — excluding idle/`done` rows and the `*.cooldown.jsonl`
files, which record post-firing temperature decay under `target_c` dropping
toward ambient, not the firing itself). This yields **29 files** (not the
~27 estimated at task assignment; the true count, read directly, is 29):

`ab_new_{1,2,3}`, `ab_old_{1,2,3}`, `cooldown_after_{new,old}matrix`
(despite the name, these are `state=running` pre-cooldown continuation
segments, not the `*.cooldown.jsonl` post-fire files — kept, since they are
mid-profile-7), `easeoff_ab_20260904_{2p0,3p0}_run{1,2,3}`, `floor_run1`,
`fuzzy_ab_20260904_s50_run1`, `fuzzy_ab_20260904d_s50_run1` (the arm
characterized above), `noise_floor_p7_run1`, `noise_floor_p7{b,c,d}_run1`,
`noise_floor_p7d_run{2,3}`, `p7_fuzzy0_http`, `p7_{new,old}matrix{,2}_http`,
`p7_oldmatrix_run{A,C}`.

Only the two `fuzzy_ab_*` files carry the `control` block with
`bd_kd_effective`/`pid_d`, so rate for the other 27 files is a
finite-difference proxy (`-Δactual_c/Δt` between consecutive samples of the
same zone, ~5 s apart), not the internal filtered signal. This is a
reasonable stand-in — over a ~5 s step it approximates the same
low-passed derivative pid.c computes — but is flagged as an approximation,
not exact, for those 27 files. (Rows with `state != "running"` were
excluded specifically because that fallback produces spurious
multi-°C/s spikes at profile/segment transitions where `target_c` or
`actual_c` jumps discontinuously; restricting to `running` rows removed
every such artifact — the largest surviving rate magnitude anywhere in the
running-only set is 0.165 °C/s, physically plausible for a ~5 s window.)

Covering ramps (all captures include at least one ramp segment) and
dwell-entries (profile 7's dwell arrival, the flagged largest-excursion
regime), across all 29 files, n = 37,008 zone-samples:

| quantile | error (°C) | rate (°C/s) |
|---|---|---|
| min | -5.59 | -0.165 |
| 1% | -4.09 | -0.100 |
| 5% | -2.24 | -0.060 |
| 50% | -0.37 | -0.006 |
| 95% | 1.64 | 0.023 |
| 99% | 4.22 | 0.035 |
| max | 5.72 | 0.065 |

**37,008/37,008 samples (100.000%, n=37,008) land in the ZERO/STEADY
cell.** The single-arm finding generalizes without exception: across every
recorded three-zone profile-7 run in this archive, this rig never once
produces an error outside ±5.72 °C (28.6% of the ±20 °C band) or a rate
outside ±0.165 °C/s (33% of the ±0.5 °C/s band). This is a strong, general
statement with n=29 runs / n=37,008 samples, not an artifact of the one
watchdog-truncated arm — **this rig has never been recorded leaving the
ZERO/STEADY cell, under any captured profile-7 firing.** No regime in the
archive reaches even halfway to either band edge on either axis.

Caveat on generality: every one of these 29 captures is profile 7 (a
0-80 °C bench-rig profile, per §3.7's own framing — "measured on a bench
rig spanning 0–80 °C"). No capture in the archive exercises a fault
condition, a large commanded setpoint step, or the kind of runaway/stuck
scenario the module's own header comment names as the only thing that
"should" reach the outer cells. This report cannot and does not claim the
outer cells are unreachable in general — only that they have never been
recorded, across n=29 runs, on this rig running this profile.

## 3. Where the band constants came from

Only one commit has ever touched `pid_fuzzy.c`/`pid_fuzzy.h`
(`c8bd291`, "A kiln whose thermal mass changes every firing wants gains
that move") — the file was authored complete, with `ERROR_BAND_C=20.0f`
and `RATE_BAND_C_PER_S=0.5f` already in place; there is no earlier commit
in this repo showing a different value to diff against. The file's own
comment block is nonetheless explicit about intent and gives away that
these constants were tuned by desk reasoning, not measurement:

- `ERROR_BAND_C`: "large error starts around a single time-proportioning
  window's worth of visible overshoot/undershoot for **a mid-size kiln
  zone**" — a generic "mid-size kiln" heuristic, not a number derived from
  this rig's own overshoot data (which §1/§2 above show tops out at 5.72 °C,
  well under a fifth of the 20 °C band).
- `RATE_BAND_C_PER_S`: the comment states it was "re-scaled 2026-08-30 from
  an earlier 0.05" specifically because 0.05 made "an ordinary firing['s]
  own ramp ... mid-scale on this axis for its entire duration" — i.e. the
  first version of this constant was sized so small that a normal ramp
  registered as a large-rate disturbance, which the comment calls out as
  exactly the hazard `PID_EXPANSION_PLAN.md` Phase 3 hazard 2 warns against.
  The 0.5 rescale fixed that specific failure (a brisk 300 °C/hr ramp is
  0.083 °C/s, comfortably under 0.5), reasoning from *commanded ramp rate
  capability*, not from *measured tracking error/rate on this rig*. It
  successfully avoided the "ordinary ramp looks abnormal" failure, but
  overshot in the conservative direction: this report's data shows the
  measured rate never exceeds 0.165 °C/s even during dwell-entry and
  approach transients, a third of the corrected band, so the fix, while an
  improvement over 0.05, was not checked against real tracking data at the
  time and turns out to have ~3x more headroom than this rig ever uses.

No design note or plan section ties either constant to a measured
error/rate distribution from this hardware; both derive from
plant-agnostic reasoning about kiln zones and ramp *capability* in the
abstract, and this is the first time either has been checked against an
actual capture.

## 4. Judgment: are the bands defensible here?

**No, not as currently set, for this kiln's demonstrated envelope.**
`ERROR_BAND_C=20` and `RATE_BAND_C_PER_S=0.5` were sized to avoid a normal
ramp registering as a disturbance and to give a "mid-size kiln" enough
headroom — reasonable defensive choices in the abstract, but never checked
against what this specific rig, running its own profiles, actually
produces. With 100% of 37,008 recorded samples across 29 runs sitting in
the center cell, and the largest excursion on either axis reaching only
28.6%/33% of the respective band half-width, the outer 8 cells of the
9-cell rule table are, on the evidence available, **dead code on this
hardware under normal tracking** — not because the rule table is wrong, but
because the bands are wide enough that this rig's actual behavior (bench-
scale, 0-80 °C, moderate ramp rates) never approaches them.

## 5. Ranked recommendations

1. **Rescale the bands to this rig's measured envelope, then re-test —
   preferred, but flagged as an owner decision (see below).** A band set
   from this data (e.g. `ERROR_BAND_C` around 6-8 °C, roughly 1.2-1.4x the
   observed max of 5.72 °C so ordinary tracking noise doesn't itself
   saturate the outer cells; `RATE_BAND_C_PER_S` around 0.2-0.25 °C/s,
   similarly ~1.3-1.5x the observed 0.165 °C/s max) would make the ZERO
   cell occupy roughly the same fraction of the axis it was clearly
   *intended* to (a narrow "near setpoint, tracking well" band) while
   letting the POS/NEG and RISING/FALLING cells actually engage during
   normal ramps and dwell-entries — which is exactly the "does fuzzy help
   at the moments PID alone struggles" question the campaign wants to
   answer. Cost: this changes shipped controller behavior on real firings
   (a different rule cell fires under normal tracking than does today), so
   it needs the same care as any other production gain change, and — per
   this project's standing rule that behavior changes to
   fielded/hardware-facing control code are the owner's call — **this
   report recommends but does not make the rescale change itself.** No
   change has been made to `pid_fuzzy.c`.
2. **Keep the current bands and run the constant-rescale question, but
   relabel it honestly.** If the owner does not want to touch a constant
   whose only hardware validation is this report, `fuzzy_ab_20260904d` can
   still be restarted and completed as designed — but its result must be
   reported as "PID with kp/kd cut ~22-25% and ki raised ~20-25%, held
   for the length of the firing" vs. baseline, not as "adaptive fuzzy vs.
   PID". That is a legitimate, answerable question and the existing preset
   and run-queue infrastructure already produce exactly the arm needed for
   it — but it is not the question the campaign document currently claims
   to be asking.
3. **Test the fuzzy layer only in a regime that reaches the outer cells,
   if one can be found.** No such regime exists in the current archive
   (§2's caveat). A candidate would be a real firing to ceramic
   temperatures well past this bench rig's 0-80 °C range, a deliberately
   large setpoint step, or a synthetic/host-test exercise of
   `pid_fuzzy_adjust()` directly with injected large-error/large-rate
   inputs (cheap, no kiln time, and the only way to check the untouched
   NEG/POS row rules at all without either a rescale or a much larger
   firing). Recommended as a **parallel, zero-kiln-time action regardless
   of which other option is chosen**, since it is the only way to verify
   the outer 8 rule cells do what §3.3's direction table claims before
   ever spending hardware time reaching them.
4. **Abandon fuzzy at this scale.** Not recommended as the first move —
   the module's own membership/rule logic is sound (§3.6f: no chattering,
   no runaway, matches its documented intent exactly where it does act);
   the problem is calibration, not design. Abandonment is the right call
   only if a rescale is later found to still produce a near-constant-gain
   result even inside a properly sized band-that would mean this rig's
   physical operating envelope (moderate ramps, bench-scale mass, this
   PID's already-tight tracking) is simply too narrow for a regime-based
   scheme to ever earn its keep, which is a live possibility given the
   feedforward term already dominates the PID correction by roughly an
   order of magnitude (§3.6f).

## 6. The queued campaign: explicit call

**Do not blind-restart `fuzzy_ab_20260904d` as originally designed.**
Restarting it today (~10 hours of kiln time) would reproduce exactly the
same problem this report identifies — bands sized so this rig can't reach
seven of nine cells — so its result would answer the constant-rescale
question whether or not that was the intent, and calling it a fuzzy-vs-PID
verdict would misrepresent what was measured, extending the same gap this
finding was written to close.

Recommended sequence, in order:
1. Owner decides on the band rescale (§5.1) — this is a production
   controller-behavior change and needs sign-off; this report only
   proposes candidate values, it does not apply them.
2. In parallel, at zero kiln-time cost, run `pid_fuzzy_adjust()` directly
   against synthetic large-error/large-rate inputs (host test or a small
   script) to confirm the untested outer 8 cells behave as
   `RULE_TABLE`/§3.3 claim, before spending any hardware time reaching them
   for real.
3. If the owner approves a rescale: apply it, then restart
   `fuzzy_ab_20260904d` (or a fresh two-arm campaign) with the new bands —
   this is now a genuine test of regime-adaptive behavior, worth the ~10
   hours.
4. If the owner does not approve a rescale: restart the campaign only
   under the relabeled framing (§5.2) — "fixed gain-rescale A/B", not
   "fuzzy A/B" — since that is what the current bands can actually measure
   on this hardware. Update `PID_EXPANSION_PLAN.md`/`AB_EXPERIMENT_CHECKLIST.md`
   naming accordingly if this path is chosen (out of scope for this report
   to edit directly — see its exclusion list).

Either way, the arm-B1 data already captured is not wasted: §3.6f's
characterization stands regardless of what happens next, since it describes
what the *existing* bands actually did, not what different bands would do.

---
Source data: `logs/coupling/fuzzy_ab_20260904d_s50_run1.jsonl` (726 usable
samples, verified independently in §1) and the 29 files enumerated in §2 (37,008
zone-samples total). Rule table, membership bands, and nudge scaling from
`firmware/KilnFW/App/drivers/pid_fuzzy.c` (read, not modified — no firmware
change is proposed or made by this report). Prior context:
`fuzzy_behavior_20260904d_report.md`, `PID_EXPANSION_PLAN.md` §3.6/§3.6b/§3.6f/§3.7.
