# Scenario simulation plan: controller behaviour under plant/model mismatch

Date: 2026-09-14. Status: PLAN. No production code is written by this
document; it is the specification a sonnet agent implements and an opus agent
reviews the result against. No board was flashed and no heating run was
performed to produce it.

---

## 0. Read this before you read anything else

### 0.1 What this simulation can and cannot settle

**A simulator we wrote cannot prove the fuzzy layer helps a real kiln.** The
plant model, its parameters, the sensor model, and the mismatch factors are
all chosen by us. If the fuzzy layer wins in this simulation, the honest
statement is "fuzzy is robust to the mismatch *we modelled*, in the
*direction* we modelled it" — not "fuzzy helps". Every number this suite
produces is conditional on a model whose high-temperature half rests on one
unmeasured constant (`RAD_LOSS_FRACTION_AT_REF = 0.05`, whose own docstring
in `tools/PcTools/src/kilnctrl/plant_sim.py` says there is no measurement of
that split at any temperature in this dataset).

What it *can* do, and why it is worth building:

1. **Rule things out.** If the fuzzy layer fails to separate from plain PID
   even under a deliberately mismatched plant at a ramp rate where separation
   is known to be visible, then the "no demonstrable benefit" verdict is no
   longer an artifact of a favourable bench — it survived the condition most
   likely to break it. That is a real strengthening of a negative result.
2. **Find the failure modes.** A scenario where fuzzy makes things *worse*
   (a mismatch direction where raising Kp on sustained error is exactly the
   wrong response) is a finding that holds regardless of model fidelity, as
   long as the mechanism is traced through real production code — which it is,
   because these harnesses link `pid.c`, `pid_fuzzy.c`, `firing_score.c` and
   `adaptive_tune*.c` themselves and mirror none of their math.
3. **Exercise interactions the bench cannot reach.** The adaptive/fuzzy
   interaction (§1.4) is a cross-run, cross-module effect. A sequence of nine
   simulated firings costs seconds; nine real firings cost days.

**Which results would justify hardware work, and which would close the
question** (decide this before running, so the answer is not fitted to the
data):

| Outcome | Action |
|---|---|
| Fuzzy separates from plain PID on over/undershoot by > 0.5 °C in a mismatch scenario, AND the matched-effective-gain static arm does NOT reproduce it | Justifies a real firing to test the same mismatch direction. This is the only outcome that does. |
| Fuzzy separates but the matched static arm reproduces it inside materiality | The benefit is the gain magnitude, not the inference. Record it; do not schedule hardware time. This is exactly the confound `1570a65a` caught. |
| No scenario separates any arm by > 0.5 °C on any objective | The negative verdict stands and is now robust to mismatch. Close the "does fuzzy pay" question as measured-negative-in-simulation, record it, and move the remaining effort to the adaptation work (§1.4). |
| Fuzzy degrades an objective by > 0.5 °C in a scenario | A finding to act on immediately — it bounds where fuzzy may be enabled. |

### 0.2 Keeping the fuzzy layer is DECIDED

Per the owner, 2026-09-14: the fuzzy layer stays. **Deletion is not a live
option and no work item in this plan is allowed to treat it as one.** The
question this plan answers is *how the fuzzy layer, the self-improving PID
(`adaptive_tune`), and their combination behave* across scenarios the ~4 W
bench cannot produce. "Does fuzzy earn its place" is replaced by "where does
each configuration help, where does it hurt, and where is it inert".

### 0.3 Hard constraints inherited, restated as rules the implementer must obey

- **Single-zone only.** The multi-zone coupling model is refuted: superposition
  breaks by ~33 % at matched ΔT, the residual sign reverses with level, two
  replacement model classes failed, and there is a written do-not-retry gate
  (`docs/audits/coupling_joint_identification_capture_2026-09-10.md`,
  `docs/audits/coupling_level_schedule_adjudication_2026-09-11.md`,
  `project_coupling_failure_is_joint_dwell_specific`). Single-column transport
  is measured *linear*, so a single zone is trustworthy. **Do not add a
  multi-zone scenario. Do not touch `sim_kiln_step()`'s coupling term.**
- **Ramp rates ≥ 100 °C/hr only** for any scenario intended to separate arms.
  At 25–50 °C/hr every arm converges and the scenario carries no information.
  A scenario at a slower rate may exist only as an explicitly-labelled
  null/control scenario.
- **The plant is deterministic.** Repeating an identical run buys nothing.
  Spend the budget on *coverage* (more scenarios, more mismatch directions),
  never on repetition. There is no Monte-Carlo ensemble in this suite.
- **No synthetic disturbance injection.** `sim_fuzzy_overshoot.c`'s ±8 °C
  poke writes `element_c`/`sensor_c` directly, bypassing `sensor_delay_s`, and
  is non-physical. Stimulus is the setpoint schedule and the plant
  configuration, nothing else. Never cite that scenario's numbers here.
- **Bench values never ship** (owner, 2026-09-14). Every constant this plan
  introduces — placement ratio, load-mass multiplier, mismatch factors,
  kiln-scale plant constants — is a **test fixture**. It lives in test code
  under `firmware/KilnFW/App/test/`, it is never written into
  `zones_config`, never into a preset, never into firmware defaults, and any
  file that holds one must say so in its header comment.
- **Every scenario asserts something falsifiable.** A harness that prints a
  verdict and returns 0 guards nothing (`project_harness_prints_verdict_exits_zero`).
- **Negative tests end with a forced full REBUILD**, not a hand-restore plus an
  empty `git diff` (`8a12521b`). Restore broken production files **by hand** —
  never `git checkout --`, which wipes concurrent sessions' work.

### 0.4 Files this plan may not edit

Owned by concurrent sessions as of 2026-09-14:
`firmware/KilnFW/App/drivers/control/pid_fuzzy.c`/`.h`,
`profile_executor_pid_tick.c`, `firmware/KilnFW/App/test/sim_fuzzy_overshoot.c`,
`ROADMAP.md`, `docs/FUZZY_CONTROLLER_PLAN.md`, and the session-summary docs.
Work item 9 is the only item that touches production control code
(`adaptive_tune_ki.c`), and it is explicitly sequenced last for that reason.

---

## 1. Part A — what `sim_plant.c` cannot express today

Established by reading `firmware/KilnFW/App/test/sim_plant.c` and
`sim_plant.h` in full, current source, not history.

### 1.1 The present model, concretely

**State per zone** (`sim_plant_state_t`), three things:

- `element_c` — one lumped thermal node. This is the *entire* thermal model.
  There is no load, no ware, no chamber air, no sensor mass.
- `delay_ring[64]` + `delay_len`/`delay_head` — a pure transport delay line on
  the *reading*.
- `sensor_c` — a first-order lag applied after the delay.

**Update equations** (`sim_plant_step()`, forward Euler, `dt_s`):

```
P_in   = clamp(u,0,1) * heater_power_w
P_loss = loss_coeff_w_per_c * (element_c - ambient_c)
element_c += (P_in - P_loss) / thermal_mass_j_per_c * dt_s
delayed   = delay_ring[head - round(sensor_delay_s/dt_s)]     (ring push element_c)
sensor_c += (dt_s/(sensor_lag_tau_s + dt_s)) * (delayed - sensor_c)
```

`sim_kiln_step()` adds, per zone, an optional radiative loss
`radiative_coeff_w_per_k4 * ((T+273.15)^4 - (Tamb+273.15)^4)` and the additive
duty-driven coupling term (out of scope here, single-zone only).

**Nonlinearities present:** duty clamp to [0,1]; the optional T⁴ loss term;
fault injection (`ELEMENT_DEAD`, `RELAY_WELDED`, `TC_DETACHED`, `TC_FROZEN`,
`TC_OPEN`); deterministic read-time noise; `sim_max31856_quantize_tc()`'s
0.0078125 °C quantiser; `sim_relay_lag_step()`'s actuation delay.

**The G1 parameterisation actually used by every harness**
(`sim_plant_from_zone_cfg()`): `heater_power_w = model_k_dc`,
`thermal_mass_j_per_c = model_tau_s`, `loss_coeff_w_per_c = 1.0`,
`sensor_delay_s = model_dead_time_s`, `sensor_lag_tau_s = 0`. Units are
nominal, not physical. **Two consequences matter for everything below:**

1. Under this mapping, "thermal mass" and "time constant" are *the same
   number*. You cannot change one without changing the other.
2. `radiative_coeff_w_per_k4` has no calibrated meaning in nominal units,
   because `loss_coeff_w_per_c` is pinned to the free scale 1.0. It is
   effectively unusable alongside G1 — and in fact no harness sets it.

Bench constants in use (`sim_measured_zone_constants.h`, live `GET /api/zones`
2026-09-10): `k_dc` = 39.2459 / 31.9669 / 31.6810 °C/duty, `tau_s` = 263.8 /
269.8 / 270.9 s, `dead_time_s` = 52.8 / 43.5 / 33.9 s.

### 1.2 What the owner's four scenarios need, and what is missing

| Owner scenario | Expressible today? | What is missing |
|---|---|---|
| **Thermocouple ~5× closer to the elements than to the load** | **No.** | There is only ONE thermal node. `sensor_c` is a delayed, lagged *copy of that same node*. There is no "load" for the sensor to be far from, so the sensor can never lead the load on rise, and on element cut-out `element_c` itself decays at the *bulk* rate, so `sensor_c` cannot fall fast while the ware stays hot. `sensor_lag_tau_s` only ever makes the reading **slower**; nothing in the model can make it **faster than the load**. Needs: a second thermal node (load) and a third (sensor tip) with an explicit conductance split. §2.1. |
| **Changed thermal mass (load added/removed)** | **No.** | `thermal_mass_j_per_c` is a fixed `cfg` field with no mutation hook, and under G1 it *is* `tau_s`, so raising it also raises the identified time constant by construction. There is no way to say "the ware got heavier, the element and the losses did not change" — which is what a load change physically is. Needs the load node from §2.1, then a capacity multiplier on it alone. §2.2. |
| **Much higher temperatures (~1200 °C)** | **No.** | Two independent blockers. (a) *Ceiling*: with the measured constants the plant asymptotes at `ambient + k_dc·u` ≈ 24 + 39 = **63 °C at full duty**. There is no duty schedule that reaches 1200 °C. (b) *Character*: `radiative_coeff_w_per_k4` adds a loss term but does **not** implement the co-scaling of `k` and `tau` that the physics requires (`K = P/C_loss` and `tau = C_thermal/C_loss` share a denominator), and it is inert under the G1 mapping anyway. Needs a kiln-scale plant configuration plus an `s(T)` conductance scale. §2.3. |
| **A PID tune that was never good** | **No — and it is not a plant feature.** | Gains are handed to `pid_init()` by each harness as literals. Nothing exists that derives a gain set from a *deliberately wrong* model, so "badly tuned" would today mean "numbers someone made up", which is neither reproducible nor interpretable. Needs a mismatch-factor tuning path. §2.4. |

### 1.3 Two further gaps, not in the owner's list but blocking the deliverable

- **There is no scenario runner.** Each existing harness
  (`sim_fuzzy_closedloop.c`, `sim_iter_tune.c`, `sim_credibility_gate*.c`,
  `sim_wide_temp_sweep.c`) is a standalone `main()` with its own hand-rolled
  orchestration and its own duplicated `make_plant_cfg`/`sim_tick`. There is
  no table of scenarios, no arm concept, no shared aggregation, no way to add
  a scenario without writing a new `main()`.
- **There is no cross-run state carrier.** `adaptive_tune` adapts at *run end*
  (`adaptive_tune_run_end()`), so any adaptive arm needs a *sequence* of
  firings with `zones_config` state carried between them. No harness does
  this today.

### 1.4 The blocker the owner named: fuzzy and `adaptive_tune`'s Ki path are mutually exclusive

This is the single most important constraint on the scenario matrix and it
must be stated wherever this suite's combination-arm results are quoted.

`adaptive_tune_refine_ki_locked()` (`adaptive_tune_ki.c`) diagnoses from the
zone's **effective** closed-loop trace (`trace_actual_c[]`/`trace_duty[]`,
populated downstream of any per-tick gain rescale) but writes its correction
relative to the **stored reference** Ki read from `zones_config_get_pid()`.
`pid_fuzzy_adjust()` rescales the applied Ki between those two points. The
correction is bang-bang — every triggering verdict emits exactly
±`ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE` (0.20), so the per-run factor is a
fixed 1.20× — and `1.2^n ≥ 5.0` gives n = 9 runs to the cumulative bound
`ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT = 5.0`. A negative test measured the
reference Ki converge to 4.2998× baseline after 10 simulated runs, matching
`1.2^8` to four decimal places. Full detail:
`docs/audits/adaptive_tune_ki_effective_reference_loop_2026-09-13.md`;
independently reproduced in the opus review `a3803057`.

The fix landed tonight (`e78fbc5b`) is **withholding**: `adaptive_tune_ki.c`
refuses any Ki correction whenever the zone is `ZONE_CONTROL_MODE_PID_FUZZY`
at non-zero strength. The review upheld that shape — the correction is a
classifier output, not a measurement, so there is no factor to divide back
out.

**Therefore, today:**

| Adaptation path | Fuzzy off | Fuzzy on |
|---|---|---|
| `adaptive_tune_model.c` SIMC path (refits `K_dc` from a duty-vs-temperature identification, recomputes Kp/Ki/Kd absolutely) | adapts | **still adapts** — a tick-level Ki rescale cannot bias a steady-state duty-vs-rise fit, confirmed in the audit |
| `adaptive_tune_ki.c` relative Ki refinement | adapts | **withheld entirely** |

So the combination arm **can** be tested for model/SIMC adaptation and
**cannot** be tested for relative-Ki refinement until the mutual exclusion is
removed. **Work item 9 is that fix, and it is a prerequisite for the
combination arm's Ki results to mean anything.** Until it lands, every
combination-arm result must carry the label `KI_WITHHELD` in the output, and
the aggregator must refuse to report a Ki-attributable difference for that arm
(§5.4). This is exactly the "loud failure rather than a silent meaningless
number" posture the owner asked for.

---

## 2. Part B — model extensions, each justified

All of these land in **new** files, not by editing `sim_plant.c`'s existing
structs in place beyond adding opt-in fields. **The default configuration
must remain bit-identical**: every existing harness that does not opt in must
produce byte-for-byte the same output, and `sim_iter_tune`'s A1 measurement
must still read exactly **24 accepts / 21 rejects / 615 unchanged** out of
660 at `mc_runs=220`. That is the acceptance criterion for work item 1.

### 2.1 Sensor placement and sensor dynamics — a three-node model

**The physical claim being modelled.** A thermocouple mounted near the
elements sees the element's radiant/convective field far more strongly than it
sees the ware. On a rise it therefore leads the load; when the elements cut
out it falls quickly, because its dominant heat source just disappeared, while
the ware — which holds nearly all the stored energy — barely moves. This is
the standard furnace-control tradeoff (§3.1 cites it).

**New state variables** (three nodes, replacing the one):

| Symbol | Meaning | Capacity |
|---|---|---|
| `E` (`element_c`, reused) | element / near-element gas | `C_e` |
| `L` (`load_c`, new) | bulk: ware + refractory mass | `C_l` |
| `S` (`sensor_node_c`, new) | thermocouple tip | `C_s` |

**Update equations** (forward Euler at `dt_s`, computed from a common
snapshot of `E,L,S` so the order of the three lines cannot matter):

```
dE = ( u*P_max  - G_el*(E - L) - G_ea*(E - Tamb) ) / C_e * dt
dL = (           G_el*(E - L) - G_la*(L - Tamb) ) / C_l * dt
dS = ( G_se*(E - S) + G_sl*(L - S)              ) / C_s * dt
E += dE;  L += dL;  S += dS
```

The existing transport-delay ring and `sensor_lag_tau_s` first-order lag are
then applied **to `S` instead of to `E`**, unchanged, so the MAX31856
quantiser, noise and fault paths downstream keep working untouched.

**The parameter that expresses "5× closer".** One number, `sensor_bias_p` ∈
[0,1] — the fraction of the sensor tip's total conductance that goes to the
element rather than to the bulk:

```
G_s  = C_s / sensor_tau_s            (total sensor conductance; sensor_tau_s is the
                                      tip's own time constant, e.g. 10 s for a
                                      sheathed kiln TC — a TEST FIXTURE value)
G_se = G_s * sensor_bias_p
G_sl = G_s * (1 - sensor_bias_p)
```

"**5× closer to the elements than to the load**" is implemented as a **5:1
conductance ratio**, `sensor_bias_p / (1 - sensor_bias_p) = 5`, i.e.
**`sensor_bias_p = 5/6 ≈ 0.8333`**. *State plainly in the code comment that
this is an interpretation, not a measurement*: "5× closer" is a geometric
statement, and mapping a distance ratio onto a conductance ratio 1:1 is a
modelling choice. (Radiative view-factor coupling falls faster than 1/d and
conduction through gas falls roughly as 1/d, so 5:1 is a conservative-to-middle
reading. Nothing in this repo measures it. Flag it at every use.)

The centre-mounted reference case is `sensor_bias_p = 0.0` (sensor sees only
the bulk).

**Reduction to today's model — the compatibility contract.** The three-node
model is reached only via a new `sim_plant_cfg_t` field
`node_model` = `SIM_NODE_LEGACY` (default, zero) or `SIM_NODE_THREE`. Under
`SIM_NODE_LEGACY` the code path is the *existing* `sim_plant_step()`
unchanged — not a three-node model with degenerate parameters. This is
deliberate: a "mathematically equivalent" reparameterisation would change
floating-point results in the last bits and break the A1 pin, and chasing that
would waste a day. **Two code paths, one default, bit-identical.**

**What this does to the effective FOPDT the controller sees.** With
`sensor_bias_p` high, the measured signal is dominated by `E`, whose own
response to a duty step has time constant `C_e/(G_el + G_ea)` — much shorter
than the bulk's `C_l/G_la`. The consequences, in the terms a FOPDT fit would
report:

- **Apparent dead time falls.** The transport path from the element through
  the bulk to the sensor is bypassed; the sensor responds essentially at the
  element's own rate plus the tip lag.
- **A fast mode appears.** The step response is no longer first-order: a fast
  rise toward a partial value, then a slow bulk-driven approach to the DC
  gain. A FOPDT fit to the *early* part of that response reports a small `tau`
  and a large initial slope per unit duty; a fit to the *whole* response
  reports something closer to the bulk. Which one autotune reports depends on
  its window — and that ambiguity is itself a finding worth recording.
- **DC gain is roughly unchanged**, because at steady state every node
  equilibrates; the split changes the *dynamics*, not the endpoint.

**Why this is exactly a mismatch a gain-adaptation layer might absorb.** A PID
tuned to `(k, tau, L)` fitted at a centre-mounted sensor, then run against a
sensor that leads with a much shorter effective `L`, sees a loop whose
high-frequency gain is far larger than the model assumed — the classic
over-aggressive-loop signature. Conversely, the *element cut-out* transient
gives a large, fast negative error rate with the load barely moving, which is
precisely the (error, rate) region `pid_fuzzy.c`'s rule table was built to
back off in. Whether the 3×3 table's actual consequents (which touch Kp
strongly, Ki only in some cells — see `docs/research/fuzzy_ramp_tracking_2026-09-13.md`)
help or hurt here is an **open empirical question this suite exists to
answer**, not a prediction this plan makes.

### 2.2 Changed thermal mass

**What "the mass changed" actually means.** A kiln load is ware placed in the
chamber. It changes the **bulk capacity** `C_l`. It does **not** change the
element's own mass, the element's power, or the loss conductance to ambient —
those are properties of the kiln, not the load. (Strictly a dense load also
slightly changes the element-to-bulk conductance `G_el` and the enclosure's
effective emissivity; both are second-order next to a 2–3× capacity change and
this plan deliberately does not model them. Say so in the code comment.)

**Decision: a capacity multiplier on `C_l` only**, `load_mass_mult`, fixed at
scenario start. Scenarios: `0.5` (tune was done loaded, firing is near-empty),
`1.0` (matched), `3.0` (tune was done empty, firing is heavily loaded).

**Rejected alternatives, with reasons:**

- *A second mass with its own coupling.* More faithful (ware and refractory
  really are different masses) but adds two free parameters neither measured
  nor identifiable from anything this project has, for an effect the single
  multiplier already produces: a slower bulk with an unchanged element.
  Rejected as unearned complexity.
- *A scheduled change mid-firing.* Not physical — nobody adds ware to a hot
  kiln. The interesting case is a **fixed** load the tune did not know about.
  Rejected. (One exception is worth *recording but not building*: a load whose
  effective capacity changes with temperature through an endothermic phase
  change — quartz inversion, carbon burnout. That is a real kiln effect and
  would appear as a stall at a fixed temperature. It is out of scope here; it
  needs its own model and its own justification.)

Note this scenario is **only expressible because of §2.1**: in the one-node
G1 model, capacity *is* `tau`, so "the mass changed" and "the time constant
changed" were the same edit and there was no way to hold the gain fixed.

### 2.3 High-temperature regime

**Reuse, do not reinvent.** Two prior pieces of work already settle the
structure, and the implementer must read both before writing a line:

- `docs/audits/high_temperature_transfer_analysis_2026-09-08.md` — derives
  that `K = P_max/C_loss` and `tau = C_thermal/C_loss` share the same
  conductance denominator, so **both scale as `1/s(T)`**, and tabulates
  `s(1200 °C) = 21.3` at `f_rad = 0.05`, i.e. `k` and `tau` each fall ~20×
  from bench to cone temperature. It also marks that magnitude **[KILN]** —
  unmeasured, spanning 5× to 80× across a plausible `f_rad` range.
- `docs/audits/gain_scheduling_design_2026-09-13.md` — establishes that
  `zone_model_at()`/`coupling_at()` exist as passthrough seams (they accept a
  temperature and ignore it), that `model_fit_temp_c` records the operating
  point a fit was taken at, and that **no `loss_conductance_scale()` exists in
  firmware at all** — it is PC-side only, in
  `tools/PcTools/src/kilnctrl/plant_sim.py`.

**What is reused, exactly:** the scale function, verbatim in form:

```
s(T) = (1 - f_rad) + f_rad * ((T + 273.15) / 328.15)^4        f_rad = 0.05  [ASSUMED]
```

normalised so `s(55 °C) = 1.0`. **Application in the three-node model:**
multiply the two loss conductances to ambient by it each tick, evaluated at
that node's own current temperature:

```
G_ea_eff = G_ea * s(E)
G_la_eff = G_la * s(L)
```

Because `K` and `tau` share that denominator, scaling the conductance scales
both correctly with **no second free constant** — which is the entire reason
this form was chosen over inventing a separate `k(T)` and `tau(T)`. `G_el` and
`G_s*` are *internal* conductances, not losses to ambient, and are **not**
scaled: the analysis's derivation is about loss conductance specifically.
State that as a modelling decision, not a fact.

**The ceiling problem is separate and must be solved separately.** `s(T)` does
not raise the plant's reachable temperature — it lowers it. A kiln-scale
scenario needs a **kiln-scale plant configuration**: `P_max`, `C_e`, `C_l`,
`G_ea`, `G_la` chosen so the plant reaches ~1300 °C at full duty with a
plausible time constant. Anchor it to `plant_sim.py`'s existing physical
high-temperature kiln model (the block below `EXTRAPOLATION_BOUNDARY_C`,
`tools/PcTools/src/kilnctrl/plant_sim.py`, which already argues why the bench
rig's identified constants cannot be extrapolated into a real kiln at all) —
**read that block and port its anchor; do not invent a second set of kiln
constants.** Every one of these numbers is a **TEST FIXTURE**.

**Honesty requirement.** Any output line from a kiln-scale scenario must carry
a marker (`[f_rad=0.05 ASSUMED]`) so no reader can quote a 1200 °C number
without the caveat travelling with it.

### 2.4 A mistuned PID, defined reproducibly

**Definition:** gains produced by running the project's *own* tuning rule
against a **deliberately wrong model**, with the wrongness stated as three
factors.

```
k'   = k_true   * mismatch_k
tau' = tau_true * mismatch_tau
L'   = L_true   * mismatch_L
gains = pid_autotune_tune_from_fopdt({k', tau', L'}, AUTOTUNE_RULE_SIMC, lambda_s = 0)
```

`pid_autotune_tune_from_fopdt()` is real production code
(`firmware/KilnFW/App/drivers/control/pid_autotune.c`): SIMC, `Kc = tau /
(K*(lambda + L))`, `Ti = min(tau, 4*(lambda + L))`, `Td = L/2`, with
`lambda = 3*L` when `lambda_s ≤ 0`. **Link it; do not reimplement the rule.**

This gives "badly tuned" a meaning that is reproducible, interpretable, and
directional:

| Name | `mismatch_k` | `mismatch_tau` | `mismatch_L` | Resulting loop character |
|---|---|---|---|---|
| `TUNE_MATCHED` | 1.0 | 1.0 | 1.0 | reference |
| `TUNE_HOT` (too aggressive) | 0.5 | 2.0 | 0.5 | believed a sluggish, low-gain plant → Kc ~4× too high vs. the true plant |
| `TUNE_COLD` (too timid) | 2.0 | 0.5 | 2.0 | believed a fast, high-gain plant → heavily detuned |
| `TUNE_SLOW_INTEGRAL` | 1.0 | 3.0 | 1.0 | Ti tracks a 3× long `tau`; integral far too slow |

Factors are powers of 2 and 3 so that the resulting gain ratios are exact and
a reviewer can verify them by hand from the SIMC formula. The *true* plant is
always the scenario's plant; only the model handed to the tuning rule is
wrong. **Test fixture values.**

---

## 3. Part C — the research, and where it does not answer

Per instruction, `docs/research/multizone_thermal_modelling_literature_2026-09-11.md`
and `docs/research/fuzzy_ramp_tracking_2026-09-13.md` were re-read rather than
re-searched. Their relevant conclusions are carried below and not repeated
elsewhere.

### 3.1 Thermocouple placement in furnace control — SUPPORTED, qualitatively

Watlow's engineering knowledge base states the tradeoff directly and in the
same terms §2.1 models it. Fetched in full:
https://www.watlow.com/resources-and-support/engineering-tools/knowledge-base/sensor-placement-in-a-thermal-system

Direct quotations:
- Near the heat source: "placing the sensor closer to the heat source will
  keep the heat fairly constant throughout the process", which minimises
  thermal lag and enables frequent cycling, "reducing the potential for
  overshoot and undershoot at the work load."
- Near the load: "placing the sensor closer to the work load will enable the
  sensor to 'see' the load temperature change faster", but the resulting
  source-to-sensor distance causes "thermal lag or delay" giving "a wider
  swing between the maximum (overshoot) and minimum (undershoot) temperatures
  at the work load."
- "place the sensor half-way between the heat source and work load to divide
  the heat transfer lag times equally."

**What this supports:** that sensor placement materially changes overshoot,
and that the two placements trade *loop* stability against *load* fidelity —
which is exactly the two-objective structure §2.1 encodes with
`sensor_bias_p`. **What it does not give:** any quantitative model, any
conductance ratio, or any guidance on what "5× closer" means numerically. The
5:1 conductance interpretation in §2.1 is **ours**, unsupported by this source.

A vendor page (Skutt, ceramic kilns) states the complementary claim — an
unresponsive thermocouple lags during ramp-up, the controller believes the
kiln is cooler than it is, and it overshoots, more dramatically at faster
ramp rates. **Marked as search-snippet-only: the page returned HTTP 403 to a
direct fetch, so this claim is recorded as reported by the search index and
was not verified against the page itself.** It is consistent with the Watlow
source and with the classical result, and it is a vendor marketing page
regardless, so it carries no independent weight.

**Note the direction this creates for our scenario set:** the owner's kiln has
the *fast*, near-element sensor. The literature's overshoot warning is aimed
at the *slow*, far sensor. The near-element case trades the opposite way — the
loop is well-informed and stable, but the sensor reports a temperature the
ware never reaches, so the *ware* undershoots on rise and overshoots on soak.
Our simulation observes the sensor, as the controller does, so we can measure
loop behaviour directly; we can also report the *load* node's own trajectory,
which no real installation of this kind can see. That is a genuine advantage
of simulation and work item 6 requires reporting it.

### 3.2 Fuzzy PID under plant/model mismatch — SUPPORTED in the literature, NOT transferable

- Jin, Renjie, *"Research on Optimized Fuzzy PID Temperature Control Strategy
  Based on Improved Particle Swarm Optimization"*, arXiv:2609.00001,
  submitted 14 May 2026. https://arxiv.org/abs/2609.00001 — **abstract
  fetched; full text not read.** The abstract states a settling time of
  105.5 s on an FOPDT model, "approximately 46.7 % faster than standard PSO",
  and that "robustness tests confirm superior stability under severe model
  mismatches". **From-abstract-only:** the specific mismatch magnitudes
  (a gain −10 % / time-constant +25 % perturbation was reported by the search
  index, not by the abstract text retrieved) are **not** confirmed from the
  source and must not be cited as if they were.
- A comparative study of expert-adjustable fuzzy control for injection-machine
  temperature control reports maximum fluctuation amplitudes of 9.4 °C (EAFC)
  vs 29 °C (PID) vs 17.3 °C (fuzzy PID) under a gain/time-constant +20 %
  perturbation: https://pmc.ncbi.nlm.nih.gov/articles/PMC9252661/ —
  **from search-result summary; the article body was not fetched.** Treat the
  ordering as indicative only.

**What the literature does and does not settle for us.** It broadly supports
the *premise* that fuzzy-adaptive PID outperforms fixed PID under parameter
mismatch — which is precisely the owner's objection to tonight's measurement,
and it is a reasonable prior. It settles **nothing** about *this* controller,
for three reasons a reviewer should hold onto:

1. Published fuzzy PIDs are overwhelmingly full-authority designs — fuzzy
   rules producing ΔKp/ΔKi/ΔKd over a wide range, often with optimised
   membership functions. `pid_fuzzy.c` is a **bounded ±50 % nudge at
   `strength_pct = 100`**, a read-only 3×3 rule table, gated by
   `MAX_NUDGE_FRACTION = 0.5`. It is a much weaker intervention and the
   published effect sizes do not transfer.
2. Most such papers compare against a *poorly* tuned baseline PID. The
   relevant comparison here is against a SIMC-tuned baseline and, critically,
   against a **matched-effective-gain static arm** — an arm almost no
   published comparison includes, and whose absence produced a wrong verdict
   in this very project (`1570a65a`).
3. `docs/research/fuzzy_ramp_tracking_2026-09-13.md` already established from
   first principles that this rule table's response to sustained ramp lag
   (Kp up, Ki untouched) is the **wrong lever** for ramp-following error,
   since `Kv = Ki·P(0)` sets the asymptotic lag. That is a structural finding
   about *our* table and no amount of literature about *other* tables
   displaces it.

**Where the literature simply does not answer:** I found nothing on fuzzy-PID
performance with a near-element (leading) sensor as the mismatch source, and
nothing on the interaction between a per-tick fuzzy gain rescale and a
cross-run relative-gain adapter — §1.4's failure mode. Those are original to
this project and must be settled by measurement here, not by citation.

### 3.3 Gain-adaptation robustness benchmarks — no usable standard benchmark found

Searching for a standard benchmark set for gain-scheduled vs fixed PID under
FOPDT model mismatch returned only application papers (load-frequency control,
CSTRs, fertigation), each with its own plant and its own criteria. **There is
no canonical benchmark to borrow.** The practical consequence is that §5's
scoring must be *our* production subscores — which is what this plan does, and
which is better anyway, because those are already validated against 33 real
dwell-zone instances on this hardware.

Sources:
- https://www.watlow.com/resources-and-support/engineering-tools/knowledge-base/sensor-placement-in-a-thermal-system
- https://arxiv.org/abs/2609.00001
- https://pmc.ncbi.nlm.nih.gov/articles/PMC9252661/
- https://link.springer.com/article/10.1007/s00521-025-11170-0
- https://ietresearch.onlinelibrary.wiley.com/doi/10.1049/rpg2.12569

---

## 4. Part D — the scenario matrix, the arms, and the parallel runner

### 4.1 Arms (controller configurations), crossed with every scenario

| Arm id | Fuzzy | `adaptive_tune` | Firings per scenario | What it isolates |
|---|---|---|---|---|
| `A_PID` | off (`strength_pct = 0`) | off | 1 (repeated N times for comparability, identical output) | Baseline. |
| `A_PID_AT` | off | **on** | **N = 9** sequential, state carried | Does the self-improving PID recover from the mismatch on its own? |
| `A_FUZZY25` | strength 25 | off | 1 | Fuzzy at a conservative strength. |
| `A_FUZZY50` | strength 50 | off | 1 | Fuzzy at the strength prior measurements used. |
| `A_FUZZY_AT` | strength 50 | **on** | **N = 9** | The combination the owner wants. **Ki path is `KI_WITHHELD` until work item 9 lands — see §1.4.** |
| `A_STATIC_MATCHED` | off | off | 1 (+ a measurement pass) | **The arm whose absence produced a wrong verdict.** Fixed Kp/Ki/Kd multipliers set to `A_FUZZY50`'s *measured ramp-phase mean* multipliers in this scenario. |

**`A_STATIC_MATCHED` must be built the way `1570a65a` proved is correct.** Its
multipliers are **not** the rule table's centre-cell maximum (×0.75 / ×1.25 /
×0.75). That was the mislabelled arm. They are the **instrumented mean applied
multipliers over the ramp phase**, which for `fuzzy_50` on the bench plant
measured ×0.8684 Kp / ×1.1618 Ki / ×0.8382 Kd — roughly half the centre-cell
perturbation. Because the mean depends on the trajectory, it must be
**re-measured per scenario**, not copied: run `A_FUZZY50` first with
instrumentation recording the applied/base multiplier each tick, take the mean
over the ramp phase, then run `A_STATIC_MATCHED` with those constants and
inference off. Two deterministic passes, same binary, no RNG. If
`A_STATIC_MATCHED` reproduces `A_FUZZY50` inside materiality on all four
objectives, **the inference contributed nothing in that scenario** and the
report must say so in those words.

**Why N = 9 for the adaptive arms.** Three independent reasons converging on
the same number: (a) `1.2^9 ≥ 5.0` is exactly the run count at which
`adaptive_tune_ki`'s cumulative bound binds, so a 9-run sequence is the
shortest that exercises the full authority envelope including its refusal;
(b) `adaptive_tune_model.c`'s EMA update has a bounded-walk invariant in
`alpha ∈ (0,1]` — at `alpha = 0.15`, 9 runs reaches `1 - 0.85^9 ≈ 77 %` of
the way to a step change, enough to see convergence direction and rate
without simulating an asymptote; (c) nine firings is a plausible real
commissioning sequence, so the answer is operationally meaningful. Record
per-run values for all 9, never only the endpoint — the *trajectory* is the
result, and a non-monotonic or oscillating walk is a finding.

**Arm selection mechanism — verified, not assumed.** The claim that
`pid_fuzzy_adjust()` at `strength_pct = 0` reproduces base gains bit-for-bit
is asserted as a safety contract in `pid_fuzzy.c`'s own header ("`strength_pct
== 0` MUST reproduce the base gains", `pid_fuzzy.h:80-81`) and is enforced by
existing bit-exact tests in `test_pid_fuzzy.c` and
`test_profile_executor_prestart.c`, independently confirmed passing during the
`e78fbc5b` audit. **It holds, so arms are selected at runtime from ONE
binary** — no compile-time switch, no separate executables. Work item 4's
acceptance criterion re-verifies it inside this suite rather than trusting the
citation.

**One confound to design out now.** A separate change may land shortly making
a zone with *no identified model* run plain PID rather than fuzzy on guessed
bands. Every scenario in this suite must therefore **always install a complete,
explicit model** (`model_k_dc`/`model_tau_s`/`model_dead_time_s`, plus
`model_fit_temp_c`) before the run, and the runner must **assert** the model is
present and that `pid_fuzzy_derive_bands()` returned true. A scenario that
silently fell back to plain PID would make a fuzzy arm identical to
`A_PID` for a reason having nothing to do with the physics — and would read
as "no separation". Assert, do not hope.

### 4.2 The scenario table

Every scenario is single-zone, on zone 0's constants unless stated. `RAMP` is
the ramp rate of the profile's rising segment. `SEP?` is the pinned
expectation: whether this scenario is believed to separate arms (§5.3 turns
this into a falsifiable assertion).

| # | Scenario id | Plant | Tune | Ramp | Purpose | SEP? |
|---|---|---|---|---|---|---|
| S0 | `NULL_SLOW` | bench, legacy node model | matched | 40 °C/hr | Control. All arms must converge. If this one separates, the harness is broken. | **no** |
| S1 | `BASELINE` | bench, legacy node model | matched | 150 °C/hr | Reproduces tonight's condition. Anchors every other scenario. | no |
| S2 | `SENSOR_NEAR_ELEMENT` | 3-node, `sensor_bias_p = 0.8333` | matched to the *centre-mounted* fit | 150 °C/hr | The owner's headline case. | **yes** |
| S3 | `SENSOR_CENTRE` | 3-node, `sensor_bias_p = 0.0` | matched | 150 °C/hr | Isolates "3-node model" from "sensor placement". Must behave like S1. | no |
| S4 | `SENSOR_NEAR_FAST_RAMP` | 3-node, `p = 0.8333` | matched | 300 °C/hr | Placement effect at a rate where the literature says overshoot grows. | **yes** |
| S5 | `MASS_HEAVY` | 3-node, `load_mass_mult = 3.0` | tuned on `mult = 1.0` | 150 °C/hr | Ware added after tuning. | **yes** |
| S6 | `MASS_LIGHT` | 3-node, `load_mass_mult = 0.5` | tuned on `mult = 1.0` | 150 °C/hr | Kiln emptied after tuning — the aggressive direction. | **yes** |
| S7 | `TUNE_HOT` | 3-node, matched plant | `TUNE_HOT` (§2.4) | 150 °C/hr | "Never good on the real kiln", aggressive. | **yes** |
| S8 | `TUNE_COLD` | 3-node, matched plant | `TUNE_COLD` | 150 °C/hr | Timid. Expect slow settle, large undershoot. | **yes** |
| S9 | `TUNE_SLOW_INTEGRAL` | 3-node, matched | `TUNE_SLOW_INTEGRAL` | 150 °C/hr | Isolates the integral term, the one `docs/research/fuzzy_ramp_tracking_2026-09-13.md` says the table barely touches. Expect fuzzy to be **inert** here; a separation would contradict that analysis and must be investigated, not celebrated. | no |
| S10 | `KILN_HIGH_T` | kiln-scale, `s(T)` on, 3-node, `p = 0.8333` | tuned at 200 °C, run to 1250 °C | 150 °C/hr | The regime the bench cannot reach. Carries `[f_rad=0.05 ASSUMED]`. | **yes** |
| S11 | `KILN_HIGH_T_SCHEDULED` | as S10 | re-tuned at each segment's own start temperature via §2.4 with `mismatch = 1` | 150 °C/hr | Upper bound: what a perfect gain schedule would buy. Bounds how much of S10's error is schedulable vs. absorbable by fuzzy. | **yes** |
| S12 | `COMPOUND_WORST` | 3-node, `p = 0.8333`, `mult = 3.0` | `TUNE_HOT` | 200 °C/hr | All three mismatches at once. Most likely place for a fuzzy benefit or a fuzzy failure to be visible. | **yes** |

13 scenarios × 6 arms. The adaptive arms are 9 firings each, so the total
firing count is 13 × (4 single-firing arms + 2 × 9) = 13 × 22 = **286
simulated firings**, plus 13 instrumentation passes for `A_STATIC_MATCHED` =
**299**.

**Profile shape, identical for every scenario** so scoring is comparable: ramp
from ambient to T1, dwell at T1, ramp to T2, dwell at T2, with T1/T2 scaled to
the plant's range (bench: 45 / 60 °C; kiln-scale: 600 / 1250 °C). Two dwell
entries per firing gives two `ENTRY_PEAK_C`/`SETTLE_S` instances per run and
two ramps for `LAG_S` — enough for a per-scenario comparison without inflating
runtime.

### 4.3 The runner, determinism, and how a reader adds a scenario

**Shape.** One new executable, `sim_scenarios.exe`, built from
`firmware/KilnFW/App/test/sim_scenarios.c` plus a scenario table in
`sim_scenario_table.c`/`.h`. Invocation:

```
sim_scenarios.exe [--shard I --of N] [--scenario <id>] [--format tsv|human]
```

**Adding a scenario without touching the runner** — the owner's explicit
requirement. A scenario is one `static const sim_scenario_t` entry appended to
one array in `sim_scenario_table.c`. The struct holds only *data*: plant
config, tune config, profile, the `SEP?` pin, and the per-scenario assertion
thresholds. No function pointers, no per-scenario code in `sim_scenarios.c`.
If a new scenario genuinely needs new behaviour, that behaviour becomes a new
*field* with a default that leaves every existing row unchanged. A
`_Static_assert` pins `ARRAY_LEN(table)` against a named constant so adding a
row is a deliberate act that shows up in review — the same posture
`FIRING_COMPARE_VOTING_MASK`'s `_Static_assert`s take.

**Determinism — the load-bearing requirement.** Parallel execution must not
change results, and the implementation achieves that by construction, not by
testing for it afterward:

1. **No shared mutable state between runs.** Each (scenario, arm) run
   constructs its own plant state, its own `pid_state_t`, its own
   `zones_config` fake, from scratch. Nothing is carried across runs *except*
   deliberately, within one adaptive arm's 9-firing sequence, which always
   executes sequentially inside a single worker.
2. **No wall-clock, no `rand()`, no address-dependent behaviour.** The only
   RNG is `sim_kiln_state_t`'s LCG, seeded to its fixed constant at every
   reset, and sensor noise is set to 0 in every scenario in this suite (a
   deterministic plant is the point; noise buys nothing here and is a separate
   study).
3. **Fixed `dt_s`** for every run in the suite (1.0 s), so no run's step count
   depends on anything but its profile.
4. **Sharding by index, aggregation by index.** Worker `I` of `N` runs
   scenarios where `index % N == I` and writes one TSV line per (scenario,
   arm, firing) to its own file. The parent concatenates and **sorts by
   scenario index then arm index then firing index** — never by completion
   order. No cross-worker reduction of floating-point values ever happens.
5. **Parallelism is process-level**, driven by
   `firmware/KilnFW/App/test/run_sim_scenarios.ps1` using `Start-Process` with
   distinct `--shard` values and distinct output paths. No threads, no OpenMP —
   nothing that could reorder a floating-point accumulation.

**The determinism assertion** (work item 7): the check runs the suite at
`--of 1` and at `--of 4`, and requires the two aggregated TSVs to be
**byte-identical**. Not "within tolerance" — byte-identical. Any difference is
a hard failure.

**Failure modes must be loud.** A scenario that cannot run — model not
installed, `pid_fuzzy_derive_bands()` returned false, the plant never reached
the first setpoint, a dwell never settled, a NaN appeared anywhere — must
print `SCENARIO_REFUSED <id> <reason>` and cause a **non-zero exit**. It must
never emit a row of numbers. "A scenario that silently produces a meaningless
number is worse than one that refuses to run."

**Honest note on the value of parallelism here.** §4.5's runtime estimate says
the whole suite finishes in a couple of seconds of compute; parallel execution
saves nothing today. It is specified because the owner asked for it, because
the sweep extensions this structure invites (a 5-point `sensor_bias_p` sweep ×
4 masses × 4 tunes is ~2000 runs) will use it, and — the part that actually
matters now — because **writing the runner to be shardable forces the
no-shared-state discipline that makes results reproducible at all.** That
discipline is the deliverable; the speed is incidental. Say so in the runner's
header comment rather than implying a performance benefit that does not exist.

---

## 5. Part E — scoring

### 5.1 Use the production subscores; score the four objectives separately

`firing_score.c` is linked, not mirrored. As of tonight it carries
`FIRING_SCORE_SETTLE_BAND_C = 2.0f` (chosen from measurement across 33 real
dwell-zone instances), `FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C` and
`FIRING_SUBSCORE_LAG_SIGNED_S`; `firing_compare.h` now carries an explicit
`FIRING_COMPARE_VOTING_MASK` with `_Static_assert`s so that enrolling an axis
in a verdict is a deliberate act.

| Owner objective | Instrument | Materiality |
|---|---|---|
| 1. Correct rate | `FIRING_SUBSCORE_LAG_S` (magnitude) **and** `FIRING_SUBSCORE_LAG_SIGNED_S` (direction, reported never adjudicated) | 0.5 °C, converted at the segment's ramp rate; report the °C equivalent alongside the seconds |
| 2. Settle quickly | `FIRING_SUBSCORE_SETTLE_S` | **No owner materiality figure exists for a TIME axis.** Report only; never declare a win. |
| 3. Settle accurately | `FIRING_SUBSCORE_STEADY_RMS_C` | 0.5 °C |
| 4. Minimal over/undershoot | `FIRING_SUBSCORE_ENTRY_PEAK_C` **and** `FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C`, reported separately | 0.5 °C each |

**Never aggregate.** No composite, no weighted sum, no "overall winner". Four
objectives, six numbers, reported side by side per (scenario, arm). An arm
that wins objective 4 and loses objective 1 has done exactly that, and the
report says exactly that.

**The 0.5 °C rule applies per objective.** A difference below 0.5 °C on an
objective is reported with its number and the label `BELOW_MATERIALITY`, and
carries no verdict. Do not chase sub-0.5 °C effects and do not let a stack of
them add up to a claim.

**`SETTLE_S`'s band caveat, from `1570a65a`.** That review found a reported
ordering that survived only at the 2.0 C settle band and reversed at 0.5 C.
Every scenario must therefore report `SETTLE_S` at **both** the production
2.0 °C band and a 0.5 °C band, as two columns. If they disagree in ordering,
the aggregator prints `SETTLE_BAND_SENSITIVE` for that cell and the settle
result carries no weight at all for that scenario. This matches `SETTLE_S`'s
current report-only classification in `FIRING_COMPARE_REPORT_ONLY_MASK`.

### 5.2 Also report what no real kiln can see

Because the three-node model exists, the suite can report the **load node's**
trajectory alongside the sensor's: `load_peak_over_target_c`,
`load_lag_s`, and `sensor_minus_load_c` at each dwell entry. On a
near-element sensor this gap *is* the physical error the owner cares about —
the ware's actual firing — and no installation of that kind can measure it.
Report it as `[GROUND_TRUTH]`-tagged columns, clearly separated from the
scored subscores, because no controller can use them and they must never
enter a verdict.

### 5.3 The falsifiable assertion every scenario carries

Three layers, all of which must fail loudly:

1. **Structural invariants** (every scenario, every arm): no NaN anywhere; the
   sensor reading stays within [ambient − 1, plant ceiling + 50]; every
   segment's dwell is entered; energy is non-negative; and — for
   `sensor_bias_p > 0.5` scenarios only — `sensor_c` **leads** `load_c` during
   the ramp (mean of `sensor_c − load_c` over the ramp is positive) and
   **falls faster** than `load_c` in the first 60 s after a dwell's first duty
   cut-out. That second pair directly asserts §2.1's physical claim; if it
   fails, the sensor model is wrong and every result built on it is void.
2. **The `SEP?` pin.** Each scenario declares whether it is expected to
   separate arms (max pairwise difference on any *material* objective exceeds
   0.5 °C). A scenario pinned `no` that separates, or pinned `yes` that does
   not, is a **failure** requiring a human decision — either the pin was wrong
   (re-pin by hand, with the reason recorded in the table's comment) or
   something regressed. Pins are set at first measurement and thereafter only
   ever changed by hand with a written reason, the same ratchet posture
   `sim_iter_tune.c`'s `A1_PINNED_MAX_ACCEPTS` takes.
3. **The inference-vs-gain verdict.** For every scenario where `A_FUZZY50`
   separates from `A_PID`, the suite asserts a recorded classification:
   `INFERENCE` (the static-matched arm does *not* reproduce it) or
   `GAIN_ONLY` (it does). Flipping classification without a code change is a
   failure.

`main()` returns non-zero if any of the three fails. The check script
(`check_sim_scenarios.ps1`) follows `check_sim_iter_tune_bars.ps1`'s
three-status convention exactly: **0 = PASS, 1 = FAIL (a real regression),
3 = SKIP (toolchain absent — `vcvarsall.bat` missing, or `cl` never ran)**.
A missing compiler must never read as a pass or as a regression.

### 5.4 Reporting format

One TSV, one line per (scenario, arm, firing index), columns:

```
scenario  arm  firing  ki_state  lag_s  lag_signed_s  settle_s_2c  settle_s_0p5c
steady_rms_c  entry_peak_c  entry_undershoot_c  load_peak_c  load_lag_s
sensor_minus_load_c  applied_kp_mult_mean  applied_ki_mult_mean  applied_kd_mult_mean
refusals  notes
```

plus a human-readable per-scenario block: the four objectives as four rows,
arms as columns, each cell carrying its value and one of
`BETTER`/`WORSE`/`BELOW_MATERIALITY`/`SETTLE_BAND_SENSITIVE`/`KI_WITHHELD`,
and a closing line naming which arms separated and which did not. `ki_state`
is `KI_ACTIVE` or `KI_WITHHELD`; **the aggregator must refuse to attribute any
difference to Ki adaptation for a `KI_WITHHELD` row** and must print the §1.4
explanation once per report rather than leaving a reader to infer it.

TSV is chosen so run-to-run results are **diffable** — a reader can `diff` two
runs and see exactly which cells moved. Float formatting is fixed-width
`%.4f` so a whitespace change never shows as a diff.

---

## 6. The adaptation question: can the fuzzy layer improve across firings?

The owner asks for the fuzzy equivalent of `adaptive_tune`. There are three
candidate things to adapt, and they are **not** equally learnable.

### 6.1 Membership bands — already adaptive, and that alone is *not* enough

`pid_fuzzy_derive_bands()` (landed at `2c49465a`) derives the error and
error-rate membership bands from the zone's own identified model
(`model_k_dc`, `model_tau_s`). `adaptive_tune_model.c` refits that model at
each clean run end. **So the bands already improve automatically as the model
improves, with no new mechanism.** That is real and it is free.

**It does not satisfy the requirement.** Adapting the bands changes *where the
cells sit on the error axis*; it does not change *what the cells do*, nor how
much authority the layer has. If the rule table's response is the wrong lever
for a given plant — which `docs/research/fuzzy_ramp_tracking_2026-09-13.md`
shows it is for ramp lag — rescaling the axis does not fix it; it just
rescales when the wrong lever gets pulled. Record the band adaptation as
**already done**, and do not let it be counted as the answer.

**This suite tests it directly:** the `A_FUZZY_AT` arm's 9-firing sequence
exercises exactly this path (model refit → band re-derivation → changed fuzzy
behaviour). Whether the bands converge, oscillate, or wander is a measurable
result of work item 8, and it is the first honest measurement of this loop
anywhere in the project.

### 6.2 `strength_pct` — learnable in principle, and the best candidate

`strength_pct` is a single scalar with a monotone, bounded effect and a
bit-exact identity at 0. That makes it the only candidate with a tractable
learning problem: a scalar hill-climb over firings with a hard revert.

**But the circularity trap is live here and must not be re-entered.** The
prior design (`docs/audits/adaptive_tune_confidence_authority_design_2026-09-13.md`)
argued a confidence estimator could be non-circular; the appended opus review
(`1142c73b`) **refuted that argument as specified**: the observation ring is
never cleared at a run boundary, so a "forward" residual is scored partly
against the run's own training data; `ADAPTIVE_TUNE_MIN_DUTY_SPREAD` does not
close the general case (under the design's own linear predictor, duty cancels
out of the residual, and the gate is a 5-point within-ring conditioning check,
not coverage); and the real leak is **selection bias**, because non-settling
runs are excluded, biasing confidence upward. The review's own verdict was
build-later, and it added a fourth ground: the design is not implementable as
written.

**Do not repeat that reasoning.** The rule this plan imposes: **a fuzzy
strength adapter must never be scored by a quantity the fuzzy layer itself
moved.** Since `strength_pct` moves tracking error *by design*, "did tracking
error improve" is circular for exactly the same reason the Ki loop in §1.4 is
— an inference read in a frame the intervention transformed. The only
defensible score for a strength adapter is a **firing-to-firing comparison
under `firing_compare`'s existing Bar 1 / Bar 2 machinery, on a *different*
firing with the new strength**, which is an A/B over runs, not an inference
within a run. That is expensive in real firings and cheap here — which is a
genuine reason to simulate it, and the content of work item 10.

### 6.3 Rule consequents — NOT learnable from a firing's own evidence

Learning the nine cells' (ΔKp, ΔKi, ΔKd) triples from a firing means
identifying 27 parameters from one trajectory that visits, per
`docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md` and the corrected
reachability analysis in `docs/audits/review_sim_fuzzy_commits_2026-09-13.md`,
only a small subset of the cells — and mostly the centre. There is no
excitation that visits all nine cells during a firing anyone would actually
run, and a firing that did would be a firing you would not want. **State
plainly: this cannot be learned from a firing's own evidence on this system,
and this plan does not propose it.** If the consequents are ever changed, it
should be by offline design against this scenario suite, reviewed by a human —
which this suite makes possible for the first time.

### 6.4 Summary of the adaptation answer

| Candidate | Learnable from a firing's own evidence? | Status |
|---|---|---|
| Membership bands | Yes — indirectly, via the model `adaptive_tune` already refits | **Already implemented** (`2c49465a`). Necessary, not sufficient. |
| `strength_pct` | Only as a cross-firing A/B, never as a within-run inference | Best candidate. Design in work item 10; do not build on hardware before the simulation says which direction it moves. |
| Rule consequents | **No** | Out of reach. Offline design only. |

---

## 7. Work items

Each is independently implementable, in order, with its own acceptance
criterion. Items 1–8 touch only `firmware/KilnFW/App/test/`. Item 9 is the
only one touching production control code and is sequenced last deliberately.

---

**WI-1 — Three-node plant model, opt-in, default bit-identical.**
Add `node_model` (`SIM_NODE_LEGACY` = 0 default, `SIM_NODE_THREE`), the new
state (`load_c`, `sensor_node_c`) and config (`c_e`, `c_l`, `c_s`, `g_el`,
`g_ea`, `g_la`, `sensor_tau_s`, `sensor_bias_p`, `load_mass_mult`) to
`sim_plant.h`/`.c`, plus a `sim_plant_three_node_step()` alongside the
existing step. §2.1, §2.2.
*Acceptance:* (a) `build_host_tests.ps1` green; (b) `sim_iter_tune.exe 220`
still reports exactly **24 accepts / 21 rejects / 615 unchanged**;
(c) a new `test_sim_plant_three_node.c` asserts, on a duty step with
`sensor_bias_p = 0.8333`: the sensor leads the load throughout the rise, the
sensor's 63 % rise time is at least 3× shorter than the load's, and after a
duty cut-out the sensor falls at least 5× faster than the load in the first
60 s; (d) the same test with `sensor_bias_p = 0.0` asserts the sensor **lags**
the load.

**WI-2 — `s(T)` conductance scale and a kiln-scale plant configuration.**
Port `loss_conductance_scale()` from `tools/PcTools/src/kilnctrl/plant_sim.py`
into a **test-only** C helper (`sim_high_temp.c`/`.h`) with `f_rad` as a named
constant carrying the `[ASSUMED]` marker in its comment; apply it to `G_ea`
and `G_la` only. Add a kiln-scale plant config anchored to `plant_sim.py`'s
physical high-temperature model. §2.3.
*Acceptance:* a host test asserts `s(55) == 1.0f` exactly, `s(1200)` within
0.5 % of 21.3 (the published table value), and that a kiln-scale plant at full
duty asymptotes between 1250 °C and 1400 °C. The file's header comment states
**"TEST FIXTURE — never shipped; bench values never ship"** verbatim.

**WI-3 — Mismatch-factor tuning path.**
A test-only helper that builds gains by calling the real
`pid_autotune_tune_from_fopdt()` with a scaled model. §2.4.
*Acceptance:* a host test asserts `TUNE_MATCHED` reproduces the gains a direct
call with the true model produces, bit-for-bit; and that `TUNE_HOT`'s Kp
exceeds `TUNE_MATCHED`'s by the exact ratio the SIMC formula predicts from the
stated factors (computed in the test from the formula, not a pasted literal).

**WI-4 — Runner skeleton, scenario table, arm selection.**
`sim_scenarios.c` + `sim_scenario_table.c`/`.h` with S0, S1, S3 only, and all
six arms. Arms selected at runtime from one binary via `strength_pct`.
§4.1, §4.3.
*Acceptance:* (a) a test in this suite re-verifies the bit-exact
`strength_pct == 0` contract itself rather than citing it; (b) S0 shows all
arms identical on all four objectives within `BELOW_MATERIALITY`; (c) S3's
results match S1's within materiality, proving the three-node model at
`sensor_bias_p = 0` is not itself a confound; (d) a scenario with a missing
model, or with `pid_fuzzy_derive_bands()` returning false, prints
`SCENARIO_REFUSED` and exits non-zero — **negative-tested**, by temporarily
zeroing the model in the table, observing the refusal, restoring **by hand**,
and forcing a **full rebuild** before re-running.

**WI-5 — `A_STATIC_MATCHED` two-pass instrumentation.**
Record per-tick applied/base gain multipliers during `A_FUZZY50`, take the
ramp-phase mean, replay as constants with inference off. §4.1.
*Acceptance:* on S1 (bench plant, the configuration `1570a65a` measured), the
recorded ramp-phase means land near ×0.87 Kp / ×1.16 Ki / ×0.84 Kd — **not**
the centre-cell ×0.75/×1.25/×0.75 — and the scenario's
`INFERENCE`/`GAIN_ONLY` classification is emitted. Deviating materially from
those reference means on the same plant is a signal the instrumentation is
wrong, and the implementer must investigate rather than re-pin.

**WI-6 — Remaining scenarios S2, S4–S12, and the `[GROUND_TRUTH]` columns.**
§4.2, §5.2.
*Acceptance:* every scenario runs, emits a complete row, and either passes its
`SEP?` pin or fails loudly naming it. First-run pins are set by measurement and
recorded in the table with a one-line reason each.

**WI-7 — Parallel shard runner and the determinism check.**
`run_sim_scenarios.ps1`, `--shard/--of`, index-ordered aggregation. §4.3.
*Acceptance:* aggregated output at `--of 1` and at `--of 4` is
**byte-identical**. The check fails on any difference. Additionally,
negative-test the determinism check itself by temporarily sorting the
aggregate by completion order instead of index, confirming the check goes red,
restoring **by hand**, and rebuilding fully.

**WI-8 — Adaptive arms: 9-firing sequences with carried state.**
`A_PID_AT` and `A_FUZZY_AT`, with a `zones_config` test fake whose state
persists across the sequence, driving real `adaptive_tune_zone_tick()` /
`adaptive_tune_run_end()`. Per-firing rows, never only the endpoint. §4.1.
*Acceptance:* (a) `A_PID_AT` on S7 (`TUNE_HOT`) shows the model/SIMC path
moving gains toward the true plant across the 9 runs — the direction is
asserted, the magnitude only reported; (b) every `A_FUZZY_AT` row carries
`ki_state = KI_WITHHELD` and the report prints the §1.4 explanation; (c) the
band-adaptation trajectory (§6.1) is reported per firing.

**WI-9 — Remove the fuzzy/Ki mutual exclusion. PREREQUISITE for the
combination arm to mean anything. Production code; do this last, and get it
reviewed on its own.**
`e78fbc5b`'s withholding is correct as a safety measure and wrong as an end
state: the combination arm the owner wants cannot adapt Ki at all while it
stands. The real fix is to make the inference operate **in the frame it
observes** — i.e. for `adaptive_tune_ki.c` to write its correction relative to
the *effective* Ki its trace was produced under, rather than relative to the
stored reference. The mechanically honest way to do that, given `a3803057`'s
finding that the correction is a bang-bang classifier output with no factor to
divide back out, is to have the control path **record the effective Ki
actually applied** alongside the trace (a per-dwell mean or the tick-weighted
applied Ki), and have the refinement apply its ±20 % relative to *that*
recorded value before writing back the reference. This removes the compounding
without discarding the correction. *Design it, review it, and negative-test
it; do not just delete the guard.*
*Acceptance:* (a) the existing regression test
`test_ki_diagnosis_withholds_correction_when_zone_is_pid_fuzzy()` is replaced
by one asserting the fuzzy zone's reference Ki converges rather than
ratchets — specifically, that under the same 10-run constant-offset trace the
reference Ki stays within a stated bound instead of reaching 4.2998×; (b) the
plain-PID ratchet test still passes unchanged; (c) negative-tested by breaking
the new logic by hand, observing red, restoring by hand, and forcing a full
rebuild; (d) after it lands, WI-8's `A_FUZZY_AT` rows flip to
`ki_state = KI_ACTIVE` and the suite is re-run.

**WI-10 — Design (do not build) a `strength_pct` adapter, tested in
simulation only.**
§6.2. A cross-firing A/B over `strength_pct` using `firing_compare`'s existing
Bar 1 / Bar 2 machinery, never a within-run inference. Deliver a design
document plus a simulation arm that exercises it across a 9-firing sequence in
S2, S5, S7 and S12.
*Acceptance:* the design states explicitly why it is not circular **without**
reusing the argument `1142c73b` refuted, and names what it would do about
selection bias from excluded non-settling runs. **No firmware change. No
hardware.** If the simulation shows the adapter wandering or failing to
converge, that is a complete and valuable result — record it and stop.

---

## 8. Expected runtime

- **Per firing:** bench scenarios are ~4 h of simulated time at `dt_s = 1.0`,
  ≈ 14 400 steps; kiln-scale scenarios ~10 h, ≈ 36 000 steps. Each step is one
  plant update, one `pid_update()`, one `pid_fuzzy_adjust()` and one
  `firing_score` tick — on the order of a few hundred nanoseconds. **Well
  under 20 ms per firing.**
- **Full suite:** 299 firings ⇒ **under 10 seconds of compute**, single
  process. Sharded four ways, a few seconds.
- **Dominant cost is the MSVC build**, ~10–20 s, consistent with the other
  standalone sim checks.
- **Budget for `check_sim_scenarios.ps1`: under 60 s wall clock**, including
  the build and both the `--of 1` and `--of 4` determinism runs. If an
  implementation exceeds that, something is wrong — say so rather than raising
  the budget.
- For context on why this budget is safe: `sim_iter_tune` already runs 220
  Monte-Carlo trials inside a host check. This suite is smaller, and unlike
  that one it runs **no ensemble at all**, because the plant is deterministic
  and repetition buys nothing.

---

## 9. What a reviewer should check first

1. Is the default path bit-identical? (`sim_iter_tune` at 24/21/615.)
2. Does `A_STATIC_MATCHED` use the **measured ramp-phase mean**, not the
   centre-cell maximum? This is the single error most likely to recur.
3. Is `--of 1` byte-identical to `--of 4`?
4. Does every `A_FUZZY_AT` row carry `KI_WITHHELD` until WI-9 lands, and does
   the report explain it?
5. Is every introduced constant marked as a test fixture, and is `f_rad`
   marked `[ASSUMED]` at every point of use?
6. Does any conclusion anywhere claim the simulation shows fuzzy helps a real
   kiln? It must not (§0.1).
