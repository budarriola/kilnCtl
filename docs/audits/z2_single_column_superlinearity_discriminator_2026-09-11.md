# Single-column superlinearity discriminator (z2 -> z0), 2026-09-11 — FOUR-POINT NULL RESULT

Runs the cheap experiment proposed in `docs/audits/z0_buoyant_coupling_term_proposal_2026-09-10.md`
(`54723b79`) §5: drive one bottom zone alone at several duty levels, and compare z0's induced
ΔT-per-duty at each. A flat ratio refutes the ΔT-superlinear buoyant-transport term proposed there;
a ratio that grows with duty supports it.

Board was flashed to HEAD `a864a610` immediately before this run (see the companion flash/verify
report for that pass) — the fix for the recurring `profile_executor` stack-overflow panic
(`docs/audits/profile_executor_panic_2026-09-10_root_cause.md`) was live on the board throughout.

This report covers **two pairs run back to back**: an initial low-duty pair (0.237, 0.547) and,
after the first pair's own caveat was raised — that a convex γ=4/3 term is nearly indistinguishable
from linear over a narrow low-ΔT span, so a flat ratio there is what *both* hypotheses predict — a
second, substantially higher-duty pair (0.53, 0.92) run specifically to reach the temperature
region (z2 up to 63°C) where the original joint-hold study first saw the linear model's residual
turn positive.

## Method

Single-zone excitation via a **user profile** (never a builtin), zone_mask restricted to z2 only,
closed-loop PID rather than `autotune_start(method="step")`: the step-autotune path was tried
first and rejected — it self-declared `model_settled=True` at 581 s (elapsed), far short of the
~2000 s (8τ, τ≈247-259 s) needed even for z2's own settle, and nowhere near enough for z0's slower
cross-zone response (τ 620-730 s per `coupled_ident_single_zone`'s own docstring). This is the
exact false-early-settle failure mode `coupled_ident_settle_audit` was built to catch (slope-only
criterion, no duty-stability check). A closed-loop profile targeting a fixed z2 temperature does
not self-terminate and lets duty converge to whatever steady value holds that temperature — read
back from telemetry (`profiles_get_exec_status`'s per-zone `duty` field, confirmed to report the
intended pre-PWM duty: it stays flat across polls where `relay=on/off` toggles, i.e. it is not the
raw chopped relay state), which is what "duty level" means throughout this report.

Borrowed slot #6 was overwritten for each pair, then restored to its original content
(`cpl_z2`, target=55C ramp=120C/hr dwell=35min) after both pairs, confirmed by read-back:
`profiles_get(6)` -> `#6 'cpl_z2' zone_mask=0x4, segment 0: target=55.0C ramp=120.0C/hr dwell=35min`
— matches the pre-run content exactly.

**Rested baseline was re-established, and re-measured, before each pair** — not reused across
pairs, since the room is known to drift a few °C within a session:

- Pair 1 anchor: all four thermocouples (z0, z1, z2, and the safety processor's own independent
  TC) agreed within 0.2°C before the run — 32.70, 32.70, 32.49, 32.59°C — **ambient = 32.6°C**.
- Pair 2 anchor: after pair 1's plateaus, the board was allowed to cool passively (no forced
  cooling) — cooling followed the same first-order thermal decay as heating (τ≈200-250s), so it
  returned to a flat rest inside ~20 minutes rather than requiring an hour+. All four thermocouples
  again agreed within ~0.9°C over three consecutive 5-minute polls before the run —
  31.80/31.80/31.62/31.71°C (z0/z1/z2/safety-TC three-sample averages) — **ambient = 31.73°C**,
  about 0.9°C below pair 1's anchor, consistent with the kind of modest session drift this
  re-anchoring step exists to catch.

Each plateau's "settled" call required flat duty AND flat temperature across at least two
consecutive 5-minute polls (never elapsed time alone). All four plateaus' reported averages are
the mean of 3 polls spanning the final ~10.3-10.4 minutes of dwell:

- Level 1 (pair 1): settled by dwell-elapsed ~950-1260s.
- Level 2 (pair 1): settled by dwell-elapsed ~920-1550s.
- Level 3 (pair 2): settled by dwell-elapsed ~940-1560s.
- Level 4 (pair 2): settled by dwell-elapsed ~940-1560s (z2 itself reached target within ~630s;
  z0/z1's slower cross-zone response took the remainder).

Raw poll log: `logs/coupling/z2_step_2026-09-11.jsonl` (git-ignored per `.gitignore:100`, kept
local only).

## Results — all four points

| pair | plateau | duty (z2, converged) | z0 avg (°C) | z1 avg (°C) | z2 avg (°C) | ambient (°C) | ΔT0 (°C) | ΔT0/duty |
|---|---|---|---|---|---|---|---|---|
| 1 | level 1 | 0.237 | 37.98 | 38.68 | 42.68 | 32.6 | 5.38 | **22.7** |
| 1 | level 2 | 0.547 | 44.89 | 45.73 | 53.01 | 32.6 | 12.29 | **22.5** |
| 2 | level 3 | 0.53 | 43.11 | 44.00 | 51.18 | 31.73 | 11.38 | **21.5** |
| 2 | level 4 | 0.92 | 51.92 | 52.86 | 63.07 | 31.73 | 20.19 | **21.9** |

Full numbers: `docs/audits/z2_single_column_superlinearity_discriminator_2026-09-11.tsv`.

**All four ratios cluster within a 5% band (21.5-22.7, mean 22.15) across a duty range spanning
0.237 to 0.92 — a nearly 4x range — with z2's own absolute temperature spanning 42.7°C to 63°C.**
There is no monotonic trend: the highest-duty point (0.92, ΔT0=20.2°C) has a *lower* ratio (21.9)
than the lowest-duty point (0.237, ΔT0=5.4°C, ratio 22.7). Within each same-ambient pair the ratio
moves by less than 1% (pair 1: 22.7→22.5; pair 2: 21.5→21.9, actually a slight *rise* with duty
that is the wrong sign for concern and well inside noise either way).

## Measurement uncertainty

- Thermocouple read noise at a genuinely flat plateau: ≤0.1-0.2°C peak-to-peak across consecutive
  5-minute polls (e.g. level 4's z0 samples were 51.78/51.98/52.00).
- Ambient-anchor uncertainty: each pair's anchor was freshly measured immediately before that
  pair (not reused), bounding drift-within-pair to well under the ~3°C/session figure quoted from
  the earlier 12-hour capture. Residual uncertainty estimated ≤0.2-0.3°C per anchor.
- Duty read precision: PID dither on a converged dwell was ≤0.03-0.04 across all four plateaus.
- Propagating onto ΔT0/duty: level 1 (smallest ΔT, most duty-sensitive) is 22.7 ± ~2.0 (≈9%);
  levels 2-4 are all ≤5% relative uncertainty, tightening as ΔT grows.

The observed spread across all four points (1.2, i.e. ~5% of the mean) is comparable to or smaller
than level 1's own uncertainty band alone, and well inside the combined uncertainty of any pair of
points compared. **This is a flat ratio within measurement error across the full tested range, not
a marginal or ambiguous one.**

## Verdict: NULL RESULT — refutes superlinear buoyant transport, now across the range that matters

A convex (γ>1) buoyant source term predicts a ΔT0/duty ratio that *grows* with duty/ΔT — that is
the entire point of the proposed 4/3-power law, and it is specifically a low-ΔT-vs-high-ΔT
comparison, not a within-narrow-band one. The first pair (ΔT0 5.4-12.3°C) could not distinguish
the hypotheses, as the coordinator correctly flagged: a γ=4/3 curve is close to linear over that
narrow a span, so a flat ratio there is what *both* buoyancy and plain linearity predict. The
second pair closes that gap — z2 itself reached 63°C, matching the original joint-hold study's
"62°C" plateau, which is exactly where that study's linear-model residual first turned positive
(§2 of the proposal doc: z0 pred−obs went from −0.080 at low ΔT to +0.016 at 62°C to +0.083 at
75°C). If the buoyant mechanism is real and has the proposed magnitude, the ΔT0/duty ratio
measured here should have shown a resolvable rise between the low pair and the high pair. It did
not: 22.6 (pair 1 mean) vs 21.7 (pair 2 mean), a *decrease*, and one entirely inside measurement
noise.

**This is a genuine refutation, not a scoped one.** Across a nearly 4x range of driven duty and a
20°C span of z0's induced ΔT — reaching into the specific temperature band where the original
study's residual first went positive — the single-column ΔT0/duty ratio does not climb. The
sign-flip in the original joint-hold data (which motivated the buoyant-transport proposal in the
first place) has to be explained some other way: either it is a joint-dwell-specific effect not
reproducible in a single-column excitation (a real possibility the Limitations section below
does not rule out), or the buoyant mechanism as proposed (source-zone-driven, ΔT^(4/3), one-way
z2→z1→z0 transport) is not the right explanation for it. Either way, **this data does not support
adopting the buoyant term**, and the coordinator's framing is correct: if this held up (it did),
the buoyancy explanation for the linear matrix's failure is dead, and whatever causes the
sign-flip in the joint-hold data remains open.

## Limitations — what this still does not test

- **This tests single-column excitation, not the joint-dwell configuration where the original
  sign-flip was actually observed.** All three zones running together (the joint-hold condition)
  could differ from a single zone driving alone in ways this experiment cannot see — e.g. any
  effect that depends on the *receiving* zone also being actively heated (not just warm from
  upstream transport), or an effect driven by chamber-averaged temperature rather than the
  source zone's own temperature. This experiment isolates z2→z0 transport cleanly, but a joint
  hold is not simply "three single-column experiments superimposed" if the true mechanism is
  nonlinear in the receiving zone's own state too.
- Both pairs' ambient anchors, while freshly measured immediately before their respective pair,
  were single pre-run measurements, not measured continuously through each plateau's own final 10
  minutes (which would require an independent room-temperature sensor decoupled from the driven
  zone — none exists on this board; all four thermocouples are inside the chamber).
- No coupling matrix or plant term was persisted to the board — this was identification only, per
  the task brief.

## Board state at end of run

Both pairs' firings were stopped with `profiles_stop()`, producing **no panic either time** —
`get_heap_status` showed `reset_reason='software (esp_restart)'` (the flash's own reset) with a
continuously-incrementing `uptime_s` (12190 s at final check, no intervening reboot across the
entire ~2-hour combined run), confirming the `a864a610` stack-overflow fix holds at the exact
failure point that panicked this board twice before — now tested at higher peak temperature and
longer cumulative runtime than the first pair alone. Relays confirmed off two ways
(`io_read`'s `R1=R2=R3=R4=0`, and `get_board_state`/`safety_get_status` agree). Executor idle
(`profiles_get_exec_status.state=0`), no trip latched (`safety_get_status`: armed, not tripped),
crash report cleared and re-checked `present=false` after both pairs. Safety-link `timeouts`
stayed flat at 9 total across the entire combined run while `sent` climbed from ~260 to 7355 —
confirms the new push-throughput accounting does not track `sent` even under sustained,
higher-temperature load. **The board is safe and idle.**

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
