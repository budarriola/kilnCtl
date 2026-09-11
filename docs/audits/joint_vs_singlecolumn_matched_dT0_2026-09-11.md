# Matched-ΔT0 comparison: single-column vs. joint dwell, 2026-09-11

Direct test of whether the joint-dwell-only nonlinearity (motivating
`docs/audits/z0_buoyant_coupling_term_proposal_2026-09-10.md`, refuted as a per-source-zone
buoyant-transport mechanism by
`docs/audits/z2_single_column_superlinearity_discriminator_2026-09-11.md`'s four-point single-column
result) is real, by matching z0's induced ΔT between a single-column excitation and a joint
(all-three-zone) dwell and asking whether the linear coupling model, using single-column-derived
coefficients, correctly predicts the joint case. This controls out every ΔT-dependent explanation
by construction — both plateaus reach the *same* z0 ΔT, so any discrepancy is attributable to
*how* that ΔT was produced (one zone vs. three), not to its magnitude.

Board remained at HEAD `a864a610` throughout — **no flash performed**, per instruction, since the
comparison is only valid against the exact binary already verified in the two prior passes.

## Method

**Rested baseline**, re-anchored fresh for this pair (not reused from either prior session): the
board was allowed to cool passively from a prior run (safety TC 43.89°C at handoff) — cooling
followed the same ~200-250s thermal time constant as heating, reaching a rest inside ~35 minutes.
All four thermocouples (z0, z1, z2, safety-processor TC) converged to within ~0.5°C of each other
over the final 10 minutes of cooldown, averaging **30.25°C** — used as the single ambient anchor
for both halves of this matched pair (both plateaus run back-to-back with no intervening
cooldown, consistent with the earlier campaigns' methodology). Room temperature continued a slow
residual downward drift even at the end of the rest window (~0.3°C per 5 min, consistent with the
previously-documented multi-°C session drift) — noted as a limitation below.

**Half 1 — single-column plateau**: zone_mask restricted to z2 alone, closed-loop user profile
(borrowed slot #6, restored after the run and confirmed by read-back to its pre-run content
`cpl_z2` target=55C ramp=120C/hr dwell=35min), target 52.0°C chosen from z2's own fitted DC gain
(34.78 C/duty) to land near a moderate, safe duty. Settled (flat duty AND flat temperature across
≥10 minutes, never elapsed time alone) by dwell-elapsed ~975-1285s. Final-10-minute average (3
polls): z2 duty converged to **0.613**, z0 reached **43.44°C** → **ΔT0 = 13.19°C**.

**Half 2 — joint plateau**: zone_mask = all three zones, all three targeting the SAME absolute
temperature as z0's single-column result (43.44°C) — a standard joint-dwell configuration. Chosen
specifically so the PID's own closed-loop convergence would reproduce z0's matched ΔT without
needing to guess duties in advance. Settled by dwell-elapsed ~931-1555s. Final-10-minute average
(3 polls): duties converged to **u0=0.163, u1=0.21, u2=0.247**; z0 reached **43.53°C** → **ΔT0 =
13.28°C**.

**Match quality: ΔT0 differs by 0.09°C between the two conditions (13.19 vs 13.28)** — well inside
thermocouple noise (~0.1-0.2°C at a flat plateau) and far under the 0.5°C threshold. The design
target — matching z0's induced ΔT while changing only how it was produced — was achieved cleanly.

Full numbers: `docs/audits/joint_vs_singlecolumn_matched_dT0_2026-09-11.tsv`. Raw poll log:
`logs/coupling/z2_step_2026-09-11.jsonl` (git-ignored, local only).

## The comparison: does the linear model predict the joint duties?

The linear coupling model is `ΔT0 = c00*u0 + c01*u1 + c02*u2`, where `c0j` is z0's response per
unit of zone j's duty. `c02` (z2's column) was independently measured this week across four
single-column plateaus (`z2_single_column_superlinearity_discriminator_2026-09-11.md`): mean
**22.15 C/duty**, consistent with the board's own on-record value (21.52). `c00` (42.73) and `c01`
(25.42) are the board's existing on-record values from `control_get_zones` — **not independently
re-measured this session** (that would require separate z0-alone and z1-alone single-column
campaigns, out of scope for this pass — see Limitations).

Plugging the joint plateau's observed duties into the model:

```
ΔT0_predicted = 42.73*0.163 + 25.42*0.21 + 22.15*0.247
              = 6.97 + 5.34 + 5.47
              = 17.77°C
```

(Using the board's own, slightly lower, `c02=21.52` instead of this week's fresh 22.15 changes
this to 17.62°C — the choice of `c02` value does not materially affect the conclusion.)

**Actual observed ΔT0 was 13.28°C. The model over-predicts by 4.3-4.5°C, roughly 33%.**

## Ruling out the ambient-reference hypothesis by magnitude

The coordinator's first candidate — that a joint dwell's self-heating of the surrounding
reference could account for the discrepancy via the documented ~0.035 cross-campaign ambient
term — is too small by an order of magnitude to explain this gap even taken at face value: 3.5%
of the 13.28°C matched ΔT0 is 0.46°C, comfortably under the 0.5°C not-worth-chasing threshold and
roughly a tenth of the observed 4.3-4.5°C discrepancy. **The ambient-reference explanation is
ruled out as the dominant cause; at most it could be a small contributor.**

## Verdict: a real discrepancy, direction is a genuine interaction/saturation signature — but attribution is NOT fully resolved

**A discrepancy well above measurement uncertainty and the 0.5°C floor was found: ~4.3-4.5°C
(~33%) at matched ΔT0.** This clears the bar the coordinator set for "worth reporting as a
finding." Its direction is informative: the linear model, built from single-column columns,
*over*-predicts z0's temperature rise given the joint duties actually used. Equivalently: reaching
a given z0 ΔT jointly required *more* combined duty than the single-column-derived model would
predict — the system delivered *less* effective heating jointly than the sum of its single-column
parts. This is the signature of candidate 2 (**interaction or saturation in the simultaneous
solve** — consistent with the already-known finding that the hold solve becomes infeasible above
~62°C) rather than candidate 3 (a joint-only recirculation *boost*, which would show the opposite
sign — less duty needed, not more). It is also the **opposite sign** from the original joint-hold
study's high-ΔT (62-75°C) finding, where z0 needed *less* of its own duty than the linear model
predicted (a boost, not a deficit) — worth flagging explicitly rather than glossing over, since
it means whatever is happening at this moderate ΔT0 (13°C) is not simply the same effect the
buoyancy proposal was built to explain at high ΔT (60-75°C absolute chamber temperature); this
run's absolute temperatures (43-44°C) are well below that regime.

**This does not cleanly attribute between "real joint-load interaction effect" and "stale
c00/c01 priors" as the source of the residual.** `c00` and `c01` are the board's existing
on-record coupling-matrix entries, not independently re-measured this session — and they were
fit by some prior process that may itself carry the same joint-dwell-specific bias under
investigation here (a form of the same circularity flagged when this audit series began: the
board's own matrix is the object being tested, so using two of its three entries as ground truth
to test the third is not fully independent). A single joint data point with two of three
coefficients held at unverified priors cannot separate "the model's c00/c01 are simply wrong" from
"joint loading genuinely changes the effective coupling." Both would produce exactly the observed
symptom (predicted ≠ actual at matched ΔT).

**What would resolve this**: independent single-column campaigns driving z0 alone and z1 alone
(this week's method, proven twice), to get freshly-measured `c00` and `c01` under the same
conditions as this week's `c02` — then repeat this matched-ΔT comparison with all three columns
independently verified. That is a further 2-3 hours of bench time (two more single-column
campaigns at minimum), not attempted in this pass.

## Limitations

- Only one matched pair was run, at one ΔT0 level (13.2-13.3°C). The coordinator noted a second
  pair at a different level would show whether any discrepancy grows with level or is roughly
  constant — not attempted here due to time.
- `c00` and `c01` are unverified priors, as discussed above — the single largest caveat on this
  result.
- Ambient continued a slow residual drift (~0.3°C/5min) even at the end of the pre-run rest
  window; the ~30.25°C anchor is a best estimate, not a hard plateau.
- This ΔT0 (13°C, absolute temperatures 43-44°C) is well below the original study's 62-75°C
  regime; whether the same or a different mechanism operates at higher joint ΔT is untested here.

## Board state at end of run

Both plateaus were stopped with `profiles_stop()`, producing **no panic either time** —
`get_heap_status` showed `reset_reason='software (esp_restart)'` (the flash's own reset, from the
prior day's `flash_firmware()` call) with a continuously-incrementing `uptime_s` (17800s at final
check, no intervening reboot across this run or either prior one). Relays confirmed off two ways
(`io_read`'s `R1=R2=R3=R4=0` and `get_board_state.io.relays=0`). Executor idle
(`profiles_get_exec_status.state=0`), no trip latched (`safety_get_status`: armed, not tripped),
crash report checked and `present=false`. Safety-link `timeouts` stayed flat at 9 total across the
entire run while `sent` grew from ~7355 (session start) to 8481 — the new push-throughput
accounting continues to hold. Borrowed profile slot #6 restored to `cpl_z2` (target=55C
ramp=120C/hr dwell=35min) and confirmed by read-back. z0's peak during this run was 44.0°C, z2's
peak 52.95°C — both far under the 75°C working maximum / 78°C abort threshold. No flash was
performed; the board remains at HEAD `a864a610`, the same binary verified in both prior passes.
**The board is safe and idle.**

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
