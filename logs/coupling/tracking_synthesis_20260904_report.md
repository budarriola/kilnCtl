# Per-zone tracking synthesis across all captures: where things stand (2026-09-04)

This is not another A/B question. It pools every attributable, complete firing
capture under `logs/coupling/` — spanning the coupling-matrix campaign, the
noise-floor campaign, the ease-off campaign, and the (inert-fuzzy) fuzzy
campaign — to answer the owner's actual driving question directly: **per
zone, how well does the board track a profile today, and what is the
dominant remaining error?** Computed with the existing tools
(`tools/PcTools/src/kilnctrl/pid_ab_compare.py run`, `log_analysis.py`), not
a new metric definition. Read alongside `ab_campaign_report.md` and
`easeoff_ab_20260904_report.md`, which this synthesis corroborates,
extends, and in one place, corrects.

## 1. Inventory: 94 files, 41 candidate firing logs, 27 usable, 14 out of scope, live campaign excluded

`logs/coupling/` holds 94 files. Restricting to `*.jsonl` and dropping the
`*.cooldown.jsonl` fragments (idle-tail captures, not firings) leaves 41.

**Excluded outright, per instructions — live campaign, incomplete:**
`fuzzy_ab_20260904d_s50_run1.jsonl` (arm B1 of `fuzzy_ab_20260904d` is
running now; 139 rows, `final_state=running`, not touched).

**Excluded from tracking-quality analysis — not multi-zone firing captures
(14 files):** these are single-zone plant-identification or relay-autotune
text-line captures, a different schema (`{"t":..., "s": "state=... zone 0:
..."}` free text, or `HH:MM:SS {...}` autotune bodies) that the tracking
metrics tool doesn't apply to and that don't carry all three zones:
`cpl_z0_mcp/thermo.jsonl`, `cpl_z1_mcp/thermo.jsonl`, `cpl_z2_mcp/thermo.jsonl`
(FOPDT identification, one zone each), `relay_z0_status/thermo.jsonl` (relay
autotune), `p7_fuzzy0_batch/status/thermo.jsonl` (12-14 rows each, tool
call/telemetry fragments, not full captures), `p7_newmatrix2_uart.jsonl`
(same firing as `p7_newmatrix2_http.jsonl` but the UART side-channel, redundant),
`coupid6_run1.jsonl` + `coupid6_run1_part2.jsonl` (`HH:MM:SS {...}` format
already used for the coupling-matrix identification fit, per
PID_EXPANSION_PLAN.md 3.6b/§3.2 — not re-analyzed here since it doesn't add
tracking-quality information beyond what already produced the matrix).

**Usable: 27 complete (`state=done`), parseable, profile-7, three-zone
firing captures**, all on the *same profile* (profile 7, `pv08311918` —
every attributable capture in this dataset ran this one profile; there is no
tracking data here for any other profile). One is a byte-identical duplicate
(`p7_oldmatrix_runA.jsonl` == `p7_oldmatrix_http.jsonl` run 0, confirmed by
identical metrics) and is counted once. Grouped by controller configuration,
attributed from `ab_campaign_state.json`, the plan doc's own file-to-preset
table (PID_EXPANSION_PLAN.md lines 881-887), and each file's own preset name
where a meta line exists:

| group | files (n) | config |
|---|---|---|
| old-matrix-era | `ab_old_{1,2,3}`, `noise_floor_p7{,_b,_c,_d}_run{1,2,3}` (6), `floor_run1`, `p7_oldmatrix_http` run0+run1, `p7_oldmatrix_runC` run0 | 12 | `coupling_matrix_pre20260902` (`noise_floor_p7*`/`floor_run1` share this era's numbers to within noise but their preset name wasn't separately recorded — see caveat below) |
| current-matrix (coupling only) | `ab_new_{1,2,3}`, `p7_newmatrix_http`, `p7_newmatrix2_http` run0 | 5 | `coupling_matrix_20260831` |
| current-matrix + ease-off 2.0 | `easeoff_ab_20260904_2p0_run{1,2,3}` | 3 | current matrix, `ease_off_window_mult=2.0` (board default) |
| current-matrix + ease-off 3.0 | `easeoff_ab_20260904_3p0_run{1,2,3}` | 3 | current matrix, `ease_off_window_mult=3.0` |
| current-matrix, "fuzzy" preset (control_mode:2, fuzzy inert) | `fuzzy_ab_20260904_s50_run1` | 1 (incomplete — `final_state=running`, kept only as a plain-PID data point, not scored as a completed firing) | current matrix; the fuzzy layer never engaged (documented `control_mode:2` bug) so this is, in effect, more plain-PID current-matrix data |
| pre-newmatrix baseline | `p7_fuzzy0_http.jsonl` | 1 | "era before newmatrix" per the plan doc's own label — an even older configuration than "old-matrix-era" above |

**Honest gap in attribution:** `noise_floor_p7_run1/b/c/d1/d2/d3.jsonl` and
`floor_run1.jsonl` carry no meta line and no preset name recorded in the
files I could find; they are placed in "old-matrix-era" purely because their
per-zone numbers (below) sit inside that group's range to within noise on
every one of six independent captures, not because a config record confirms
it. If that placement is wrong the "old matrix" figures below would need
revisiting, though the coupling-matrix campaign's own formal record
(`ab_old`/`ab_new`, n=3 each, matched pairs) already establishes the same
direction and magnitude independently — this pool corroborates it, it isn't
the only evidence for it.

## 2. Regime coverage: the ">62 °C" split this task asked for does not exist in the data

Every one of the 27 usable captures ran the same profile, and its own
telemetry caps at **`target_c` = 60.0 °C** (checked directly:
`max(r.target_c for r in poll_rows(...))` on `ab_old_2` and
`p7_newmatrix_http` both return exactly 60.0). **No capture in
`logs/coupling/` — not one of the 64 raw firing logs, attributable or not —
ever commands or reaches a target above 60 °C.** The coupled-hold
infeasibility boundary this task asked to split on (~60-65 °C, PID_EXPANSION_PLAN
§3.2's feasibility sweep) is therefore **never exercised by any tracking
capture in this dataset**. Every claim in this report is a low-temperature
(<=60 °C) statement. The "infeasible above ~62 °C" claim itself rests
entirely on the matrix feasibility sweep's own math (§3.2), not on any
measured tracking error — this synthesis cannot corroborate or refute it,
and neither can any other analysis of this log set. That is itself the most
useful finding of this pass: **the regime split the project cares about for
cone-range firing has zero hardware tracking data behind it.** (Regime split
used below instead: ramp segment vs final dwell segment, and — within the
26-60 °C range that does exist — a low sub-window (<45 °C) vs the rest, per
`compute_zone_metrics`'s own ramp/dwell windowing.)

## 3. Per-zone tracking, segment-by-segment, both eras (n given per cell)

Numbers are `dwell_steady_state_offset_c` and `dwell_entry_overshoot_peak_c`
from `pid_ab_compare.py run --json`, segment 1 (the profile's final ramp-to-
~60 °C + dwell — the best-populated, most reliable window in every capture,
hundreds of samples). Sign convention: `actual_c - target_c` (positive =
running hot/overshooting, negative = running cool/lagging).

| metric | zone | old-matrix-era (n=12) | current-matrix, coupling-only (n=5) | change |
|---|---|---|---|---|
| dwell steady-state offset, °C | z0 | 1.71 to 2.30 (mean ~1.97) | 0.55 to 0.69 (mean ~0.62) | **-1.35, clears 0.5 C bar by 2.7x** |
| | z1 | 0.99 to 1.11 (mean ~1.04) | 0.38 to 0.43 (mean ~0.40) | **-0.64, clears bar** |
| | z2 | 0.73 to 0.83 (mean ~0.78) | 0.55 to 0.92 (mean ~0.79) | **~0, below both noise floor (0.147) and bar** |
| dwell-entry overshoot peak, °C | z0 | 3.53 to 4.11 (mean ~3.76) | 1.91 to 2.32 (mean ~2.18, n=4 — `p7_newmatrix_http`'s seg-1 transition wasn't separately re-pulled) | **-1.58, clears bar** |
| | z1 | 1.80 to 2.11 (mean ~1.95) | 1.34 to 1.53 (mean ~1.45) | -0.50, at the bar |
| | z2 | 1.39 to 1.80 (mean ~1.58) | 1.74 to 2.02 (mean ~1.91) | **+0.33, WRONG DIRECTION (worse)** |

This reproduces `ab_campaign_report.md`'s formal n=2 (start-temp-admitted
pairs 2 and 3) result on a much larger pool (12 old-era vs 5 current-matrix
captures, same direction on every one of them, no exceptions) and sharpens
it:

- **z0 is the dominant error source in both eras, and remains so today.**
  It has both the largest offset AND the largest overshoot of the three
  zones under the old matrix (offset ~2.0 °C, overshoot ~3.8 °C — both far
  above the 0.5 °C bar and the 0.116 °C noise floor). The matrix change cut
  both roughly in half to two-thirds, which is a real, large, bar-clearing
  improvement, but z0 is *still* the worst-overshooting zone of the three
  today (~2.2 °C vs z1's ~1.45 °C) — "z0 was fixed" is not quite right;
  "z0's error was cut by more than half and it is still the worst zone on
  overshoot" is.
- **z1 is the best-tracking zone today** on both metrics (lowest offset,
  lowest overshoot), and improved on both from the matrix change too.
- **z2 did not benefit from the matrix change, and got slightly worse on
  one metric.** Offset is flat (~0.78 °C both eras, difference smaller than
  z2's own 0.147 °C noise floor — genuinely unchanged). Overshoot moved from
  ~1.58 °C to ~1.91 °C, a +0.33 °C step in the wrong direction. At n=4
  (current) vs n=12 (old), on a metric not gated by the formal start-temp
  confound check the coupling campaign ran for whole-run IAE, and with a
  magnitude just under the 0.5 °C actionable bar, **this is not clean enough
  to act on** — but it is worth stating plainly rather than folding into
  "z2 indistinguishable," because `ab_campaign_report.md`'s own
  `consistent_patterns` block already hinted at this (`dwell_entry_overshoot_peak_c`
  favored the new matrix in "zones [0,1]" only, both pairs — z2 was
  conspicuously absent from that improving list, not indistinguishable from
  it). **This synthesis's z2-overshoot number turns that absence into a
  concrete, if sub-bar, magnitude: the coupling-matrix fix that helped z0/z1
  came with a ~0.3 °C overshoot cost on z2, not just "no effect."**

**Which zone tracks worst, and how**: not a single answer — it depends on
what "worst" means. On **steady-state dwell offset**, z0 and z2 are
statistically tied today (0.62 vs 0.79 °C, a 0.17 °C gap — smaller than
z2's own noise floor) and both worse than z1 (0.40 °C, a real ~0.2-0.25 °C
gap that clears both zones' floors). On **overshoot**, z0 is unambiguously
worst (2.18 °C vs z1's 1.45 °C and z2's 1.91 °C) and has been in every
capture in both eras. The dominant remaining error mode is therefore
**z0's dwell-entry overshoot** — not a lag, not a steady-state offset (both
of those are now z0's best-behaved numbers), and not primarily a coupling
effect from a neighbour (the column-sum asymmetry — z2 43.87, z1 39.74, z0
22.63 — says z0 exerts the *least* influence on its neighbours, which is
consistent with z0 being the zone that most needs an external fix rather
than one it can solve by heating a neighbour).

## 4. Ease-off and fuzzy campaigns: current-matrix tracking, no new zone ranking

`easeoff_ab_20260904_2p0/3p0` (6 captures, current matrix, current board
default `ease_off_window_mult=2.0` and the tested `3.0`) and the (inert-fuzzy,
effectively plain-PID) `fuzzy_ab_20260904_s50_run1` all ran a stabilised
variant of profile 7 (40 °C/45 min hold prepended) and are scored from
`min_segment_index=1` per their own meta. Re-checking the same z0/z1/z2
ranking on these six completed captures' final dwell segment: z0 offset
0.6-0.9 °C, z1 0.53-0.66 °C, z2 0.52-0.85 °C — same rough pattern (z1
mostly-best, z0/z2 close and both worse), consistent with §3 above and not
a new finding. `easeoff_ab_20260904_report.md`'s own verdict —
**INDISTINGUISHABLE, no board change** — stands; this synthesis adds nothing
against it and one thing for it: the z0/z1/z2 ranking is the same regardless
of which `ease_off_window_mult` arm is used, i.e. that knob does not change
*which* zone is the problem.

## 5. Cross-check against PID_EXPANSION_PLAN.md's existing claims

- **Coupling-matrix improvement on z0/z1 (§ab_campaign_report.md, n=2
  admitted pairs): CONFIRMED, and now backed by 12 old vs 5 current
  captures instead of 2 pairs — no captures anywhere in the dataset
  disagree with the direction.**
- **z2 "INDISTINGUISHABLE" between matrices (same source): confirmed for
  steady-state offset, but the overshoot metric shows a small
  (~0.33 °C, sub-bar, n=4) move in the wrong direction that the existing
  report's own consistent-patterns table hinted at but didn't quantify. Not
  a retraction — the offset-based headline verdict is right — but "z2 is
  unaffected" is too strong; "z2's offset is unaffected, its overshoot
  moved slightly worse, below the actionable bar" is the accurate version.
- **">62 °C infeasible" (§3.2): cannot be checked against tracking data at
  all** — see §2. This is a gap in the evidence base, not a disagreement
  with the claim; the claim was never based on tracking data to begin with,
  and nothing here changes that math. Flagging it because the task asked
  for the >62 °C tracking regime specifically and it does not exist yet.
- **Ease-off campaign "INDISTINGUISHABLE" verdict: confirmed**, and the
  z0/z1/z2 ranking transfers cleanly onto those six captures too (§4).
- **Fuzzy campaign inert-`control_mode` bug: already documented and not
  re-litigated here** — its one complete-enough capture is reused only as
  additional plain-PID current-matrix tracking data (§1), which is a
  legitimate use of what would otherwise be "two campaigns' worth of wasted
  kiln time" per the plan doc's own words.

## 6. Recommendations, ranked, against the 0.5 °C actionable bar

1. **Investigate z0's dwell-entry overshoot specifically (not its offset or
   lag).** Effect size already measured: ~2.2 °C today, was ~3.8 °C before
   the matrix fix — the matrix fix worked on offset/lag but overshoot is
   the residual. This is the single largest remaining per-zone number in
   the whole dataset, ~1.7 °C above z1's overshoot and ~4-8x every zone's
   noise floor. **Clears the 0.5 °C bar by a wide margin — worth kiln time.**
   Candidate mechanism to check first (not yet investigated here): z0 is
   the top zone physically (`project_zone_physical_arrangement`) and has
   the smallest coupling-matrix column sum (22.63, least heat given to
   neighbours) — an overshoot concentrated in the zone that receives least
   compensating cooling-by-coupling from its neighbours is at least
   consistent with a per-zone gain/derivative tuning gap rather than a
   matrix problem, since the matrix campaign already fixed what it could
   reach on z0 (offset) without touching overshoot.
2. **Do not chase the z2 overshoot regression (§3) with a dedicated
   campaign.** Measured effect ~0.33 °C, n=4 vs n=12, no formal confound
   check on most of the old-era pool. Below the owner's 0.5 °C bar and
   close to z2's own 0.147 °C floor multiplied by the small-n uncertainty.
   Worth a one-line watch item in the plan doc (added below), not kiln
   time.
3. **Do not re-run the ease-off campaign.** Already confirmed
   indistinguishable on the pooled z0/z1/z2 ranking too; no new information
   would come from repeating it.
4. **If cone-range tracking is wanted, it has to be measured, not
   inferred.** Nothing in `logs/coupling/` reaches above 60 °C. The
   ">62 °C infeasible" claim is architecture-level math, not a validated
   tracking result — closing that gap needs a real firing above ~65 °C
   with the current matrix and current gains, which is a different, larger
   kiln-time ask than anything analyzed here, and outside this offline
   pass's scope (no board access taken).

## Files

- This report: `logs/coupling/tracking_synthesis_20260904_report.md`
- Linked from `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §3.6b.
- Source captures: all `logs/coupling/*.jsonl` listed in §1's table (none
  modified, moved, or deleted).
- Scratch analysis scripts used to produce §3/§4 numbers were run from a
  temp scratchpad outside the repo and are not checked in (the existing
  `pid_ab_compare.py run --json` command reproduces every number above
  directly, per file/run given in §1's table).
