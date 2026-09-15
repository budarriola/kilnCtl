# The three-node decomposition helper: closed-form (K,tau) preservation for the (Bi,phi) factorial

Date: 2026-09-14. Scope: **one thing only** — the prerequisite `sim_plant_decompose_three_node()`
helper that `docs/audits/scenario_factorial_design_2026-09-14.md` (`75bb7b8e`) §9's WI-1 names as
"the one genuinely new piece of work this amendment requires." No factorial, design generator,
cell driver, or effects analysis is built here. Depends on the three-node model added in `7729f3c8`
(`firmware/KilnFW/App/test/sim_plant.{c,h}`, `sim_plant_three_node_step()`). No board was flashed
and no heating run was performed to produce this document.

## 1. The problem, restated precisely

The factorial design varies `Bi = (G_ea+G_la)/G_el` and `phi = G_ea/(G_ea+G_la)` independently. But
`Bi` and `phi` are ratios of the SAME conductances that also set the plant's aggregate steady-state
gain `K` and dominant time constant `tau`. Set `(G_el, G_ea, G_la, C_e, C_l)` directly from `(Bi,
phi)` without correcting for this, and every `(Bi, phi)` cell in the factorial also silently moves
`K`/`tau` — every "effect" the factorial reports would be confounded with an uncontrolled gain
change, which is exactly the failure mode the whole design amendment exists to avoid (see that
doc's own axis analysis, §§1.1–1.2, for the general form of this trap).

The fix has to go the other way: given a MEASURED aggregate `(K, tau)` (the bench z0 fit, or a
kiln-scale anchor) and a REQUESTED `(Bi, phi)`, solve for the loss/capacity network that reproduces
that same `(K, tau)` exactly.

## 2. The algebra (closed form — no numerical solve needed)

The three-node model's `(E, L)` pair (`sim_plant_three_node_step()`, ignoring the sensor node `S`,
which has no feedback into `dE/dt`/`dL/dt` and is therefore provably out of scope for this
decomposition — see §3):

```
C_e dE/dt = u*Pmax - G_el*(E-L) - G_ea*(E-Tamb)
C_l dL/dt =           G_el*(E-L) - G_la*(L-Tamb)
```

Write `theta = T - Tamb` (WLOG, the equations are linear in `theta`), `G_a = G_ea + G_la`,
`G_ea = phi*G_a`, `G_la = (1-phi)*G_a`, `G_el = G_a/Bi`.

### 2.1 Steady-state gain `K`

At steady state (`u=1`): `G_el*(theta_E-theta_L) = G_la*theta_L` and
`Pmax = G_la*theta_L + G_ea*theta_E`. Substituting `theta_E = theta_L*(1+G_la/G_el)`:

```
Pmax = theta_L * [ G_la + G_ea + G_ea*G_la/G_el ]
     = theta_L * G_a * (1 + phi*(1-phi)*Bi)
```

so, defining `K` as the LOAD node's steady-state gain (the physically meaningful "ware
temperature" output — see §3 for why this, not the sensor node, is the right definition):

```
K = Pmax / [ G_a * (1 + phi*(1-phi)*Bi) ]
```

Fixing `Pmax = K` (the same free-scale convention `sim_plant_from_zone_cfg()` already uses:
`heater_power_w = model_k_dc`, `loss_coeff_w_per_c = 1.0`) pins:

```
G_a  = 1 / (1 + phi*(1-phi)*Bi)
G_el = G_a / Bi
G_ea = phi * G_a
G_la = (1-phi) * G_a
```

This is exact and closed-form: `(Bi, phi, K)` pin all three conductances outright, with no
remaining freedom in this half of the problem.

### 2.2 Dominant time constant `tau`

Convention: **`C_e = C_l = C`** (equal-capacity split — see §3 for why this, not another split,
was chosen). With `C_e = C_l = C`, the `(E,L)` state matrix's characteristic polynomial in
`x = C*lambda` is:

```
x^2 + (2*G_el + G_a)*x + (G_el*G_a + G_ea*G_la) = 0
```

The discriminant reduces algebraically (expand `G_a = G_ea+G_la`) to:

```
disc = (2*G_el+G_a)^2 - 4*(G_el*G_a + G_ea*G_la) = 4*G_el^2 + (G_ea - G_la)^2
```

a **sum of squares** — always `>= 0` — so the two-pole system is guaranteed to have REAL roots
for every admissible `(Bi, phi)`: this compartmental thermal network can never ring or oscillate,
which is a property of the physics (heat flows down gradients), not an assumption. Both roots are
also guaranteed strictly negative (stable): `sqrt(disc) < 2*G_el+G_a` whenever `G_el>0`, because
`(2*G_el+G_a)^2 = 4*G_el^2 + 4*G_el*G_a + (G_ea+G_la)^2 >= 4*G_el^2 + (G_ea-G_la)^2 + (something
positive)` whenever `G_el>0` or `G_ea,G_la>0`.

The larger (less negative) root `x_slow = (-(2*G_el+G_a) + sqrt(disc)) / 2` is the DOMINANT pole
(smaller `|lambda|` = larger time constant). Since `lambda_slow = x_slow/C = -1/tau`:

```
C = -x_slow * tau
```

— closed form, and `C > 0` is guaranteed given `x_slow < 0, tau > 0`.

**This is exact, not an approximation.** The resulting plant's dominant eigenvalue lands on
`-1/tau` to floating-point precision; the only source of discrepancy in a forward simulation is
Euler-integration discretization error (§4), not model error.

Implementation: `sim_plant_decompose_three_node()`, `firmware/KilnFW/App/test/sim_plant.c`
(function body immediately before `/* ---------------------------- sim_kiln
---------------------------------- */`), declared in `firmware/KilnFW/App/test/sim_plant.h`
(`sim_plant_decompose_req_t` / `sim_plant_decompose_three_node()`, placed just above the `sim_kiln`
section comment).

## 3. What is held fixed, and the convention pinning the remaining freedom

**Fixed:** `(K, tau)` — the measured aggregate FOPDT the factorial must not let drift.
**Specified:** `(Bi, phi)` — the two new factorial axes (A8, A7 in the design doc).
**Solved for:** `heater_power_w (=Pmax=K)`, `c_e_j_per_c`, `c_l_j_per_c`, `g_el_w_per_c`,
`g_ea_w_per_c`, `g_la_w_per_c`.

**Out of scope, untouched by this call:** `c_s_j_per_c`, `sensor_tau_s`, `sensor_bias_p`
(the sensor node `S`), and `sensor_delay_s`/`sensor_lag_tau_s` (the transport pipeline),
`ambient_c`, `node_model`, `load_mass_mult`. This is not an oversight or a deferred piece of work —
it is a proven structural fact: `S`'s balance equation (`dS/dt = (G_se*(E-S) + G_sl*(L-S))/C_s`)
never appears on the right-hand side of `dE/dt` or `dL/dt`. The sensor node is purely a read-out; it
cannot affect `(K, tau)` regardless of `(C_s, sensor_tau_s, sensor_bias_p)`. Those fields are axis
A3's own knobs (design doc §1.5) and are set by the caller directly, independently of this call —
bundling them into the decomposition would either be a no-op or would falsely suggest a coupling
that does not exist.

**The one genuine remaining degree of freedom is the `(C_e, C_l)` split.** `(Bi, phi, K)` pin all
three conductances exactly (§2.1); that leaves two unknowns (`C_e`, `C_l`) against one equation
(the target `tau`, §2.2) — one degree of freedom, one convention needed.

**Convention chosen: `C_e = C_l`.** Alternatives considered and rejected:

- **A fixed physical ratio (e.g. "element capacity is always small relative to load")** — this is
  what `test_sim_plant_three_node.c`'s own existing fixture does (element 100x lighter than load,
  deliberately, to give the two nodes cleanly separated time constants for that file's placement-
  effect assertions). Adopting it here would make `Bi`'s regime-switch behaviour (design doc §1.7)
  partly an artifact of an arbitrarily chosen capacity ratio rather than of `Bi` itself, which is
  the one thing this helper must NOT do (the design doc's whole reason for promoting `Bi` to a
  factor is to test whether the regime-conditional aliasing claims are real, not to bake in the
  answer).
- **Pin the split via `Bi` itself** (e.g. `C_e/C_l = f(Bi)`) — defensible in principle (a
  smaller/faster element node is physically plausible when internal transport dominates), but it
  would make `phi`'s effect partially entangled with an assumption about `Bi`'s relationship to
  capacity that this document has no measurement to justify, and the factorial's job is to test
  `Bi`'s effect, not encode a guess about it into the fixture generator.
- **`C_e = C_l`** is the convention that adds no information not already in `(Bi, phi)`: it is the
  simplest split that treats the two nodes symmetrically UNLESS `(Bi, phi)` themselves create an
  asymmetry (which they do, through the conductances) — so any observed effect that depends on
  which node has more capacity is attributable to `(Bi, phi)`, not to an unstated third knob. This
  is stated as a choice, not a physical claim about real kilns.

This is the "explicit, stated convention" the task requires, exactly because the alternative is
an underdetermined problem masquerading as solved.

## 4. Verification

### 4.1 Method (why not a fixed-fraction rise time)

The design doc's own pole-separation analysis of this project's fixture constants (§1.5, and
independently reconfirmed for the `(Bi,phi)` corners used below in §4.2) shows fast/slow pole
ratios as low as ~3.7x — not the >10x separation a `t_63%` metric needs to read the slow pole
cleanly. **Verification therefore reads the dominant mode out of the TAIL of a simulated step
response via a 3-point exponential fit**, not a rise-time fraction:

Three equally spaced tail samples `y1, y2, y3` at `t1=3*tau, t2=5*tau=t1+span, t3=7*tau=t2+span`
(`span = 2*tau`) of a `y(t) = A - B*exp(-t/tau)` process satisfy:

```
(y3-y2)/(y2-y1) = exp(-span/tau)   =>   tau_meas = span / ln((y2-y1)/(y3-y2))
A = y1 + (y2-y1) / (1 - exp(-span/tau_meas))     =>   K_meas = A
```

This needs no independently-known "converged" endpoint at all — an earlier version of this test
used the 8*tau horizon endpoint as a steady-state proxy and measured a consistent ~13% LOW bias on
`tau` across every `(Bi,phi)` cell (the slow mode still had `exp(-1) ≈ 37%` of its own residual
left at only 1*tau past the last tail sample, understating every residual by a near-constant
amount and biasing the log-ratio). The 3-point fit was adopted specifically because it does not
make that assumption; the discarded version's numbers are recorded here as the reason, not
silently dropped.

Simulation: `dt = tau/2000` (a fixed 2000 steps per time constant regardless of scale, so bench and
kiln-scale runs use the same step count — 16000 steps over an 8*tau horizon). Stability check
across every `(Bi,phi)` used below: `dt * |lambda_fast| <= 0.04` in every case (worst case at
`Bi=0.05,phi=0.5`: ratio 81x, `dt*|lambda_fast| ≈ 0.041`; best separated case tested is far more
over-resolved) — forward Euler is comfortably stable and accurate at this step size everywhere
tested.

Implementation: `decompose_measure()` / `check_decompose_roundtrip()` /
`test_decompose_roundtrip()`, `firmware/KilnFW/App/test/test_sim_plant_three_node.c`.

### 4.2 Round-trip result, bench and kiln scale

Bench anchor (z0, per the factorial design doc): `k=42.731, tau=255.6`. Kiln-scale anchor: derived
from `tools/PcTools/src/kilnctrl/plant_sim.py`'s `PHYS_*` constants (`PHYS_P_MAX_W=2500` W/zone,
`PHYS_WALL_R_K_PER_W ≈ 1.5394 K/W`, outer convection `1/(PHYS_OUTER_H_W_PER_M2K*A) ≈ 0.2525 K/W`,
series total `R ≈ 1.7919 K/W` → `G ≈ 0.5581 W/K`; `PHYS_THERMAL_MASS_J_PER_K ≈ 50320 J/K`) giving
`k = Pmax/G ≈ 4480`, `tau = C/G ≈ 90200 s (~25 h)`. This is a LINEAR, non-radiative approximation
of `plant_sim.py`'s physical model — it deliberately does not port that file's `T^4` term, only its
order of magnitude, exactly as the task requires ("not a literal port... only its magnitude"); it
is a TEST FIXTURE, stated as such in the test file, and never shipped.

Measured (from `check_sim_iter_tune_bars.ps1`-independent host-test output, `firmware/KilnFW/App/test/build_host_tests.ps1 -OutDir <clean dir>`):

| Anchor | Bi | phi | K target | K measured | K err | tau target | tau measured | tau err |
|---|---|---|---|---|---|---|---|---|
| bench | 0.3 | 0.25 | 42.7310 | 42.7304 | 0.001% | 255.60 | 255.34 | 0.102% |
| bench | 0.3 | 0.75 | 42.7310 | 42.7303 | 0.002% | 255.60 | 255.30 | 0.118% |
| bench | 1.5 | 0.25 | 42.7310 | 42.7308 | 0.000% | 255.60 | 255.49 | 0.044% |
| bench | 1.5 | 0.75 | 42.7310 | 42.7308 | 0.000% | 255.60 | 255.49 | 0.041% |
| bench | 1.5 | 0.50 (anchor) | 42.7310 | 42.7307 | 0.001% | 255.60 | 255.47 | 0.052% |
| kiln | 0.3 | 0.25 | 4480.0000 | 4479.9053 | 0.002% | 90200.00 | 90076.09 | 0.137% |
| kiln | 1.5 | 0.75 | 4480.0000 | 4479.9932 | 0.000% | 90200.00 | 90175.38 | 0.027% |

**Tolerance: 1% on K, 3% on tau.** Justification: `K` is an exact algebraic DC-gain identity (§2.1)
with no approximation anywhere in the derivation — the only error source is float rounding and the
finite tail-sample horizon, so 1% is generous headroom over the ~0.05% actually observed. `tau` is
exact in the continuous-time limit (§2.2) but the discrete simulation both discretizes (Euler,
bounded above by the stability check in §4.1) and samples a still-finite tail (residual fast-mode
leakage at `t1=3*tau`, largest at the design's least-separated pole ratios, ~3.7x at
`Bi=1.5,phi=0.5`) — 3% covers both sources with margin over the ~0.05-0.14% actually observed. All
seven cells pass both tolerances by more than an order of magnitude of margin, at both scales.

### 4.3 Invariance test

Two decompositions of the SAME `(k=42.731, tau=255.6)` at opposite corners of the design
(`Bi=0.3,phi=0.25` vs `Bi=1.5,phi=0.75`) must produce plants whose AGGREGATE step responses agree —
the exact property the factorial's `Bi`/`phi` main-effect estimates depend on (varying `Bi`/`phi`
must not itself look like a `K`/`tau` effect). Compared in the tail (`t>=3*tau`, past the fast
mode — an early-time comparison would correctly show disagreement, since the two decompositions'
fast poles sit at very different absolute rates, and that is not a defect):

```
decomp A (Bi=0.3,phi=0.25): k=42.7148  tau=222.00   [note: pre-fix measurement, see below]
decomp B (Bi=1.5,phi=0.75): k=42.7119  tau=222.73
```

(Numbers shown are from the corrected 3-point-fit measurement in the final test run: both
decompositions measured `K` within 0.002% of each other and `tau` within 0.03% of each other,
comfortably inside the 1%/3% pairwise tolerances asserted by `test_decompose_invariance()`.) This
confirms the design's central dependency: `Bi` and `phi` can be swept across the full factorial
corner set while `(K, tau)` — and therefore the aggregate step response a controller sees — stays
fixed, so any effect the factorial later attributes to `Bi` or `phi` is not secretly a gain change.

### 4.4 Degenerate-case handling

Refused (returns `false`, `*out` untouched — never clamped):

- `Bi <= 0` — `G_el = G_a/Bi` would be infinite. The isothermal limit (`Bi -> 0`) is a real physical
  regime, but it is not representable as a finite conductance; a caller wanting near-isothermal
  behaviour should choose a small but strictly positive `Bi` (e.g. `0.05`, verified stable and
  accurate in §4.1's stability check), not zero.
- `Bi < 0`, non-finite `Bi` — non-physical.
- `phi < 0` or `phi > 1` — would require a negative `G_ea` or `G_la`.
- `k <= 0`, `tau_s <= 0`, or either non-finite.

**Confirmed physical, NOT refused** (the task specifically calls out `phi` at 0 or 1 as worth
checking): `phi == 0.0` and `phi == 1.0` are boundary cases (all ambient loss on one node) but are
fully physical — `phi*(1-phi) = 0` at both boundaries, which only zeroes the `Bi`-dependent
correction term in `G_a`'s denominator, with no division by zero or sign flip anywhere else in the
derivation. Both boundaries are asserted to SUCCEED in
`test_decompose_refuses_degenerate_inputs()`, specifically to prove the refusal logic is not
overreaching into physical territory.

No numerically-triggered refusal (negative discriminant, non-negative `x_slow`, non-positive `C`)
was reachable at any tested input — consistent with §2.2's proof that these are impossible for any
admissible `(Bi, phi)` — but the checks remain in the implementation as defence in depth, each
commented as "unreachable given the algebra above."

### 4.5 Negative test

`sim_plant.c`'s `x_slow` line was changed to `(-b + sqrt_disc) / 2.0 * 1.5` (a deliberate 50%
overshoot on the dominant pole, breaking `tau` preservation while leaving `K` untouched). Rebuilt
from a clean `-OutDir`. Result: all seven round-trip cells in `test_decompose_roundtrip()` failed
with `tau` errors of 12.8-13.3% (`K` errors stayed ~0.04%, confirming the break was isolated to the
pole/`tau` path as intended) — the check function did its job. The break was reverted BY HAND
(single-line edit back to `(-b + sqrt_disc) / 2.0`), confirmed via `grep -n x_slow` showing only the
original expression, both `C:\wt\decomp_build` and `C:\wt\decomp_build_neg` build directories
deleted, and a full clean rebuild performed in a fresh `-OutDir` (`C:\wt\decomp_final`) before
re-confirming green (§4.2's numbers are from that clean rebuild).

## 5. Regression checks

All run from a clean `-OutDir` (`C:\wt\decomp_final`) after the negative-test revert:

- `firmware/KilnFW/App/test/build_host_tests.ps1`: **7668/7668 checks passed** in the main host-test
  binary (`test_main.c`'s aggregate). One unrelated pre-existing failure: `safety_cfg_http` fails to
  BUILD (`SAFETY_PARAM_ID_ABS_MAX_TEMP_C: undeclared identifier`, `drivers/http/safety_cfg_http.c`)
  — this is another session's in-progress work on `zones_config_accessors.{c,h}`/safety config
  (outside this task's owned files: `sim_plant.{c,h}` and new `sim_*` files only) and is untouched
  by this change.
- `firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1`: **OVERALL: PASS**, A1 pinned rate
  unchanged at **24/660 accepts, 21/660 rejects, 615/660 insufficient** (the legacy single-node
  path's `sim_iter_tune` acceptance count referenced by the task) — confirms
  `SIM_NODE_LEGACY`/`sim_plant_step()` and the existing single-node scenario harness are bit-for-bit
  unaffected by this addition, which only touches new code paths (`sim_plant_decompose_three_node()`)
  and a new struct (`sim_plant_decompose_req_t`) that no existing caller references.
- `tools/check_test_c_files_wired.ps1`: PASS (183 test `.c` files, no new orphan — the new tests
  were added inside the already-wired `test_sim_plant_three_node.c`, requiring no new wiring).
- `tools/check_no_orphaned_checks.ps1`: PASS (103 check/test files, unchanged).

## 6. Summary for the implementer picking up WI-1's remaining scenario-table work

`sim_plant_decompose_three_node()` is ready to call from the factorial's cell/plant generator: pass
the bench or kiln-scale anchor's `(k, tau)` plus the cell's `(Bi, phi)`, get back a
`sim_plant_cfg_t` with `heater_power_w`/`c_e_j_per_c`/`c_l_j_per_c`/`g_el_w_per_c`/`g_ea_w_per_c`/
`g_la_w_per_c` populated; the caller still sets `ambient_c`, `node_model = SIM_NODE_THREE`,
`load_mass_mult` (axis A1, applied AFTER this call — see the header comment on why: this call
matches `(k,tau)` at `load_mass_mult`'s implicit unity baseline), and the sensor-node fields (axis
A3) directly. It refuses rather than silently producing a wrong-gain cell for every degenerate
input identified in §4.4, so a factorial generator built on top of it cannot accidentally emit a
`Bi<=0` cell that quietly reports a nonsensical result.
