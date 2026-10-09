# Does the temperature-control design hold at cone temperatures?

Analysis, 2026-09-08. No control code changed; no hardware touched. This
document does not edit the simulator — section 9 is the specification another
agent's `sim_plant.c`/`sim_kiln` work should be tested against.

**Question as originally posed:** every gain, model and coupling coefficient in
this controller was fitted below ~62 °C on a rig whose ceiling is 80 °C. Does
that transfer to a kiln firing above 1000 °C?

**Question as reframed by the owner mid-analysis, and the one this document
answers:** the real kiln *will be tuned at its real operating points* — autotune
and `iter_tune` will run up there, and the numbers will be fitted where they
matter. So the transfer question is not "do the constants carry over" (they do
not, and they do not need to). It is:

1. Does the identification **method** still work when losses are radiative and
   plant gain per unit power has collapsed?
2. A single firing traverses every regime. Is one gain set adequate **across a
   firing**, even one fitted at the top?

Short answers: the method **mostly** holds, with one identification mode that
degrades sharply and a specific mitigation; and **yes, one part of the gain set
needs scheduling — the integral term, and only the integral term.** The
proportional gain is, to a good approximation, temperature-invariant *for
structural reasons this document derives*, which is a better result than
expected.

Every significant claim below carries a tag: **[SIM]** if the simulation can
settle it, **[KILN]** if it needs real high-temperature data and the simulation
can only test the *consequence* of an assumption, **[ANALYTIC]** if it follows
from algebra and needs neither. Section 9 turns them into experiments.

---

## 0. What is measured, what is assumed

Everything downstream depends on knowing which is which.

**Measured (bench, ≤80 °C):**

- Per-zone FOPDT fits, e.g. `k=31.96 °C/duty, tau=166.9 s, L=41.1 s`; the
  coupling matrix's own diagonal reads ~35–38 °C/duty
  (`docs/bench_snapshots/2026-09-04.md`,
  `docs/audits/firing_preflight_2026-09-07.md`).
- The coupling matrix, `[affected][stepped]`, adopted `78f2134`:

  ```
  [[38.13, 27.32, 21.72],
   [14.30, 35.90, 22.15],
   [ 8.33, 12.42, 35.32]]      cond 4.64
  ```

  Every cell has coverage of exactly **one** observation — no redundancy, no
  error bar.
- Live gains: z0 `0.0318/0.00010/0.8401`, z1 `0.0485/0.00020/1.0548`,
  z2 `0.0631/0.00020/1.0690`.
- `ff_hold_infeasible` is 0.0 % of ticks at ≤56 °C, 9.9 % at 64 °C, **100 % at
  72 °C on all three zones**.
- `iter_tune`'s noise floor, six repeat firings,
  `tools/PcTools/config_presets/noise_floor.json`: per-zone σ of normalized IAE
  0.053 / 0.032 / 0.054 °C; two-sample prediction intervals 0.192 / 0.116 /
  0.197 °C (`firmware/KilnFW/App/drivers/control/iter_tune.h`).
- Relay identification, once it worked: z0 at 45 °C, `Ku=0.19540, Tu=334.3 s`,
  relay half-amplitude **3.03 °C**.

**Assumed, and load-bearing:** `RAD_LOSS_FRACTION_AT_REF = 0.05` in
`tools/PcTools/src/kilnctrl/plant_sim.py` — the share of total loss that is
radiative at the 55 °C calibration point. Its own docstring says there is no
measurement of this split at any temperature in this dataset. **Every
quantitative statement below about how much k and tau move is a function of
this one unmeasured number, and it spans more than a decade.** I flag it at each
use rather than burying it. Where a conclusion depends strongly on its
magnitude, that conclusion is a **measurement request, not a finding**, and is
marked **[KILN]**.

---

## 1. Plant gain and time constant versus temperature

### The scaling law the repo already encodes

`plant_sim.loss_conductance_scale()` is the right structure and I use it rather
than inventing a second one:

```
s(T) = (1 - f_rad) + f_rad * ((T + 273.15) / 328.15)^4       f_rad = 0.05 (ASSUMED)
```

At steady state `K = P_max / C_loss` and `tau = C_thermal / C_loss` share the
same conductance denominator, so both scale as `1/s(T)`. That co-scaling is not
a modelling convenience — it is the whole reason section 4's result comes out
the way it does. **[ANALYTIC]**

| T | s(T), f_rad=0.05 | k (from 31.96) | tau (from 166.9 s, C fixed) | tau with C ×1.4 |
|---|---|---|---|---|
| 55 °C (ref) | 1.00 | 31.96 | 167 s | 167 s |
| 600 °C | 2.55 | 12.5 | 65 s | 92 s |
| 1000 °C | 9.35 | 3.4 | 18 s | 25 s |
| **1200 °C** | **21.3** | **1.50** | **7.8 s** | **11 s** |
| 1285 °C (cone 10) | 26.4 | 1.21 | 6.3 s | 8.8 s |

`C_thermal ×1.4` is the rise in specific heat of alumina/firebrick between
~300 K and ~1500 K; a literature figure, not measured here, and it affects only
tau.

**Sensitivity to the one assumed constant, at 1200 °C:**

| f_rad at 55 °C | s(1200 °C) | k | tau (C ×1.4) | L/tau |
|---|---|---|---|---|
| 0.01 | 5.05 | 6.3 | 46 s | 0.89 |
| 0.05 (repo default) | 21.3 | 1.50 | 11 s | 3.7 |
| 0.20 | 82.0 | 0.39 | 2.9 s | 14 |

So: **k falls by somewhere between 5× and 80×, central estimate ~20×, and tau
falls by very nearly the same factor.** The *direction* and the *co-scaling*
are solid physics **[ANALYTIC]**; the *magnitude* is unmeasured **[KILN]**.

Dead time `L` is transport and sensor lag — geometric, largely
temperature-independent, if anything slightly shorter as radiative transport is
effectively instantaneous. Treat it as ~41 s, unchanged, with the honest caveat
that a real kiln's `L` is its own number.

### The consequence that actually matters: the dimensionless regime changes

| | bench 60 °C | 1200 °C (f_rad=0.05) | 1200 °C (f_rad=0.01) |
|---|---|---|---|
| `L/tau` | 41.1/167 = **0.25** | 41.1/11 = **3.7** | 41.1/46 = **0.89** |

`L/tau` decides how hard a loop is to control, and it approaches or crosses 1 in
**every** case across the plausible range. The bench validates this controller
at `L/tau ≈ 0.25` — a comfortable, PID-friendly regime. A cone-firing kiln at
temperature is dead-time-dominant, a materially different tuning problem (the
regime where PID begins losing to explicit dead-time compensation).

**This is the one finding that survives the owner's reframe intact.** Fitting
the numbers on the real kiln fixes the numbers. It does not fix the fact that
the controller's *structure* has only ever been exercised at `L/tau ≈ 0.25`.
The bench cannot raise its own `L/tau` by heating — both are properties of the
rig — but the simulator can, and should. **[SIM]**

### Effect of a fixed PID tuned at 60 °C, taken to 1200 °C

Loop gain `Kp·k` falls ~20×. Direction matters: falling loop gain **detunes**;
it does not destabilise. Phase margin improves, the loop gets sluggish, and
integral action eventually removes the offset. So the naive fear — "bench gains
will make the kiln oscillate at cone" — is **wrong, and backwards**. The
dangerous direction is the other one, and section 4 is about that. **[SIM]**

---

## 2. The coupling matrix: rescale, or restructure?

**Verdict: restructure. Not a rescale.** Three independent mechanisms move it,
and they move different entries differently.

**a) The asymmetry's physical origin does not survive. [ANALYTIC + KILN]**
Zones are stacked vertically with z2 at the bottom
(`project_zone_physical_arrangement`). Below 100 °C the dominant inter-zone
transport is buoyant convection, which is *intrinsically directional* — hot air
rises. That is visible in the matrix as z1→z0 = 27.32 against z0→z1 = 14.30, a
1.91× upward bias, and in z2 (bottom) heating both zones above it. Radiative
exchange is **reciprocal**: `A_i F_ij = A_j F_ji`. As radiation takes over, the
mechanism that produced the asymmetry is displaced by one that cannot produce
it. The matrix should trend toward symmetry, and the 1.91× bias is a bench
artefact, not a kiln property.

**b) Off-diagonal grows faster than diagonal. [ANALYTIC]** The diagonal is loss
to ambient, which for a well-insulated chamber is substantially
conduction-through-wall — closer to linear in ΔT. The off-diagonal is
chamber-internal radiative exchange, growing as `4σεT³` — ×86 from 333 K to
1473 K. So the off-diagonal/diagonal ratio *rises*. In the limit, internal
radiation homogenises the chamber and the kiln approaches isothermal: zone
control authority drops, the matrix approaches singularity, and its condition
number — 4.64 on the bench — worsens. That is a well-known practical property of
high-fire kilns, and the opposite of the bench's regime where zones are
comparatively independent.

**c) The load participates. [KILN]** At cone temperature, ware and shelves
radiate between zones and form a substantial part of the exchange path. Shelf
placement changes the view factors. The bench matrix has no load term at all.
A real kiln's matrix is therefore *load-dependent*, and one fitted with a given
shelf arrangement is not valid for another. Nothing in the simulator or the
bench can settle this; it is a property of the physical kiln and its contents.

**What this means for the coupled feedforward solve:** the solve itself — a
linear system about an operating point — stays legitimate. A Jacobian is always
locally valid. What is not legitimate is treating **one** matrix as valid across
a firing. The right mental model is `G(T, load)`, re-identified at operating
points, with the solve unchanged. The code already anticipates half of this:
`coupling_coeff[]` deliberately has no compiled-in firmware default so a bench
number can never ship to a real kiln (`zones_config_json.h`). That decision was
correct and should be extended, not relaxed.

Worth noting for (b): worse conditioning makes the "one parameter, one zone, per
firing" rule in `docs/ITER_TUNE_REDESIGN_PLAN.md` **more** necessary at
temperature, not less. Simultaneous perturbation is unattributable when zones
are near-isothermal.

---

## 3. Why `ff_hold` goes infeasible, and whether it recurs

### The mechanism [ANALYTIC]

`profile_executor_feedforward.c` documents the term:

```
u_ff = (T_sp - T_ambient)/K_dc  +  (dT_sp/dt) * tau / K_dc
```

The hold term divides a **span from ambient** by `K_dc`. But `K_dc` is fitted as
a **local slope** — the incremental `dT/du` about a step's operating point.
Using a local slope as if it were a secant through ambient is valid only if
`ΔT(u)` is a straight line through the origin.

It is not. Loss is convex in T (it carries a `T⁴` term), so `ΔT(u)` is
**concave**, so the secant-from-origin exceeds the local slope. Dividing by the
smaller quantity over-predicts required duty. The demand exceeds 1 and the solve
declares itself infeasible.

The threshold falls out directly: uncoupled, the term reaches 1.0 when
`ΔT ≈ K_dc`, i.e. ~32–38 °C above ambient. The coupled solve borrows neighbour
duty and pushes that to ~62 °C — exactly where it was measured to break. The
hardware corroborates the concavity independently: at the 70 °C dwell z0 holds on
0.14–0.15 duty, an *effective* secant gain of 48/0.145 ≈ 330 °C/duty against a
fitted `K_dc` of ~32 (`project_ff_hold_infeasible_above_62c`).

### Artefact of an out-of-range matrix, or structural?

**Structural.** It is not the matrix being extrapolated; it is the hold term
having the wrong *functional form*. A linear gain cannot represent a plant whose
loss carries a fourth-power term, and re-fitting `K_dc` at any single operating
point yields a term correct at exactly that point and wrong on both sides —
over-predicting duty above, under-predicting below.

**And it worsens with temperature. [ANALYTIC, magnitude KILN]** At 1200 °C `ΔT`
is ~20× larger and `K_dc` ~20× smaller, so `ΔT/K_dc` is ~400× further outside
`[0,1]`. Re-tuning on the real kiln does *not* rescue this term. It is unusable
at cone temperature in its present form and would be infeasible on essentially
every tick of a real firing.

### The fix, and why it is small

Reformulate hold as a loss model rather than a linear gain:

```
u_hold  = Loss(T_sp) / P_max        Loss(T) = a*(T - T_amb) + b*(T^4 - T_amb^4)
u_climb = C_thermal(T) * (dT/dt) / P_max
```

Three properties this buys:

- **Feasible by construction.** `u_hold ∈ [0,1]` whenever `P_max` is real and
  the setpoint reachable — infeasibility then means "this kiln cannot hold this
  setpoint", which is *true information*, not a modelling failure.
- **Physically parameterised.** `a`, `b`, `C_thermal`, `P_max` are quantities a
  kiln owner can look up or that autotune can fit, and they are only *weakly*
  temperature-dependent, unlike `K_dc`.
- **The climb term barely changes.** `C_thermal` rises ~40 % across a firing and
  `P_max` is fixed, so climb is well-behaved and largely transfers as-is. Hold
  is the broken half; climb is not. That asymmetry is worth knowing before
  anyone rewrites both.

Keep `ff_hold_infeasible` regardless. A model that publishes its own validity
domain is the reason this was found at all, and it will be the reason the next
one is found.

---

## 4. Does one gain set work across a firing? (the central question)

Split the gains, because they behave completely differently.

### Kp is very nearly temperature-invariant, and here is why [ANALYTIC]

SIMC: `Kc = tau / (k * (tau_c + L))`. Substitute `k = P_max/C_loss` and
`tau = C_thermal/C_loss`:

```
Kc = C_thermal / (P_max * (tau_c + L))
```

**`C_loss` cancels exactly.** The conductance change — the entire `T⁴` effect,
the whole 5×–80× — drops out of the proportional gain. What remains depends only
on thermal mass, element power and dead time, none of which move much with
temperature. **This result is insensitive to `f_rad`**, which is precisely why it
is the most reliable conclusion in this document.

Numerically: bench (k=31.96, tau=167, L=41.1, tau_c=L) gives `Kc = 0.064`,
consistent with the live z1/z2 `kp` of 0.0485/0.0631. At 1200 °C (k=1.50,
tau=11, same L) it gives `Kc = 0.089`. **A 1.4× change across the entire
firing**, and most of that 1.4× is the assumed `C_thermal` rise, not the loss
model. That is inside ordinary PID robustness margins. **Kp does not need
scheduling.**

### Ki does, by roughly the full conductance factor [ANALYTIC form, KILN magnitude]

`tau_I = min(tau, 4*(tau_c + L))`, and `tau` is the branch that binds at
temperature:

| | tau | tau_I | ki = Kc/tau_I |
|---|---|---|---|
| bench 60 °C | 167 s | 167 s | 0.00038 (live: 0.0001–0.0002) |
| 1200 °C | 11 s | 11 s | 0.0081 |

**~20× — the same factor as the conductance, and the same 5×–80× uncertainty
band.** This is the term that does not survive a single fixed value across a
firing.

### The asymmetry, which is the thing to worry about [SIM]

The owner asked directly whether high-temperature-fitted gains could be
*actively unstable* low down. On this analysis:

- **Fixed gains from the bench, taken up:** Kp fine, Ki ~20× too slow. Symptom is
  sluggish offset recovery on dwells and a persistent ramp lag. Benign, visible,
  correctable by feedforward. Not a safety concern.
- **Fixed gains from 1200 °C, taken down through the climb:** Kp fine, **Ki ~20×
  too fast**, applied to a plant that is simultaneously ~20× *higher* gain per
  unit duty and ~15× *slower*. A fast integrator on a slow, high-gain plant with
  41 s of dead time is the textbook overshoot recipe. On the bench's own numbers
  an integral time of 11 s against a 167 s plant with L=41 s is roughly 4×
  shorter than the dead time — that loop is not merely sloppy, it is a genuine
  oscillation risk.

So the owner's instinct is right, and I would state it more strongly: **the
low-temperature portion of the firing is the hazardous one under top-fitted
gains, not the high-temperature portion.** This inverts the usual intuition that
the hot end is where things go wrong.

Two things partly contain it and neither is sufficient. `pid.c`'s integral floor
is `-ff_hold` — it bounds windup *downward* only, so it does nothing about an
over-fast integrator driving overshoot upward. And the ramp-lock band is 25 °C
(`project_ramp_lock_band_is_25c`), wide enough that a low-temperature overshoot
of this kind can develop well inside it.

### Recommendation

Not a survey — this is what I would do:

1. **Do not schedule Kp.** The derivation says it does not need it, and an
   unnecessary schedule is another thing that can be wrong.
2. **Schedule Ki, on measured zone temperature, with one curve:**
   `ki(T) = ki_ref * s(T) / s(T_ref)`, reusing `loss_conductance_scale()`. One
   free parameter (`f_rad`), fittable per kiln, defaulting to inert (`s ≡ 1`) so
   the change is a no-op until switched on.
3. **Schedule on measured temperature, not setpoint and not estimated loss.**
   Setpoint scheduling injects a gain step at every profile segment boundary —
   a discontinuity in the loop exactly where the trajectory is already changing.
   Estimated loss is a derived quantity with no independent observer on this
   board; `project_load_estimator_cannot_observe_load` records all three zones
   closing negative on precisely that attempt. Measured temperature is directly
   sensed, already filtered, and already the input to everything else.
4. **Fix the feedforward first (section 3), then re-assess whether the Ki
   schedule is still needed.** Right now a wrong hold term forces the integrator
   to do the feedforward's job, which inflates the apparent need for integral
   action and would confound any schedule fitted on top of it. Doing these in the
   wrong order means fitting a schedule to an artefact.

---

## 5. Does the identification METHOD work at high temperature?

This is the reframed question and deserves the most careful answer, because the
three identification layers behave differently.

### Step identification: degrades, and the fix is counter-intuitive [SIM]

Signal-to-noise falls with `k`. A 0.3-duty step producing ~9.6 °C of rise on the
bench produces `0.3 × 1.5 ≈ 0.45 °C` at 1200 °C (f_rad=0.05) — against 0.1 °C
sensor quantization and thermocouple drift. Central estimate: **SNR falls by the
same ~20×.**

Compounding it: the informative step amplitude is bounded by available headroom,
`1 - u_hold`, and `u_hold` is *high* at temperature. The bench never met this
because `u_hold` was small.

Two mitigations, both cheap:

- **Step downward at high temperature.** The available *negative* step is
  `u_hold`, which is large exactly where the positive one is small. A
  cooling-side step is just as informative for an FOPDT fit and the headroom runs
  the right way.
- **Identify on the way up, not at the top.** Around 800–1000 °C, `k` is
  ~3.4 °C/duty and headroom is still real. Fitting there and interpolating beats
  fitting at 1285 °C with no headroom.

Partially offsetting: `tau` is ~15× shorter, so a step settles in tens of seconds
rather than ten minutes. Runs are short, so **averaging many steps is
affordable** in a way it never was on the bench. That recovers a good part of the
lost SNR, and it is a method change the bench can validate at 80 °C.

### Relay identification: the timing problem is fixed; a signal problem replaces it [SIM]

The bench failure (`project_relay_ident_actuation_lag`) was that 1 Hz bang-bang
decisions were actuated through a 60 s PWM window comparable to the 34–53 s dead
time, jittering periods past the 20 % spread limit. `c84abff` forces the window
to restart on a branch flip, and the method then completed (`Ku=0.19540,
Tu=334.3 s`).

**That specific problem gets *better*, not worse.** It was a ratio of PWM window
to dead time; dead time is roughly temperature-independent and the fix removed
the quantization entirely. `Tu` is dead-time-dominated (≈334 s measured against
L≈41 s), so relay periods stay in the same range at temperature — run durations
and spread tolerances all still make sense.

**But a different problem appears.** The relay's oscillation half-amplitude is
`a ∝ k*d`. The bench's 3.03 °C at `d=0.35` becomes `3.03/20 ≈ 0.15 °C` at
1200 °C for the same `d` — at or below quantization, and below any sane
hysteresis band. Since `Ku = 4d/(π√(a² - h²))`, a shrinking `a` against a fixed
`h` drives that expression toward a singularity and then into nonsense. So:

- **`d` and `h` must scale with plant gain.** `d` scaled to hold `a` at a few °C,
  `h` held at a fixed fraction of `a`. Both are already parameters (`relay_d`,
  `relay_h_c`), so this is a scheduling change, not a redesign. The
  `relay_d = -1.0` auto-select default should become gain-aware.
- **`d` is capped by headroom**, the same bound as the step method and the same
  mitigation: identify below the top of the firing, prefer the downward side.

Note also the empty-window bug class from `17e67ee` (`max_temp_c - 50` versus
`min_temp_c + 50` jointly unsatisfiable): a cone kiln's 1300 °C span makes that
window enormous, so the span-proportional headroom fix is the right one and
should not be reverted to absolute margins.

### `iter_tune`: transfers essentially intact — the strongest result here

`iter_tune` needs no informative excitation. Its own header states the reason:
every identification fit in this codebase has proven fakeable by something that
is not plant dynamics, and this layer only asks "did the whole firing track
better than last time". That question is **temperature-agnostic by
construction** **[ANALYTIC]**, and it is the layer least disturbed by everything
in sections 1–3.

What does change, and must be handled:

- **The noise floor must be re-measured on the real kiln. [KILN]** The bench
  figures (σ 0.053/0.032/0.054 °C; prediction intervals 0.192/0.116/0.197 °C) are
  properties of the *rig*. At cone temperatures absolute tracking errors are
  larger while thermocouple relative accuracy is comparable, so the floor moves
  and probably grows in absolute terms. The **procedure** for measuring it —
  N ≥ 5 repeat firings, gains held fixed, rested start, two-sample prediction
  interval `t(.975, n-1)·σ·√2` — transfers exactly, and is the valuable artefact.
- **The relative-vs-absolute mismatch worsens. [SIM]**
  `ITER_TUNE_MIN_RELATIVE_IMPROVEMENT` is relative (20 %) while the floor is
  absolute (°C). `project_relative_threshold_vs_absolute_floor` already records
  that 20 % sits below z2's absolute floor on the bench and *weakens as tuning
  improves*. Across a firing whose IAE magnitude varies enormously between low
  and high segments, one relative threshold is even less defensible.
  **Recommend: make the accept test explicitly the absolute prediction interval,
  with the relative fraction as a secondary sanity bound, not the primary gate.**
- **Metric normalisation across segments needs stating. [SIM]** If normalized IAE
  is dominated by the high-temperature segments simply because the numbers are
  bigger there, `iter_tune` optimises the top of the firing and ignores the
  bottom — precisely the region section 4 identifies as hazardous. Check it, and
  if true, weight the metric by segment.

**Verdict on the method overall:** it holds. Step identification needs
directional and staging changes; relay needs gain-scheduled amplitude;
`iter_tune` needs a re-measured floor and an absolute accept gate. None is a
redesign, and all three are specifiable now.

---

## 6. What the bench can and cannot establish

Given that the real kiln supplies the numbers, the bench's job is now sharply
defined.

**Transfers (the bench can prove it):**

- Algorithm correctness: the coupled solve, Gaussian elimination, the PID form,
  bumpless transfer, the PWM renderer, the relay-window fix.
- Safety containment: every guard, the interlocks, `heater_output`, the load cap,
  `relay_authority_zone_blocked`, the Pico's independent ceiling.
- Accept/reject logic: that `iter_tune` reverts a bad trial, never wanders,
  respects its bounds, and cannot ratchet on noise.
- The tuning *method* and its statistics: rested-baseline discipline, the
  prediction-interval arithmetic, one-parameter-one-zone-per-firing.
- Diagnostics: `ff_hold_infeasible`, stack-margin regime, flag/telemetry
  plumbing.
- Bug-class discipline: reset-one-side audits, consumer-without-producer sweeps,
  negative-testing every check. None of these is temperature-dependent.

**Does not transfer:**

- Every number: `k/tau/L`, all nine coupling cells, all nine PID gains, `ff`
  coefficients, ambient, feasible ranges, the noise floor,
  `relay_d`/`relay_h_c` defaults.
- The 1.91× coupling asymmetry — a convection artefact (section 2).
- The dimensionless regime: `L/tau ≈ 0.25` versus 1–4. **This one is not merely
  a number.** It means the controller structure has been validated in a regime
  the real kiln does not occupy, and no amount of on-kiln re-tuning fixes a
  *structural* validation gap. Only the simulator can close it.
- Anything involving the load: the bench has no ware, no shelves, no view
  factors.

**Therefore the bench validation programme should be:**

1. `iter_tune` end-to-end at 80 °C: does it accept a real improvement, revert a
   real regression, and reject a noise-sized difference? Three experiments, all
   affordable.
2. Prove the accept gate against the measured floor — including a deliberate null
   trial (perturb nothing, confirm no accept).
3. Prove gains cannot wander: bounds respected, revert path exercised, state
   survives a reboot.
4. Prove the metric's segment normalisation on a multi-segment profile.
5. Prove the identification methods' *plumbing* — relay `d`/`h` scheduling,
   downward steps, multi-step averaging — at 80 °C where the answers are known.
6. Prove safety containment holds while all of the above misbehaves on purpose.

Note what is absent: producing tuning constants. That is no longer the bench's
job, and effort currently spent chasing sub-degree bench improvements is better
spent on items 1–6.

---

## 7. What this means for the work in flight

### `iter_tune` redesign: yes, build it — with three amendments

It is the layer that transfers best and the mechanism by which the real kiln
will supply its own numbers. Build it. But build it to *run on the real kiln*:

- The accept gate should be the **absolute prediction interval**, with the
  relative fraction demoted to a sanity bound.
- Every firing record must carry its **operating band**, and comparisons must
  refuse across bands.
- The **noise floor must be a per-kiln, re-measurable artefact**, not a
  compile-time constant fitted on the rig.

### Simulation: it needs the temperature-dependent loss model to mean anything

`loss_conductance_scale()` already exists and `sim_wide_temp_sweep.c` already
drives ambient-to-cone setpoints through the real `pid.c` and
`zone_coupling_solve.c`. The hook is there; section 9 specifies what to do with
it.

A linear-loss simulation is *provably less faithful than the bench*, because it
cannot reproduce the `ff_hold` infeasibility real hardware already exhibits.
That is a usable falsification test for the sim, and it is E3a below.

### Cheap restructuring to do now

Ranked by value per unit effort. All are seams; none change behaviour.

1. **Record the operating point with every identification result.** Today
   `model_k_dc`/`model_tau_s`/`model_dead_time_s` are stored in `zone_cfg` with
   no record of the temperature or ambient they were fitted at. A fit without its
   operating point cannot be assembled into a schedule later — it is unusable
   data. Add `model_fit_temp_c`/`model_fit_ambient_c` at the next
   `ZONES_CFG_VERSION` bump. **Highest-value change in this document, and it
   costs one struct field.**
2. **Route every model read through `zone_model_at(zi, T)`**, returning today's
   constant. A no-op now; the seam where `s(T)` slots in without touching call
   sites. Same for `coupling_at(T)`.
3. **Store `ki_ref` plus its reference temperature**, and apply `s(T)` at the tick
   behind a default-off flag. Section 4's recommendation then becomes a config
   change rather than a code change.
4. **Log the operating point alongside `ff_hold_infeasible`.** It is the model's
   validity-domain alarm and currently reports a boolean without saying where.
5. **Keep `coupling_coeff[]` with no compiled-in default.** Already the case,
   already correct, worth restating so it is not "simplified" later.

Nothing here requires deciding the loss model's form now. That is the point.

---

## 8. The bottom line

**Do the tuning numbers transfer?** No — essentially none of them, and the
coupling matrix's most distinctive feature (its asymmetry) is a convection
artefact that should disappear at temperature. Under the owner's reframe this is
fine: the real kiln will supply its own numbers. **The algorithm holds; the
numbers must come from the real kiln.** That is the finding, and it is a good
outcome, not a discouraging one.

**Does the METHOD hold at high temperature?** Broadly yes, with specific,
addressable degradations. `iter_tune` transfers essentially intact and is the
strongest asset here. Relay identification's known failure mode (PWM-window
quantization) actually *improves*, but relay amplitude must be scheduled on plant
gain or the oscillation shrinks below the noise. Step identification loses ~20×
SNR and needs downward steps, staging below the top of the firing, and multi-step
averaging — all cheap, all bench-testable.

**Does the controller need gain scheduling across a firing?** **Yes — but only
the integral term, and this is a better answer than expected.** After
substituting the shared conductance, `Kc = C_thermal / (P_max·(tau_c + L))`: the
entire `T⁴` effect cancels out of the proportional gain, which moves ~1.4× across
a whole firing. The integral time follows `tau` and moves ~20×. So schedule `ki`
on measured temperature with one curve and one fitted parameter; leave `kp`
alone.

**The risk to watch is at the bottom of the firing, not the top.** Gains fitted
at 1200 °C put an ~11 s integral time onto a plant with a 41 s dead time during
the low-temperature climb — roughly 4× faster than the dead time. A genuine
overshoot and oscillation risk, and the existing `-ff_hold` integral floor does
not contain it because it bounds windup only downward.

**The feedforward is broken in a way re-tuning cannot fix.** `u_hold = ΔT/K_dc`
applies a locally-fitted slope as a secant through ambient; valid only for a
linear plant. The measured 100 %-infeasible-above-62 °C result is the first
visible signature of loss nonlinearity on this hardware. At cone temperature the
same expression is ~400× further out of range. Reformulating hold as
`Loss(T_sp)/P_max` makes it feasible by construction. The climb term is fine and
largely transfers.

**And the honest caveat on all of it:** every quantitative statement about *how
much* k and tau move rests on `RAD_LOSS_FRACTION_AT_REF = 0.05`, which the code
itself labels ASSUMED and which spans 5×–80× across plausible values. The
*structure* of the argument — the co-scaling, the `Kc` cancellation, the `ki`
dependence, the `L/tau` regime crossing — is robust to that uncertainty. The
specific multipliers are not. **Measuring that split on a real kiln is worth more
than any further bench tuning**, and it is the single measurement request this
document makes.

---

## 9. Simulation specification: propositions, experiments, and sensitivity

This section is the deliverable for the agent extending
`firmware/KilnFW/App/test/sim_plant.c` / `sim_kiln`. **I have not edited the
simulator.** Each proposition below is stated so it can be confirmed or refuted,
with the result that would count as confirmation and the sensitivity to the
unmeasured radiative coefficient.

### 9.0 Prerequisites the model must have

Without these, none of the experiments below mean anything.

- **P0-a. Temperature-dependent loss conductance.** `s(T)` per section 1,
  applied to **both** `K` and `tau` with the same factor (the co-scaling is the
  physics; scaling only one invents a second free constant). `f_rad` must be a
  **swept input**, not a compile-time constant.
- **P0-b. Sensor quantization and noise.** 0.1 °C quantization at minimum, plus a
  drift/noise term. Without it the simulator will cheerfully report that step
  identification works at 1200 °C, because the 0.45 °C rise is perfectly visible
  to a noiseless model. `project_idealized_test_input_bug_class` documents this
  exact failure already occurring in this repo — treat P0-b as mandatory, not
  optional realism.
- **P0-c. Duty saturation at `[0,1]` and the real PWM window.** The `ff_hold`
  infeasibility and the headroom bounds in section 5 are both saturation
  phenomena; a model that lets duty exceed 1 cannot exhibit either.
- **P0-d. `C_thermal(T)`** rising ~40 % across the range, or an explicit note that
  it is held constant (which biases `tau` low at temperature by ~1.4×).

### 9.1 Proposition P1 — k and tau fall together as `1/s(T)`

**Claim:** `k(T)·s(T)` and `tau(T)·s(T)` are both constant; `k/tau` is
independent of temperature.

- **Experiment E1.** Run open-loop steps at 60, 300, 600, 900, 1200 °C in the
  simulator and fit FOPDT at each. Plot `k·s(T)`, `tau·s(T)`, and `k/tau`.
- **Confirmation:** `k·s` and `tau·s` flat to within the fit noise, and `k/tau`
  flat to within `C_thermal(T)`'s own variation (~40 %).
- **Refutation:** either product trending — meaning the model is not applying the
  conductance to both, i.e. a P0-a implementation defect.
- **Sensitivity to `f_rad`:** **none.** This tests internal consistency of the
  model, not the value of `f_rad`. E1 is therefore a *model self-check* and
  should be run first; if it fails, every other result is void.
- **[SIM] — the simulation settles this completely.**

### 9.2 Proposition P2 — `L/tau` crosses 1, changing the control regime

**Claim:** at cone temperature the plant is dead-time-dominant, a regime the
bench has never exercised.

- **Experiment E2.** From E1's fits, tabulate `L/tau` against temperature, for
  `f_rad ∈ {0.01, 0.02, 0.05, 0.10, 0.20}`. Then, separately, run the *bench*
  plant with `tau` artificially reduced to force `L/tau ∈ {0.25, 1, 2, 4}` and
  measure closed-loop overshoot, settling and oscillation with SIMC gains fitted
  at each.
- **Confirmation:** `L/tau > 1` at 1200 °C for `f_rad ≥ 0.02`; and the
  `L/tau` sweep showing controller performance degrading materially somewhere in
  1–4 (which would establish where the structure stops being adequate).
- **Refutation:** `L/tau` staying below ~0.5 across the whole plausible `f_rad`
  range — which would only happen if `L` also falls with temperature, so E2
  should also report the sensitivity to `L(T)`.
- **Sensitivity to `f_rad`:** **moderate.** The crossing happens for `f_rad ≥
  0.02`; at `f_rad = 0.01` it only reaches 0.89. So *whether* the regime changes
  is fairly robust; *how far past 1* it goes is not. **The `L/tau` sweep itself
  is `f_rad`-independent and is the more valuable half of E2** — it establishes
  what the controller structure tolerates regardless of which regime the real
  kiln lands in.
- **[SIM] for the sweep; [KILN] for where the real kiln sits on it.**

### 9.3 Proposition P3 — `ff_hold` infeasibility is structural, not an out-of-range matrix

**Claim:** the infeasibility follows from a linear hold term applied to a
nonlinear-loss plant, and recurs at every operating point where
`ΔT > ~K_dc(T)`.

- **Experiment E3a (falsification test for the model itself).** Run the
  simulator with the bench matrix and bench `K_dc`, sweeping dwell targets
  30→80 °C. Measure the fraction of ticks with `ff_hold_infeasible`.
  **Confirmation: 0 % at ≤56 °C, small but non-zero near 64 °C, ~100 % at 72 °C
  — reproducing the measured hardware result.** If the simulator does not
  reproduce this, its loss model is wrong and nothing else it says about high
  temperature should be believed. This is the single most valuable calibration
  point available, because it is the one high-temperature-flavoured phenomenon
  the bench has actually measured.
- **Experiment E3b (structural claim).** Re-fit `K_dc` *at* each dwell
  temperature — i.e. give the model the best possible locally-fitted gain — and
  re-run. **Confirmation: infeasibility still appears once `ΔT` exceeds roughly
  the local `K_dc`, at every operating point tested.** That refutes "out-of-range
  matrix" and confirms "wrong functional form".
  **Refutation:** local re-fitting eliminating infeasibility everywhere, which
  would mean the problem *is* extrapolation and re-identification on the kiln
  would fix it. This is the experiment that decides between the two hypotheses,
  and it is decisive either way.
- **Experiment E3c.** Implement `u_hold = Loss(T_sp)/P_max` in the simulator only
  and confirm it never reports infeasible for a reachable setpoint, at any
  `f_rad`.
- **Sensitivity to `f_rad`:** **E3a is a calibration of `f_rad`, not a test that
  depends on it.** Report which `f_rad` best reproduces the measured 0/9.9/100 %
  profile — that is the closest thing to a *measurement* of the radiative share
  this project can obtain without a real kiln, and it should be stated as such
  (with the caveat that other model errors could absorb the same discrepancy).
  E3b and E3c are `f_rad`-insensitive.
- **[SIM], and E3a is additionally a partial measurement.**

### 9.4 Proposition P4 — Kp is temperature-invariant; Ki is not

**Claim:** SIMC `Kc` varies ~1.4× across a firing while `ki` varies by the full
conductance factor.

- **Experiment E4.** At each of the E1 operating points, compute SIMC gains from
  the locally-fitted model. Plot `Kc(T)` and `ki(T)` normalised to their 60 °C
  values, for the full `f_rad` sweep.
- **Confirmation:** `Kc` within ~2× across the whole range **and across every
  `f_rad`**; `ki` spanning roughly `s(1200)/s(60)` in each case.
- **Refutation:** `Kc` varying by more than ~3× — which would mean the
  conductance is *not* cancelling and either P0-a is wrong or `L` is strongly
  temperature-dependent.
- **Sensitivity to `f_rad`:** **`Kc`'s invariance is `f_rad`-independent — that
  is the whole point of the cancellation, and E4 should demonstrate it by showing
  the `Kc` curves collapsing on top of each other across the `f_rad` sweep while
  the `ki` curves fan out.** That single plot is the clearest statement of this
  document's central result.
- **[SIM] for the structure; [KILN] for `ki`'s actual magnitude.**

### 9.5 Proposition P5 — top-fitted gains overshoot at the bottom of the firing

**Claim:** the low-temperature portion is the hazardous one; the asymmetry runs
opposite to intuition.

- **Experiment E5.** Full climb from ambient to 1200 °C with a realistic
  multi-segment profile, three arms:
  1. gains fitted at 1200 °C, held fixed throughout;
  2. gains fitted at 60 °C, held fixed throughout;
  3. `ki` scheduled per section 4, `kp` fixed.
  Report per-segment peak overshoot, IAE, oscillation, and guard activations.
- **Confirmation:** arm 1 showing materially worse overshoot **below ~300 °C**
  than arms 2 and 3, and arm 2 showing sluggishness but no overshoot at the top.
  A quantified statement of arm 1's worst low-temperature overshoot is the number
  that decides how urgent the `ki` schedule is.
- **Refutation:** arm 1 well-behaved throughout — which would mean the dead time
  and the PWM window are damping the fast integrator enough that no schedule is
  needed, and the recommendation in section 4 should be dropped.
- **Also record:** whether `-ff_hold`'s integral floor engages in arm 1 (it
  should not help, per section 4) and whether the 25 °C ramp-lock band is ever
  approached.
- **Sensitivity to `f_rad`:** **high.** The whole effect is the ~20× `ki` ratio,
  which *is* the conductance ratio. Run arm 1 at `f_rad ∈ {0.01, 0.05, 0.20}` and
  report the overshoot as a function of the ratio, not of `f_rad` — that makes
  the result reusable once a real kiln measures the ratio. **The existence of the
  asymmetry is robust; its severity is a measurement request.**
- **[SIM] for direction and mechanism; [KILN] for severity.**

### 9.6 Proposition P6 — identification SNR collapses; downward steps and averaging recover it

- **Experiment E6.** With P0-b's quantization and noise active, run step
  identification at 60 / 600 / 1200 °C: (a) upward step within available headroom,
  (b) downward step of magnitude `u_hold`, (c) N repeated downward steps averaged.
  Report fit success rate and parameter spread.
- **Confirmation:** (a) failing or producing wide spreads at 1200 °C, (b)
  materially better, (c) recovering bench-comparable spread. That would justify
  the section 5 method changes concretely.
- **Refutation:** (a) succeeding at 1200 °C — check P0-b is actually active
  before believing it.
- **Experiment E6r.** Relay identification at the same three temperatures with
  fixed `d = 0.35`, then with `d` scaled to hold half-amplitude at ~3 °C. Report
  `Ku`/`Tu` recovery error and rejection reasons.
- **Confirmation:** fixed-`d` runs failing at 1200 °C on amplitude/spread limits
  (not timing — the timing fix should hold), and gain-scaled `d` recovering.
  Confirming that the *timing* problem does not return is itself valuable.
- **Sensitivity to `f_rad`:** high in magnitude, low in direction. Report SNR
  against the gain ratio rather than against `f_rad`.
- **[SIM].**

### 9.7 Proposition P7 — the coupling matrix restructures

- **Experiment E7.** This is the weakest one the simulator can do, and it should
  be labelled as such. The simulator's coupling growth
  (`coupling_growth_scale`, capped at 2×) is *assumed*, not measured, and it
  reuses `loss_conductance_scale` rather than modelling view factors. So E7 can
  test **consequences of an assumed structure**, not the structure itself:
  re-identify the matrix at 60 / 600 / 1200 °C in the simulator and report the
  asymmetry ratio, off-diagonal/diagonal ratio and condition number.
- **Confirmation of the *code path*, not the physics:** that the coupled solve
  remains numerically well-behaved as conditioning worsens, and that it degrades
  gracefully (falls back to the uncoupled diagonal) rather than producing
  nonsense as the matrix approaches singular. **That is genuinely worth testing
  and the simulator can settle it.**
- **What the simulator CANNOT settle:** whether the real asymmetry flattens,
  whether radiative reciprocity dominates, and anything about load participation.
  Those need real kiln data. **[KILN]**
- **Recommendation:** run E7 as a *solver robustness* test with a deliberately
  near-singular matrix, and do not present its coupling numbers as physics.

### 9.8 Proposition P8 — guard behaviour at the regime boundary

- **Experiment E8.** Across the full climb, log every guard's arming and firing.
  Particular attention to guard 1's expected-rate check (rates achievable at
  1200 °C are far lower for the same duty) and to the `PROGRESS_DUTY_MIN = 0.5`
  chop threshold, since `u_hold` at temperature sits *above* 0.5 for much of the
  firing — the opposite of the bench, where relay drive chopped constantly and
  guard 2 could essentially never accumulate its window.
- **Confirmation:** either no spurious trips across the whole climb, or a
  specific named guard whose thresholds are bench-scaled and need
  temperature-awareness. **Either outcome is a useful result**, and this is
  squarely bench/sim territory — guard logic is exactly the kind of thing the
  simulator is best at and the kiln is worst at.
- **Sensitivity to `f_rad`:** moderate; run at the sweep endpoints.
- **[SIM].**

### 9.9 What the simulation can settle, and what it cannot

| Claim | Simulation verdict |
|---|---|
| k and tau co-scale as `1/s(T)` | **Settles** (E1, self-consistency) |
| `L/tau` regime crossing degrades control | **Settles** the degradation curve (E2); real kiln's position on it is [KILN] |
| `ff_hold` infeasibility is structural | **Settles** (E3b) — decisive either way |
| The loss model reproduces measured 62 °C infeasibility | **Settles, and partially measures `f_rad`** (E3a) |
| `Kc` is temperature-invariant | **Settles** (E4), and `f_rad`-independently |
| `ki` needs a ~20× schedule | Settles the *form*; magnitude is [KILN] |
| Top-fitted gains overshoot at the bottom | **Settles** direction and mechanism (E5); severity is [KILN] |
| Step/relay SNR collapse and its mitigations | **Settles** (E6, E6r) — provided P0-b is real |
| Coupled solve stays robust as conditioning worsens | **Settles** (E7 as robustness test) |
| The coupling matrix's physical restructuring | **Cannot settle** — [KILN], needs a real kiln with a load |
| Guard behaviour across the regime boundary | **Settles** (E8) |
| The actual value of `f_rad` | **Cannot settle** except via E3a's indirect calibration — [KILN], and this is the measurement request |

**Priority order if time is limited:** E3a (calibrates the model and falsifies it
cheaply), E4 (the central result, and `f_rad`-independent), E5 (the safety
question), E8 (guards), then the rest.

---

**Sources.** `firmware/KilnFW/App/drivers/control/profile_executor_feedforward.c`,
`pid.c`, `iter_tune.h`, `zone_coupling_solve.h`;
`firmware/KilnFW/App/drivers/persist/zones_config_json.h`;
`firmware/KilnFW/App/test/sim_wide_temp_sweep.c`;
`tools/PcTools/src/kilnctrl/plant_sim.py`;
`tools/PcTools/config_presets/noise_floor.json`;
`docs/bench_snapshots/2026-09-04.md`;
`docs/audits/firing_preflight_2026-09-07.md`;
`docs/ITER_TUNE_REDESIGN_PLAN.md`.
Commits cited: `78f2134` (adopt the re-solved 3x3 coupling matrix), `c84abff`
(relay-step PWM window restart), `17e67ee` (relay setpoint headroom), `813ad90`
(a refused tune looked like a successful one) — all verified to resolve.
