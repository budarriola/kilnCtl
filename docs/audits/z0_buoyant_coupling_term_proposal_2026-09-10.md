# Candidate replacement term for z0's coupling row: buoyant transport, ΔT-superlinear

**Status: design proposal, paper + arithmetic only. Nothing flashed, no board touched, no
simulator code changed, no `run_all_checks.ps1` baseline moved.** Written against
`docs/audits/coupling_joint_identification_capture_2026-09-10.md` (`b64fe09d`), which closed
the question of whether any linear `G` can fit z0's row (it cannot — sign-flip, not scale).

## 1. The term

**Physical picture.** z0 is the top zone, z2 the bottom (`project_zone_physical_arrangement`).
Hot air in the chamber rises. The energy z0 receives from its neighbours is not a diffusive
exchange proportional to the *temperature difference between zones* — that model was already
tried and rejected in this codebase for exactly this reason (the simulator's original
`coupling_w_per_c[i][j] * (T_j - T_i)` term went identically to zero at a uniform-temperature
dwell, `project_sim_firmware_coupling_model_mismatch`, fixed in `d63a5591`). A joint dwell is
exactly the operating point that matters here, and at a joint dwell all three zones sit close to
the same temperature — so any candidate term must stay **non-zero when `T_j ≈ T_i`**, driven
instead by how hot the source zone runs **relative to ambient**, not relative to its neighbour.

**Form.** For each zone `i` and each zone `j` physically below it:

```
P_buoy(i <- j) = k_buoy[i][j] * max(0, T_j - T_amb)^gamma
```

summed over `j` below `i`. z0 receives from z1 and z2; z1 receives from z2; z2 (bottom) receives
none — which is a direct structural explanation, not a coincidence, for why z2's row is the one
that came back clean and scale-only (`c ≈ -0.04`, no offset) in the joint-hold acceptance test:
z2 is the one row this term does not touch at all.

**Exponent.** Natural-convection heat transfer scales as `Nu ~ Ra^n`, and `Q ~ Nu * ΔT ~
ΔT^(1+n)`. Turbulent free convection (the regime a kiln's ΔT and length scale plausibly put it
in, `Ra` likely well above `10^9` between shelves) gives `n = 1/3`, i.e. `Q ~ ΔT^(4/3) =
ΔT^1.333`. That is close to the value prior analysis independently measured by log-log fit
across eight plateaus for z0: **1.366** (`project_z0_coupling_is_shape_not_scale` /
`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`). Laminar free convection (`n = 1/4`) gives
`ΔT^1.25`, still superlinear but a looser match. **`gamma = 4/3` is the proposed value — read
off a standard convection-scaling regime, not fitted to this data — and it lands within ~2% of
the independently measured exponent, which is the check the prompt asked for: the physics
constrains the exponent to a narrow, cite-able range, and the previously measured number sits in
it.** z2's own measured exponent, ≈1.04 (clean, near-linear), is exactly what this form predicts
for a zone with no zone below it: its own diagonal term is ordinary conduction/duty-driven and
this buoyant term contributes zero to its row.

**Why this isn't the same trap as the old bug.** The rejected term was zero at `T_j ≈ T_i`. This
term is `max(0, T_j - T_amb)^gamma`, which is large precisely when the whole chamber (including
the receiving zone) is hot — the joint-dwell condition where the old term vanished and this
project's own credibility gate went blind to coupling. It does not require z0 to be cooler than
its neighbours; it only requires the source zone to be hot.

## 2. Sanity check against the already-published data (no new fitting)

Re-deriving the sign pattern from `coupling_joint_identification_capture_2026-09-10.md`'s own
Criterion A table (`pred`, `obs` duty per plateau) confirms the direction this term predicts,
using only arithmetic on numbers already in that doc:

| Plateau | z0 pred−obs |
|---|---|
| low ΔT (~11.7°C) | **−0.080** (linear model *under*-predicts z0's own duty need) |
| 62°C | +0.016 |
| 70°C | +0.054 |
| 75°C | **+0.083** (linear model *over*-predicts z0's own duty need) |

A linear (additive, `gamma=1`) coupling term, once calibrated anywhere in this range, must
either under- or over-predict consistently — it cannot cross zero, because a straight line
through the origin has one slope everywhere. A **convex** (`gamma>1`) source term does exactly
what the data shows: at low ΔT it delivers *less* free heat than a line calibrated at high ΔT
would suggest (z0 has to supply more of its own duty — matches the negative residual), and at
high ΔT it delivers *more* free heat than that same line assumes (z0 needs less of its own duty
— matches the growing positive residual). This is the qualitative signature `gamma>1` produces,
independent of the exact value of `gamma`; it is not proof of `4/3` specifically, only proof
that convexity, not a scale error, is the right shape correction — which is exactly what
`b64fe09d`'s sign-change check already concluded from the same numbers.

z1 and z2 do not show this crossing (both are monotonic in the pred−obs sign across the four
plateaus in the same table), consistent with the structural claim that the buoyant term touches
z0 (and to a lesser degree z1) but not z2.

**This is confirmation of direction and plausibility, not a fit.** Four points per row, already
spent on rejecting the linear matrix, cannot also identify a two-parameter nonlinear term
(`k_buoy`, and in principle `gamma`) without just refitting noise — see §4.

## 3. What this costs in the firmware's model class

The firmware solves `G·u = b` with `G = diag(model_k_dc) + coupling_coeff`, entirely linear and
duty-driven (`firmware/KilnFW/App/drivers/control/zone_coupling_solve.c`). A `ΔT`-driven,
exponentiated term is a genuinely different class from that solve, and the honest costs are:

- **The solve is no longer linear.** `G` would need an entry that depends on `T_j`, i.e. on the
  *result* of the system, not just the commanded duty. `G·u=b` can no longer be inverted once
  and reused; it becomes `G(u)·u = b`, requiring iteration (fixed-point or one Newton step per
  tick) rather than a single linear solve.
- **Hold feedforward** (a fixed target) tolerates this well: iterate `u -> G(u)·u=b` a handful of
  times per control tick until it converges, exactly the kind of gain-scheduling this codebase
  already does for radiative loss at cone temperature (`sim_kiln_step`'s
  `radiative_coeff_w_per_k4` term, evaluated at the *current* `T_i` each tick rather than solved
  in closed form) — same technique, not a new one.
- **Climb feedforward** is the harder case: duty needed changes continuously as `T_j` moves, so
  the linearization point has to be re-evaluated every tick rather than once per segment. That
  is added per-tick compute in code that memory already flags as doing too much per tick under
  lock (`project_httpd_wedge_is_dram` / the `dashboard_get_status()` note in `CLAUDE.md` about
  never holding a module lock across producer calls) — a fixed-point iteration should be done
  **outside** any lock, on a cached snapshot, for the same reason already established there.
- **Practical mitigation, if this is ever adopted on-hardware:** don't solve `G(u)` in closed
  form at all. Treat the buoyant term as a *correction added after* the existing linear solve,
  using **last tick's** `T_j` (already available, no new sensor read) rather than this tick's
  unknown `u`. That turns it into a one-step-lagged, still-linear-per-tick calculation — the
  same "recompute the schedule from the last known state" trick as the radiative term — at the
  cost of a small one-tick lag in the correction itself, which is negligible against `tau_s`
  values of 250+ seconds.
- **Same-class requirement (the trap named in this task).** If this is ever implemented, it must
  land in the simulator and the firmware in the same form at the same time. Implementing it only
  in the simulator (to make a credibility-gate bar look better) while the firmware stays purely
  duty-linear would silently reintroduce exactly the class of bug `d63a5591` fixed — a simulator
  that scores well against captures for a mechanism the real board does not implement. **No code
  change is proposed here for that reason: this document is the design, not the patch.** A
  future patch should add the term to `zone_coupling_solve.c` and `sim_plant.c`'s
  `sim_kiln_step()` in the same commit, gated by one config value read by both.

## 4. What is underdetermined

- **`k_buoy[i][j]` is not identified by anything on hand.** The existing data was captured to
  reject a linear matrix, not to fit a nonlinear one; extracting `k_buoy` from it would mean
  fitting a 2-parameter nonlinear model to 4 points already spent on a different rejection —
  not defensible.
- **The exponent `4/3` is a physics-motivated default, not a measured constant for this
  specific chamber.** The 1.366 log-log measurement it is checked against came from 8 plateaus
  of a *different* (now-superseded) fitting pass and was explicitly flagged there as needing
  confirmation, not as final.
- **Whether z1 needs its own (smaller) instance of this term, or whether its `−2.03°C` offset
  has a different cause entirely**, is open — z1's pred−obs sign does not cross zero in the
  available data (§2), which this term's mechanism does not by itself explain; the doc's own
  "z1 intermediate" characterization stands without a mechanism yet.
- **Whether `T_amb` (session-start ambient) is the right reference, versus a live/room ambient
  that itself drifted ~3°C during the capture session**, is unresolved — the capture doc itself
  flagged ambient drift as material enough to re-anchor each cooldown rather than use a fixed
  number.

## 5. The discriminating measurement

The 2026-09-10 capture cost ~12 hours and was already spent rejecting the linear matrix; it
cannot also identify a nonlinear exponent. What would, cheaply:

**Repeat a single-column step (z2 alone, or z1 alone — a bottom-zone driving neighbour) at TWO
different held duty levels, e.g. `u=0.30` and `u=0.60`, each run to full settle, and measure z0's
induced steady-state ΔT (or induced duty need, held at a fixed low z0 target) at each level.**

- Under the **existing linear model**, z0's induced rise per unit of the driving zone's duty is
  constant across the two levels (a straight line through the origin — the ratio
  `ΔT0_induced / u_driver` is the same at both).
- Under the **proposed `gamma=4/3` term**, driven by `(T_driver - T_amb)` rather than `u`
  directly, the induced ratio should be measurably larger at the higher-duty step, since
  `T_driver - T_amb` itself scales roughly with `u` at steady state, making the induced z0
  contribution scale as `u^gamma`, not `u`.
- This is a **single-column, two-level** protocol (no joint hold, no three-zone dwell), an order
  of magnitude cheaper than the full capture, and it directly answers "is z0's coupling
  superlinear in the source zone's own operating point," independent of whatever z1's unresolved
  offset turns out to be.

## 6. Bottom line

The proposed term (`k_buoy * max(0, T_j - T_amb)^(4/3)`, summed over zones below `i`) is
physically derived (turbulent natural-convection scaling), lands within ~2% of the independently
measured z0 exponent, reproduces the exact sign-flip direction in the already-published data by
arithmetic alone (§2), and structurally explains why z2's row came back clean while z0's did
not (z2 has nothing below it to receive this term from). It is **not yet fitted, not yet coded,
and not yet checked against the simulator's credibility-gate harness** — doing either
responsibly requires either the two-level discriminating measurement in §5, or accepting that
`k_buoy` (and possibly `gamma` itself) remain an open, explicitly-flagged parameter rather than
a number quietly baked into the simulator. Implementing it asymmetrically (simulator only) would
recreate the exact mismatch class `d63a5591` fixed, so no code change accompanies this proposal.
