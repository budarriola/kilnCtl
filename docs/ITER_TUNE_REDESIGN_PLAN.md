# `iter_tune` redesign — tracking-quality-driven iterative tuning

> **Status update, 2026-09-16: step 6 (Monte-Carlo acceptance, sec 7 A1-A8)
> built and run; the plan STOPS HERE per its own sec 8 rule ("any miss ends
> the plan at this line with a report, not a workaround").** Verified against
> code, not this doc's own prior headers (steps 4/5 below were previously
> claimed at different states than the code actually showed):
>
> - **Step 4** (null-experiment noise-floor artifact) is CLOSED, `602a07a8`.
> - **Step 5** (`iter_tune.c` rewrite against the new comparator) is CLOSED,
>   `8f80a4de` plus two defect-fix passes `249ce287`/`ce55440d`.
> - **Step 6** (Monte-Carlo run of sec 7's A1-A8): A2, A5, A6 PASS (0% worse,
>   100% terminate within budget, 0 cage violations, `check_sim_iter_tune_bars.ps1`).
>   A1 PASSES only against a deliberately pinned known-failure ceiling
>   (24/660, 3.64%, `e3458846`/`docs/audits/a1_false_accept_root_cause_2026-09-14.md`)
>   -- an honest, previously-investigated and individually-defended property
>   of this plant/scoring design, not the sec 7 2.0% design target. A3/A4
>   were checked and found **structurally unreachable** on this plant model:
>   `sim_iter_tune.c`'s own Part 0 oracle grid shows the whole reachable
>   tracking-error spread from moving kp/ki is smaller than the owner's
>   0.5 C floor at every start point tried, so "refuses every trial" is
>   correct behaviour here, not an algorithm gap -- this is a fact about the
>   model, already noted in the harness's own comments, not new this pass.
>   **A8 (profile independence) was newly built this pass (`sim_iter_tune.c`
>   Part 4, `g_profile_b`) and genuinely FAILS**: re-running the A1
>   null-experiment procedure with baseline and trial firings on two
>   different profiles that still share all 6 segment classes (differing
>   only in per-segment dwell duration) measures 45/660 (6.82%) false
>   accepts, against a 38.4-count ceiling three sampling standard deviations
>   wide around A1's own pinned rate -- roughly double, and confirmed not to
>   be sampling noise (a sanity run with the two profiles made identical
>   reproduces 30/660, inside the same ceiling). **Only the A1-half of A8 was
>   built; the A2-half (never-worse re-measured across profiles) is NOT yet
>   built** -- it needs `tune_run()` itself to take a profile per firing,
>   which this pass does not touch. Per the plan's own rule this is where
>   the plan stops and reports, not where it is worked around: A8's bar is
>   deliberately kept OUT of `check_sim_iter_tune_bars.ps1`'s exit code (the
>   same informational, non-blocking treatment the sec 6.5 credibility gate
>   got below) so a genuine, freshly-discovered doubling of the false-accept
>   rate is not quietly pinned the way A1's already-litigated number was --
>   pinning THIS number would be exactly the "workaround" sec 8 forbids.
>
> **Status update, 2026-09-16 (later pass): A8's A2-half (never-worse
> re-measured across profiles) is now built and gated.** `tune_run()` was
> extended to take a separate search profile and eval profile per call
> (`sim_iter_tune.c`, new params on `tune_run`); Part 1 and Part 3 pass
> `g_profile`/`g_profile` for both, unchanged from before. New **Part 5**
> reuses Part 3's mismatched-plant ensemble (220 plants, 660 zone-runs) but
> searches on `g_profile` and evaluates the final accept/reject cost on
> `g_profile_b`, the same never-worse floor (>0.5 C) as A2 itself. Measured
> result: **0 worse of 660 (0.00%), 2 better, 658 unchanged** -- a clean,
> reproducible PASS well inside the <=1% bar, unlike the A1-half's genuine
> 6.82% miss above. This bar (`"A8 A2-half bar: ..."`, `check_sim_iter_tune_bars.ps1`)
> IS now gated into the script's exit code, since a genuine pass carries no
> risk of pinning a workaround. Negative-tested: sabotaging the comparison
> (`<=` to `>=`) reproducibly FAILs (exit 1); hand-restored and confirmed via
> md5/git-hash-object against a pristine backup (not `git show`, CRLF); a
> full clean rebuild afterward reproduces PASS/exit 0. (A side finding from
> the sabotage run: the original label `"A8 bar (A2 half): ..."` did not
> match the script's `"bar:.*-> FAIL"` summary regex, so a failing A2-half
> would print FAIL and exit 1 but never appear in the human-readable failing-
> bar list; renamed to `"A8 A2-half bar: ..."` to match the convention used
> by every other bar and fix the summary.)
>
> **Closed by owner decision: the A1-half of A8's cross-profile miss (45/660,
> 6.82%, vs a 38.4-count ceiling) is accepted as a known gap, not made
> green.** Do not re-dispatch work to close it. Both halves of A8 are now
> settled: the A2-half closed on its own merits above, and the A1-half
> closed by explicit owner acceptance of the gap rather than an algorithm
> change.
>
> **Status update, 2026-09-10 (later pass):** steps 1, 2, 3, 4, 5 and the
> write-surface part of 7 are now IMPLEMENTED. The status paragraph that used
> to stand here (dated 2026-09-09) said steps 3-4 and the sec 6.5 credibility
> gate were not attempted and blocked on G1-G4 harness work that "does not
> exist yet" — that is stale as of this pass and was corrected here rather
> than left to mislead the next reader. What actually landed since:
>
> - Steps 1, 2, 5: `control/firing_score.c`, `control/firing_compare.c` and a
>   rewritten `control/iter_tune.c` (`8f80a4de`, three defect fixes in
>   `249ce287`/`ce55440d`). Numbers and two design defects the simulation
>   found in the plan's own sec 4 step schedule are in
>   `docs/audits/iter_tune_redesign_sim_2026-09-09.md`.
> - Step 7's write-surface guard: `check_iter_tune_write_surface.ps1`
>   (`f3fcd597`) is IMPLEMENTED and negative-tested — it fails if
>   `iter_tune.c`/`.h` ever calls a setter/persistence/hardware API directly,
>   and separately fails if any production file outside `test/` calls an
>   `iter_tune_*` function at all (today, none does).
> - Step 3, the G1-G4 sim-harness gaps: all four are IMPLEMENTED in
>   `firmware/KilnFW/App/test/sim_plant.c`/`.h` (`e0d2e006`) —
>   `sim_plant_from_zone_cfg()`, the real `heater_output.c` PWM window via
>   linking (not reimplementation), relay actuation lag, and MAX31856
>   quantisation.
> - The sec 6.5 credibility gate: IMPLEMENTED as
>   `firmware/KilnFW/App/test/sim_credibility_gate.c` (`225d4b91`), wired into
>   `build_host_tests.ps1` as an informational (non-blocking) step. It first
>   failed outright (ramp MAE 8-10 °C against a 3 °C bar) for a reason
>   diagnosed in `docs/audits/sim_credibility_gate_real_cause_2026-09-10.md`:
>   the simulator coupled zones by conservative *exchange* (`g·(T_j−T_i)`)
>   while the firmware's own `zone_coupling_solve.c` couples by additive
>   *source-gain* (`diag(k)+coupling_coeff`) — two different model classes
>   that agree only in differential mode, and the recorded dwell operating
>   point is almost pure common mode. `d63a5591` changed `sim_kiln_step()` to
>   the firmware's own model class. **Current state as of this pass (rebuilt
>   from HEAD, `logs/coupling/noise_floor_p7_run1.jsonl` /
>   `noise_floor_p7d_run1.jsonl`): ramp MAE now PASSES on 5 of 6 zone-runs**
>   (calibration 1.481/1.710/3.367 °C, hold-out 1.489/1.335/2.780 °C against a
>   3.0 °C bar — only calibration z2 misses, at 3.367), **dwell offset misses
>   on 5 of 6** (−2.1 to −4.8 °C against ±1.5, one hold-out z0 pass at −1.412),
>   dwell-entry-peak is mixed pass/fail per segment, and the noise-floor
>   spread check fails 4 of 6 keys (simulated spread pessimistic vs. 2×
>   `noise_floor.json`). (These ramp-MAE and dwell-offset numbers predate the
>   `69118a66` sim_plant dead-time cap fix — see the 2026-09-21 addendum atop
>   `docs/audits/credibility_gate_dwell_offset_2026-09-14.md` and
>   `docs/audits/credibility_gate_scalar_adoption_2026-09-14.md` for current
>   numbers; the gate's conclusion is unchanged.) **The gate's overall verdict
>   is still `GATE FAILS`**
>   — three of its four bars are open, most acutely dwell-entry peak, which
>   the audit doc's own sec 6 says is not yet demonstrated by any variant
>   tried. Per plan sec 6.5 this means: simulation results are materially
>   credible for *tracking-error* purposes (the ramp MAE bar, which is what
>   steps 1/2/5's own validation and A1-A8 depend on) but not yet for
>   dwell-entry overshoot specifically, and the plan does not proceed past
>   this gate to treat the simulator as evidence for anything dwell-entry-peak
>   related until that bar closes.
>
> **Still NOT done as of this status block's original pass** (see the
> 2026-09-10 status update below sec 9 for why): the noise-floor artifact
> (Bar 2, sec 3.1/4), and shadow mode (step 8). **Superseded for step 7:**
> persistence + HTTP surface landed (the "iter_tune: add persistence + HTTP surface (plan step 7)" commit, see row 7 below
> for the current, fully-closed acceptance status). Shadow mode remains
> blocked on net-new C engineering of the same "dedicated pass" size as
> G1-G4 was, now compounded by the still-open dwell-entry-peak credibility
> bar above. Nothing is wired into `profile_executor.c` and the module
> proposes nothing on hardware.
>
> **Regression found in this pass: `d63a5591`'s coupling-model fix moved A1's
> false-accept rate off zero.** Re-running `sim_iter_tune.exe 220` (the exact
> harness size behind the `docs/audits/iter_tune_redesign_sim_2026-09-09.md`
> baseline) from `HEAD` gives **660 null comparisons: 24 ACCEPT (3.64 %),
> 21 REJECT, 615 INSUFFICIENT** — up from the recorded baseline of
> **0 ACCEPT (0.00 %)**. This is still under A1's 5 % hard-fail line but above
> its 2 % target bar, so A1 now reads **FAIL** where it previously PASSed. Part
> 1 (24 zone-runs: 0 improved, 24 unchanged, 0 regressed, 24/24 converged) and
> Part 3 (A2/A5/A6, mismatched ensemble: 660 zone-runs, 0 worse, 0 cage
> violations, all PASS) are unchanged in shape from the baseline. Nothing in
> `iter_tune.c`, `firing_score.c` or `firing_compare.c` changed between the
> baseline run and this one — the only relevant change on the path is
> `sim_plant.c`'s coupling model class (additive source-gain, landed for the
> sec 6.5 gate above). Not root-caused or fixed in this pass — flagged here
> per the standing instruction that a rise in false accepts means something is
> wrong, and left for the same dedicated pass as the other open items, since
> diagnosing it properly means understanding how the new coupling model
> changes the null experiment's own noise characteristics, not just the
> comparator or `iter_tune.c`'s decision logic.
>
> **Owner decision, 2026-09-08:**
> *keep `iter_tune`, but redesign it* — "design it better so it does not
> require the same starting point. what i care about is how well it tracks the
> target temperature. test it in simulation until you are confident we have a
> good algorithm." (The "design only, nothing implemented" line that used to
> stand here is stale as of 2026-09-09 — see the status update above.)
>
> This document **supersedes** `docs/audits/iter_tune_decision_2026-09-07.md`
> (`3bf773af`), whose recommendation was "wire the existing module as-is".
> The existing decision core (`firmware/KilnFW/App/drivers/control/iter_tune.c`,
> built in `fe14ddf`/`17f7ebd`) is **not** wired and must not be wired in its
> current form.

---

## 0. Why the existing design cannot simply be wired

Three defects, in the order they matter:

1. **The score is a whole-firing scalar.** `iae_normalized`
   (`profile_executor_firing_stats.c`, `firing_stats_snapshot()`) is
   `iae_raw_c_s / (duration_s * setpoint_span_c)`. Every tick of the run is
   folded into one number, including the ticks where the zone was still
   climbing to meet the profile from whatever temperature the kiln happened to
   start at. Two firings that differ only in start temperature therefore score
   differently for a reason that has nothing to do with the gains.
2. **The comparability rule papers over that with a window, not a fix.**
   `ITER_TUNE_START_TEMP_TOLERANCE_C = 2.0` refuses to compare firings more
   than 2 °C apart. That makes the mechanism *correct* and *nearly always
   idle*: the only two full captures this repo has of the same profile and
   build differ by 4.8 °C at the first sample. The owner's requirement is
   precisely that this constraint go away.
3. **The accept threshold rests on an unknown floor.** `iter_tune.h`'s own
   analysis is honest and worth keeping: the six-repeat `noise_floor.json`
   campaign gives per-zone two-sample prediction intervals of 0.19/0.12/0.20 °C
   on `iae_normalized_whole_c`, the 20 % relative bar is **already below**
   zone 2's interval at today's baseline magnitude, and it gets *worse* as
   tuning succeeds. The absolute floor added on top (0.20 °C) is a patch on a
   metric that should not have been a whole-firing scalar in the first place.

What is worth keeping, unchanged, from the old module:

- The **exact-revert posture**: gains are never recomputed on revert;
  `iter_tune_active_gains()` returns the same float bits that were accepted.
  Keep this verbatim.
- **Pure decision logic, no ESP-IDF / NVS / lock / FreeRTOS**, with the caller
  owning persistence and the one run-boundary call site. Keep.
- **Opt-in per zone, default OFF.** Keep.
- The refusal to touch `kd`. Keep.

## 1. Closed avenues this design must not re-open

Recorded here so a later reader does not redesign back into them:

- **Closed-loop two-point FOPDT ramp fitting** returns an artifact that is a
  function of the commanded rate, with no plant content. This plan does no
  identification of any kind.
- **Dwell-entry climb decay** worsened overshoot on all three zones and tripped
  guard 2; reverted. Not revisited.
- **Coupling lead compensation** reduces at t=0 to the already-reverted
  uncoupled formula. Not revisited.
- **The load estimator cannot observe load** (all three zones closed negative).
  This plan therefore treats load/ware mass as an *unobserved nuisance
  variable* to be rejected by design, never as something to estimate.
- **"Dwell credit" is structurally unreachable** (the 25 °C lag gate and the
  half-cone-step band are mutually exclusive). Not used.
- **Differences below 0.5 °C are not worth chasing** (owner). This is promoted
  below into a hard accept-rule term, not just advice.

---

## 2. The tracking metric

### 2.1 The unit of comparison is a *matched segment*, not a firing

This is the central change. A firing is not scored as a whole; each
**segment** of the profile (a ramp at a commanded rate, or a dwell at a
commanded temperature) is scored independently, per zone. Two *different*
firings — different profiles, different lengths, different start temperatures
— are compared by matching their segments into pairs of the same **segment
class** and comparing pairwise.

A segment class key is `(zone_index, kind, rate_bucket, temperature_bucket)`:

| Field | Definition |
|---|---|
| `kind` | `RAMP_UP`, `RAMP_DOWN`, `DWELL` |
| `rate_bucket` | commanded °C/hr, bucketed at 25 °C/hr granularity (`DWELL` → bucket 0) |
| `temperature_bucket` | the segment's mean target, bucketed at 25 °C |

Rationale for bucketing on temperature: plant gain is not constant with
temperature on this bench — the coupled `ff_hold` solve is already known to be
infeasible above ~62 °C, i.e. the identified model's validity is itself
temperature-bounded. A ramp at 100 °C and a ramp at 500 °C are not the same
experiment and must not be pooled.

**Start temperature stops mattering** because of two exclusions applied before
any segment is scored:

- **Capture-transient exclusion.** Every tick before the zone *first* comes
  within the profile-executor's own tracking band of the target is discarded,
  for the whole firing. That is exactly the work whose size depends on the
  start temperature. A firing started 5 °C warm and one started cold produce
  the same scored ticks; they just have different amounts of discarded prefix.
- **Infeasibility exclusion.** A tick is excluded when the zone commanded
  ≥ 98 % duty for the whole preceding PWM window and the error is still
  negative — the zone is saturated and the gains are not what is limiting it.
  Scoring saturated ticks measures the heater, not the controller.

A segment with fewer than `MIN_SCORED_TICKS` (60, i.e. one PWM window at the
1 Hz `PROFILE_EXECUTOR_TICK_MS` rate) surviving ticks is dropped entirely.

### 2.2 Three sub-scores per segment, no invented weights

Candidates weighed:

| Candidate | Verdict |
|---|---|
| Normalised IAE over the whole profile | **Rejected** — §0.1, the defect being fixed. |
| ITAE | **Rejected** — the time weight is arbitrary across a multi-segment profile and makes a long dwell dominate a short critical ramp for no physical reason. |
| Time-in-band against the guard's own band | **Rejected as the primary** — it saturates. Once a zone is always in band the metric is flat and the search has no gradient, which is exactly when we most want to know whether a change helped. Kept as a *reported* diagnostic and as a safety veto (see §3). |
| Worst-case ramp lag | **Kept, as one of three.** |
| Dwell-entry overshoot | **Kept, as one of three.** |
| Steady dwell error | **Kept, as one of three.** |
| Single weighted composite of the above | **Rejected as the accept rule** — the weights would be invented, and a composite silently permits trading overshoot away for lag. A composite *is* still computed and displayed for humans; it never decides anything. |

The three sub-scores, all per (segment, zone), all **lower is better**:

1. **`lag_s` — ramp tracking lag, in seconds.** For `RAMP_*` segments only:
   `median over scored ticks of ( |actual − target| / |commanded_rate_C_per_s| )`.
   Expressing lag in *seconds* rather than °C is what makes ramps at different
   commanded rates comparable: a 0.5 °C error on a 50 °C/hr ramp and a 5 °C
   error on a 500 °C/hr ramp are the same 36 s of lag and should score the
   same. Segments with `|rate| < 10 °C/hr` are classified `DWELL`, not ramp,
   so this never divides by a near-zero rate. Median, not mean, so one
   excluded-tick boundary artifact cannot move the score.
2. **`entry_peak_c` — dwell-entry overshoot, in °C.** For `DWELL` segments
   only: the maximum positive `actual − target` within the *entry window*, the
   first `L + 2τ` seconds of the dwell using this zone's own persisted
   `model_dead_time_s` and `model_tau_s`. Using the identified time constants
   makes the window physically sized per zone instead of a fixed constant.
3. **`steady_rms_c` — steady dwell error, in °C.** For `DWELL` segments only:
   RMS of `actual − target` over the dwell *after* the entry window. RMS, not
   mean-absolute, because a symmetric limit cycle should be penalised and a
   mean-absolute figure already exists in the firing stats.

A `RAMP` segment therefore yields one sub-score; a `DWELL` yields two. That
asymmetry is deliberate and harmless — comparison is per sub-score.

### 2.3 How two dissimilar firings become comparable

1. Score every segment of firing A and firing B into
   `(class_key) → {sub-score → value}`.
2. Intersect on `class_key`. Only classes present in both contribute.
3. For each sub-score, take the **paired differences** across the matched
   classes: `d_i = B_i − A_i` (negative = B better).
4. Aggregate by **median of the paired differences**, and record `n` = number
   of matched pairs. A single scalar per (zone, sub-score), with an explicit
   sample size attached.

Two firings of entirely different profiles that happen to share two ramp
classes yield `n = 2` for `lag_s` and `n = 0` for the dwell sub-scores — the
mechanism then simply has nothing to say about dwell behaviour from that pair,
and says so, rather than inventing a comparison. **`n = 0` is a first-class
outcome, not an error.**

---

## 3. The accept / reject rule

Two bars must **both** be cleared, per sub-score, for a trial to be accepted;
and a third condition must hold for it not to be rejected.

**Bar 1 — the owner's floor (known today, needs no experiment).**
The median paired improvement must be at least **0.5 °C**, or for `lag_s`, the
lag equivalent of 0.5 °C at that class's commanded rate
(`0.5 / rate_C_per_s` seconds). Differences smaller than this are explicitly
not worth kiln time, by owner instruction. This bar is available from day one
and is what the mechanism runs on before any noise floor exists.

**Bar 2 — the statistical floor (needs the floor, and is skipped until it
exists).** The median paired improvement must exceed
`floor[zone][class_kind][sub_score]`, a per-key two-sided prediction interval,
**and** the paired differences must be consistently signed: at least
`ceil(0.75 * n)` of the `n` paired differences must be improvements, with
`n >= 5`. A sign-consistency requirement across matched segments is far
stronger evidence than one scalar clearing one threshold, and it is available
*within a single firing pair* because a firing contains many segments — which
the old whole-firing design threw away.

**No-degradation veto.** The trial is **rejected** if *any* sub-score with
`n >= 3` degrades by more than its own Bar-1 floor, or if time-in-band (the
diagnostic of §2.2) falls at all outside noise. This makes the rule a
non-dominance test: a trial that buys 1 °C of lag by adding 1 °C of overshoot
is rejected, not silently traded.

### 3.1 Establishing the noise floor

- **In simulation (cheap):** the floor is *estimated* by the null experiment —
  run the same profile on the same plant with **identical gains**, N ≥ 200
  times, varying only the modelled stochastic inputs (§6.1), and take the
  97.5 % two-sided interval of the paired differences per key. This is the
  number Bar 2 uses.
- **On hardware (expensive):** the floor is *validated*, not established, by
  the shadow-mode phase (§8, step 8): ≥ 5 real firings with `iter_tune` scoring
  and proposing nothing. If the observed hardware spread exceeds the simulated
  floor by more than 2×, the simulated floor is discarded and Bar 2 is
  disabled until a real hardware floor campaign is run — the mechanism then
  runs on Bar 1 alone, which is the conservative direction.
- **Before any floor exists:** Bar 2 is skipped and Bar 1 plus the
  no-degradation veto are the whole rule. **Refusing to act is a legitimate
  outcome and is the default one.** The mechanism is designed to spend most of
  its life reporting `REFUSED_INSUFFICIENT_EVIDENCE`.

The existing `noise_floor.json` (six repeats, 2026-09) is **not** reusable as
Bar 2's floor: it is keyed on the *old* metric set, and its own
`start_conditions` block flags the six runs as not strictly like-for-like
(start temps span 1.29 °C). It is kept as a cross-check on the simulator's
realism (§6.3), not as a threshold source.

### 3.1.1 Can a real hardware noise floor be determined today? (2026-09-10)

Checked against the 65 (now 108, counting derivative/cooldown files) local
`logs/coupling/*.jsonl` captures. Answer: **partially — a genuinely
comparable rested pair exists, but it only bears on the old whole-firing
metric, not on the new per-segment sub-scores (`lag_s`/`entry_peak_c`/
`steady_rms_c`) that Bar 2 actually needs.**

- **A comparable pair exists.** `noise_floor.json`'s own
  `start_conditions.runs` (per-channel thermocouple start reading, not the
  coarser whole-tick sample used to flag the six-repeat set as NOT
  like-for-like) shows `noise_floor_p7_run1.jsonl` and
  `noise_floor_p7d_run3.jsonl` — both profile 7, both the campaign's own
  claimed same preset (`coupling_matrix_pre20260902`) — starting within
  **0.10-0.15 °C per channel** (channel means 28.643 °C vs. 28.603 °C, a
  0.04 °C gap). That is inside the plan's own 0.5 °C "not worth chasing" bar
  and far tighter than the 1.29 °C spread that made the six-run set as a
  *whole* fail its own like-for-like check, and dramatically tighter than the
  4.8 °C-apart pair §0's original critique of the old design was about (a
  different, older pair — not this campaign). **This retracts nothing in §0
  or §3.1's existing text** (both statements are about different pairs/sets
  and remain true as written); it says a better pair than either of those
  now exists in the local capture set.
- **What it cannot yet give Bar 2.** `firing_score.c`'s three sub-scores have
  only ever been run against `sim_plant.c` output (`sim_iter_tune.c`,
  `sim_credibility_gate.c`) — there is no tool in this repo that runs
  `firing_score.c` against a real `.jsonl` capture's recorded actual/target
  series to produce `lag_s`/`entry_peak_c`/`steady_rms_c` values for it. Until
  that (small, bounded) adapter exists, "the real per-segment noise floor" for
  Bar 2's new metrics cannot be computed from this pair or any other —
  reporting a number here without that adapter would be exactly the kind of
  invented floor this section was written to prevent. The old whole-firing
  `iae_normalized_whole_c` floor for this pair specifically was not computed
  either, since `noise_floor.json`'s existing six-repeat aggregate already
  supersedes any single-pair number for that metric and using this pair alone
  would only *discard* information, not add it.
- **Recommended next step, not done here:** write a small host tool
  (`firing_score.c` + a `.jsonl` reader, no `sim_plant.c` dependency) that
  replays `noise_floor_p7_run1.jsonl` and `noise_floor_p7d_run3.jsonl`'s
  recorded per-tick actual/target/segment data through the real scoring
  function and reports the paired differences per sub-score. That is the
  concrete capture (already in hand) that would settle Bar 2's real-hardware
  floor question, once that adapter is written.

---

## 4. Perturbation strategy

| Aspect | Decision |
|---|---|
| **What moves** | `kp` and `ki` only. `kd` is never touched (noise-sensitive, no model backing a blind nudge). No other field of any kind is ever written — see §5. |
| **How many at once** | **One parameter, one zone, per firing.** Never two. The measured coupling matrix is large and asymmetric (z0 rises 27.32 per z1 step, z1 only 14.30 per z0 step; adopted `78f2134`, series `813ad90`) — a simultaneous two-zone perturbation is unattributable by construction. Coordinate descent, cycling `kp` → `ki` → next zone. |
| **Step size** | Per-zone, adaptive: starts at **±10 %** of the current baseline value; **halves** after two consecutive rejects; **doubles**, capped at 20 %, after two consecutive accepts. Sign alternates when the previous trial in the same direction was rejected. |
| **The cage** | Every proposed gain is clamped to `[0.5×, 2.0×]` of the **commissioned** gains — the values written by autotune/hand-tuning at commissioning time, persisted once and never rewritten by this module — *and* to `[ITER_TUNE_GAIN_FLOOR_C, ITER_TUNE_GAIN_CEIL_C]`. Anchoring to a persisted commissioning value, not to the rolling baseline, is deliberate: a baseline-relative cage ratchets, which this repo has already been bitten by. **Settled (§9.1):** the anchor is captured automatically — the *first* time `iter_tune` is enabled for a zone, the gains active at that moment are snapshotted as "commissioned" and persisted; there is no separate manual capture step. The anchor is exposed as an explicit, operator-triggered "re-anchor" action (distinct from the existing "restore commissioned gains" revert of §4's Revert path) for the owner to deliberately move the cage centre later, e.g. after a hand-tuning pass. |
| **Stopping rule** | A zone stops (`CONVERGED`, tuning disabled, status sticky) on any of: step size below 3 %; three consecutive rejects at minimum step; **6** scored trials on that zone (§9.2 — reduced from an earlier 12); any gain hitting a cage edge twice. |
| **Not spending kiln time** | The module **never requests a firing.** It arms a trial only for a firing the operator was going to run anyway. If a firing yields `n = 0` matched pairs for every sub-score, the trial stays armed and unscored — but at most **3** such carries, after which it disarms and reverts, so a stale trial cannot ride indefinitely against a moving plant. |
| **Revert path** | Unchanged from the existing module, and this is the part worth keeping verbatim: trial gains live only in `pending_gains`; `active_gains()` returns `baseline.gains` the instant `has_pending` clears. Revert is a flag clear, never an arithmetic undo, so it is bit-exact. Baseline is persisted only on accept, so a power loss mid-trial reverts by itself. A single operator action ("restore commissioned gains") writes the persisted commissioning values back and disables the module for that zone. |

---

## 5. Safety and containment

Hard rules, each with the mechanism that enforces it:

1. **Never widens a limit.** The module's only write path into board state is
   `zones_config_set_pid(zone, kp, ki, kd)` with `kd` passed through
   unmodified. It has no reference to `max_temp_c`, `min_temp_c`,
   `max_ramp_c_per_hr`, `abs_max_temp_c`, any guard threshold, the coupling
   matrix, the model fields, or any safety-processor config.
2. **Enforced mechanically.** A new `check_iter_tune_write_surface.ps1` greps
   the module's translation unit for any setter other than the one allowed
   call and fails the build. Per this repo's standing rule, the check ships
   with a **negative test** proving it can fail — the negative test breaks the
   *production* function, and is **restored by hand**, never with
   `git checkout --`.
3. **Cannot propose an out-of-bounds gain.** The cage of §4 is applied inside
   `iter_tune_propose_perturbation()`, which is pure and host-tested at both
   edges; the caller additionally re-clamps before writing, so a bug in one
   layer is caught by the other.
4. **A bad proposal degrades tracking; it cannot defeat a guard.** The safety
   layers (SaftyFW's independent guards, the ESP's thermal guard, the
   profile-executor watchdog) are untouched and read the same inputs they do
   today. The worst outcome of a bad trial is a worse-tracking firing that the
   guards still bound.
5. **A fault ends it.** Any guard trip, `FAULTED` transition, or operator halt
   during a trial firing → the trial is discarded unscored, gains revert,
   `iter_tune` for that zone is **disabled** with a sticky operator-visible
   reason. It does not retry.
6. **Opt-in, per zone, default OFF**, with no path that auto-enables it —
   including the setup wizard.

---

## 6. The simulation harness

**A plant model already exists and this plan extends it — it does not build a
new one.** `firmware/KilnFW/App/test/sim_plant.c/.h` (added 2026-08-16, live,
maintained, already linked by `test_iter_tune.c` and `test_sim_kiln.c`) is the
asset. It is not what was deleted on 2026-08-28: `SimFW`/`kilnsim` were a
*bench-fixture* stack — a second RP2040 modelling TC/relay/CT electrical
interfaces over `benchproto` — not a thermal model. An earlier investigation
also compared `sim_plant.c` against the `thermal_model.c` that exists only in
git history and found `sim_plant.c` strictly more capable; that older file is
deliberately not resurrected.

What `sim_kiln` already gives us, for free:

- 3 coupled zones (`SIM_KILN_MAX_ZONES`), simultaneous-update coupling via a
  full `coupling_w_per_c[i][j]` conductance matrix, so coupling is not
  order-dependent;
- per-zone first-order lag plus transport delay, with a *separate*
  thermocouple lag (`sensor_lag_tau_s`) and delay (`sensor_delay_s`);
- a **radiative loss term** (`radiative_coeff_w_per_k4`, Stefan-Boltzmann in
  absolute temperature) — the one thing that makes plant gain fall with
  temperature, which a purely linear model can never show;
- per-zone fault injection (dead element, welded relay, TC detached / frozen /
  open) and a `sensor_map[]` for miswired connectors;
- deterministic, reproducible noise (one LCG advance per tick, so two reads in
  one tick agree).

### 6.1 The four gaps to close

Everything else in this section is scoped to exactly these. Nothing else about
`sim_plant.c` is to be rewritten.

| # | Gap | Why it matters here | Work |
|---|---|---|---|
| **G1** | **Real measured parameters are not loaded.** `sim_plant_cfg_t` is parameterised *physically* (`thermal_mass_j_per_c`, `heater_power_w`, `loss_coeff_w_per_c`) and `coupling_w_per_c` is a conductance; the board's measured data is *FOPDT* (`model_k_dc`, `model_tau_s`, `model_dead_time_s`) plus a fitted cross-gain matrix. Today's tests hand-pick constants. | **This is the most important one.** A model driven by measured plant data is evidence for the owner's "validated before it touches the kiln"; a model driven by invented constants is not. | A new `sim_plant_from_zone_cfg()` (see §6.2) plus a checked-in data file carrying the measured values. |
| **G2** | **No 60 s PWM window.** `sim_kiln_step()` takes a continuous `duty` in [0,1]. | The executor does not deliver continuous duty; `heater_output.c` time-proportions it over `HEATER_DEFAULT_WINDOW_MS` = 60 000. The dwell-entry overshoot and the `steady_rms_c` sub-score are *dominated* by window phase at a 1 Hz tick. Scoring against continuous duty measures a controller the board does not have. | Drive the sim from the **real `heater_output.c`**, linked into the harness — not a reimplementation. |
| **G3** | **No relay actuation lag.** The commanded relay state reaches the element instantly. | This is the exact mechanism that defeated relay-based autotune identification on this bench: 1 Hz switching actuated through a 60 s window, every cycle jittering past the fit tolerance. A model without it cannot reproduce a known real failure. | A fixed transport delay on the *relay state* (distinct from `sensor_delay_s`, which sits on the reading), configurable per zone, default from the bench-measured lag. |
| **G4** | **No MAX31856 quantisation.** `sim_kiln_reading_c()` returns a smooth float plus uniform noise. | Unquantised synthetic input is a bug class this project has already paid for — it hides whole branches while the suite reports green. Two of the three sub-scores (`entry_peak_c`, `steady_rms_c`) are sub-degree quantities where the LSB is not negligible. | Quantise at `MAX31856_TC_TEMP_C_PER_LSB` (`max31856_codec.h`) in the read path, after noise, before return. |

### 6.2 G1 in detail: mapping measured data onto `sim_plant_cfg_t`

The conversion is exact up to one free scale, and must be **written down and
then verified numerically**, never trusted algebraically.

For one zone, `sim_kiln_step()` integrates
`dT/dt = (P·u − h·(T − Ta)) / C`, so its steady-state gain is `P/h` and its
time constant is `C/h`. Given measured `K = model_k_dc` and `τ = model_tau_s`,
pick the free scale `h = loss_coeff_w_per_c = 1.0` and set:

- `heater_power_w   = K`
- `thermal_mass_j_per_c = τ`
- `sensor_delay_s   = model_dead_time_s`, `sensor_lag_tau_s = 0`
  (the identified `τ` already lumps the sensor's own lag; splitting it would
  double-count)

Units become nominal rather than physical — that is fine, because nothing in
the scoring cares about watts, only about the input/output dynamics, which are
then exact.

> **RETRACTED, 2026-09-10 — do not implement the two paragraphs below.** They
> describe `coupling_w_per_c[i][j]` as a conductance on `(T_j − T_i)`
> (temperature-difference exchange), including the algebraic first cut and
> the "sim's coupling also loads zone *j*" both-ways-energy-flow argument,
> and an iterative fit procedure to match that model class to the measured
> cross-gains. That model class was tried and retired:
> `docs/audits/sim_credibility_gate_real_cause_2026-09-10.md` found it
> disagreed with the firmware's own `zone_coupling_solve.c` (additive
> *source-gain*, `diag(k)+coupling_coeff`, energy added to zone *i* without
> being removed from zone *j* — deliberately, matching the firmware's own
> static-gain model) badly enough to fail the sec 6.5 credibility gate
> outright. `d63a5591` changed `sim_kiln_step()` to the additive model and
> removed the iterative-fit function this section describes
> (`sim_kiln_coupling_fit_iterative()` no longer exists). In the additive
> model class the measured cross-gain `coupling_coeff[i][j]` IS the
> `coupling_w_per_c[i][j]` sim_kiln wants, directly — no fit, no iteration,
> no free scale, and no energy-conservation caveat to make (the model was
> never energy-conserving under either class; the additive class doesn't
> pretend to load the other zone in the first place, so there is nothing to
> retract there). See `firmware/KilnFW/App/test/sim_credibility_gate.c`'s
> `ensure_coupling_fitted()` for the current, load-bearing mapping. Anyone
> reading "plan sec 6.2" cited elsewhere (including in
> `sim_credibility_gate.c`'s own comments) for the coupling model should land
> here, not on the paragraphs below.
>
> Original text, kept for history only:
>
> Coupling is harder and is the part that must be **measured inside the sim, not
> derived**. The firmware's `coupling_coeff[i][j]` is a fitted steady-state
> cross-gain (zone *i*'s rise per unit when zone *j* is stepped, with
> `coupling_diag_k_dc` as that identification's own diagonal); the sim's
> `coupling_w_per_c[i][j]` is a conductance on `(T_j − T_i)`. The algebraic
> first cut is `g_ij ≈ h_i · coupling_coeff[i][j] / coupling_diag_k_dc[j]`, but
> the sim's coupling also loads zone *j* (it is a conductance, energy flows both
> ways), so this is only a starting point. **Procedure:** set the first cut,
> then run a single-zone step test *in the simulator*, read off the resulting
> cross-gains exactly the way the bench identification did, and iterate the
> matrix until the simulated cross-gains match the measured ones
> (`[0, 27.32, 21.72] / [14.30, 0, 22.15] / [8.33, 12.42, 0]`) to within 10 %.
> That fitted matrix is checked in as data alongside the measured one, with the
> residual recorded.
>
> One honest consequence to state: the measured matrix is **asymmetric**, and a
> conductance model of the form `g·(T_j − T_i)` is symmetric in form. `sim_kiln`
> permits an asymmetric `g` (its own header says so), so the numbers can be
> matched — but the resulting model is then not energy-conserving. That is
> acceptable for scoring a controller and is **not** acceptable as a physical
> claim about the kiln.

Measured values the mapping is driven from (`tools/PcTools/config_presets/`):

| Zone | `model_k_dc` | `model_tau_s` | `model_dead_time_s` |
|---|---|---|---|
| z0 | 31.96 | 166.9 | 41.1 |
| z1 | 23.48 | 129.1 | 38.1 |
| z2 | 21.74 | 114.8 | 37.2 |

(`tuned_baseline_20260831.json`; coupling matrix from
`coupling_matrix_20260831.json`, adopted `78f2134`, series `813ad90`.)
The loader reads these from a **checked-in JSON snapshot**, not from a
compiled constant table, so the day the board is re-identified the model is
updated by replacing data, and a diff shows what changed.

### 6.3 The harness

Following the pattern `heater_output_pwm_drift_harness.c` already establishes:
**link the real production `.c` files**, do not re-implement them.

Linked as-is: `pid.c`, `pid_fuzzy.c`, `heater_output.c` (this is G2),
`profile_executor_feedforward.c`, `zone_coupling_solve.c`, the new scoring and
comparison modules, and the new `iter_tune.c`. New code is limited to G1–G4
plus the driver loop and the Monte-Carlo runner.

**Deliberate model mismatch is mandatory.** The controller's feedforward
already *assumes* an FOPDT-plus-coupling plant. Scoring the algorithm against
the very model its controller assumes flatters it. Every acceptance run in §7
is therefore a Monte-Carlo over a **mismatched** ensemble: `K` ±30 %, `τ`
±40 %, `L` ±50 %, coupling ±50 %, `radiative_coeff_w_per_k4` swept from 0 to a
value that visibly bends the gain, plus randomised relay lag — while the
*controller* keeps the nominal measured parameters throughout.

### 6.4 What the model still cannot tell us, even with G1–G4 closed

Stated plainly, because a simulation that flatters the algorithm is worse than
none:

- **It cannot validate absolute gain values.** It validates the *decision
  algorithm's statistical behaviour* — false-accept rate, false-reject rate,
  never-worse, termination. Any gain it "finds" is a property of the model.
- **The measured parameters it is driven from were identified at low
  temperature.** The coupled `ff_hold` solve is already known to be infeasible
  above roughly 62 °C, i.e. the identification's own validity is
  temperature-bounded. G1 closes "invented constants"; it does **not** create
  evidence at cone temperature. The radiative term gives the model the right
  *shape* for falling gain with temperature, but its coefficient is not
  measured on this kiln — so high-temperature results are a sensitivity study,
  never a prediction.
- **Its noise floor remains a lower bound.** G4 adds sensor quantisation and
  the existing LCG adds sensor noise, but drafts, ware mass and placement,
  mains voltage variation, element ageing and thermocouple drift are absent
  and their magnitudes on this kiln are unknown. The hardware floor will be
  larger. This is exactly why §3.1 makes the hardware phase *validate* the
  floor and disable Bar 2 if the simulated floor proves optimistic by more
  than 2×.
- **The coupling model is fitted, not physical** (§6.2's asymmetry note), and
  a fitted matrix reproducing three measured cross-gains is not the same as a
  correct heat-transfer model — it will not extrapolate to a different profile
  shape with confidence.
- **It exercises no safety, persistence or UI path.** Guard trips, the safety
  link, NVS schema migration and the HTTP surface are outside it and need
  their own host tests and the bench. (`sim_kiln`'s fault injection can
  provoke guard *inputs*, but the guard wiring itself is not in this harness.)
- **It cannot settle anything below 0.5 °C.** Per the owner's rule, sub-0.5 °C
  effects are reported and not acted on, in the simulator as on the kiln.

### 6.5 The model's own credibility gate

Before any algorithm result is believed, the extended `sim_kiln` must
reproduce a **recorded real firing**. Drive it with the profile and gains from
one of the existing captures in `logs/coupling/*.jsonl` (64 files available)
and require, per zone, over the scored region:

- ramp-phase mean absolute error between simulated and recorded temperature
  ≤ **3 °C**;
- dwell steady-state offset within **1.5 °C** of the recorded one;
- dwell-entry peak within **2 °C** of the recorded one;
- the per-key spread produced by the null experiment to be within **2×** of
  the corresponding entry in the existing six-repeat `noise_floor.json`. The
  model coming out *optimistic* (smaller spread) is tolerated and handled by
  §3.1; coming out *pessimistic* is a failure to investigate, because it means
  the model has a noise source the plant does not.

Held out: fit the coupling matrix (§6.2) against one capture, gate against a
**different** one. If this gate fails, the plan stops here and reports — it
does not proceed to §7 with a model known not to reproduce the plant. Note
that G2 and G3 are load-bearing for this gate: without the PWM window and the
relay lag, the simulated dwell-entry peak has no mechanism to match a recorded
one.

## 7. Acceptance criteria — what "confident" means

All measured over the mismatched Monte-Carlo ensemble of §6.1, ≥ 200 plants,
after the §6.5 credibility gate has passed.

| # | Criterion | Bar |
|---|---|---|
| A1 | **False accept** — null experiment (trial gains *identical* to baseline, only noise differs): fraction of trials returning `ACCEPTED` | ≤ 2 % (hard fail above 5 %) |
| A2 | **Never worse** — final tracking cost vs. starting cost, per sub-score, per zone: fraction of runs ending worse by more than one Bar-1 floor | ≤ 1 % of runs. *This is the criterion that matters most*; a mechanism that cannot improve is acceptable, one that degrades is not. |
| A3 | **Can do something** — from a deliberately detuned start (gains at a cage edge), median improvement in `steady_rms_c` after ≤ 8 accepted trials | ≥ 0.5 °C |
| A4 | **True accept** — where an oracle grid search on the same plant proves a better gain exists inside the cage, fraction of runs that find an improvement within 8 trials | ≥ 60 % |
| A5 | **Termination** — fraction of runs reaching a stopping rule within 15 trials | ≥ 95 % |
| A6 | **Cage** — gains outside `[0.5×, 2×]` commissioned, or outside the absolute bounds, at any tick | 0 occurrences (assert, not a rate) |
| A7 | **Start-point independence** — A1 and A2 re-measured with start temperature randomised over ±15 °C | unchanged within their own bars |
| A8 | **Profile independence** — A1 and A2 re-measured with trial and baseline firings on *different* profiles sharing ≥ 5 segment classes | unchanged within their own bars |

A7 and A8 are the criteria that directly encode the owner's second
requirement; a design that passes A1–A6 but fails A7 or A8 has not been
redesigned, only re-tuned.

---

## 8. Ordered implementation steps

Riskiest last. No hardware exposure before step 8, no heat before step 9.

| # | Step | Risk | Gate to proceed |
|---|---|---|---|
| 0 | This document; supersede the 2026-09-07 brief; ROADMAP row. | none | — |
| 1 | `control/firing_score.c/.h` — pure per-segment scoring (§2.1–2.2): capture-transient and infeasibility exclusion, the three sub-scores, class keying. No wiring, no persistence. Host tests including quantised inputs. | low | host tests green |
| 2 | `control/firing_compare.c/.h` — matched-pair comparator and the accept rule of §3, pure, `n = 0` a first-class outcome. Host tests, including a negative test that breaks the *production* function (restored by hand). | low | host tests green |
| 3 | Extend the **existing** `test/sim_plant.c` with G2 (PWM window, via the real `heater_output.c`), G3 (relay actuation lag) and G4 (MAX31856 quantisation), then G1 — `sim_plant_from_zone_cfg()` loading the measured `k`/`τ`/`L` and the fitted coupling matrix from a checked-in snapshot (§6.1–6.3). Existing `test_sim_kiln.c`/`test_iter_tune.c` users must keep passing: the three new effects default OFF. | medium | **§6.5 credibility gate passes on a held-out capture** — if it fails, stop and report |
| 4 | Null-experiment noise-floor estimation in the simulator; floors emitted as a **data artifact**, not compiled constants. | low | floors within 2× of `noise_floor.json` where the keys correspond |
| 5 | Rewrite `control/iter_tune.c` decision core against the new comparator: cage anchored to persisted commissioned gains, adaptive step, stopping rule, carry limit. Keep the bit-exact revert posture verbatim. Replace `test_iter_tune.c`. | medium | host tests green; the old whole-firing path fully removed, not left dual |
| 6 | Monte-Carlo acceptance run → §7 A1–A8. | medium | **all eight criteria met.** Any miss ends the plan at this line with a report, not a workaround |
| 7 | **Landed, acceptance MET (2 of 2 gaps closed 2026-09-23)** (step 7 review, 2026-09-23, found two acceptance gaps; named exactly below, not glossed over). Persistence + surface: new `iter_tune` NVS namespace + cfg LittleFS dual-write (`iter_tune_store.c/.h`, own `ITER_TUNE_STORE_VERSION`, never `adap_tune`'s namespace), status + "restore commissioned gains" HTTP control (`iter_tune_http.c/.h`, ROUTE_TIER_ADMIN, refuses 409 while autotune owns the zone), `check_iter_tune_write_surface.ps1`/`iter_tune_write_surface_check.py` extended (parses `iter_tune.h`'s real function set rather than a hand-maintained list) to allow the HTTP surface to call only the non-proposing subset, negative-tested. Host tests green (round trip, wrong-version reject, truncated reject, cfg_fs tie-break). **Per-zone opt-in is only a stored field today — nothing in production can set it yet** (no caller flips `enabled`; that waits on step 8 wiring a real producer). **`restore_commissioned` returns 409 on real hardware today and will keep doing so until step 8 produces an anchor/baseline to restore** — there is no path yet that ever sets `has_anchor`/`has_baseline`. Still proposes nothing on hardware. **Acceptance gap (1) CLOSED, 2026-09-23**: the `ui_page_diagnostics.c:1009` format-truncation build break is fixed on `main` (`f7285b9a`); after rebasing onto it, the KilnFW target build now succeeds against this step's code (`check_00_kilnfw_target_build.ps1` PASS). `.dram0.bss` grew from 96856 to 96952 bytes (+96 B) against an `f7285b9a`-only baseline built the same way -- small and expected, all from the new `iter_tune` NVS/cfg_fs blob and its status/restore HTTP handlers; no other section moved. **Acceptance gap (2) CLOSED, 2026-09-23 (schema-migration follow-up pass, REVISED same day after code review REJECTED the first landing)**: `ITER_TUNE_STORE_VERSION` bumped 1->2, byte-compatible (v1's always-zero reserved byte becomes v2's `carry_count`, unused by any writer today). `iter_tune_store_start()` migrates a v1 blob forward **IN RAM ONLY** -- review finding 1 (HIGH) caught that the first landing's eager on-disk re-persist-on-bare-load made a rollback to v1 firmware after that boot lose the whole store (v1's `validate()` only accepts version==1). The on-disk NVS/cfg_fs bytes now stay tagged v1 until `iter_tune_store_set_zone()` performs a REAL write, which always persists the current, already-migrated blob -- so a v1 rollback with no intervening write is fully lossless, and only a rollback after a real post-migration write loses that write, same as any other store in this tree. A version NEWER than this build's `ITER_TUNE_STORE_VERSION` is refused (never partially trusted, same as before) and reported loudly (`ESP_LOGE` plus a new `iter_tune_store_schema_refused()` getter, surfaced on `GET /api/iter_tune/status` as `schema_refused_version`) instead of being silently indistinguishable from a truncated/corrupt blob -- **except a size-CHANGING future version, a documented current limitation** (review finding 3, MED): `note_schema_verdict()` only fires when the on-disk blob is exactly today's size, since a differently-sized blob is already rejected earlier by the NVS/cfg_fs length check. Deliberately NOT a `ZONES_CFG_VERSION` bump — this store never participated in that chain (see this file's and `CONFIG_MIGRATION_CHAIN_PLAN.md` sec 0.1's notes), so none of `ZONES_CFG_VERSION`'s rollback hazard applies. Host tests: `test_v1_old_layout_migrates` (rewritten per review finding 2 to read the RAW NVS/cfg_fs bytes directly, proving the on-disk copy stays v1 through a bare load and only becomes v2 after a real write), `test_newer_version_refused_and_reported`, and `test_larger_blob_size_change_not_reported_current_limitation` (locks in finding 3's documented limitation). Cheap advisories also landed: `_Static_assert`s pinning the blob/zone struct sizes and `carry_count`'s offset (finding 6), a `test_iter_tune_http.c` case for the `schema_refused_version` JSON branch (finding 10), and a comment on the mixed-store-resolution branch noting a refused-newer NVS blob can be overwritten by a valid older file (finding 5). `config_convert.py` (PC-side converter) does not handle this blob family at all — nothing there to extend. | medium | target build + host tests green (2026-09-23); schema migration and its rollback-safety property tested both directions (CLOSED) |
| 8 | **Wired, awaiting five firings.** `control/firing_shadow.c/.h` (pure module, calls no `iter_tune_*` function) hooks `profile_executor_firing_stats.c`'s existing per-tick/per-firing calls -- `firing_stats_zone_tick()` feeds `firing_shadow_zone_tick()` every tick of every zone (zone index recovered from `z`'s address by a `uintptr_t` range compare against `s_exec.zones` -- not `z - s_exec.zones`, which is undefined for a pointer outside the array -- with a silent skip rather than an assert for any `z` outside it, since `test_profile_executor_prestart.c`'s own pre-existing tests legitimately call this function with standalone `zone_runtime_t` locals not part of `s_exec.zones` -- see the comment at that call site), `firing_stats_persist()` calls `firing_shadow_finish_firing()` at firing end. No new HTTP route and no opt-in: every real firing is scored unconditionally, per this step's own description above. A compact verdict-summary blob persists in its own NVS namespace (`shadow_tune`, key `sdwblob`, `kiln_nvs` partition -- never `iter_tune`'s namespace), the in-progress/previous-firing reference is RAM-only. Writes nothing: no gain-write API exists in this module at all. `GET /api/iter_tune/status` surfaces the summary as a top-level `"shadow"` object (`firing_shadow_get_status()`, read-only). Host-tested (`test_firing_shadow.c`): first-firing-has-no-verdict, second-firing-produces-and-persists, reboot-survival of the counter, wrong-version/truncated blob rejection, invalid/out-of-range tick skip, and a structural check that the module's own NVS namespace is never `iter_tune`'s; `test_profile_executor_prestart.c` proves a real `&s_exec.zones[1]` tick reaches the shadow module as zone 1 and a standalone local reaches nothing, and `test_firing_compare_alloc.c` proves each of `firing_compare()`'s three heap allocations fails safe (`NO_MATCHED_PAIRS`) without leaking. Negative-tested (sabotage/restore/rebuild). Awaiting **≥ 5 real firings on hardware** before comparing observed spread to the simulated floor (§3.1) and deciding Bar 2. | medium | observed floor ≤ 2× simulated, else Bar 2 stays disabled and the mechanism runs on Bar 1 alone |
| 9 | **Enable trials on one zone, owner present, bench fixture kiln only** (§9.3), one parameter, cage active, with the operator able to stop and restore commissioned gains at any point. | highest | owner sign-off |

Steps 1–7 need no kiln time at all. Steps 8 and 9 are the only ones that do,
and step 8 spends none of its own — it rides firings the operator was running
anyway.

---

## 9. Owner decisions (settled 2026-09-08)

These three questions were posed as open in the prior revision of this
document. All three are now decided. Each subsection also states which other
part of this plan the decision constrains — §4's cage and stopping rule are
already updated to match; this section is the record of *why*.

### 9.1 Gain anchor — settled: automatic snapshot, re-settable

**Decision:** adopt the plan's own recommendation as written. The cage anchor
("commissioned gains") is captured automatically, with no separate manual
step: the *first* time `iter_tune` is enabled for a zone, the gains active at
that instant are snapshotted and persisted as that zone's commissioned
baseline. The owner's existing hand-tuned values therefore become the cage
centre with no extra action required at rollout. A distinct, explicit
operator action ("re-anchor") lets the owner deliberately move the cage
centre later — e.g. after a fresh hand-tuning pass makes the old anchor
stale — without that being confused with the existing "restore commissioned
gains" revert action (§4, Revert path), which moves gains, not the anchor.
Reflected in §4's "The cage" row.

### 9.2 Trial budget — settled: 6 scored trials per zone

**Decision:** neither the 12-trials-per-zone figure nor single-zone-only
tuning. The owner chose a smaller per-zone budget, covering all three zones
but stopping sooner on each and accepting partial improvement over
convergence. The chosen figure is **6 scored trials per zone** (half of the
originally drafted 12), reflected in §4's "Stopping rule" row in place of the
old "12 scored trials on that zone".

**Why 6, not some other number smaller than 12:** the plan's own statistical
bar (§3, Bar 2) requires `n >= 5` matched paired-difference samples, with the
sign-consistency check needing `ceil(0.75 * n)` of those `n` in agreement.
Six is the smallest round number strictly *above* that floor — it leaves one
trial of margin over the bar's own minimum, rather than landing exactly on
it with zero slack for a trial that scores fewer than the maximum possible
comparisons. Anything at or below 5 would make hitting Bar 2 at all a
knife-edge case dependent on every single trial contributing a countable
comparison; §4's own "not spending kiln time" rule already allows a trial to
go unscored (`n = 0`) and carry over up to 3 times, which by itself can
consume trials from the budget without ever producing a comparison. Note
that this per-zone trial budget is a distinct control from Bar 2's `n`: `n`
counts *matched segment-class pairs* found by comparing two firings, which
can be several even within one trial's before/after pair, while the 6-trial
budget bounds how many separate gain proposals a zone gets before the
stopping rule fires regardless of outcome. The two are related but not the
same count — the 6-trial cap does not by itself guarantee `n >= 5` on every
trial, it only keeps the exercise from stalling out if the owner's floor
(Bar 1) is what ends up doing the work, which is the expected common case
per §3.1 ("refusing to act is a legitimate outcome and is the default one").

**What this permits:** at one trial per firing, coordinate descent still gets
enough trials to run at least one full step-size adaptation (halve after two
rejects, double after two accepts) on both `kp` and `ki` if the schedule
favors one parameter early; it keeps Bar 2 reachable in the common case where
most trials produce a scorable comparison; and across three zones the total
operational cost is roughly **18 firings** (3 zones × 6 trials, one parameter
change per firing), versus roughly 36 under the original 12-trial figure —
consistent with "accepting partial improvement rather than convergence."

**What this forbids:** it does not give a zone enough trials to fully explore
both `kp` and `ki` through multiple step-size halvings each, so a zone is
expected to stop at `CONVERGED` (trial-budget exhausted) having tried only a
handful of proposals per parameter, not to reach the same degree of
refinement the 12-trial figure targeted. It does not change or relax Bar 2's
`n >= 5` requirement itself — a zone whose firings keep landing on `n = 0` or
`n < 5` matched pairs still cannot clear Bar 2 inside 6 trials any more than
it could inside 12, and falls back to Bar 1 (the 0.5 °C floor) plus the
no-degradation veto exactly as §3.1 describes. This budget is independent of
the simulation-only acceptance criteria A3/A4 (§7), which test the
*algorithm's* statistical capability at a fixed 8-trial figure in the
Monte-Carlo harness before anything is deployed; A3/A4 are unchanged by this
decision, since it governs deployed hardware behavior, not the simulator
acceptance bar.

### 9.3 Scope — settled: bench fixture only, revisitable

**Decision, in the owner's words:** *"Bench fixture only for now. Prefer the
situation until you see real success."* Step 9 (§8) is therefore limited to
the bench fixture kiln, and this is a **current boundary, not a permanent
one** — it is explicitly revisitable, not a closed avenue in the sense of
§1.

**What "real success" means, in terms of this plan's own criteria:** the
bench-fixture phase is judged successful once step 9 has run enough on the
bench fixture to show, on real hardware, the same shape of result the
simulator's acceptance criteria (§7) describe in simulation — concretely,
observed shadow-mode spread staying within the §3.1 bound (≤ 2× the
simulated floor, keeping Bar 2 usable rather than falling back to Bar 1
alone), and enabled trials on the bench fixture clearing Bar 1 or Bar 2 with
no instance of A2's "never worse" failure mode (a trial ending worse by more
than a Bar-1 floor) and no cage violation (A6). In short: the bench fixture
needs to demonstrate, on hardware, that the mechanism behaves the way §7
predicted it would — not merely that it runs without crashing.

**Widening beyond the bench fixture is not an incremental step.** Moving
step 9 to a production firing with ware in the kiln requires its own,
separate safety sign-off from the owner — it is not something this plan's
existing step 9 gate ("owner sign-off") already covers, and it is not
triggered automatically by "real success" on the bench fixture. This
document does not attempt to define that sign-off's criteria in advance;
that is deliberately left to be decided when it is proposed.
