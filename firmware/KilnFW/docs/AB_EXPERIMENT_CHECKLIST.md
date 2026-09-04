# A/B Experiment Checklist (control-law campaigns)

Pre-flight checklist, not a tutorial. Run it in order before spending kiln
time. Background and full derivations live in `PID_EXPANSION_PLAN.md` §3.6b
(worked example, tooling) and §3.6d (the day this list got rewritten).

## 1. Before writing the presets: find every gate, not just the parameter

The varied field must actually reach the control path, unattenuated by a
switch above it.

- List every mode/enable flag, commissioning-state check, and guard window
  between "preset value" and "actuator output" — not just the parameter under
  test. Trace the consumer function, don't assume.
- **Worked counter-example:** two fuzzy-PID campaigns shipped with both
  presets carrying `control_mode: 2` (`ZONE_CONTROL_MODE_PID`); the fuzzy
  layer only runs under mode 3. Both campaigns compared the controller
  against itself and were reported as verified because the live readback
  checked `fuzzy_strength_pct` (the field of interest) rather than
  `control_mode` (the field that actually gates it). See §3.6, "control_mode:2"
  bug, and the audit in §3.6b.
- Checking that the config readback shows your new value is **not** this
  step. A readback proves the preset was applied, not that anything
  downstream cares.

## 2. Before spending kiln time: live reachability proof

With a profile RUNNING (not just configured):

1. Apply arm A, sample `GET /api/control`'s `duty_breakdown` (`bd_*` fields)
   a few times live.
2. Apply arm B, sample again.
3. Confirm the sampled value differs between arms **and** varies with the
   input it's supposed to key on (e.g. moves as the error sign crosses zero,
   or as the ramp progresses) — not just "arm A's number != arm B's number
   once."

After the fact, against captured `.jsonl` logs (requires `capture_control_bd`,
see §5):

```
python -m kilnctrl.bd_reachability_check ARM_A.jsonl ARM_B.jsonl \
    --field bd_kp_effective --field bd_ki_effective --field bd_kd_effective
```

Exit 0 REACHABLE = genuinely differs; exit 1 INERT = bit-identical across
arms (the fuzzy-PID signature); exit 2 = refuses rather than guess when the
capture predates `capture_control_bd`.

For the whole campaign at once, not one pair by hand:

```
python tools/PcTools/scripts/fuzzy_ab_analyze.py \
    --log-dir logs/coupling --prefix <campaign_prefix> --pairs N
```

It runs `bd_reachability_check` first per pair (gating — an INERT pair is
excluded before its tracking numbers are even computed), then
`pid_ab_compare`, then applies the `>=3`-key decision rule.

**Stop the proof-firing before it runs long enough to matter thermally** —
this is a reachability check, not the campaign itself.

## 3. Choosing the probe field: verify it can discriminate, on real data, first

Not every field that looks like it should move actually does.

- **Counter-example:** a proposed pre-flight probe compared
  `bd_ff_rate_pretaper` against `bd_ff_rate_posttaper`. Those two fields are
  **equal on 96.5% of samples** — spot-sampling them would have reported a
  genuinely working arm as inert. The fix in that case was to compare
  **taper onset time** across the continuous capture instead of spot values
  (§3.6d, "Second pass").
- **Rule:** before the campaign, pull a field from an existing capture (or a
  short live poll) and confirm it actually separates two known-different
  configurations. Do this check *before* committing to that field as the
  campaign's reachability probe, not after the campaign fails to show an
  effect.

## 4. Power and design

- Compute required n from the **relevant within-config SD**, not a guess.
  Per-zone measured noise floors on this rig: z0 0.116 °C, z1 0.077 °C,
  z2 0.147 °C.
- The owner's actionable bar is **0.5 °C** — an effect has to clear this,
  not just be statistically distinguishable from noise.
- **Counter-example:** a proposed campaign ran 3 runs/arm, which gave ~47%
  power at the pooled within-config SD (0.312 °C) — a coin flip, not
  evidence. Prefer the stock profile (SD as low as 0.04-0.06 °C, >99% power
  at n=3) over a stabilized/modified profile with higher baseline variance,
  or use n=5/arm if you must use the noisier profile.
- **Keep baseline and treatment on the SAME profile.** The same proposal's
  expected-effect arithmetic mixed a stock-p7 baseline (2.2 °C) with a
  stabilised-p7 campaign's own baseline (1.93 °C) — two different profiles'
  numbers subtracted as if comparable.
- **"Same profile" includes stabilisation-hold status, not just the
  profile ID.** (2026-09-04, `PID_EXPANSION_PLAN.md` noise-floor
  re-derivation section.) `run_queue.py` has prepended a 40 °C/45 min
  stabilisation hold onto every queued profile by default since
  2026-09-03 (`--skip-stabilization-hold` is the opt-out); scoring then
  starts at `min_segment_index: 1`, past the hold. Pooling
  `ab_new_{1,2,3}` (bare 2-segment profile 7, no hold, no `meta` record)
  with `easeoff_ab_20260904_2p0_run{1,2,3}` (3-segment stabilised profile
  7, `meta: {stabilized: true, min_segment_index: 1}`) as "same nominal
  config, current matrix, stock ease-off" looked reasonable by profile ID
  and preset name alone, but made the pooled `iae_normalized_whole_c`
  range jump from ~0.06 (each triplet alone) to ~0.25 °C — an apparent
  hidden between-campaign confound that was actually just two different
  profiles under one nickname. Before pooling or comparing runs across
  campaigns for noise-floor or A/B purposes, check the run's own capture
  for a `meta.stabilized`/`meta.min_segment_index` record (or the
  campaign's `run_queue.log` for the "prepending a stabilisation hold"
  line) — don't rely on the profile ID or campaign name matching.
- **Use untreated zones as negative controls.** A zone-scoped intervention
  (e.g. z0-only) should show the effect in the targeted zone and NOT in the
  others; pre-register that expectation.
- `CONSISTENT_PATTERN_MIN_KEYS = 3` counts **(zone, metric, segment)** keys,
  not zones — a single-zone intervention can clear the bar entirely within
  that one zone, across metrics/segments, with the other zones serving as
  negative controls rather than needed for the count.

## 5. Capture requirements

- `capture_control_bd` is **on by default** in `run_queue.py` since `a62e14e`
  (pass `--no-capture-control-bd` only for a campaign that genuinely does not
  need it). Cost: ~371 B/zone per poll, ~7.9 MB/arm for a typical campaign —
  budget for it.
- Capture the fields your reachability check and your probe need, not just
  the metric you expect to change. A campaign whose own data cannot prove it
  was reachable after the fact (`ease_off_window_mult`'s original captures
  kept only `exec`/`status` snapshots, not `bd_ff_rate_pretaper`/
  `posttaper`) forces re-deriving reachability from source code instead of
  from evidence — always possible, never as strong.
- The rule: **the campaign's own captures must be able to prove the
  campaign's own validity.** If asked "was this reachable" a week later, the
  answer should come from the `.jsonl`, not from re-reading the firmware.

## 6. After the run

- **Completeness gate.** Each arm's last recorded `exec.state` must be
  terminal (`done`/`faulted`), cross-checked against the runner's
  `<prefix>_state.json` when present (capture's own data wins on
  disagreement — a `"completed"` state-file entry next to a non-terminal
  last row is a stale claim). A short/truncated arm compared against a
  complete sibling produces a large *apparent* difference that is pure
  artifact. `pid_ab_compare.compare_runs` checks this automatically; do not
  pass `--skip-completeness-check` in a real campaign.
- **Per-zone reachability gate.** A partially-inert pair (reachable in some
  zones, not others) can still cast votes from zones where the treatment
  never ran, inflating the `>=3`-key count with zeros. Check reachability
  per zone, not just per pair, before counting keys.
- **Multiplicity caveat.** An unreplicated (n=1, or single-pair) result is a
  candidate for a follow-up campaign, not a conclusion — say so explicitly in
  the writeup rather than letting a clean-looking single number read as
  settled.

## 7. Known dead ends — do not re-run

- **`ease_off_window_mult` 2.0 vs 3.0** — indistinguishable, re-validated
  (`51e3d59`); the knob only shifts *when* within the ramp's final
  dead-time multiples the taper starts, not whether one exists. A follow-on
  z0-only 3.5× variant was independently withdrawn (§3.6d, "Third pass") —
  `zone_taper_climb_rate()` scales only the feedforward climb term, never
  the PID/coupling terms or the plant's actual state, and by the sampled
  window z0's own duty has already crashed to 0.05-0.10, leaving nothing for
  a wider window to remove.
- **Dwell-entry climb decay** — reverted; worsened overshoot on all zones and
  tripped guard 2 (`project_dwell_entry_climb_decay_failed`).
- **Load estimator** — not observable from these captures; all three zones
  closed negative (`project_load_estimator_cannot_observe_load`).

## Reference: what these lessons cost

- Structurally inert campaign (both control_mode:2) — §3.6, audited §3.6b.
- No evidence trail for a valid campaign (`ease_off_window_mult`,
  `a62e14e` added the fix) — §3.6b.
- Wrong reachability probe (pretaper/posttaper 96.5% equal) — §3.6d,
  commit `68711df`.
- Underpowered/mismatched design (3 runs/arm, ~47% power, mixed baselines)
  — §3.6d, commit `68711df`.
- Partially-inert pair casting votes from untreated zones — `f99c350`.
- Truncated arm compared against a complete one — `3dab6c0`.
