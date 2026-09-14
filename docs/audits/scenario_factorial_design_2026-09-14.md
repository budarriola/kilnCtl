# Scenario coverage as an experiment: a factorial design for the simulation suite

Date: 2026-09-14. Status: DESIGN AMENDMENT to `docs/SCENARIO_SIMULATION_PLAN.md`
(`32b10a34`). **This document writes no production code and edits no plan.** It
is written to be folded into that plan by whoever is implementing it; until it
is, the plan as written stands. No board was flashed and no heating run was
performed to produce it.

Scope note: this amendment replaces **only** §4.2 of the plan (the 13-row
hand-picked scenario table). §§0-3, §4.1 (arms), §4.3 (runner, determinism),
§5 (scoring), §6 (adaptation) and §7 (work items) are unchanged and this
document depends on all of them. The plan's 13 named scenarios survive as
named anchor points (§7 below).

---

## 0. The question and the short answer

The owner asks whether the scenario space is *systematically* covered: larger /
smaller kiln, higher / lower temperature, sensor closer to / further from the
elements, faster / slower heating, higher / lower / **uneven** losses — and
their permutations. The plan answers with 13 hand-picked cases.

At under 20 ms per simulated firing there is no cost argument for hand-picking.
But "run everything" is not the answer either, because **most of the nominal
axes are not independent in the plan's own equations**, and a factorial over
aliased axes is mostly duplicate cells wearing different names.

Short answer, worked out in §1:

| Nominal axis the owner named | Verdict |
|---|---|
| Larger / smaller **kiln** | **Not an axis.** Exactly degenerate — an exact scaling invariance of the equations. It decomposes into two things that *are* axes: bulk time constant and power headroom. |
| Heavier / lighter **load** | **Axis** (bulk time constant `tau_L`). The same axis a "bigger kiln" moves; distinguished from it only by headroom. |
| Higher / lower **temperature** | **Axis, but only as a SPAN.** At a *fixed* temperature it is an exact alias of a `tau`-mismatch. Its independent content is that the loss conductance drifts ~20x *during* one firing, which no static mistune can express. |
| Higher / lower **loss magnitude** (coordinator's axis 1) | **Alias.** Collapses onto exactly the same ray as fixed-temperature `s(T)`: both scale the ambient-loss denominator that `K` and `tau` share. Folded into the temperature-span axis as its starting point. Not a separate axis. |
| **Uneven** losses, element vs load (coordinator's axis 2) | **Axis.** Genuinely independent; the coordinator's reading is upheld. It sets the element-to-load gradient, which is the quantity sensor placement weights. Strong predicted interaction with placement. |
| Sensor closer to / further from the elements | **Axis — but regime-conditional.** Independent only where the duty hits a bound or the nodes are not near-isothermal; otherwise it aliases onto (dead-time, `tau`) mismatch. |
| Faster / slower **heating** | **Axis** (excitation, not plant). Couples to headroom through the required duty. |
| PID tune quality | **Axis**, categorical, 2 degrees of freedom (loop-gain error, integral-time error), not the 3 the mismatch triple suggests. |
| Element power **headroom** | **Axis.** Survives §1 and is kept explicitly. |
| **Internal vs external resistance** (`Bi`, §1.7) | **Axis, and the hidden one.** Not on anyone's list. It is the regime switch that decides whether three of the axes above are independent at all. Promoted to a factor. |

Eight factors. **Design: a full 2^8 factorial (256 cells) with a computed
feasibility mask removing one 3-factor corner (32 cells), leaving 224**, plus a
39-cell second-stage block for the two factors where curvature is predicted and
for the two remaining tune levels. **263 cells, ~6,100 firings, ~2 minutes of
compute.** A full 2-level factorial at this size has **zero aliasing** —
resolution VIII — so there is no fractional-factorial confounding structure to
declare, which is the cleanest available answer to "which interactions are
aliased".

---

## 1. Step 1, the crux: which axes are real

All of this is worked from the plan's §2.1 three-node equations, not from the
axis names. Write `theta = T - T_amb` throughout; the equations are linear in
`theta` apart from the duty clamp and `s(T)`.

```
dE = ( u*Pmax  - G_el*(E-L) - G_ea*(E-Tamb) ) / C_e * dt
dL = (           G_el*(E-L) - G_la*(L-Tamb) ) / C_l * dt
dS = ( G_se*(E-S) + G_sl*(L-S)              ) / C_s * dt
```

with `G_s = C_s/sensor_tau_s`, `G_se = G_s*p`, `G_sl = G_s*(1-p)`, then the
existing transport-delay ring (`sensor_delay_s`) and first-order lag applied to
`S`. Nine plant parameters: `Pmax, C_e, C_l, C_s, G_el, G_ea, G_la,
sensor_tau_s, p`.

### 1.1 The exact invariance: "bigger kiln" is not an axis

Multiply `Pmax`, `C_e`, `C_l`, `C_s`, `G_el`, `G_ea`, `G_la` all by the same
`lambda`. Every right-hand side above is unchanged: each numerator scales by
`lambda`, each denominator scales by `lambda`. `p` and `sensor_tau_s` are
ratios and are untouched. **The trajectory in `(E, L, S)` is bit-for-bit
identical.**

This is not an approximation, it is an exact symmetry of the model, and it is
the same free scale the plan's §1.1 already notes when it says
`loss_coeff_w_per_c` is "pinned to the free scale 1.0" and the units are
"nominal, not physical".

**Consequence:** a kiln that is twice as big *in every respect* — twice the
element power, twice the mass, twice the surface area to lose through — fires
identically. "Kiln size" as a standalone axis generates 100 % duplicate cells.
What makes a big kiln behave differently is only ever **disproportion**:

- mass relative to power and to loss -> the bulk time constant `tau_L = C_l/G_la`;
- power relative to loss -> the ceiling and the saturation margin.

So **"larger kiln" = (`tau_L` up) x (headroom down)**, and **"heavier load" =
(`tau_L` up) at headroom held fixed**. They are the same axis plus or minus the
second one. Modelling both is modelling one twice.

This also disposes of one of the unphysical cells the task asked about in
advance: "a huge kiln with tiny thermal mass" is not unphysical, it is
*meaningless*, because absolute size is not a coordinate of this model at all.
Only `tau_L` and headroom are, and both are expressible at any scale.

### 1.2 Loss magnitude is an alias of fixed-temperature `s(T)` (coordinator's axis 1)

Take the near-isothermal limit `G_el >> G_ea, G_la` (nodes strongly coupled,
which is the ordinary kiln case). Then `E ~= L = theta` and the steady-state
balance `u*Pmax = G_la*theta_L + G_ea*theta_E` collapses to

```
K   = Pmax / (G_ea + G_la)          tau_eff = (C_e + C_l) / (G_ea + G_la)
```

Scaling both ambient conductances by `g` divides **both** `K` and `tau` by `g`.
That is precisely the ray the plan's `s(T)` moves along — the plan's §2.3 says
so in the same words ("`K = P_max/C_loss` and `tau = C_thermal/C_loss` share
the same conductance denominator, so both scale as `1/s(T)`"), citing
`docs/audits/high_temperature_transfer_analysis_2026-09-08.md`.

**Therefore a better- or worse-insulated kiln and a hotter or colder kiln
produce the same family of trajectories.** The coordinator's suspicion is
confirmed, and it is stronger than "partly aliased with `k`": it is aliased
with the *joint* `(k, tau)` movement, not with `k` alone — a lossier kiln does
not just reach a lower rise, it also gets *faster*.

Worse (for the idea of it being an axis), in the controller's frame it is
nearly invisible. Under SIMC with `lambda = 3L`:

```
Kc = tau / (K * 4L)      ->   (tau/g) / ((K/g) * 4L)  =  unchanged
Ti = min(tau, 16L)       ->   min(tau/g, 16L)          =  changed
Td = L/2                 ->   unchanged
```

Because the transport delay `L` is **not** a loss conductance and is not
scaled, a pure loss-magnitude change leaves the SIMC proportional gain exactly
invariant and only shortens the integral time. **Static loss magnitude is an
alias of `mismatch_tau`,** which the tune axis already covers.

**Decision: no loss-magnitude axis.** It is realised instead as the *starting
point* of the temperature-span axis (§1.3), whose levels already span
`s = 1.00` (bench) to `s = 24.16` (cone 10) — a 24x range of effective loss
magnitude, far wider than any insulation difference worth modelling, and
obtained for free.

### 1.3 Temperature is an axis only as a SPAN

`s(T) = 0.95 + 0.05*((T+273.15)/328.15)^4` (`f_rad = 0.05` **[ASSUMED]**,
normalised `s(55) = 1.0`). Recomputed here, independently of the plan:

| T (deg C) | 24 | 55 | 60 | 200 | 600 | 900 | 1200 | 1250 |
|---|---|---|---|---|---|---|---|---|
| `s(T)` | 0.9836 | 1.0000 | 1.0031 | 1.1661 | 3.4563 | 9.1176 | **21.2581** | 24.1587 |

`s(1200) = 21.2581` reproduces the published table value 21.3 to within 0.2 %,
so the port in WI-2 has a checkable target. (WI-2's acceptance already asks for
0.5 %; it will pass.)

By §1.2, `s` evaluated at a *fixed* temperature is an alias. What is **not** an
alias is that `s` changes *within a single firing*:

- a bench firing 24 -> 60 deg C spans `s` 0.9836 -> 1.0031, a ratio of **1.02** — flat;
- a kiln firing 200 -> 1250 deg C spans `s` 1.1661 -> 24.1587, a ratio of **20.7**.

No static mistune, no load change, and no insulation change can produce a plant
whose `K` and `tau` both fall 20x *during the run the controller is running*.
That drift is the entire independent content of the temperature axis, and it is
the only thing in this model that makes a gain *schedule* (scenario S11) a
different object from a gain *choice*.

**Axis A4 is therefore defined as the loss-scale span ratio
`R_s = s(T_end)/s(T_start)`, not as "temperature".** Two levels, `1.0` and
`~20`, and the axis is monotone in "how much the plant moves under the
controller's feet" — so two levels, not three (§3).

On the coordinator's second meaning of "uneven" — loss varying with temperature
rather than position: it is **already this axis**, exactly. `s(T)` is the
radiative/convective split (`f_rad` is literally the radiative fraction at the
reference), so a kiln "whose loss character changes across its range" *is* a
kiln with `R_s > 1`. It needs no new axis and no new constant. `plant_sim.py`'s
`loss_conductance_scale()` is the sanctioned route (WI-2 ports it);
`radiative_coeff_w_per_k4` stays unused, for the reason the plan's §1.1 gives —
it is inert under the G1 mapping because `loss_coeff_w_per_c` is pinned to 1.0.
Using both would be two parameterisations of one effect.

### 1.4 Loss unevenness IS an axis (coordinator's axis 2, upheld)

Define the split at fixed total: `phi = G_ea/(G_ea + G_la)`.

Steady state gives `G_el*(E-L) = G_la*(L - Tamb)`, i.e. the element-to-load
gradient is

```
(E - L) = (G_la/G_el) * theta_L
```

so `E - L` is proportional to `G_la = (1-phi)*G_a_total`. **`phi` is the direct
control on the element-load gradient**:

- `phi -> 0` (all loss leaks at the load): every watt must cross `G_el` to be
  lost, so the gradient is at its maximum — the element runs far above the ware.
- `phi -> 1` (all loss leaks at the element chamber): almost no power crosses
  `G_el`, so `E ~= L` and the kiln is near-isothermal.

That is not reachable by any of the other axes: `tau_L` moves the bulk's speed
without touching the steady gradient; `p` moves what the sensor *weights*
without touching what it weights *between*; loss magnitude scales both `G_ea`
and `G_la` together and leaves `phi` fixed by construction. **Independent.**

Two further points the coordinator flagged as worth testing rather than
assuming, both of which hold:

1. **`phi` interacts with `p` multiplicatively, not additively.** The
   sensor's steady-state error against the ware is
   `S - L = p*(E - L) = p*(1-phi)*(G_a_total/G_el)*theta_L`. It is a *product*.
   At `p = 0` the sensor sees the load and `phi` has no direct sensing effect at
   all (it still changes the *load's own* trajectory, which is why `phi` is a
   main effect too, not only an interaction). At `phi = 1` the nodes are
   isothermal and `p` has no effect. **Neither factor can be assessed without
   the other**, which is exactly why this is a factorial and not a one-factor-
   at-a-time sweep. This interaction is estimated cleanly in the design of §4.
2. **It is "the same class of mismatch as sensor placement reached by a
   different route", as the coordinator put it — but the two are not
   substitutes.** `p` changes the sensor's *dynamics* (a fast lead mode
   appears); `phi` changes the steady *offset* between sensor and ware. A
   controller can be lied to about rate or about level and those are different
   lies.

Constraint respected: all of this is **within** the single-zone three-node
structure (element vs load vs ambient). Nothing here is a cross-zone asymmetry.
The multi-zone coupling model is refuted and carries a do-not-retry gate
(plan §0.3); no cell in this design touches `sim_kiln_step()`'s coupling term,
and `n_zones = 1` everywhere.

### 1.5 Sensor placement is independent only in a regime

`p` is *not* fully independent. Two regimes:

- **Never-saturating, near-isothermal loop.** If `E ~= L` (high `phi`, or low
  `Bi` per §1.7) the sensor sees one node no matter what `p` is, and `p` does
  nothing. If the nodes *do* differ but the duty never hits a bound, the
  sensor's response to duty is well approximated by a shorter effective dead
  time and a smaller effective `tau` — i.e. `p` **aliases onto
  `(mismatch_L, mismatch_tau)`**, which the tune axis already spans. Running `p`
  as a separate factor here buys duplicate cells.
- **Saturating loop.** When the duty clamps at 0 (a dwell entry, a cut-out) the
  element's heat source disappears and `S` falls fast while `L` barely moves.
  That asymmetry is produced by the *clamp*, which is a nonlinearity no linear
  mismatch factor can reproduce. **Here `p` is genuinely independent**, and it
  is the case the plan's §2.1 and the Watlow source are actually about.

**So `p` is real, but its effect is predicted to live largely in a `p x
headroom` interaction rather than in the `p` main effect.** That is a
falsifiable claim about this design, stated in §6, and if the factorial finds a
large `p` main effect at ample headroom, this section is wrong.

### 1.6 Tune mismatch has 2 degrees of freedom, not 3

The plan's §2.4 offers three factors `(m_k, m_tau, m_L)`. Through SIMC they
reach the loop as

```
Kc'/Kc = m_tau / (m_k * m_L)
Ti'    = min(m_tau*tau, 16*m_L*L)
Td'/Td = m_L
```

so in the ordinary case `tau < 16L'` the triple collapses to two observable
directions: **loop-gain error** and **integral-time error** (`Td` follows `m_L`
and is nearly inert here, `Td = L/2` with SIMC). Treat the tune axis as
**categorical with 2 informative contrasts**, not as three ordinal knobs.

**Two arithmetic notes for the implementer**, offered because WI-3's acceptance
requires the ratio be computed from the formula rather than pasted:

- `TUNE_HOT` = (0.5, 2.0, 0.5) gives `Kc'/Kc = 2.0/(0.5*0.5) = `**8x**, not the
  "~4x" the plan's §2.4 table states. One factor appears to have been dropped in
  the prose. WI-3's computed assertion will surface this; the table's wording
  should be corrected, not the test.
- On bench z0 (`tau = 255.6`, `L = 40.3`, so `16L = 644.8`), `TUNE_MATCHED` has
  `Ti = min(255.6, 644.8) = 255.6`, while `TUNE_HOT` has
  `Ti = min(511.2, 322.4) = 322.4` — the `min()` switches branch, so `Ti` rises
  only **1.26x**, not 2x. `TUNE_HOT` is therefore `Kc x8, Ti x1.26, Td x0.5`:
  far more aggressive than the table's prose implies. Worth stating in the
  report, because "8x proportional gain" is where a fuzzy back-off layer has the
  most room to help — and it makes S7 a more, not less, informative cell.

### 1.7 The hidden axis nobody listed: `Bi`

Everything above turns on one dimensionless group:

```
Bi = (G_ea + G_la) / G_el          (external loss resistance vs internal transport)
```

- `Bi -> 0`: the nodes are isothermal, the three-node model **collapses to the
  plan's one-node legacy model**, and `p`, `phi` and loss magnitude *all* become
  aliases of `(K, tau, L)` simultaneously. Every interesting question in this
  design becomes unanswerable, and answering "no separation" in that regime
  would be an artifact of the plant configuration, not a result.
- `Bi = O(1)`: the element runs meaningfully above the ware, `E - L` is large,
  and `p` and `phi` both bite.

`Bi` is not a nuisance parameter to be fixed and forgotten — it is **the
condition under which three of the other seven axes exist**. It is therefore
promoted to a factor at 2 levels, so the aliasing claims in §1.2, §1.4 and §1.5
are themselves tested by the suite rather than merely asserted by this
document. This is the single most valuable addition this amendment makes, and
it costs 128 extra cells (~25 s).

### 1.8 Summary of step 1

| # | Factor | Symbol | Independent? | Notes |
|---|---|---|---|---|
| A1 | Bulk time constant / load | `tau_L` via `load_mass_mult` | **Yes** | Absorbs "bigger/smaller kiln" jointly with A2 |
| A2 | Power headroom | `Pmax` mult | **Yes** | The saturation nonlinearity; interacts with A1, A5 |
| A3 | Sensor placement | `p` | **Regime-conditional** | Aliases onto `(L, tau)` mismatch unless saturating / `Bi` high |
| A4 | Loss-scale span | `R_s = s(Tend)/s(Tstart)` | **Yes** | Absorbs "higher/lower temperature" AND "higher/lower loss" |
| A5 | Ramp rate | `r` | **Yes** (excitation) | Couples to A2 through required duty |
| A6 | Tune mismatch | categorical | **Yes**, 2 dof | Not 3; see §1.6 |
| A7 | Loss split (unevenness) | `phi` | **Yes** | Interacts multiplicatively with A3 |
| A8 | Internal/external resistance | `Bi` | **Yes** | Regime switch; decides whether A3/A7 exist |
| -- | Kiln size | -- | **NO — exact invariance** | Dropped, §1.1 |
| -- | Loss magnitude | -- | **NO — alias of A4's start** | Dropped, §1.2 |
| -- | Temperature-dependent loss | -- | **NO — is A4** | Dropped, §1.3 |

---

## 2. Step 2: element power headroom, kept explicitly

It survives §1 and it is kept, for the reason the task gives: **a duty-limited
zone cannot be rescued by any gain.** That is a qualitative change in what the
arms can possibly do, not a quantitative one, and it is the mechanism most
likely to explain why effects vanish at low ramp rates — at 40 deg C/hr nothing
saturates, so every arm has full authority and they converge, which is exactly
what the plan's §0.3 observed empirically without naming the cause.

It also interacts with the scoring in a known way the plan already records:
`LAG_S` drops saturated-and-short ticks. So in a saturating cell the *scored*
lag is computed over a subset of the ramp, and a cell can look better on `LAG_S`
precisely because more of it was excluded. **The aggregator must emit the
fraction of ramp ticks dropped by `LAG_S` per row** (a new column,
`lag_ticks_dropped_frac`), and any cell above 0.25 carries
`SATURATION_TRUNCATED` and is excluded from `LAG_S` effect estimation. Without
that column the headroom main effect on objective 1 is uninterpretable.

**Levels.** Defined by the *required feedforward duty at the top of the ramp*,
`u_req ~= (C_total * rdot + G_a_total * theta) / Pmax`, computed by the runner
from the cell's own constants — never hardcoded:

- **AMPLE**: `Pmax` set so the reference cell (A1=1, A5=150) peaks at `u_req ~= 0.23`.
- **TIGHT**: half that `Pmax`, reference cell peaks at `u_req ~= 0.45`.

Under TIGHT, the mass term (~0.20 of that 0.45) triples with A1 and doubles with
A5, so the corner `A1=heavy & A5=fast` needs `u_req ~= 1.45` — infeasible —
while `A1=heavy & A5=150` needs ~0.85 and `A1=1 & A5=fast` needs ~0.65, both
feasible and both hard against the bound during transients. **The TIGHT level is
chosen deliberately so that exactly one 3-factor corner becomes infeasible**
(§4), which is what keeps the masked design balanced for every main effect and
every 2-factor interaction.

---

## 3. Step 3: levels per axis, and why two or three

| Factor | Levels | 2 or 3? | Justification |
|---|---|---|---|
| **A1** `load_mass_mult` | `0.5`, `1.0`(anchor), `3.0` | **2 in stage 1** (`0.5`/`3.0`), 3 with the anchor centre | Monotone: more mass is slower, with no interior optimum any mechanism predicts. The centre exists only so the plan's named scenarios sit on-grid. |
| **A2** headroom | TIGHT, AMPLE | **2** | Monotone in authority; the *interesting* behaviour is the boundary crossing, and crossing is produced by the A1/A5 interaction at TIGHT rather than by a third level. |
| **A3** `sensor_bias_p` | `0.0`, `0.5`, `0.8333` | **3 (stage 2 curvature block)** | **A reversal is predicted by the source itself.** Watlow's page (plan §3.1) says to "place the sensor half-way between the heat source and work load to divide the heat transfer lag times equally" — an explicit claim of an *interior optimum*. Two levels cannot see it. `0.8333` is the plan's 5:1-conductance reading of "5x closer" and carries its interpretation caveat unchanged. |
| **A4** `R_s` | `1.02` (bench 24->60), `20.7` (kiln 200->1250) | **2** | Monotone in "how far the plant drifts under the controller". Also absorbs static loss magnitude over a 24x range (§1.2). A third level (600->900, `R_s = 2.64`) is *not* run: it is interior to a monotone axis and the kiln-scale plant configuration is the expensive part, not the span. |
| **A5** ramp | `150`, `300` deg C/hr | **2** | Both >= 100, per the plan's hard constraint that below that every arm converges. `40 deg C/hr` is **not** a factorial level; it is the S0 null control, run outside the design exactly as the plan specifies. |
| **A6** tune | MATCHED, HOT (stage 1); + COLD, SLOW_INTEGRAL (stage 2 block) | **2 + 2** | Categorical, so "levels" is a misnomer. MATCHED vs HOT is the screening contrast because HOT (`Kc x8`, §1.6) is the direction a bounded back-off layer could plausibly rescue. COLD and SLOW_INTEGRAL are *diagnostic*, not screening: SLOW_INTEGRAL's whole purpose is to be the cell where fuzzy is predicted **inert**, which is a prediction about one cell, not an effect to be marginalised. |
| **A7** `phi` | `0.25`, `0.5`(anchor), `0.75` | **2 in stage 1**, 3 with the anchor centre | Monotone in the gradient it sets (§1.4, gradient linear in `1-phi`). The centre is the anchor value so the plan's named scenarios sit on-grid, and it lets the `p x phi` product be checked for curvature in stage 2. |
| **A8** `Bi` | `0.3` (near-isothermal), `1.5` (gradient) | **2** | A regime switch, not a continuum: the question is whether the aliasing of §1.2/§1.4/§1.5 actually appears at low `Bi`. Two levels answer it; a third does not. |

Anchoring to something real, as required:

- The bench plant is the measured z0 fit: `k = 42.731`, `tau = 255.6`,
  `dead = 40.3`. Every bench cell's base constants are that fit, decomposed into
  three nodes at the cell's own `Bi` and `phi` so the *aggregate* `(K, tau)`
  reproduce it. Constructing the decomposition to preserve the measured
  aggregate is a WI-1 acceptance addition (§9).
- The kiln-scale plant is the `plant_sim.py` anchor WI-2 already ports, at
  `f_rad = 0.05`, giving the `s` table of §1.3.
- The ramp levels are anchored to the documented fact that arm separation only
  appears at `>= 100 deg C/hr`.

**Every one of these numbers is a TEST FIXTURE.** Bench values never ship
(plan §0.3). They live under `firmware/KilnFW/App/test/`, never in
`zones_config`, never in a preset, never in a firmware default, and the file
holding them says so in its header.

---

## 4. Step 4: pruning the unphysical, and step 5: the design

### 4.1 What a naive factorial would generate, and why most of it is already gone

The task warns about "a huge kiln with tiny thermal mass", "a sensor at the load
in a kiln with no load", "kiln-scale temperature with bench-scale power". **The
axis work of §1 removes all three by construction, before any mask runs:**

- Absolute size is not a coordinate (§1.1), so "huge kiln with tiny mass" cannot
  be written down. Only `tau_L` exists.
- `A1`'s low level is `0.5x`, not `0`. `C_l` is refractory **plus** ware and
  never approaches zero, so "a kiln with no load" is outside the axis range.
  A load-facing sensor at `0.5x` is entirely physical: an empty kiln still has a
  chamber and a wall.
- **A1, A2, A7 and A8 are defined as dimensionless multipliers on the cell's own
  base plant**, so a kiln-scale cell gets kiln-scale power automatically.
  "Kiln temperature with bench power" is unrepresentable. This is the single
  most important design decision in this section and it is worth stating as a
  rule: *never put an absolute physical constant on a factorial axis; put a
  ratio.*

### 4.2 The one real constraint: feasibility

What remains is not unphysical but **uninformative**: a cell whose profile the
plant cannot follow at all. Every arm pins at 100 % duty, every trajectory is
identical, and the scoring is corrupted by `LAG_S`'s saturated-tick exclusion.

**Constraint:** prune a cell from the arm comparison iff the runner's
feedforward check gives `u_req > 0.98` at any point of the profile. By §2's
level choice this is exactly the corner

```
A1 = heavy  AND  A2 = TIGHT  AND  A5 = fast
```

which is `2^5 = 32` of the 256 stage-1 cells (the other five factors free).

**They are not deleted.** "No gain rescues a duty-limited zone" is a *finding*,
and asserting it costs almost nothing: each masked cell runs **2 arms**
(`A_PID`, `A_FUZZY50`) with a structural assertion that they agree within
`BELOW_MATERIALITY` on all four objectives, and the row carries
`SATURATION_LIMITED`. A masked cell where the arms *do* differ is a loud failure
requiring investigation — either the feasibility check is wrong or something
downstream of the duty clamp is not what we think.

**Surviving cells: 224 of 256.**

### 4.3 The design

**Stage 1 — full 2^8 factorial, 256 cells, mask 32, run 224.**

Because it is a *full* two-level factorial, it is **resolution VIII: every main
effect and every interaction of every order is estimated with zero aliasing.**
There is no confounding structure to declare, and therefore no risk of the
"which interaction did we lose" failure. This is only possible because the
firings are free; it is the direct payoff of the observation that motivated this
document.

The mask costs balance in exactly one place: the `A1 x A2 x A5` three-factor
interaction and anything containing it. All 8 main effects and all 28
two-factor interactions remain **fully balanced** — this is why §2 chose the
TIGHT level to make precisely one corner infeasible rather than a ragged set.
State that limitation once: *the `A1 x A2 x A5` three-way and higher terms
containing it are estimated on a 224/256 unbalanced subset and are reported as
indicative only.*

**Stage 2 — 39 cells, everything else at reference:**

| Block | Cells | Purpose |
|---|---|---|
| Curvature / interior-optimum block: `A3 (3) x A7 (3) x A2 (2)` at reference A1, A4, A5, A6, A8 | 18 | Tests Watlow's half-way-optimum claim on `p`, and the predicted `p x phi` product structure with a centre point. |
| Tune block: `A6 (4) x A3 (3)` at reference elsewhere | 12 | Brings COLD and SLOW_INTEGRAL in, crossed with placement only. |
| High-`Bi` confirmation: `A8 = 1.5` replicates of the curvature block's nine `A3 x A7` cells | 9 | Confirms curvature findings are not a low-`Bi` artifact. |

**Total: 224 + 39 = 263 cells** (plus 32 masked cells at 2 arms, plus the S0 null).

**Firing count and runtime**, using the plan's §4.1 arm structure (4
single-firing arms + 2 nine-firing adaptive arms = 22 firings, plus 1
instrumentation pass for `A_STATIC_MATCHED`):

```
263 cells x 23             =  6,049
 32 masked cells x 2 arms  =     64
  1 null control (S0) x 23 =     23
                             -------
                              6,136 simulated firings
```

At the plan's "well under 20 ms per firing", **under ~2 minutes of compute**,
single-process; sharded four ways per the plan's §4.3, well under a minute. The
kiln-scale cells are ~2.5x the steps of bench cells, so call the honest budget
**under 5 minutes wall clock including the MSVC build**. That exceeds the plan's
current 60 s `check_sim_scenarios.ps1` budget, which is the correct place to
resolve it: **the full factorial is a separate, manually-invoked target
(`--suite factorial`), and the CI check keeps running the 13 named anchors at
the existing 60 s budget.** Do not raise the check's budget.

### 4.4 Why not fractional, and why not a screening design

Both were considered and both are *worse* here:

- A `2^(8-3)_IV` fraction would run 32 cells instead of 224, saving ~20 seconds
  and buying a 2FI aliasing structure in which `A3 x A2` and `A3 x A7` — the two
  interactions this entire document says are the point — would be confounded
  with other 2FIs. **Paying an interpretability cost to save 20 seconds of
  compute is the wrong trade**, and it is the trade the plan's hand-picked
  13 cases implicitly made.
- A screening design (Plackett-Burman, main effects only) would answer "which
  factors matter" and nothing about `p x phi` or `p x headroom`, which are the
  claims §1 actually makes.

The failure mode the task names — 1,458 rows nobody can read — is **not**
avoided by running fewer rows. It is avoided by fixing the *analysis* in advance
so the 224 rows collapse to 8 main effects and 28 interactions per objective
(§5). That is the real defence, and §5 is where it lives.

---

## 5. Step 5 continued: aggregation and analysis, specified before the run

### 5.1 The response is a difference between arms, never a raw score

For each cell and each of the four objectives (plan §5.1), compute:

```
D_fuzzy     = obj(A_FUZZY50)       - obj(A_PID)             "does fuzzy do anything"
D_inference = obj(A_FUZZY50)       - obj(A_STATIC_MATCHED)  "is it the inference or the gain"
D_adapt     = obj(A_PID_AT, run9)  - obj(A_PID, run1)       "does adaptation fix it alone"
D_combo     = obj(A_FUZZY_AT,run9) - obj(A_PID_AT, run9)    "does fuzzy add on top of adaptation"
```

`D_inference` is the one the project has got wrong before (`1570a65a`), and it
is the reason `A_STATIC_MATCHED` must use the **measured ramp-phase mean
multipliers, re-measured per cell** — on the bench plant those measured
`x0.8684 Kp / x1.1618 Ki / x0.8382 Kd`, roughly half the rule table's
centre-cell maximum. **Never the centre-cell `x0.75 / x1.25 / x0.75`.** With 263
cells this must be enforced mechanically, not by review: the instrumentation
pass is a precondition of the `A_STATIC_MATCHED` run and the runner refuses the
cell if it is missing.

### 5.2 There is no significance test, and that is correct

The plant is deterministic and there is no replication, so the residual degrees
of freedom for any ANOVA are zero and an F-test is undefined. **Do not compute
one.** Every difference this suite reports is *exact*; the question is never
"is it real" but "is it large enough to matter".

**Materiality replaces significance.** An estimated effect is REAL iff its
magnitude exceeds **0.5 deg C on that objective**, per the plan's §5.1 table and
the standing rule not to chase sub-0.5 deg C effects. For objective 1 the effect
is converted from seconds at the segment's ramp rate and both are reported.
Objective 2 (`SETTLE_S`) has **no owner materiality figure for a time axis** and
is reported without ever producing a verdict, at both the 2.0 and 0.5 deg C
bands, flagging `SETTLE_BAND_SENSITIVE` on disagreement.

### 5.3 How main effects are separated from interactions

Standard two-level factorial contrasts, on the balanced 224 (or the full 256
for terms not containing `A1 x A2 x A5`):

- **Main effect of factor X** = mean(D over cells with X high) - mean(D over
  cells with X low). 8 numbers per response per objective.
- **Two-factor interaction X:Y** = half the difference of X's main effect
  computed within Y-high and within Y-low. 28 numbers.
- **Third-order and above**: computed, but reported only as a single summary —
  the **max absolute 3FI-and-higher term**. This is the interpretability guard:
  if that maximum is **below 0.5 deg C**, the factorial *is* summarisable by its
  36 first- and second-order numbers, and the 224-row table never needs to be
  read. **If it exceeds 0.5 deg C, the model is not additive at second order and
  the report must say so and fall back to naming the specific cells** rather
  than quoting effects. Deciding this in advance is what stops a large table
  from being quietly averaged into a false summary.

### 5.4 What the report is

Per objective, one page:

1. A **Pareto-ordered bar list** of the 36 effects, each labelled REAL or
   `BELOW_MATERIALITY`, with the sign convention stated (negative = fuzzy better).
2. The max-higher-order-term line and its verdict from §5.3.
3. The `INFERENCE` / `GAIN_ONLY` classification **count** across cells — the
   single most decision-relevant number in the whole suite.
4. The named-anchor table (§7): the 13 plan scenarios by name, so results stay
   comparable with the plan as written.
5. The `SATURATION_LIMITED` and `SATURATION_TRUNCATED` cell counts.

Everything else stays in the TSV, diffable, in the plan's §5.4 format plus the
new columns `cell_id, a1..a8, u_req_peak, lag_ticks_dropped_frac, mask_reason`.

**No composite score, no overall winner** — the plan's §5.1 rule is unchanged
and a factorial makes it more important, not less: with 263 cells there will
always be *some* cell where an arm wins on *some* objective.

### 5.5 `KI_WITHHELD` labelling

Fuzzy and `adaptive_tune`'s relative-Ki path are mutually exclusive today
(`e78fbc5b`). **Every `A_FUZZY_AT` row in every one of the 263 cells carries
`ki_state = KI_WITHHELD`**, the aggregator refuses to attribute any `D_combo`
difference to Ki adaptation, and the plan's §1.4 explanation is printed once per
report. `D_combo` in this suite measures the **model/SIMC path plus band
re-derivation only**. When WI-9 lands, the whole factorial is re-run — at two
minutes, that is free, which is itself an argument for building it this way.

---

## 6. Step 6: advance predictions, stated before the suite runs

Stated now so the result is informative rather than fitted. This project has
repeatedly found that an unpredicted result was an artifact
(`project_zone2_dead_time_not_observable`, `project_fuzzy_bands_never_leave_center`),
so a wrong prediction here is a *successful* use of the method, not a failure.

### 6.1 Cells predicted to SEPARATE the arms

| Prediction | Cell region | Mechanism |
|---|---|---|
| **P1 (strongest)** | `A6 = HOT` x anything | `TUNE_HOT` is `Kc x8` (§1.6). Fuzzy's rule table backs Kp off on large sustained error, which is exactly the right lever against excess proportional gain. Expect `D_fuzzy` material on objective 4 (overshoot). |
| **P2** | `A3 = 0.8333` x `A2 = TIGHT` x `A5 = 300` | The placement effect needs the 0-duty clamp to express its asymmetry (§1.5). Largest expected `p`-attributable separation. |
| **P3** | `A7 = 0.25` x `A3 = 0.8333` | Maximum element-load gradient x maximum sensor weight on it = maximum sensor-vs-ware error (the product of §1.4). Expect the largest `[GROUND_TRUTH]` `sensor_minus_load_c` in the suite, and a real chance the *sensor* looks excellent while the *ware* is worst in the suite. |
| **P4** | `A4 = 20.7` (kiln span) x `A6 = MATCHED` | A matched tune at 200 deg C is a 20x mismatched tune at 1250 deg C by the time the run ends. A gain schedule (S11) should beat it; whether fuzzy's bounded +/-50 % nudge can cover any of a 20x drift is the question, and my expectation is **it cannot** — see P8. |
| **P5** | `A8 = 1.5` x `A3` x `A7` | The regime where §1's non-degeneracy claims hold. |

### 6.2 Cells predicted NOT to separate

| Prediction | Cell region | Mechanism |
|---|---|---|
| **P6** | `A6 = SLOW_INTEGRAL`, all cells | `docs/research/fuzzy_ramp_tracking_2026-09-13.md` establishes the table moves Kp strongly and Ki barely, and that `Kv = Ki*P(0)` is what sets ramp lag. Fuzzy should be **inert** here. **A separation would contradict that analysis and must be investigated, not celebrated** — the plan says this about S9 and it holds across the whole `A6 = SLOW_INTEGRAL` face. |
| **P7** | `A8 = 0.3` (low `Bi`), all `A3`/`A7` contrasts | This is the direct test of §1.2/§1.4/§1.5. At low `Bi` the nodes are isothermal, so the `A3` and `A7` main effects and the `A3 x A7` interaction should be **below materiality**. If they are not, §1's regime analysis is wrong and this document should be amended. |
| **P8 (the sharp one)** | `D_inference` below materiality in **>= 80 % of cells** | `pid_fuzzy.c` is a bounded +/-50 % nudge with a read-only 3x3 table, and its *measured* ramp-phase mean multipliers are only ~0.87/1.16/0.84. A static arm at those means reproduces most of what a trajectory-dependent schedule of them does, unless the trajectory drives the table into genuinely different cells at genuinely different times. `GAIN_ONLY` should dominate. **If `INFERENCE` dominates instead, that is the most interesting possible outcome of this suite** and would justify hardware work under the plan's §0.1 first row. |
| **P9** | `A2 = AMPLE` x `A6 = MATCHED` x `A4 = 1.02` | This is the plan's S1 / tonight's measured condition. It must reproduce the existing null result; if it separates, the harness is broken. |
| **P10** | S0 null control at 40 deg C/hr | All arms identical. Unchanged from the plan. |

### 6.3 A prediction about the design itself

**P11:** the `A3` main effect will be **smaller** than the `A3 x A2` interaction,
and the `A7` main effect **smaller** than the `A3 x A7` interaction. Both follow
directly from §1.4 and §1.5 — placement and unevenness act through products, not
sums. If either main effect dominates its interaction, the mechanism in §1 is
wrong even if the numbers are large, and the interpretation must be redone
before any conclusion is drawn from them.

---

## 7. Step 7: the 13 named scenarios survive as anchors

Every one of the plan's §4.2 scenarios is a **named coordinate** in this design,
not a replacement for it. They keep their ids, keep their pinned `SEP?`
expectations, and are the set the 60 s CI check runs (§4.3). The factorial is
the manually-invoked superset.

| Plan id | Factorial coordinate | In stage 1? |
|---|---|---|
| `S0 NULL_SLOW` | outside the design: `A5 = 40`, null control | no, kept separate |
| `S1 BASELINE` | A1=1, A2=AMPLE, A3=0.0, A4=1.02, A5=150, A6=MATCHED, A7=0.5, legacy node model | anchor; legacy-path cell |
| `S2 SENSOR_NEAR_ELEMENT` | as S1 but A3=0.8333 | stage 2 (A3 centre grid) |
| `S3 SENSOR_CENTRE` | as S1, three-node, A3=0.0 | stage 2; still the S1-vs-S3 confound check |
| `S4 SENSOR_NEAR_FAST_RAMP` | A3=0.8333, A5=300 | **stage 1** |
| `S5 MASS_HEAVY` | A1=3.0 | **stage 1** |
| `S6 MASS_LIGHT` | A1=0.5 | **stage 1** |
| `S7 TUNE_HOT` | A6=HOT | **stage 1** |
| `S8 TUNE_COLD` | A6=COLD | stage 2 tune block |
| `S9 TUNE_SLOW_INTEGRAL` | A6=SLOW_INTEGRAL | stage 2 tune block |
| `S10 KILN_HIGH_T` | A4=20.7, A3=0.8333 | **stage 1** |
| `S11 KILN_HIGH_T_SCHEDULED` | A4=20.7 + per-segment retune | **outside the factorial**: a re-tuning *policy*, i.e. a 7th arm, not a plant coordinate. Run as an extra arm on the `A4=20.7` face only (128 cells -> +128 firings). It is the upper bound P4 is measured against. |
| `S12 COMPOUND_WORST` | A1=3.0, A3=0.8333, A6=HOT, A5=300, A2=TIGHT | **masked** by §4.2's feasibility rule at A2=TIGHT; runs at A2=AMPLE in stage 1, and at TIGHT as a `SATURATION_LIMITED` 2-arm row |

Note the last line: **the plan's "compound worst case" is, at tight headroom,
not runnable at all** — the profile cannot be followed. That is itself a finding
the hand-picked list could not have surfaced, and it is a good illustration of
what the systematic design buys.

Anchor values `A1 = 1.0`, `A3 = 0.0/0.8333`, `A7 = 0.5` were chosen as grid
levels in §3 precisely so these rows land on-grid and stay numerically
comparable with the plan as written.

---

## 8. Step 8: the honest limits

**Every cell is our own model.** A factorial over a model we wrote cannot prove
anything about a real kiln, and 263 cells do not make it more true than 13 —
they make the *same* conditional statement over a wider region of *our own*
parameter space. The plan's §0.1 applies unchanged and is not weakened by scale.
Specifically:

- `f_rad = 0.05` is **[ASSUMED]**, unmeasured at any temperature in this
  dataset, and plausibly spans 5x to 80x. Every `A4 = 20.7` result carries the
  marker. A full half of the design rests on one uncalibrated number.
- The 5:1 conductance reading of "5x closer" is **ours**, unsupported by the
  Watlow source, which gives no quantitative model at all.
- `phi`, `Bi`, `C_e`, `C_s` and `sensor_tau_s` are **unmeasured on this
  hardware**. Their levels are chosen to bracket a plausible range, not
  identified from data. The factorial tells you the *shape* of the response
  surface over that bracket; it does not tell you where the real kiln sits on it.
- Nothing here is validated against a second implementation. The reference model
  that used to serve that role was deleted (`project_simfw_and_kilnsim_removed`),
  so this suite's only defence against a modelling error is that it links the
  real `pid.c`, `pid_fuzzy.c`, `firing_score.c` and `adaptive_tune*.c` and
  mirrors none of their math.

**What would justify hardware work** (extending the plan's §0.1 table, same
posture, decided before running):

| Factorial outcome | Action |
|---|---|
| A **contiguous region** of cells where `D_fuzzy` is material AND `D_inference` is material (`INFERENCE`, contradicting P8) | Justifies one real firing aimed at the centre of that region. A *region* is required, not a cell: an isolated cell in a deterministic 263-cell sweep is more likely a scoring-boundary artifact than an effect. |
| Fuzzy **degrades** an objective by > 0.5 deg C over a contiguous region | Act immediately: it bounds where fuzzy may be enabled. No hardware needed to act on this; it is a configuration decision. |
| `GAIN_ONLY` dominates (P8 confirmed) | **Closes the question.** The benefit is the gain magnitude, not the inference. Record it, schedule no hardware time, and move remaining effort to the adaptation work (plan §6). |
| No cell separates any arm materially | The negative verdict is now robust across 8 axes including the kiln-scale regime the bench cannot reach. Close as measured-negative-in-simulation. |
| P7 fails (`A3`/`A7` effects survive at low `Bi`) | **Closes nothing and invalidates part of this document.** Stop, re-derive §1, do not report factorial effects until it is understood. |
| Any effect that appears only at `A4 = 20.7` | **Never justifies hardware on its own**, because it is downstream of `f_rad`. It justifies *measuring* `f_rad`, which is a different and much cheaper experiment. |

Finally, the thing a factorial genuinely buys that 13 cases cannot: it can show
**where a controller configuration is fragile** — a region, with a boundary, and
the interaction terms that produce it. That is a statement about robustness,
which is what the owner's original objection was actually about, and it is worth
having even though every number in it is conditional on a model we wrote.

---

## 9. What the implementer needs to change, minimally

Folded against the plan's §7 work items; nothing here is new production code.

- **WI-1**: add `g_ea`/`g_la` as an explicit split `(G_a_total, phi)` and
  `Bi = G_a_total/G_el` as the construction parameters, so A7 and A8 are
  directly settable. **New acceptance:** a helper that, given a measured FOPDT
  `(k, tau, dead)` plus `(Bi, phi)`, produces three-node constants whose
  aggregate step response refits to that same `(k, tau)` within 1 % — otherwise
  A8 and A7 change the plant's gain as a side effect and every effect estimate
  is confounded with it. This is the one genuinely new piece of work this
  amendment requires.
- **WI-2**: unchanged. `s(1200) = 21.2581` is confirmed here against the
  published 21.3.
- **WI-3**: unchanged, and its computed-from-formula assertion will surface the
  `Kc = 8x` / `Ti = 1.26x` arithmetic of §1.6.
- **WI-4/6**: the scenario table gains an 8-tuple `cell_id` and is *generated*
  by nested loops over the level arrays rather than hand-listed — the plan's
  "a scenario is one `static const` row" rule is preserved for the named
  anchors, which stay hand-listed. The `_Static_assert` on the anchor table's
  length is unchanged.
- **New `--suite factorial`** target, separate from the CI check, which keeps
  running only the anchors at the existing 60 s budget (§4.3).
- **New TSV columns**: `cell_id, a1..a8, u_req_peak, lag_ticks_dropped_frac,
  mask_reason`.
- **Every constant introduced here is a TEST FIXTURE**; bench values never ship.

---

## 10. Reviewer checklist

1. Is "kiln size" absent from the axis list, and is §1.1's invariance argument
   correct as written?
2. Is loss magnitude folded into A4 rather than run as its own axis, and is the
   `Kc`-invariant / `Ti`-only argument in §1.2 right?
3. Is `Bi` a factor, so §1's aliasing claims are tested rather than asserted?
4. Are A1/A2/A7/A8 defined as **dimensionless multipliers**, so no cell can pair
   kiln temperature with bench power?
5. Does `A_STATIC_MATCHED` use the **re-measured per-cell ramp-phase mean**, not
   the centre-cell maximum? (263 cells; this must be mechanical.)
6. Is there **no significance test**, and does materiality carry the verdict?
7. Is the max-higher-order-interaction guard in §5.3 implemented, so the report
   cannot silently average a non-additive surface?
8. Does every `A_FUZZY_AT` row carry `KI_WITHHELD`?
9. Is `n_zones = 1` in every cell, with `sim_kiln_step()`'s coupling term
   untouched?
10. Does any conclusion claim the simulation shows fuzzy helps a real kiln? It
    must not.
