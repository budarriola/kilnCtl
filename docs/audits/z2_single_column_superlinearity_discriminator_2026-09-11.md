# Single-column superlinearity discriminator (z2 -> z0), 2026-09-11 — NULL RESULT

Runs the cheap experiment proposed in `docs/audits/z0_buoyant_coupling_term_proposal_2026-09-10.md`
(`54723b79`) §5: drive one bottom zone alone at two duty levels, roughly 0.30 and 0.60, and compare
z0's induced ΔT-per-duty at each. A flat ratio refutes the ΔT-superlinear buoyant-transport term
proposed there; a ratio that grows with duty supports it.

Board was flashed to HEAD `a864a610` immediately before this run (see the companion flash/verify
report for that pass) — the fix for the recurring `profile_executor` stack-overflow panic
(`docs/audits/profile_executor_panic_2026-09-10_root_cause.md`) was live on the board throughout.

## Method

Single-zone excitation via a **user profile** (never a builtin), zone_mask restricted to z2 only,
closed-loop PID rather than `autotune_start(method="step")`: the step-autotune path was tried
first and rejected — it self-declared `model_settled=True` at 581 s (elapsed), far short of the
~2000 s (8τ, τ≈247-259 s) needed even for z2's own settle, and nowhere near enough for z0's slower
cross-zone response (τ 620-730 s per `coupled_ident_single_zone`'s own docstring). This is the
exact false-early-settle failure mode `coupled_ident_settle_audit` was built to catch (slope-only
criterion, no duty-stability check). A closed-loop profile targeting a fixed z2 temperature does
not self-terminate and lets duty converge to whatever steady value holds that temperature — read
back from telemetry, not commanded — which is what "duty level" means throughout this report.

Target temperatures were chosen from z2's own measured DC gain (`coupling_diag_k_dc[z2]=33.85
C/duty` from `control_get_zones`) to land near duty 0.30 and 0.60: 42.7°C and 52.9°C. Actual
converged duty came in at 0.237 and 0.547 — close to, not exactly, the design targets, which is
fine since the discriminator compares the two *observed* ratios, not the two nominal setpoints.

Borrowed slot #6 (`cpl_z2`, target=55C ramp=120C/hr dwell=35min) was overwritten with the two-level
schedule, then restored byte-for-byte after the run, confirmed by read-back:
`profiles_get(6)` -> `#6 'cpl_z2' zone_mask=0x4, segment 0: target=55.0C ramp=120.0C/hr dwell=35min`
— matches the pre-run content exactly.

**Rested baseline**, verified against an absolute anchor independent of `is_rested()`: all four
thermocouples (z0, z1, z2, and the safety processor's own independent TC) agreed within 0.2°C
before the run — 32.70, 32.70, 32.49, 32.59°C — taken as the ambient anchor, 32.6°C.

Each plateau's "settled" call required flat duty AND flat temperature across at least two
consecutive 5-minute polls (not elapsed time alone): plateau 1 settled by dwell-elapsed ~950-1260s
(z0/z1 moved <0.1°C between checks), plateau 2 by dwell-elapsed ~920-1550s. Each plateau's reported
average is the mean of 3 polls spanning the final ~10.4 minutes of dwell.

Duty logged is the **commanded/converged PID duty from `profiles_get_exec_status`'s per-zone
`duty` field**, which reports the intended duty computed upstream of PWM chopping (confirmed this
is not raw relay state: the `relay=on/off` field toggles between polls at fixed `duty`, i.e. the
chopper's instantaneous relay state, while `duty` itself stays flat).

Raw poll log: `logs/coupling/z2_step_2026-09-11.jsonl` (git-ignored per `.gitignore:100`, kept
local only).

## Results

| plateau | duty (z2, converged) | z0 avg (°C) | z1 avg (°C) | z2 avg (°C) | ΔT0 = z0−ambient (°C) | ΔT0/duty |
|---|---|---|---|---|---|---|
| level 1 | 0.237 | 37.98 | 38.68 | 42.68 | 5.38 | **22.7** |
| level 2 | 0.547 | 44.89 | 45.73 | 53.01 | 12.29 | **22.5** |

Full numbers: `docs/audits/z2_single_column_superlinearity_discriminator_2026-09-11.tsv`.

**The two ratios agree to within 1% (22.7 vs 22.5).**

## Measurement uncertainty

- Thermocouple read noise at a genuinely flat plateau: ≤0.1°C peak-to-peak across consecutive
  5-minute polls (e.g. level 2's z0 samples were 44.89/44.97/44.91).
- Ambient-anchor uncertainty: this run was ~1.5 hours end to end (not the 12-hour session where
  ~3°C room drift was observed), so drift over this run is expected to be a small fraction of
  that — estimated ≤0.2°C, not independently re-measured at a second rest point (see Limitations).
- Duty read precision: PID dither on a converged dwell was ≤0.03 (level 1: 0.22-0.25; level 2:
  0.53-0.57).
- Propagating these onto ΔT0/duty: level 1 (smaller ΔT, more duty-sensitive) is 22.7 ± ~2.0
  (≈9%); level 2 is 22.5 ± ~0.9 (≈4%).

The observed difference (0.2, i.e. ~1%) is far inside level 1's own ±9% uncertainty band. **This
is a flat ratio within measurement error, not a marginal or ambiguous one.**

## Verdict: NULL RESULT — refutes superlinear buoyant transport in the tested range

A convex (γ>1) buoyant source term predicts a ΔT0/duty ratio that *grows* with duty/ΔT — that is
the entire point of the proposed 4/3-power law. Measured flat instead. Per the effects-below-0.5°C
guidance, the observed 1% (0.2 out of ~22.6) difference is not just below the 0.5°C threshold
mentally, it's below it by an order of magnitude in the units that matter here (ΔT-per-duty units,
not raw ΔT) — there is no effect here worth chasing, and no basis in this data for adopting the
buoyant term.

## Limitations — this null result is scoped, not universal

- **ΔT range tested (5.4-12.3°C total z0 rise) is well below the range where the original
  sign-flip was observed** (`z0_buoyant_coupling_term_proposal_2026-09-10.md` §2's four plateaus
  spanned ~11.7-75°C *joint* ΔT, i.e. all three zones hot together, not one zone driving alone).
  This experiment shows no superlinear trend in a low-to-moderate single-column ΔT band; it does
  **not** directly test the higher-temperature joint-dwell regime (60-75°C) where the linear
  model's residual crossed zero. The two regimes are not the same measurement (single-column vs.
  joint hold), so this null result narrows the case for the buoyant term without fully closing it.
- Ambient was anchored once, pre-run, not re-measured at a second independent rest point — the
  ≤0.2°C drift estimate is a bound based on run duration, not a direct second measurement.
- No coupling matrix or plant term was persisted to the board — this was identification only, per
  the task brief.

## Board state at end of run

`profiles_stop()` was called and produced **no panic** — `get_heap_status` showed
`reset_reason='software (esp_restart)'` (the flash's own reset) and a continuously-incrementing
`uptime_s` (5625 s) with no intervening reboot, confirming the `a864a610` stack-overflow fix holds
at exactly the failure point that panicked this board twice before. Relays confirmed off two ways
(`get_board_state.io.relays=0` and `io_read`'s `R1=R2=R3=R4=0`). `profiles_get_exec_status.state=0`
(idle), no trip (`safety_get_status`: armed, not tripped), crash report cleared and re-checked
`present=false`. Safety-link `timeouts` stayed at 9 total across the entire ~80-minute run while
`sent` climbed from ~260 to 4289 — confirms the new push-throughput accounting does not track
`sent` even under sustained load. Board is safe and idle.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
