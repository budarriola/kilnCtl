# Joint-load model class design pass — 2026-09-11

**Status: paper design only. No board touched, nothing flashed, no heating run, no `.kicad_*`
file touched, no simulator or firmware code changed.** Written to the standing brief: the
adopted linear coupling matrix `G` (`docs/audits/coupling_joint_identification_capture_2026-09-10.md`,
commit `b64fe09d`) is refuted — Criterion A fails 11/12, Criterion B needs `c=-8.00C` (z0) /
`-2.03C` (z1), z2 clean at `-0.04C` — and a matched-ΔT0 comparison
(`docs/audits/joint_vs_singlecolumn_matched_dT0_2026-09-11.md`, commits `5844a3e8`/`947709a8`)
shows the linear model *over*-predicts z0's joint-dwell ΔT by 4.3-4.5C (~33%), with coefficient
noise ruled out (±0.3-0.5C bound) and ambient-reference error bounded at ≤0.46C, leaving ≥90% of
the discrepancy as a genuine joint-load effect. The direction is a **saturation-type**
over-prediction (the joint case needed *more* combined duty than superposition predicts), the
**opposite sign** from the original 62-75C joint study
(`docs/audits/coupling_joint_identification_capture_2026-09-10.md`'s own Criterion A table, where
the same linear model *under*-predicted at high ΔT). Buoyancy (a per-source-zone `ΔT^(4/3)` term)
is separately **refuted** as the explanation for this specific discrepancy: two single-column
discriminator campaigns (`docs/audits/single_column_superlinearity_discriminator_2026-09-11.md`,
commits `cf1f8ce9`/`1b9afd4f`) found z2→z0 induced ΔT-per-duty flat across duty 0.237→0.92 (z2 up
to 63.1C) — 22.7/22.5/21.5/21.9, within 5%, no trend — so single-column transport into z0 is
linear, and the joint-only 33% effect cannot be a source-zone buoyancy term, because that term
would also have shown up in the single-column campaign (it did not).

This document does not re-derive any of the above. It takes all of it as given and asks: what
model class, still linear in single-column operation (per the discriminator campaigns), can
produce a **joint-load-specific** deviation, sized ~33% at ΔT≈45C total, with the observed
saturation sign — and account for the opposite-sign finding at the original 62-75C joint study?

## 0. What "joint-specific" rules in and out

The defect is not in any single zone's own diagonal (`model_k_dc`, single-column FOPDT fits) —
those are separately measured and stable (`947709a8`'s repeatability check, c02 2.9% week-over-
week). It is not simple pairwise linear coupling — the freshly re-identified, internally
consistent, well-conditioned (`cond=5.04`) matrix still fails held-out joint data
(`b64fe09d`). It is not a per-source-zone nonlinearity in the *source's own* temperature — the
discriminator (`cf1f8ce9`/`1b9afd4f`) drove one zone alone across the same duty/ΔT range the
joint dwell reached and found no superlinearity. **The one variable the discriminator campaigns
could not vary is the number of zones simultaneously on.** Every candidate below is judged
against that constraint: it must be *invisible* when only one column is driven, and it must
appear specifically when multiple zones are driven together.

## 1. Candidate model classes

### (A) Shared-ambient / enclosure-node model

An extra lumped thermal node `T_enc` (the shared air/insulation mass of the chamber, distinct
from any one zone's own thermocouple) with its own first-order dynamics:

```
C_enc * dT_enc/dt = sum_j( h_j * (T_j - T_enc) ) - loss_enc(T_enc, T_amb)
```

and each zone's own equation gains a term `+ h_i * (T_enc - T_i)` instead of (or in addition to)
the pairwise `coupling_coeff[i][j] * u_j` terms currently in `zone_coupling_solve.c`. All zones
couple through the ONE state `T_enc`, not pairwise.

- **Linear in single-column operation?** Yes, structurally — with only one zone driven, `T_enc`
  is driven by that one zone's `T_j` alone, and the system reduces to a two-state linear system
  (zone + enclosure) exactly as diagonal FOPDT identification already assumes if the enclosure's
  time constant is folded into the zone's own measured `tau_s`. This is consistent with the
  discriminator finding flat single-column behavior.
- **Predicts over-prediction at low level?** Only if `loss_enc` is itself linear in `T_enc`;
  under that assumption the model is fully linear everywhere and predicts NO joint/single
  discrepancy at all — it would need `loss_enc` to be superlinear (folding it back into class B
  below) to produce the observed 33% effect. On its own, a linear-loss enclosure node changes
  the *effective coupling matrix's shape* (it can explain a low-rank, common-mode correction
  applied identically to all three rows — plausibly relevant to z1's newly-found `-2.03C` offset
  as well as z0's) but does not by itself produce a magnitude that grows with total power.
- **Sign reversal at high level?** Not by this mechanism alone. A linear enclosure node adds a
  fixed common-mode gain; it does not flip sign between the low-ΔT joint-hold point and the
  62-75C `cplval75` points. The original study's opposite-sign result (under-prediction at
  62-75C) would need a *second*, separately-signed mechanism layered on top — this class alone
  does not explain fact 5.
- **New parameters / identifiability**: 2 new parameters per zone (`h_i`, and `C_enc`/`loss_enc`
  shared across zones, so effectively 3 `h_i` + 1-2 enclosure parameters = 4-5 total). Not
  identifiable from data already captured: every existing joint-hold/plateau data point has all
  three zones driven together, so `T_enc`'s own trajectory is never isolated from the zones'
  trajectories — this needs a dedicated experiment (see §4).

### (B) Total-power saturation / superlinear enclosure loss

A single scalar correction driven by **total instantaneous power**, not by any individual zone's
state:

```
P_loss_total = k_loss * max(0, P_total_w)^gamma_total     (gamma_total > 1)
```

split back across zones by some fixed allocation (equal share, or weighted by each zone's own
`model_k_dc`), subtracted from the effective heat delivered. Physically: radiative and
convective loss from the whole enclosure's external skin scales faster than linear in the
enclosure's own temperature rise, and total temperature rise scales with total power delivered,
so loss (not gain, note the sign) grows superlinearly with the SUM of what all zones are doing —
even though each zone driven alone never sees enough total power to leave the linear regime.

- **Linear in single-column operation?** Yes by construction — with one zone on,
  `P_total_w` equals that zone's own power, and `k_loss * P^gamma_total` is a fixed (if
  nonlinear-in-that-zone) term that is already absorbed into the single column's own measured
  `model_k_dc`/dead-time fit, because single-column identification never varies `P_total`
  independently of that one zone's own duty — it cannot distinguish "this zone's own loss
  curve" from "total-power loss with one contributor." This is exactly why single-column data
  is structurally blind to this term, matching why the discriminator campaign (which stayed
  single-column) saw nothing.
- **Predicts over-prediction at low level?** This is the class's weak point as stated: a pure
  loss term is a *drag*, monotonic in `P_total`, so it predicts the joint case needs MORE duty
  than superposition at ANY total power, not specifically at one level — it does not by itself
  predict the sign to flip at higher ΔT (fact 5). It matches fact 3/4 (33% under-delivery,
  saturation sign) directly and simply: with three zones simultaneously drawing power at a
  modest joint-hold ΔT (~45C total, `low ΔT joint hold`, `coupling_joint_identification_capture
  _2026-09-10.md`), a superlinear-in-total-power loss is already larger than three separate
  linear single-zone loss terms would predict, which is precisely "the joint case needs more
  combined duty than the columns imply."
- **Sign reversal at high level?** Only via a second mechanism (see note below) — a pure
  monotonic loss term cannot itself flip sign. However: the *original* 62-75C joint study's
  under-prediction was measured against the OLD, mixed-provenance matrix
  (`cplval75_coupling_verdict_2026-09-10.md`, referenced in `b64fe09d`), which itself was later
  found to have a "mixed-provenance gain deficit" per `b64fe09d`'s own verdict section — i.e.
  the original study's sign was partly an artifact of comparing against a matrix built from
  non-contemporaneous columns, not a property of the plant necessarily requiring a second
  physical mechanism to explain. This document does NOT resolve which of these two is true —
  see §3's honesty requirement.
- **New parameters / identifiability**: 2 (`k_loss`, `gamma_total`), or 1 if `gamma_total` is
  fixed by a physical prior (e.g. 4/3 turbulent-convection scaling, per
  `z0_buoyant_coupling_term_proposal_2026-09-10.md`'s own citation, applied here to total
  enclosure loss rather than a per-zone source term). NOT identifiable from data on hand: no
  existing capture varies `P_total` while holding per-zone allocation fixed and comparable
  across levels — the joint-hold captures used only ONE joint-power level per session (the low-
  ΔT hold, and separately the 62/70/75C `cplval75` points, which are a different protocol
  entirely). A within-protocol multi-level joint sweep is needed (§4).

### (C) Duty-product / bilinear interaction term

Add cross terms proportional to the PRODUCT of two zones' duties, not their sum:

```
correction_i = sum_{j != i} ( k_bilinear[i][j] * u_i * u_j )
```

- **Linear in single-column operation?** Yes trivially — with only one `u_j` nonzero, every
  product term vanishes (any other zone's `u=0`). This is the cleanest match to "invisible in
  single-column, present only when >=2 zones are on simultaneously" of any candidate here.
- **Predicts over-prediction at low level?** Depends entirely on the sign of `k_bilinear`, which
  this document has no data to fix — nothing in the existing captures isolates a `u_i*u_j` cross
  term from either an additive `G` term or a total-power loss term, because in every joint
  capture to date all commanded duties moved together (the joint hold's three duties were solved
  jointly from one target, not swept independently against each other).
  A negative `k_bilinear` (each zone's presence increasing another's loss) reproduces the
  saturation direction (fact 3/4) at low-to-moderate joint duty.
- **Sign reversal at high level?** Structurally possible only if `k_bilinear` itself is duty-
  dependent (e.g. saturates or changes sign as duty approaches 1) — as a plain bilinear (constant
  coefficient) term it CANNOT flip sign on its own, any more than class B can. It shares the
  weakness of class B on fact 5, while adding more free parameters (up to 3 independent
  `k_bilinear[i][j]` pairs, or 6 if asymmetric) with no more identifying data than class B has.
- **New parameters / identifiability**: 3 (symmetric) to 6 (asymmetric) new coefficients. Worse
  identifiability than B for the same reason: 3 unknowns from data where all duties moved
  together is underdetermined — the existing joint-hold and `cplval75` points cannot separate
  "total power" (class B, 1-2 params) from "which specific pair" (class C, 3-6 params) because
  in every capture so far all three zones were on together, never a controlled two-of-three
  subset.

### (D) Per-zone element-resistance / supply-sag effect

Physical question: does turning on MORE relay elements simultaneously reduce the power actually
delivered to each one (mains sag under combined load, or a shared supply impedance)? Checked
against source rather than assumed:

- `mains_voltage_v` (`firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c:239-251,
  363-385`) is a SINGLE scalar read once from config and applied identically to every zone's
  expected-current calculation (`zone_sweep_expected_coil_current_a()`, line 371) — there is no
  live, per-zone or per-poll voltage measurement anywhere in this path; it is a configured
  constant (120V, confirmed live at `mains_voltage_v=120` in
  `coupling_joint_identification_capture_2026-09-10.md`'s preconditions section), not something
  that could reflect real-time sag even if sag existed.
  A per-tick sag effect could exist physically without being visible in ANY telemetry this board
  currently captures, because nothing samples supply voltage during a run.
- `coil_power_w` (`firmware/KilnFW/App/drivers/persist/zones_config_json.h:919`, added
  `ZONES_CFG_VERSION` 24->25) is a per-zone CONFIGURED nameplate figure used to derive expected
  current for the CT sweep (`zone_sweep_expected_coil_current_a()`), not a live measurement of
  delivered power — it cannot itself detect sag, only assume its absence.
- The CT/current-sense path (`zones_current_sweep_task.c`,
  `docs/audits/s14_s15_followup_summed_kct_and_averaging_2026-09-10.md`) is explicitly
  characterized as measuring at or below its own noise floor on this bench — "this ~4W bench
  load is intrinsically too small ... to produce a trustworthy `i_normal_a`" — so even if this
  document wanted to check for sag empirically via the CT channel, that channel is documented as
  unusable at this load level. **No instrumentation on this board today can confirm or refute
  supply sag.**
- **Linear in single-column operation?** Yes by definition — sag by construction requires
  multiple simultaneous loads.
- **Predicts over-prediction at low level, sign reversal at high?** Plausible in principle (sag
  increases with total combined current draw, an effect that would look exactly like "joint
  case needs more commanded duty than columns imply" since each zone is silently getting less
  actual power than commanded) but **entirely unconstrained by any number in this repo** — there
  is no measured mains impedance, no per-zone delivered-power telemetry, and the CT channel that
  could someday measure it is independently documented as below its usable range at this bench's
  ~4W scale (`project_bench_is_a_4w_test_fixture`). This candidate cannot be ranked against the
  others on evidence; it can only be flagged as unfalsifiable with current instrumentation.
- **New parameters / identifiability**: 1 (a supply/source impedance), NOT identifiable at all
  today — no path exists to measure it on this bench (per the CT-floor finding above) without
  new instrumentation (an external mains voltage logger) that is out of scope here.

## 2. Summary table

| Candidate | Linear single-column? | Explains low-level over-prediction (33%, fact 3/4)? | Explains high-level sign reversal (fact 5)? | New params | Identifiable from data on hand? |
|---|---|---|---|---|---|
| A. Enclosure node (linear loss) | Yes | Only as common-mode offset, not magnitude growth | No | 4-5 | No — needs isolated `T_enc` experiment |
| B. Total-power superlinear loss | Yes | Yes, directly and simply | No on its own; plausible if original 62-75C sign was itself a mixed-provenance artifact | 1-2 | No — needs multi-level joint sweep |
| C. Duty-product bilinear | Yes | Yes, if `k_bilinear<0` (unconstrained by data) | No (constant-coefficient) | 3-6 | No — worse-determined than B |
| D. Supply sag / element resistance | Yes | Plausible, unconstrained | Plausible, unconstrained | 1 | No — no instrumentation exists at this load |

**No candidate here explains the sign reversal (fact 5) as a first-order consequence of its own
mechanism.** That is stated as a finding, not papered over: either (i) the original 62-75C
under-prediction was itself partly an artifact of the mixed-provenance matrix it was measured
against (`b64fe09d`'s own characterization of that earlier failure mode), and the "genuine"
plant behavior across the full range is single-signed saturation (favoring B), or (ii) there
are genuinely two effects with different signs and thresholds (e.g. B saturating at moderate
total power, with a *separate*, still-unidentified mechanism dominating at higher ΔT) — this
document cannot distinguish these without new data (§4).

## 3. Ranking and recommendation

**Recommended: (B) total-power superlinear enclosure loss**, for three reasons:
1. It is the only candidate whose core mechanism directly and simply produces the *measured*
   sign and rough scale (a superlinear loss term evaluated at low-to-moderate total power is
   naturally smaller in magnitude than at higher total power — matching a real deficit already
   quantified at 45C joint total to be ~33%, `947709a8`) with the fewest new parameters (as few
   as 1, if `gamma_total` is fixed to a physical prior as `z0_buoyant_coupling_term_proposal
   _2026-09-10.md` already did for its own (refuted, for this discrepancy) source-side term).
2. It is structurally consistent with why the single-column discriminator saw nothing (§1B) —
   the *cleanest* fit to the one hard constraint every candidate must satisfy.
3. Its identification experiment (§4) is the same shape of experiment (a held multi-plateau
   sweep) already proven out by this project's existing joint-hold protocol
   (`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`), just varied along total power instead of
   per-zone columns — lower process risk than inventing a new protocol from scratch.

(A) is a reasonable secondary candidate specifically for z1's newly-found `-2.03C` offset (a
common-mode correction fits an offset better than a magnitude-scaling one), but it is ruled out
as the primary z0 explanation because a linear enclosure node cannot produce the magnitude
growth fact 3/4 requires without importing a nonlinearity — at which point it collapses into a
variant of B with an extra state. (C) is strictly worse-determined than B with the data on hand
and is not preferred while B remains untested. (D) is flagged honestly as **currently
unfalsifiable** on this bench — it should be tracked as an open question but not pursued with
firmware changes, since no instrumentation exists to confirm or refute it (this is the same
"do not build against an unmeasured parameter" reasoning the CT-floor audit already applied to
S14/S15).

**The sign reversal (fact 5) is NOT explained by any candidate here.** That is reported as-is; do
not adopt a conclusion the evidence does not support. The discriminating experiment below is
designed to also produce this decision, not just confirm B's magnitude.

## 4. The discriminating experiment (design only — not to be run in this task)

**Goal:** separate "total power" (class B, all three zones' contribution pooled into one scalar)
from "any one specific pairing" (class C) from "no nonlinearity at all," AND determine whether
the 62-75C study's opposite sign is a real second mechanism or a mixed-provenance artifact.

**Protocol:**
1. Pick a single **total commanded duty budget**, `U_total` (sum of the three zones' held
   duties), and run it at THREE different **allocations** across zones, each run to full settle
   (same 8τ/settling-gate discipline as `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`):
   - Allocation 1: concentrated on one zone (`u = [U_total, 0, 0]`, i.e. a single-column point —
     already have this class of data).
   - Allocation 2: split two zones (`u = [U_total/2, U_total/2, 0]`).
   - Allocation 3: split three zones evenly (`u = [U_total/3, U_total/3, U_total/3]`).
2. Repeat the same three-allocation triple at TWO different values of `U_total`: one near the
   existing low-ΔT joint-hold total (~0.51, matching `coupling_joint_identification_capture
   _2026-09-10.md`'s low-ΔT hold duties 0.1499+0.1728+0.1853) and one near the `cplval75`-range
   total (~1.6-2.0, matching the 70-75C plateaus' summed duties) — six settled plateaus total,
   each measuring the sum of all three zones' steady-state ΔT (or equivalently the total duty
   needed to hold a fixed total ΔT target, whichever is easier to command given the executor).
3. **Measured outcome:** total system ΔT (or duty) as a function of allocation, at fixed
   `U_total`, at each of the two power levels.

**What discriminates B from C:**
- Under **class B** (total-power loss), all three allocations at a fixed `U_total` should show
  the SAME total loss/ΔT deficit (loss depends only on the sum, not on how it is split) —
  allocations 1/2/3 should agree with each other within measurement noise (per `947709a8`'s own
  ±0.3-0.5C coefficient-noise bound as the reference tolerance).
- Under **class C** (pairwise duty-product), the loss should scale with the number of nonzero
  PRODUCT terms, which differs by allocation: allocation 1 has zero cross terms (matches existing
  single-column data, no deficit), allocation 2 has one active pair, allocation 3 has three active
  pairs — so allocation 3 should show a measurably larger deficit than allocation 2, and both
  should exceed allocation 1's near-zero baseline. A within-a-power-level ordering of
  (allocation 3 deficit) > (allocation 2 deficit) > (allocation 1 deficit ≈ 0) is the class-C
  signature; a flat deficit across allocations 2 and 3 at matched `U_total` is the class-B
  signature.

**What discriminates a real sign-reversal mechanism from a mixed-provenance artifact (fact 5):**
- Running BOTH power levels (low ~0.51 and high ~1.6-2.0 total) with the SAME, freshly and
  consistently identified matrix used for both predictions (rather than comparing against the
  now-known-flawed original `cplval75` matrix) answers whether the deficit's *sign* stays
  saturation-type (over-prediction, i.e. real ΔT/duty need bigger than superposition) at both
  power levels, or whether it flips to under-prediction at the high level even against a
  same-session, correctly identified matrix. If it stays single-signed once compared consistently,
  the original opposite-sign finding was a provenance artifact, not a second mechanism — a
  concrete, checkable resolution of the open question in §2, without inventing new physics.

**Numeric outcome that distinguishes the top two candidates:** at the low `U_total` level,
compare allocation 2's and allocation 3's total ΔT deficit (predicted-minus-observed, same
convention as the existing Criterion-A tables) against allocation 1's near-zero baseline. If
allocation 2 and 3 agree with each other to within the ±0.3-0.5C coefficient-noise bound already
established (`947709a8`), that is a class-B (total-power) result. If allocation 3's deficit
exceeds allocation 2's by more than that bound, that is a class-C (pairwise) result. This is a
6-plateau, single-session experiment (comparable cost to the existing low-ΔT joint hold plus one
extra allocation variant), well inside the scale of captures already run in this project, and it
does not require inventing new firmware — it can be commanded through the existing
`profiles_get_exec_status`/hold-target executor path exactly as the low-ΔT joint hold was
(`coupling_joint_identification_capture_2026-09-10.md`, "Low-ΔT joint hold" section), just with
different per-zone target splits.

## 5. What would let the existing linear matrix remain usable in a bounded region

The cheapest near-term answer, if true, would avoid touching the control solve at all: **restrict
the linear `G` to a total-duty or total-ΔT envelope small enough that the (currently unmeasured)
nonlinear correction stays below a chosen tolerance.**

For this to be defensible, ALL of the following would need to be true, and none of them is
currently established by anything in this repo:
- A **quantified deficit-vs-total-power curve** (not just the two data points currently on hand:
  ~0% deficit at single-column, ~33% deficit at the one measured joint-hold total of ~0.51) —
  without at least a third point, "small below some duty" is asserted, not measured; the
  existing captures cannot rule out the deficit already being significant well below the tested
  low-ΔT hold's total duty.
- An explicit **tolerance budget** tied to what the control loop actually needs — e.g. if a
  X% duty error at a given total is acceptable for hold-mode PID (which has integral action to
  absorb a *steady* modeling error) but not for climb-mode feedforward (which does not get to
  integrate away a per-tick error the same way), the bound would likely need to be different
  for hold vs. climb, and climb-mode's tolerance is not stated anywhere in the audits reviewed
  for this pass.
- Confirmation that the **~0.51 total-duty low-ΔT-hold point, which already shows an 11/12
  Criterion-A failure and an 8-11.3% absolute duty miss per zone** (`b64fe09d`'s own table), is
  itself ALREADY outside any tolerance band anyone would accept — which appears very likely
  given the magnitude of the failures reported there, meaning the "bounded region" would have to
  sit at a LOWER total duty than the lowest point already tested, i.e. below any point this
  project has ever validated the matrix against. That is a real constraint working against this
  option: the region where linear `G` might still be trustworthy has not actually been probed —
  every joint capture so far (low-ΔT hold, `cplval75` 62/70/75C) already falls in the region
  where the matrix demonstrably fails.

**Bottom line for this section:** bounding the operating region is a legitimate mitigation in
principle, but as of this writing there is no evidence in this repo that a usable bounded region
exists above zero — the lowest joint-load point tested already fails by 8-11% per zone. Treat
"restrict the linear matrix's operating envelope" as *unconfirmed and likely narrower than any
already-tested condition*, not as a ready fallback, until a lower-total-duty joint point is
captured and checked against Criterion A.

## Sources

- `docs/audits/coupling_joint_identification_capture_2026-09-10.md` (commit `b64fe09d`) —
  matrix values, Criterion A/B tables, sign-change finding, provenance-flag citation
  (`firmware/KilnFW/App/drivers/control/zone_coupling_solve.c:292`).
- `docs/audits/z0_buoyant_coupling_term_proposal_2026-09-10.md` — buoyancy term design,
  `gamma=4/3` physical citation, z2-has-nothing-below-it structural argument.
- `docs/audits/single_column_superlinearity_discriminator_2026-09-11.md` (commits `cf1f8ce9`,
  `1b9afd4f`) — z2-alone duty/ratio data (0.237/0.547/0.53/0.92, ratios 22.7/22.5/21.5/21.9)
  refuting source-side buoyancy as the joint-specific mechanism.
- `docs/audits/joint_vs_singlecolumn_matched_dT0_2026-09-11.md` (commits `5844a3e8`,
  `947709a8`) — matched-ΔT0 comparison (13.19 vs 13.28C), 33% duty over-prediction, coefficient-
  noise bound (±0.3-0.5C from c02 2.9% week-over-week spread), ambient-reference bound (≤0.46C).
- `docs/audits/s14_s15_followup_summed_kct_and_averaging_2026-09-10.md` — CT-channel noise-floor
  finding used in candidate D (`i_normal_a` unusable at this bench's load).
- `firmware/KilnFW/App/drivers/control/zone_coupling_solve.c` — `G·u=b` hold solve,
  `zone_coupling_matrix_provenance_ok()`, `use_measured_diag_k_dc` gate.
- `firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c` (lines 239-251, 363-385) —
  `mains_voltage_v` single-scalar usage, `zone_sweep_expected_coil_current_a()`.
- `firmware/KilnFW/App/drivers/persist/zones_config_json.h` (line 919, and `ZONES_CFG_VERSION`
  24->25 note at line 248) — `coil_power_w` field.
- `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` — existing joint-hold protocol reused as the
  basis for §4's experiment design.
- User memory `project_bench_is_a_4w_test_fixture`, `project_zone_physical_arrangement` — bench
  scale and zone stacking context cited in candidate discussions.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>

---

# Review, 2026-09-11 — RECOMMENDATION (B) REFUTED BY DATA ALREADY ON HAND

**Adversarial review of the above. Do not act on §3's recommendation.** Analysis only: no board
touched, nothing flashed, no heating run, no `.kicad_*` file touched, no firmware or config
changed. Read-only `control_get_zones` was the only board call made.

**Headline:** the load-bearing claim in §1B/§3 — that a total-power superlinear loss term is
*structurally invisible* in single-column operation — is **false for the single-column data this
project has actually captured**. The four-point z2-alone discriminator
(`z2_single_column_superlinearity_discriminator_2026-09-11.md`) swept a total power that
**brackets and exceeds** the joint plateau's, and found the ΔT0/duty ratio flat. A term that is
monotone increasing in total power cannot be zero at a higher total power and 33% at a lower one.
Class (B) is refuted, for any `k_loss` and any `gamma_total > 1`, without needing a new run.

## R1. The total-power arithmetic (the decisive point)

`P_total = sum_i( u_i * coil_power_w[i] )`. Two preliminaries:

- **`coil_power_w` has no value anywhere in this repo or on the live board.** It was added at
  `ZONES_CFG_VERSION` 24->25 and no audit, preset, or readback records a non-zero figure;
  `zones_config_json.c:397` and `zones_http_post_parse.c:295` both treat `0.0f` as *unset*. So
  `P_total` cannot be evaluated in watts today at all — a fact §1B should have stated, since it
  is the very scalar class (B) is defined on. The comparison below is therefore done in
  **duty-share** terms, which is sufficient because only the *ratio* of the two `P_total` values
  matters.
- **`mains_voltage_v` is not part of this arithmetic.** It is read once from Pico config and used
  solely to derive expected CT current (`zones_current_sweep_engine.c:371`); it never scales
  delivered thermal power in any model. Its live value is `120`
  (`esp_bring_up_to_head_2026-09-10.md`, `cplval45_scale_vs_offset_2026-09-10.md`,
  `coupling_joint_identification_capture_2026-09-10.md`); the `240` in
  `safety_commissioning_completion_2026-09-09.md` is the superseded commissioning value. The doc
  above states `120` correctly. Either way it cancels out of the comparison.

**The joint plateau that produced the 33% deficit** (`947709a8`) held
`u = [0.163, 0.210, 0.247]`, so `sum u = 0.620`.

**The single-column discriminator** (`cf1f8ce9` / `1b9afd4f`) drove **z2 alone** at
`u2 = 0.237, 0.547, 0.530, 0.920`, so `sum u = 0.237, 0.547, 0.530, 0.920`.

Assuming equal per-zone coil power `P` (corroborated below):

| condition | `sum u` | `P_total / P` | `P_total` vs joint | ΔT0/duty |
|---|---|---|---|---|
| single-column L1 | 0.237 | 0.237 | **0.38x** | 22.7 |
| single-column L3 | 0.530 | 0.530 | **0.86x** | 21.5 |
| single-column L2 | 0.547 | 0.547 | **0.88x** | 22.5 |
| **joint (33% deficit)** | **0.620** | **0.620** | **1.00x** | — (deficit 33%) |
| single-column L4 | 0.920 | 0.920 | **1.48x** | 21.9 |

**The single-column sweep spanned 0.38x to 1.48x the joint case's total power. Its top point
carried 48% MORE total power than the joint plateau where the 33% deficit was measured.** On the
~4 W fixture (`project_bench_is_a_4w_test_fixture`, ~1.33 W/zone) that is ~1.22 W single-column
against ~0.83 W joint. **A pure total-power term must therefore have shown up in the
single-column sweep, larger there than in the joint case. It did not: the ratio was flat within
5% (22.7 / 22.5 / 21.5 / 21.9), non-monotonic, and the highest-power point had a *lower* ratio
than the lowest-power point by less than its own noise band.**

**Quantitatively, calibrating class (B) to the observed effect** — pick `k_loss` so the deficit is
exactly 33% at `P_total = 0.620`, with `gamma_total = 4/3` (deficit fraction `= k * P^(1/3)`,
giving `k = 0.387`):

| `sum u` | class-B predicted deficit | class-B predicted ΔT0/duty (normalised at L1) | measured |
|---|---|---|---|
| 0.237 | 23.9% | 22.7 | 22.7 |
| 0.530 | 31.3% | 20.5 | 21.5 |
| 0.547 | 31.7% | 20.4 | 22.5 |
| 0.920 | 37.6% | **18.61** | **21.9** |

Class (B) predicts the ratio falls **17%** across the sweep; the measurement moves **3.5%**, in
the wrong direction, inside noise. At L4 class (B) predicts `ΔT0 = 17.12 °C` against a measured
`20.19 °C` — a **3.1 °C** miss, six times the 0.5 °C not-worth-chasing floor and far outside that
point's ≤5% uncertainty band.

**This refutation does not depend on `gamma_total = 4/3`, or on `k_loss`, or on the allocation
rule.** Any `P^gamma` with `gamma > 1` gives a deficit fraction monotone increasing in `P_total`.
Calibrating it to 33% at `P_total = 0.620` forces a deficit **strictly greater than 33%** at
`P_total = 0.920`. The single-column data shows ~0% there. The contradiction is structural.

**Sensitivity to unequal coils.** The only escape is if z2's coil is much weaker than z0's/z1's.
Solving `0.920*P2 < 0.163*P + 0.210*P + 0.247*P2` gives `P2 < 0.554 * P` — z2's element would
have to be under 56% of the others'. Nothing in this repo claims that, and the live diagonal
argues against it: `coupling_diag_k_dc = 42.73 / 32.40 / 33.85 °C/duty` (`control_get_zones`,
read live this session) puts z2 *above* z1. Since `k_dc = P_coil * R_th`, making `P2` half of
`P1` would require z2's thermal resistance to be ~1.8x z1's — an unevidenced extra assumption
introduced solely to rescue the hypothesis.

**Where §1B's reasoning goes wrong.** §1B argues the term is "already absorbed into the single
column's own measured `model_k_dc`". Absorption into a *gain* is only possible if the term is
**linear** in that zone's duty — i.e. `gamma_total = 1`, which by construction produces no
joint/single discrepancy at all. The moment `gamma_total > 1` the term is no longer absorbable
and appears as **curvature in ΔT-per-duty**, which is precisely the quantity the discriminator
measured at four levels. §1B conflates "absorbable into a gain" with "nonlinear", and in doing so
asserts a blindness that the actual experiment did not have. The discriminator was not a
single-column *identification* (which would indeed be blind); it was a single-column *ratio
sweep*, which is exactly the right instrument for this term.

**Second, independent contradiction from existing data.** Class (B) is monotone in `P_total`, so
its deficit must be *largest* at the highest joint total power. The 62-75 °C plateaus
(`b64fe09d`) carry joint totals of ~1.13-1.57 — roughly 2x the 0.620 point — and there the linear
model failed in the **opposite** direction. §2 records this as merely "not explained by any
candidate". It is stronger than that for class (B) specifically: it is a **sign contradiction at
the operating point where B's own effect should dominate**. §3 nonetheless recommends B while
parking the reversal behind a possible mixed-provenance artifact. Even granting that escape hatch
in full, R1 above stands on its own and does not need it.

**Verdict on §3: refuted.** `P_loss = k_loss * max(0, P_total)^gamma` is not a viable model class
for this discrepancy. Whatever produces the joint-only deficit is **not a function of total power
alone** — it must depend on *how* the power is distributed. That is class (C)'s defining property
(and, in a different form, class (A)'s), so the ranking in §2/§3 should be considered inverted by
this review: the evidence points away from B and toward mechanisms that vanish when a single zone
carries the whole load.

## R2. `gamma = 4/3` reuse — not justified; a familiar number recycled

Plainly: **no.** Three separate problems, only the first of which the doc acknowledges at all.

1. **Wrong independent variable.** `Nu ~ Ra^(1/3)` gives `h ~ ΔT^(1/3)` and hence
   `Q ~ ΔT^(4/3)` — a law in **temperature difference**. §1B applies the same exponent to
   **power**, `P_total^(4/3)`. These agree only if `ΔT ∝ P_total`, i.e. only if the plant is
   linear — the very thing under dispute. Transplanting the exponent across that substitution is
   not a "physical prior"; it is dimensional coincidence.
2. **Wrong mechanism for the constraint it must satisfy.** The 4/3 free-convection law describes
   loss from a surface at a given excess temperature, *however that temperature was produced*. An
   enclosure-skin loss term is therefore exactly the kind of term the single-column sweep **does**
   see — and that sweep took z2 to 63.1 °C and z0 to 51.9 °C, well *above* the joint plateau's
   43-44 °C. The physical picture offered in §1B actively undercuts §1B's own invisibility claim,
   independently of R1's arithmetic.
3. **Provenance.** The exponent's only citation in this repo is
   `z0_buoyant_coupling_term_proposal_2026-09-10.md`, whose mechanism was **refuted on hardware**
   (`z2_single_column_superlinearity_discriminator_2026-09-11.md`, four-point null result). §3
   reason 1 explicitly counts "fixed by a physical prior ... as `z0_buoyant_coupling_term_proposal`
   already did" as an argument *for* B's parsimony — i.e. it converts a refuted hypothesis's
   parameter into a free parameter saved. That is not parsimony; the prior carries no evidential
   weight here. The doc correctly marks buoyancy refuted in its own preamble, which makes the
   reuse in §3 harder to defend, not easier.

Note also that at a 13 °C rise over ~30 °C ambient on a ~4 W fixture, *any* convective/radiative
loss nonlinearity is small: over 303 K -> 316 K a `T^4` radiative law is nearer `~ΔT^1.0` in
excess-temperature terms across this span. A 33% effect is not plausibly enclosure-loss curvature
at this scale, quite apart from R1.

## R3. Direction/sign — the doc is CORRECT here

Checked, and it holds in the doc's favour. Over-prediction means the joint dwell reached a
*lower* ΔT0 than `G*u` predicts, i.e. it needed *more* combined duty. An added loss term removes
delivered heat, lowering predicted ΔT for the same duties, moving the prediction toward the
observation. The sign is right and is not backwards.

**The sign is simply not the binding constraint.** Classes (C) and (D) reproduce the same sign
equally well (§1C, §1D), so sign agreement discriminates nothing among them. §3 reason 1 leans on
"directly and simply produces the measured sign" as if it were selective. It is not.

§3 reason 1 also claims B produces "the rough scale". **No fit was performed and `k_loss` is
never computed anywhere in the doc** — the scale is asserted, not shown. R1 above appears to be
the first time the term has been evaluated numerically, and doing so is what refutes it.

## R4. The §4 experiment — partly already run, mis-specified at the high level, and underpowered

**(a) Its class-B arm is already answered.** Allocation 1 (`[U_total, 0, 0]`) *is* a single-column
plateau, and §4 says so ("already have this class of data"). Combined with R1, the B-vs-nothing
comparison has already been made at four total-power levels and came out negative. Running it
again is not discriminating; it is confirmatory of a result already in hand.

**(b) Identity slip.** §4's allocation 1 is written `[U_total, 0, 0]` — **z0-alone**. The
four-point ratio data is **z2-alone**, and z0 is the *top* zone with nothing above it while z2 is
the *bottom* (`project_zone_physical_arrangement`). "Already have this class of data" is true for
z2-alone, not for z0-alone. Either the allocation should be written `[0, 0, U_total]` or the claim
should be dropped.

**(c) Two of the six plateaus are unrunnable as specified.** §4 step 2 sets the high level at
`U_total ~ 1.6-2.0`. Allocation 1 then commands a single zone at duty **1.6-2.0** — impossible.
Allocation 2 at `U_total = 2.0` commands **1.0 / 1.0**, both at the rail, with zero PID headroom,
so it cannot settle to a meaningful converged duty either. Only allocation 3 is feasible at the
high level; that row of the design collapses to one usable cell.

**(d) The discrimination tolerance is understated by 2-3x.** §4 adopts `947709a8`'s ±0.3-0.5 °C
**coefficient-noise** bound as the acceptance band. That bound covers coefficient uncertainty
only. The dominant per-plateau term is missing: converged PID duty dither is 0.03-0.04
(`z2_single_column_superlinearity_discriminator_2026-09-11.md`, "Measurement uncertainty"), and
propagating 0.03 through `[c00, c01, c02] = [42.73, 25.42, 22.15]` over a 3-poll average gives
**±0.94 °C** on the predicted ΔT0 alone. Add the ≤0.46 °C ambient-reference term and the
≤0.2-0.3 °C anchor uncertainty and a realistic **per-plateau** figure is **~1.1-1.3 °C**; comparing
two plateaus is ~1.5-1.8 °C. The stated band is not the right yardstick.

**(e) Underpowered for its own class-C signature.** §4 claims allocation 3 ("three active pairs")
should show a "measurably larger" deficit than allocation 2 ("one active pair"). Doing the
arithmetic §4 omits: at fixed `U_total`, allocation 2's bilinear sum is `(U/2)^2 = U^2/4`;
allocation 3's is `3*(U/3)^2 = U^2/3`. The ratio is **1.33x, not 3x.** If allocation 3 carries the
whole measured 4.3 °C, allocation 2 carries ~3.2 °C, so **the class-C signature is a ~1.1 °C
difference** — at or below the ~1.5-1.8 °C two-plateau uncertainty from (d). With n=1 per cell and
six cells (four usable, per (c)), the design has roughly **0.6-0.7σ** of separation on the very
comparison it is built to make. **Not adequately powered.** It would need replication (2-3
plateaus per allocation) and/or a more extreme allocation contrast (e.g. `[U/2, U/2, 0]` versus
`[0, U/2, U/2]` versus a strongly uneven three-way split), which separate *which pair* rather than
*how many* and give much larger signal ratios.

**(f) Ambient confound is not the limiting factor**, to be fair to §4: the ~0.035 row-independent
ambient term is bounded at ≤0.46 °C against a 4.3-4.5 °C effect, and §4's fixed-`U_total`,
same-session design makes it largely common-mode across allocations. It is (d)'s duty-dither term
and (e)'s small signature, not ambient and not the 2.9% coefficient repeatability, that sink the
power budget.

**(g) What survives.** The allocation-sweep *idea* — vary the split at fixed total — remains the
right instrument, and R1 strengthens the case for running it, because R1 says the mechanism must
depend on the split. It should be re-specified as a **class-A / class-C** discriminator at a
single feasible total (~0.6-0.9, matching the point where the 33% is already measured), with
replicated plateaus and pair-identity contrasts, and the `U_total ~ 1.6-2.0` row dropped.

## R5. Overstatement, uncited numbers, and contradictions with established facts

Hypothesis written as conclusion:

- **§1B**: "a superlinear-in-total-power loss is already larger than three separate linear
  single-zone loss terms would predict, which is precisely 'the joint case needs more combined
  duty than the columns imply'". Stated as established. It is an unfitted hypothesis, and per R1
  a false one.
- **§3 reason 1**: "produces the *measured* sign and rough scale". The scale was never computed.
- **§3 reason 2**: "structurally consistent with why the single-column discriminator saw nothing —
  the *cleanest* fit to the one hard constraint every candidate must satisfy." This is the exact
  claim R1 refutes; B is in fact the **only** listed candidate that the existing single-column
  data actively contradicts. (C) and (D) satisfy the constraint trivially and correctly.

Uncited / incorrect numbers:

- **"~33% at ΔT≈45C total" (§0 and §4).** No source says this. The 33% was measured at
  **ΔT0 = 13.28 °C** (absolute 43.53 °C, ambient 30.25 °C) — `947709a8`. The "45" appears to be
  `b64fe09d`'s joint hold **target of 45.4 °C absolute**, read as a ΔT. Two different quantities
  from two different runs.
- **The joint total duty (§4 step 2, §5).** §4 anchors the low level at "~0.51 ... matching
  `coupling_joint_identification_capture`'s low-ΔT hold duties 0.1499+0.1728+0.1853", and §5
  repeats "the one measured joint-hold total of ~0.51". But the **33% deficit was measured at a
  different plateau**: `u = [0.163, 0.210, 0.247]`, `sum u = 0.620` (`947709a8`). The doc
  attributes the deficit to the 0.508 hold. This matters directly — 0.620 is the number class (B)
  must be evaluated at, and it is the number R1 uses.
- **"c02 2.9% week-over-week" (§0).** `947709a8` states the two measurements were **one day
  apart**, not a week. Propagated mislabel.
- **Source filename wrong, twice** (§0 preamble and Sources): cited as
  `docs/audits/single_column_superlinearity_discriminator_2026-09-11.md`; the file is
  `docs/audits/z2_single_column_superlinearity_discriminator_2026-09-11.md`. The `z2_` prefix is
  load-bearing — it is what makes slip (b) above visible.

Consistency with established facts — checked, and the doc is **correct** on: buoyancy refuted
(stated plainly, not hedged); single-column transport linear; superposition failing by ~33%; zone
stacking (z0 top, z2 bottom) cited correctly in §1; and candidate (D) honestly flagged as
unfalsifiable on this bench rather than ranked on thin evidence. §2's refusal to paper over the
sign reversal, and §5's conclusion that no validated bounded region exists, are both sound and
should be kept as written. The 0.5 °C floor is respected throughout — the effects under discussion
(4.3-4.5 °C, and R1's 3.1 °C miss) are all well above it, and the ≤0.46 °C ambient term is
correctly set aside rather than chased.

## R6. Summary

| Point | Verdict |
|---|---|
| §1B/§3 invisibility claim | **Refuted.** Single-column swept 0.38x-1.48x the joint total power; ratio flat. |
| §3 recommendation of class (B) | **Refuted**, independent of `gamma` and `k_loss`. Do not adopt. |
| `gamma = 4/3` reuse | **Not justified.** Wrong variable, wrong mechanism, refuted provenance. |
| Sign/direction of the loss term | **Correct**, but non-selective — (C) and (D) match it too. |
| §4 experiment | Class-B arm already answered; high-power row infeasible; **~0.6-0.7σ, underpowered**. |
| §2 "sign reversal unexplained" | Correct, and **stronger against (B) than stated** — a sign contradiction at (B)'s own worst case. |
| §5 bounded-region analysis | Sound; keep. |

**Recommended next step (design only, nothing run here):** treat "the deficit depends on how total
power is split, not on the total" as the surviving finding, and re-specify §4 as a replicated
class-A / class-C allocation contrast at a single feasible total near `sum u ~ 0.62`, sized
against a ~1.1-1.3 °C per-plateau uncertainty rather than ±0.3-0.5 °C.

Reviewed against: `docs/audits/joint_vs_singlecolumn_matched_dT0_2026-09-11.md` (`5844a3e8`,
`947709a8`), `docs/audits/z2_single_column_superlinearity_discriminator_2026-09-11.md`
(`cf1f8ce9`, `1b9afd4f`), `docs/audits/coupling_joint_identification_capture_2026-09-10.md`
(`b64fe09d`), `docs/audits/cplval75_coupling_verdict_2026-09-10.md`,
`docs/audits/z0_buoyant_coupling_term_proposal_2026-09-10.md`, and a live read-only
`control_get_zones`.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
