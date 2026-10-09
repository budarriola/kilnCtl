# Section 8 dwell-length fix: what it bought, and why gate 1 still fails (2026-09-16)

**Status: gate 1 still FAILS. This is a follow-on to
`adaptive_fuzzy_section8_campaign_2026-09-16.md` (`9e0485c9`), not a
replacement for it — read that document first.** That run diagnosed gate 1's
failure as a dwell-length problem: the factorial's per-cell dwell was sized
at roughly `6*tau`, while `adaptive_tune`'s harvest gate needs a settled
window on that order before confidence can move. This session lengthened
the dwell and re-ran the campaign. The fix produced a real, measured
improvement, but the diagnosis was only partially right — a second,
independent structural cause caps activity well under the plan's 30%/10%
bars regardless of dwell length, and this document identifies it.

## What was verified before changing anything

`adaptive_tune`'s harvest gate, read directly from
`firmware/KilnFW/App/drivers/control/adaptive_tune_internal.h` and
`adaptive_tune.c` (not taken on trust from the prior document):

- `ADAPTIVE_TUNE_SETTLE_MIN_S = 180.0f` — a dwell observation is only
  recorded once `settle_elapsed_s >= 180s` past the tick where the
  temperature slope first drops below `ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S
  = 0.003f` C/s (`adaptive_tune.c:372-377`).
- `ADAPTIVE_TUNE_MIN_OBSERVATIONS = 4u` and `ADAPTIVE_TUNE_MIN_DUTY_SPREAD =
  0.05f` (`adaptive_tune_internal.h:204,206`) — `adaptive_tune_refine_zone_locked()`
  refuses outright below either bar (`adaptive_tune_model.c:39-58`).

The factorial's per-cell dwell, `sim_factorial_driver.c:556` (pre-fix):
`dwell_ticks = (int)(6.0f * model_tau_s / DT_S)`, clamped to `[1200, 20000]`
ticks at `DT_S = 1.0f` — i.e. dwell length in seconds is `6*tau`, clamped to
`[1200s, 20000s]`. At bench `tau ≈ 255.6s` that is `1534s`; at kiln `tau ≈
488s`, `2928s`. Both numbers are comfortably larger than the 180s settle
requirement in isolation — the campaign document's "6*tau vs 16*tau" framing
was directionally right (lengthening dwell measurably helps, see below) but
understated how much of a `6*tau` dwell a badly-mismatched cell's own
transient and residual oscillation can consume before the slope gate is
ever satisfied, and — as the new evidence below shows — dwell length is not
the whole story.

## The dwell-length fix

`sim_factorial_driver.c`'s dwell formula was split so the three
single-firing arms (`ad == NULL`) are byte-for-byte unchanged, and the two
adaptive-chain arms (`A_PID_AT` / `A_FUZZY_AT`, `ad != NULL`) share one
lengthened formula between them (preserving gate 2's firing-1 bit-identity
requirement, since both read the same branch):

```c
if (ad) {
    dwell_ticks = (int)(28.0f * model_tau_s / DT_S);
    if (dwell_ticks < 7000)  dwell_ticks = 7000;
    if (dwell_ticks > 90000) dwell_ticks = 90000;
} else {
    dwell_ticks = (int)(6.0f * model_tau_s / DT_S);   // unchanged
    if (dwell_ticks < 1200)  dwell_ticks = 1200;
    if (dwell_ticks > 20000) dwell_ticks = 20000;
}
```

No change was made to the builtin schedule table, `target_c`,
`ramp_c_per_hr`, `dwell_min`, `segment_count`, or any bench-span physical
parameter — only the adaptive arms' own dwell-per-firing length.

A first attempt at `16*tau` (clamp `[4000, 60000]`) was tried and measured
before settling on `28*tau`:

| dwell multiplier | activity (bar: ≥30%) | differs-from-control (bar: ≥10%) |
|---|---|---|
| `6*tau` (original) | 14/260 = 5.4% | 5/260 = 1.9% |
| `16*tau` | 72/260 = 27.7% | 14/260 = 5.4% |
| `28*tau` (adopted) | 75/260 = 28.8% | 15/260 = 5.8% |

The jump from `6*tau` to `16*tau` was large; the further jump from `16*tau`
to `28*tau` (nearly double again) moved the numbers by barely one point.
That flattening is the signal that dwell length was not the only thing
capping activity — it was investigated directly (below) rather than pushed
further on the assumption that a still-longer dwell would eventually clear
the bar.

## The second cause: most cells have nothing to learn, or can never settle

Cross-referencing the `28*tau` run's `ADAPTIVE_DIAG` rows against each
cell's `a6_tune` mismatch category (`A_PID`'s own row for that cell) shows:

- **142/260 cells (54.6%) are `a6_tune=MATCHED`** — the belief model's
  `K_dc` already equals the true plant gain. For these, `adaptive_tune_refine_zone_locked()`
  correctly harvests dwell data (ring fills) but then refuses via its own
  `ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC` check — the fitted gain barely
  moves from the current one, so it is refused as "not a material change,"
  and `fuzzy_confidence_c` stays at 0 forever. This is correct behavior, not
  a bug: a matched cell has no gain error for adaptation to find, so it
  should never activate. No dwell length fixes this, because there is
  nothing to harvest a *correction* from.
- Of the remaining **118 mismatched cells, 105 are still stuck at
  `confidence_c_after=0`** even at `28*tau`. Sampling those rows
  (`ST1-004..007`, `HOT`, `belief_k_dc=21.37` vs `true_k_dc=42.73`, roughly
  2x off) shows `oscillation_tripped=yes`, `dwell_zero_crossings` in the
  150-250 range per firing, and **`ring_count=0`** — the dwell error never
  damps below the 0.003 C/s slope floor at all, so no observation is ever
  recorded regardless of how long the dwell runs. This is also arguably
  correct: harvesting a gain estimate from data that is still oscillating
  would poison the fit, and the settle gate exists precisely to prevent
  that. Lengthening the dwell cannot fix a residual that never converges.

So the ~30% activity ceiling this run is bumping against is largely
structural to the factorial's own mismatch distribution (54.6% of cells
built with zero gain error) and to `adaptive_tune`'s deliberate refusal to
learn from an unsettled dwell (a design choice worth keeping, not a defect
to work around) — not primarily a dwell-length artifact. The dwell-length
fix genuinely helped the fraction of cells that were both mismatched *and*
capable of settling (it went from single digits to double digits), but a
30% activity bar assumes a materially larger population of "mismatched and
recoverable" cells than this factorial's mismatch table (`a6_tune`
distribution: 142 `MATCHED`, 112 `HOT`, 3 `COLD`, 3 `SLOW_INTEGRAL`) can
ever supply.

## Re-run results (28*tau, fresh build)

Built from scratch in a clean detached worktree at `origin/main`
(`c25e70b0`) under `C:\wt\af8dwell`, `run_sim_factorial.ps1 -Shards 4` (no
`-SkipDeterminism`).

- **Determinism: PASS.** `--of 1` and `--of 4`: 10,140 rows, byte-identical.
- **GATE1_FAIL** — activity: 75/260 cells (28.8%) reached `strength_pct > 0`,
  against the 30% bar (up from 5.4%); 15/260 (5.8%) differ from `A_PID_AT`
  at firing 9 by >0.5°C on any gate objective, against the 10% bar (up from
  1.9%). Per-objective: `STEADY_RMS_C` 0, `ENTRY_PEAK_C` 10, `LAG_SIGNED_C` 7
  (`ENTRY_UNDERSHOOT_C` 8 reported, not gated).
- **GATE2_PASS** — 260/260 cells, floor identity intact.
- **GATE3_FAIL** — same seven pinned cells, same shape as the original run:
  `A_FUZZY_AT` crossing counts (9, 729, 9, 693, 9, 9, 675) are **identical**
  to `A_PID_AT`'s on every one of the seven — the gate's own pre-existing
  objection (it cannot separate a fuzzy-induced limit cycle from ordinary
  adaptive-PID dwell settling) still applies unchanged.
- **INTEGRITY_FAIL** — same 3 disclosed kiln-span refusals as before
  (`4891a6fb`), not a new defect.

## Section 8 tally (reported for completeness, gate 1 still failing)

Per the plan's own rule, this is not tallied as a verdict — gate 1 has not
passed. Computed the same way as the original campaign document (`D_adapt_combo`
over `STEADY_RMS_C`, `ENTRY_PEAK_C`, `LAG_SIGNED_C`, firing 9 vs firing 9,
0.5°C materiality floor):

| Subgroup | cells | improved | degraded | improved-only | degraded-only | within-floor |
|---|---|---|---|---|---|---|
| All 260 | 260 | 3 | 7 | 3 | 7 | 250 |
| 151 bench-span | 151 | 0 | 0 | 0 | 0 | 151 |
| 109 kiln-span **[EXTRAPOLATION]** | 109 | 3 | 7 | 3 | 7 | 99 |

Bench-span (the only hardware-corroborated regime) still shows **zero**
cells with any difference at all, same as the original run. The small
signal that does exist is entirely in kiln-span extrapolation territory,
same as before, and remains too small and one-sided a sample to read as
evidence either way.

## `run_all_checks.ps1`

Run in the same clean worktree (`C:\wt\af8dwell`, after `tools/setup.ps1` to
generate the gitignored `PcTools` venv): **95 passed, 0 skipped, 0 failed.**
`run_sim_factorial.ps1` remains outside this suite, as before.

## Recommendation

**Still not decisive; do not treat this as evidence for keeping or removing
adaptive fuzzy.** The dwell-length fix was worth making (it is a real,
measured improvement and a legitimate correction to an under-sized
factorial parameter, not a hack chasing the bar), but it is not, by itself,
enough to make section 8 interpretable under the plan's registered bars.
The remaining gap is not a knob this driver can tune further — it is a
question about the factorial's own mismatch-cell mix (currently
majority-`MATCHED`, which by construction can never activate adaptive
fuzzy) and about whether the plan's 30%/10% bars were set assuming a richer
population of "mismatched and recoverable" cells than a 263-cell factorial
built primarily to also serve the three original single-firing arms
actually contains. That is a plan-amendment question for the owner:
either accept a bar recalibrated against the *mismatched-and-settling*
subpopulation specifically, or grow the mismatched fraction of the design
(a change to `sim_factorial_design.c`'s own generation, out of scope for
this session, which was told not to touch the builtin schedule table but
also had no mandate to redesign the mismatch distribution).

Gate 3's standing objection (raised in both the original campaign and the
2026-09-14 progress note) is unchanged by any of this and still needs the
owner's attention on its own terms.
