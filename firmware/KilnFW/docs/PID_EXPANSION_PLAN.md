# PID Expansion Plan — control-algorithm work for KilnFW

Status doc for the per-zone control algorithms on the ESP32-S3 side. Rewritten
2026-09-01 after the bulk of it shipped; the previous 1364-line version, with
the full ten-paper literature review and the phase-by-phase checklists of
completed work, is in git history immediately before this commit if the
reasoning behind a landed decision is ever needed.

Conventions this doc follows: **the code is truth, not the checkboxes** — the
old version was stale in both directions, marking built work as pending and
pending work as built. Anything claimed done below names the commit. Anything
measured names the run.

---

## 1. Where this stands

Per-zone control mode (off / PID / PID+feedforward / fuzzy-PID), step and relay
autotuning, SIMC / ZN / Tyreus-Luyben / Cohen-Coon rules with machine-readable
refusals, coupled cross-zone feedforward, per-zone quality statistics, adaptive
tuning from ordinary firings, and the supporting HTTP/UI/telemetry surfaces are
all in the tree and tested.

What follows is only what is **not** done, plus the results and dead ends that
constrain it.

---

## 2. Measured results

Profile 7, three zones, ramp to 45 °C, dwell, ramp to 60 °C, dwell. Normalized
IAE (time-weighted mean absolute error, °C), whole run:

| | z0 | z1 | z2 |
|---|---|---|---|
| baseline, 2026-09-01 | 1.630 | 1.291 | 1.425 |
| after the day's work | **0.878** | **0.540** | **0.695** |
| RMS | 2.17 → 1.06 | 1.71 → 0.69 | 1.78 → 0.84 |
| max overshoot, °C | 5.24 → 1.69 | 4.48 → 1.50 | 4.67 → 1.84 |
| mean error, °C | +1.10 → −0.30 | +0.70 → +0.02 | +1.00 → +0.40 |

Roughly half the tracking error and a third of the overshoot, with the mean
centred on all three zones. The consistent hot bias that prompted this is gone. The original
complaint — a 70 °C target reaching 80.1 °C — traced to five defects stacked on
each other, each hidden by the one above it: a settle detector firing
mid-transient, a fit reading its last sample as the asymptote, guard 1 blocking
honest step tests, PWM chopping disarming four guards at partial duty, and an
uncoupled climb term over-driving zone 0 roughly tenfold.

Identified plant (bench rig, 0–80 °C): z0 K=39.25 τ=263.8 L=52.8; z1 K=31.97
τ=269.8 L=43.5; z2 K=31.68 τ=270.9 L=33.9. Coupling matrix in wire form
`[stepped][affected]`: z0 39.25/15.78/9.70, z1 26.61/31.97/11.38, z2
20.73/21.09/31.68. RGA diagonal 1.53/1.69/1.34. Off-diagonal τ is 620–730 s
against 264 s on the diagonal, dead time 135–158 s against 34–53 s — cross-zone
heat arrives far later than a zone's own element, which is the root of the
remaining ramp-onset error.

**Orientation, which has been swapped by mistake more than once:** persistent
storage is `coupling_coeff[affected][stepped]`; `/api/autotune/matrix` reports
the transpose.

---

## 3. Remaining work

### 3.1 Dwell-entry overshoot — CLOSED 2026-09-01

Was the largest remaining error: 2.0–2.7 °C on entering every dwell, peaking
70–110 s after the ramp ended. Now 0.4–1.8 °C.

Diagnosis that led to the fix: duty does **not** crash to zero at the boundary
(the analyzer's duty-off-to-peak lag is `n/a` for 5 of 6 transitions), peak
timing tracks each zone's own dead time at 1.5–2×, and zone 2's second-dwell peak
lines up with the cross-zone dead time while its neighbours are still driving.
So the plant was arriving at setpoint with stored rate, not being over-driven
after arrival — which is why reducing rate before the boundary worked and
holding duty through it (§4) did not.

- [x] **Terminal ease-off** (`48940a5`) — taper the commanded rate as the target
      is approached so the plant arrives with less stored rate. Linear taper over
      a window of 2× each zone's own identified dead time, applied to the
      feedforward's rate input only, never to the setpoint schedule, so a stall
      is structurally impossible and the worst case is ordinary tracking lag.

      **Measured on hardware, improvement on every zone and every metric:**

      | | z0 | z1 | z2 |
      |---|---|---|---|
      | dwell-entry overshoot, seg0 | 0.59 → 0.41 | 1.99 → 0.88 | 2.67 → 1.80 |
      | dwell-entry overshoot, seg1 | 2.02 → 1.50 | 2.04 → 1.50 | 2.37 → 1.84 |
      | seg1 ramp RMS | 1.55 → 0.84 | 1.23 → 0.57 | 1.03 → 0.63 |
      | whole-run normalized IAE | 1.034 → 0.878 | 0.723 → 0.540 | 0.821 → 0.695 |

      The ramps improved as well as the dwells, which was not the goal: arriving
      with less stored rate leaves less to correct on both sides of the boundary.

      **This change was simulated, rejected, and reported as a dead end before
      the simulator's ramp-rate bug was found (§3.4).** It is the best single
      result of the session and it was nearly discarded on a bad model. When a
      simulation rejects a physically sound idea, check the simulation.

### 3.2 Zone 2's model over-predicts its hold duty

The coupled solve wants 0.861 duty where the kiln actually needs under 0.74.
Masked by integral action rather than corrected. Not diagnosed.

### 3.3 Adaptive tuning — the layers not built

Shipped (`fcc1fc0`, `a772d78`): dwell harvesting, diagonal-only least-squares
gain refinement, bounded application at run end, per-zone opt-in default off,
HTTP endpoint and zones-page UI.

- [ ] **Full coupled identification** from dwell observations — solve
      `A·u = (T − ambient)` across all zones rather than per-zone diagonal.
      The observations already record every zone's duty and temperature at each
      settled dwell, so the data is there; only the solver is missing.
- [ ] **Integral diagnosis from dwells** — residual offset, drift and limit
      cycling each imply a specific Ki correction, and a detected limit cycle
      yields Ku/Tu without a dedicated relay test.
- [ ] **Dynamics from ramps** — re-fit τ and dead time only from segments with
      genuine excitation, scoring each candidate and **refusing** when too flat
      rather than fitting noise.
- [ ] **Iterative tuning** — treat each firing as one experiment scored by the
      normalized IAE already recorded per zone, perturb gains slightly, keep the
      change only if the next run scores better. This is the mechanism that
      actually delivers "gets better every firing"; it needs no informative data
      in the identification sense and is much harder to fool.
- [ ] **One-click revert** to the last accepted gain set.
- [ ] Consolidate the opt-in flag into the zone config blob. It currently lives
      in adaptive_tune's own NVS namespace (`adap_tune`) because `zones_http.c`
      was held by another agent when it was written.

### 3.4 The simulator — calibrated 2026-09-01, and what it is worth

For most of this session the simulator was wrong in both directions: it
predicted 44–53% ramp recovery where hardware delivered essentially full
recovery, and it favoured the climb-decay change hardware then measured as worse
on every zone. It showed 5–7 °C ramp error for a controller tracking to
~0.03 °C.

**The whole cause was one constant: it drove itself at a hardcoded 10 °C/min
while profile 7 ramps at 2–3.5 °C/min.** Coupled climb feedforward is linear in
commanded rate, so a 3–5× rate error produced the entire phantom lag — which is
why retuning its gains never moved it (a gain sweep left the error pinned
regardless of kp/ki/kd). Tick rate, PID form, anti-windup, the integral floor and
the coupled solve were all checked and all matched the firmware.

Calibrated against all five captures, aggregate residual is **1.45 °C RMS across
60 windows**, against hardware's own 0.4–2.8 °C run-to-run noise. Now lives in
the repo as `tools/PcTools/src/kilnctrl/plant_sim.py` (`7b56a8a`) with the
captures as checked-in fixtures, a CLI, an MCP tool, and a regression test that
fails if the ramp rate is ever hardcoded again.

**What it can be trusted for**, per its own module docstring: ramp magnitude and
sign on coupled-feedforward builds; dwell behaviour generally. **Not** the
uncoupled baseline's exact saturation dynamics, not zone 2's second dwell in
isolation (it runs 1.8–2.6 °C cold on every coupled build, reproducing §3.2's
open defect rather than a sim error — a point in its favour), and nothing below
the ~1 °C noise floor.

The lesson worth keeping: every sim verdict in this chain was quoted with
confidence while resting on an unvalidated driving condition. A simulator is not
evidence until it reproduces a measurement someone actually took.

### 3.5 Documentation

- [ ] `docs/PID_CONTROL.md`: a "Fuzzy adjustment" section matching the existing
      "Feedforward" one — formula, when it is off, and bench measurements.
- [ ] `docs/PID_CONTROL.md`: tuning-rule comparison table. Deliberately deferred
      until bench data exists; do not write it from the literature alone.

### 3.6 Validation gap

Everything above is measured on a bench rig spanning 0–80 °C. Radiative transfer
goes as `T⁴`, so the plant at kiln temperatures is not the plant identified here.
Every result in §2 validates the **mechanism**, not the behaviour at firing
temperature. This stays open until a real firing.

---

## 4. Rejected approaches — do not re-propose without new evidence

Three control-theory ideas were designed and rejected or reverted on 2026-09-01.
Each was plausible; recording why they failed is cheaper than rediscovering it.

**Coupling lead compensation (three design rounds, never implemented).** The
off-diagonal delay is real, but every bounded design either reduced at t=0 to the
diagonal-alone solve — bit-for-bit the uncoupled climb formula measured that same
day as a 10× over-drive on zone 0 and 5 °C overshoot — or, once the correction
was sized honestly against a 1.5 °C budget through the coupling matrix, was worth
under 1 °C-equivalent and inert whenever the integral sat on its floor.
The lagged sequential form is Jacobi iteration on `A·u = b` with spectral radius
**0.984** on this matrix: it converges in about 11 hours, and a 2% error in the
off-diagonals — which are two-point fits on unexcited peer traces — pushes it
past 1 into divergence.

**Integral floor at `−ff_u` (`b7289db`, superseded by `e690f8a`).** Flooring the
integral at the *total* feedforward means a bound integral exactly cancels the
whole feedforward, so commanded duty runs on P+D alone — and during a constant
ramp, once bound it does not release until the ramp ends. The floor must be
`−ff_hold`, the steady-state component only. Because the commanded rate is
exactly zero during a dwell, the climb term is exactly zero there and the dwell
behaviour is byte-identical to the old floor; only the ramp changes. Any future
change of this shape must check what the bound value is made of.

**Decaying the climb term into the dwell (`b8b192d`+`a77db88`, reverted
`f9d8445`).** Intended to cover the plant's dead time instead of stepping the
climb term to zero at the boundary. Measured worse on every zone in both dwells,
and on the second dwell drove an oscillation that tripped thermal guard 2
("heating commanded but temperature falling") and aborted the firing. Holding
*more* climb duty into a dwell adds heat exactly where the plant already has too
much. The guard trip was correct.

Two process notes from those three. A supporting simulation showing a gain on
one zone of three is a weak signal, not a green light. And the simulations in
this chain have been unreliable in **both** directions — one predicted 44–53%
ramp recovery where hardware delivered essentially full recovery; another
favoured a change that made hardware worse. Treat their mechanism comparisons as
informative and their magnitudes as not.

---

## 5. Rules that keep being relearned here

- **A dwell is a DC-gain measurement.** Steady duty against steady rise over
  ambient, with no asymptote to extrapolate and no settle detector to get wrong.
  Every bug in the overshoot chain existed because a step test must guess where
  the temperature would eventually land; a dwell is sitting at the answer.
  Different phases of a firing identify different parameters, and each must only
  be used for what it can support — fitting τ from a dwell is fitting noise.
- **Autotune needs a rested baseline.** Residual heat biases the fitted gain low;
  check every zone is at ambient, not just the one under test.
- **Never test a control change at rate = 0 or error = 0.** Neither can
  distinguish a climb-related change from a hold-related one. Two vacuous tests
  shipped in this area for exactly that reason.
- **Recommend rather than auto-apply, by default.** Silently rewriting the gains
  of a kiln that fires unattended, on evidence from a run nobody reviewed, is how
  a bad firing happens that nobody can explain afterwards. Auto-apply is opt-in,
  bounded per run, recorded, and revertible.
