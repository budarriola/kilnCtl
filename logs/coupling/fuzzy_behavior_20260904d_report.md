# Fuzzy-PID behavioral characterization, `fuzzy_ab_20260904d` arm B1 (single arm, descriptive only)

**This is not an A/B result.** Arm A1 (`fuzzy_ab_20260904d`, `strength_pct=0`)
never ran; the campaign died to a task-watchdog reset ~64 minutes into arm
B1's ~104-minute run and is currently stopped pending investigation elsewhere
(`PROJECT_STATUS.md`, out of scope here). `bd_reachability_check` needs two
arms and the completeness gate correctly refuses to pair this capture with
anything. **No statement below compares fuzzy-on tracking to fuzzy-off
tracking, or claims fuzzy is better or worse than baseline.** What follows is
a within-arm characterization of what the rule base actually did, using the
one capture on hand: `fuzzy_ab_20260904d_s50_run1.jsonl` (arm B1,
`fuzzy_ab_strength50_20260903`, strength 50, profile 7 with the prepended
40 °C / 45 min stabilization hold).

This is also the first hardware capture in the project's history with the
fuzzy layer verified actually engaging (`PID_EXPANSION_PLAN.md` §3.6b,
09910377): two earlier campaigns were structurally inert
(`control_mode: 2`, 89066869), so nothing about the rule base's real
behavior has been checked on hardware before this capture.

## 0. Capture facts

726 of 727 lines parse as complete JSON `control`/`exec` records (line 0 is
the `{"meta": ...}` header, read separately; no truncated/corrupt line was
found in this file — the last line closes cleanly, so nothing needed to be
discarded on that account). Wall-clock span: 63.8 minutes across the
recorded `t` values, consistent with "~64 minutes into a ~104-minute arm."
Sample interval ≈5.3 s. Base gains (from
`tools/PcTools/config_presets/fuzzy_ab_strength50_20260903.json`, the exact
preset this arm ran):

| zone | base kp | base ki | base kd |
|---|---|---|---|
| 0 | 0.0318 | 0.0001 | 0.8401 |
| 1 | 0.0485 | 0.0002 | 1.0548 |
| 2 | 0.0631 | 0.0002 | 1.0690 |

Segment structure observed in `exec`: segment 0 = the prepended 40 °C/45 min
hold (`segment_count=3`, `min_segment_index=1` in the capture's own meta
excludes this segment from the project's standard tracking metrics, same
convention `pid_ab_compare.py run` applies below); segment 1 = profile 7's
own ramp into a 45 °C dwell; segment 2 = the next ramp, target climbing past
57 °C when the capture ends — **`dwelling` never went `true` for segment 2**.
The board was mid-ramp, still short of its own next dwell, at the moment of
the reset.

## 1. What the rule base did, per zone, against its actual inputs

`firmware/KilnFW/App/drivers/pid_fuzzy.c` keys on two axes, each a
triangular membership over {NEG, ZERO, POS} / {FALLING, STEADY, RISING}:
`error_c` (band ±20 °C) and `error_rate_c_per_s` = `pid.c`'s
`d_filtered` = `-d(measured)/dt` (band ±0.5 °C/s — order-of-magnitude above
any commanded ramp rate this kiln uses, so a normal firing's *own* ramp
should sit near STEADY on this axis by design; see the constant's own
comment in `pid_fuzzy.c`). The 9-cell Mamdani table (`RULE_TABLE`) is then
weighted-averaged and scaled by `strength_pct/100 * 0.5` (max nudge
fraction), so at `strength_pct=50` the largest possible single-cell nudge is
±25%.

Across the whole captured stretch, error stayed inside roughly ±5.6 °C for
all three zones — well inside the ±20 °C error band — so membership sits
solidly in the **ZERO** bucket (`e_zero` dominant) for nearly the entire
capture, and the achieved ramp rates (a 5 °C climb over several hundred
seconds, ≈0.01-0.02 °C/s) sit solidly in **STEADY** on the rate axis, exactly
as the module's own design comment predicts. The ZERO/STEADY cell's rule is
`{kp: -1, ki: +1, kd: -1}` ("settled → coast on I"), and that is exactly the
signature all three zones show:

| zone | kp deviation | ki deviation | kd deviation |
|---|---|---|---|
| 0 | -25.0% .. -12.2% (mean -22.1%) | +10.0% .. +20.0% (mean +19.6%) | -24.9% .. -11.8% (mean -22.3%) |
| 1 | -25.0% .. -12.0% (mean -22.7%) | +10.0% .. +25.0% (mean +23.4%) | -25.0% .. -12.5% (mean -22.6%) |
| 2 | -25.0% .. -12.6% (mean -22.7%) | +10.0% .. +25.0% (mean +23.4%) | -24.9% .. -11.8% (mean -22.7%) |

kp and kd move down together, ki moves up, every sample, every zone — the
sign pattern the ZERO/STEADY rule prescribes, and the magnitude sits at or
near the ±25% ceiling that `strength_pct=50` allows, consistent with `e_zero`
membership sitting close to 1.0 for most of the run.

Whole-run Pearson correlation of `kp` deviation against instantaneous error
is weak (|r| ≤ 0.15, all three zones) — expected, *not* a red flag: with
error confined to a narrow slice of a ±20 °C band, the ZERO-cell rule
dominates almost everywhere and there simply isn't much variation in
membership to correlate against. The place the rule base's error-sensitivity
*does* show up is the one stretch of the capture with a genuinely larger
error: the initial approach to the 40 °C hold (segment 0, "ramp" rows before
`dwelling` went true), mean error +3.5 to +3.9 °C across zones. There,
`e_pos` membership rises materially and the observed kp deviation compresses
from the steady-state ~-22% to -15.8% .. -16.9% — exactly the direction
expected, since the POS/STEADY rule (`{kp:+1, ki:0, kd:0}`, "steady approach
→ push harder") is now mixing in and partially cancels the ZERO cell's
kp-down. This is the one place in this capture where the rule table's
error-axis behavior is directly checkable, and it matches the table's stated
intent (§3.3 direction table).

No sample in any zone was bit-identical to base gain (zero rows on the
inert/`control_mode=2` signature), corroborating the reachability finding in
09910377 with the full 726-sample series rather than the ~360-sample
mid-firing snapshot that commit was based on.

## 2. Where it acted, where it did not

- **Acted with near-maximal, near-constant magnitude**: the 40 °C
  stabilization hold (segment 0, `dwelling=true`, 512 of 726 samples, 71% of
  the capture) and both profile-7 dwell/ramp windows at low error. This is
  the regime where the ZERO/STEADY cell governs almost undiluted, and the
  fuzzy layer sat at essentially its full ±25% nudge on kp/ki/kd for the
  entire duration — not "engaged occasionally," but continuously active for
  most of the run.
- **Acted with reduced (but still nonzero) magnitude**: the initial approach
  ramp into the 40 °C hold (segment 0, `dwelling=false`, error ~3.5-3.9 °C)
  and both profile-ramp segments (segments 1 and 2), where kp deviation
  eased toward -22% to -23% (still large, but measurably less than the
  steady-state ceiling) as `e_pos`/`e_neg` membership picked up.
- **Did not act more strongly than at any point observed**: the layer never
  approached a qualitatively different regime (e.g. the large-error,
  attack/ease-off rules in row 0 or row 2 of `RULE_TABLE`) because error
  never approached the ±20 °C band edge in this capture. That is a property
  of this firing's tracking quality, not evidence the rule base can't reach
  those cells — it simply never had to.
- A fuzzy layer legitimately contributing little at zero error is not what
  happened here: because the ZERO/STEADY *rule itself* prescribes a
  substantial, non-neutral adjustment (unlike a fuzzy scheme where the
  center cell is a no-op), "near-zero error" in this design means "near-25%
  adjustment," not "near-zero adjustment." That is a design property worth
  flagging for the reader of this report, not a defect: it is documented
  directly in `pid_fuzzy.c`'s own comment on why 0/0 must NOT be treated as
  the neutral cell (raising ki blindly on a sensor fault is exactly the
  failure mode that comment rules out), and the same non-neutral-center
  choice is simply visible here under normal healthy tracking too.

## 3. Anomalies checked for

- **Saturation**: yes, by design — kp/kd sit at or within ~2 percentage
  points of the -25% floor and ki within ~2 points of the +25% ceiling for
  most of the run. This is the module's documented `MAX_NUDGE_FRACTION=0.5`
  ceiling at `strength_pct=50`, not an uncontrolled runaway; the clamp
  (`clamp_gain`) never needed to reject a negative or non-finite gain in
  this capture.
- **Chattering**: none found. Sample-to-sample change in `bd_kp_effective`
  never exceeded 1.8% of that zone's base kp (zone 0), 1.5% (zone 1), 1.4%
  (zone 2) — smooth membership drift, no rule-to-rule oscillation.
- **Fighting the feedforward term**: not observed. `bd_ff_hold` +
  `bd_ff_climb` dominate `bd_final_commanded` by roughly an order of
  magnitude over the P/I/D terms in every sample inspected (e.g. zone 2 at
  one representative point: `ff_hold=0.75`, `ff_climb=0.24` vs `pid_p=-0.04`,
  `pid_i=-0.11`, `pid_d=-0.02`), so the fuzzy layer's ±25% nudge to kp/ki/kd
  moves a small correction term, not the dominant duty contribution. No
  case was found where the fuzzy-adjusted PID term and the feedforward term
  had materially opposed signs at large magnitude.
- **A zone behaving unlike its peers**: no. All three zones show the same
  sign pattern, the same near-ceiling magnitude, and the same softening
  during the initial approach ramp. Zone 2 shows the tightest ki deviation
  range (always ≥+10%, never below) and zone 0 the widest spread toward
  smaller-magnitude kp/kd deviations at its largest negative error
  (-4.55 °C, kp dev -12.2%) — differences of degree, not kind.
- **Duty clamp events**: 13-21 samples per zone (of 726) show
  `bd_pre_clamp_total != bd_post_clamp_total`, all confined to segment 0
  (the initial approach and early hold) — ordinary 0/1 duty saturation
  (a brief >1.0 commanded duty on zone 2's initial climb, small negative
  commanded duties clamped to 0 as the hold settled). These are unrelated to
  the fuzzy layer specifically (the clamp is downstream of the full duty
  sum, dominated by the feedforward terms) and are not anomalous.

## 4. Single-arm tracking statistics (descriptive only — no baseline exists)

Computed with `tools/PcTools/src/kilnctrl/pid_ab_compare.py run` (unmodified,
same metric definitions as every other number in `PID_EXPANSION_PLAN.md`'s
§2/§3 tables), `min_segment_index=1` as the capture's own meta specifies.
**These numbers are single-arm, uncontrolled, and not comparable to any
other arm's numbers in this document set** — there is no fuzzy-off run of
this same firing, and the project's own noise-floor work
(`pid_ab_compare.py`'s module docstring) has already shown that run-to-run
swings of 20-50% on these same metrics can arise from an uncontrolled
start-temperature difference alone, with the true noise floor still
undetermined. Treat everything in this section as "what this one run looked
like," not "what fuzzy=50 does to tracking":

| zone | start °C | whole-run IAE (°C) | seg1 (45 °C dwell) ramp mean/worst err | seg1 dwell-entry overshoot / time-to-peak / settle | seg2 (truncated) ramp mean/worst err |
|---|---|---|---|---|---|
| 0 | 40.89 | 1.191 | -0.80 / 1.49 °C | +2.29 °C / 91 s / n/a | -0.02 / 2.39 °C |
| 1 | 40.71 | 0.776 | -1.14 / 1.80 °C | +2.00 °C / 86 s / 154 s | +0.08 / 1.30 °C |
| 2 | 40.65 | 0.906 | -0.72 / 1.29 °C | +2.24 °C / 91 s / 186 s | -0.45 / 2.27 °C |

Zone 0 shows no settle time for its segment-1 dwell-entry — its error was
still outside the ±1 °C settle band when segment 1 ended and segment 2's
ramp began, so `ramp_to_dwell_transitions` correctly reports "not settled by
segment end" rather than a number. All three zones tracked the ramp into the
45 °C dwell with worst-case error under ~2.4 °C and a dwell-entry overshoot
in the 2.0-2.3 °C range — unremarkable by this project's own historical
range for profile 7, but again: with no fuzzy-off counterpart for this exact
firing, that comparison cannot be made honestly here.

## 5. What the truncation costs

The reset landed 367 s into segment 2's ramp, target still climbing (last
recorded target 57.26 °C), with `dwelling=false` throughout segment 2. This
capture therefore has **zero** dwell-entry data for segment 2 — no
overshoot peak, no time-to-peak, no settle time, no dwell steady-state
offset for whatever temperature profile 7's second dwell targets. Given this
project's own history flags dwell-entry overshoot as a known-sensitive
metric (`z0_dwell_overshoot_mechanism_20260904_report.md`), and segment 2 is
presumably the higher-temperature, more thermally demanding dwell of the
two profile-7 segments, this is very likely the more diagnostically
important dwell-entry event of the two — and it is exactly the one this
capture cannot speak to. Nothing about the rule base's behavior at larger
sustained error (the NEG/POS row extremes of `RULE_TABLE`, never reached in
this capture because error stayed under ~5.6 °C throughout) can be
characterized from this data either; that would require either a
capture that reaches actual large-error excursions or a synthetic/host-test
exercise of `pid_fuzzy_adjust()` directly.

## 6. Bottom line

Within the single arm captured, the fuzzy layer engages continuously, in
the direction and (given the narrow error range actually seen) the
magnitude the rule table's documented intent predicts, on all three zones,
with no chattering, no runaway saturation beyond the designed ±25% ceiling,
and no observed conflict with the feedforward term. This retires the
*behavioral* half of "fuzzy has never been observed running above zero on
hardware" — the rule base does what it says it does. **It does not, and
cannot, answer whether fuzzy=50 tracks better or worse than fuzzy=0**; that
question is exactly as open as it was before this capture, pending a
completed A1 arm and a resolved watchdog root cause.

---
Source: `logs/coupling/fuzzy_ab_20260904d_s50_run1.jsonl` (726 usable
control/exec samples, 63.8 min). Base gains from
`tools/PcTools/config_presets/fuzzy_ab_strength50_20260903.json`. Rule table
and membership bands from `firmware/KilnFW/App/drivers/pid_fuzzy.c` (read,
not modified). Tracking metrics from
`tools/PcTools/src/kilnctrl/pid_ab_compare.py run` (unmodified). Prior
context: `PID_EXPANSION_PLAN.md` §3.6b (09910377, reachability proof) and
89066869 (the two earlier inert campaigns).
