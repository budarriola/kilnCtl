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

### 3.2 Zone 2's model over-predicts its hold duty — DIAGNOSED 2026-09-01, not fixed

The coupled solve wants 0.861 duty where the kiln actually needs under 0.74.
Masked by integral action rather than corrected.

**Diagnosis.** Every settled dwell tail (last 150 s of each of 12 dwell
windows across 6 hardware captures — `baseline`/`after`/`ifix`/
`holdfix_clean`/`final`/`track3zone_easeoff`) gives an independent,
extrapolation-free reading of `u_pred = A⁻¹·(T − ambient)` against the
matching `u_actual`, using the §2 coupling matrix `A` in its
`[affected][stepped]` persistence orientation and each dwell's own
first-sample actual_c as the ambient reference (the CJ-based ambient the
firmware actually uses isn't in these poll captures).

Solved as a diagonal-only single-zone gain, all three zones disagree with
their identified `K` in the *same* direction (implied K 1.5–5× identified,
worst on z0) — which looked at first like ambient or fit-method error. But
solving the **full 3×3 system** instead (all three zones' duty and
temperature simultaneously) collapses z1 to near-zero mean error
(`u_pred − u_actual` = **−0.007**, 12 samples) while z0 and z2 stay biased
and diverge in **opposite directions**: z0 **−0.086** (model under-predicts
the duty z0 needs), z2 **+0.108** (model over-predicts — the ticketed
symptom), growing with dwell temperature on z2 (~0.12–0.15 at the 45 °C
dwell, ~0.13–0.21 at 60 °C, 5 of 6 non-baseline captures).

That rules out: the ambient reference (one shared scalar can't flip sign
between zones, and z1 fits `A` with the same scalar); zone 2 physical
degradation/duty-ceiling (z0's row is off by *more*, in the opposite
direction, on the same rig, same day, same matrix); and zone 2's own
diagonal `K` in isolation (the diagonal-only check that seemed to implicate
it is exactly the confound the coupled solve resolves — see
`kilnctrl-plant-sim`/`log_analysis.py` note below).

**Conclusion: it's the coupling matrix, specifically its z0 and z2
rows/off-diagonals, not zone 2's diagonal gain.** Consistent with the
matrix's own documented provenance (§4: off-diagonals are "two-point fits
on unexcited peer traces", condition number ~5.3) and with §3.3's
already-scoped, not-yet-built "full coupled identification from dwell
observations" item — which is the right fix (re-solve `A` from the dwell
data adaptive_tune is already harvesting, or a fresh MIMO bench
identification), not a hand-edit of these matrix entries from this one
offline pass, and not a code change to compensate for what is a
measurement problem. No firmware change made here.

### 3.3 Adaptive tuning — the layers not built

Shipped (`fcc1fc0`, `a772d78`): dwell harvesting, diagonal-only least-squares
gain refinement, bounded application at run end, per-zone opt-in default off,
HTTP endpoint and zones-page UI.

- [x] **Full coupled identification** from dwell observations — built
      (`6f6c8fd`), then hardened over four review rounds (`825bd82`, `e129f7c`,
      `12d709d`, `89ff20b`). Joint-observation ring committed once per dwell,
      normal-equations solve reusing the existing pivot floor as the
      conditioning refusal, off-diagonal only. **Cleared for hardware.**
      Not yet validated against the real dwell observations in §3.2 — the test
      matrix is synthetic, so the solve is proven correct but not yet proven
      *better* than the matrix it would replace.
- [x] **Integral diagnosis from dwells** — built and hardened alongside the
      above. Classifies steady offset, drift and limit cycle; a detected limit
      cycle yields Ku/Tu without a relay test. Per-run move capped at
      `ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE` (20%), cumulative growth capped at
      5x a per-zone baseline that is persisted to NVS and re-latched whenever
      the model layer writes a fresh SIMC Ki, with a symmetric lower floor at
      baseline/5. **Code-level blockers all closed and mutation-proven** across
      six review rounds: the reboot ratchet (baseline in RAM while Ki persisted,
      measured reaching 850.6x after one power cycle), the dead remedy (nothing
      cleared the baseline, so the refusal's "re-autotune" advice was a no-op —
      now cleared by `autotune_engine_accept()`), a vacuous decreasing-direction
      test, and a real board-wide deadlock reached by Accept on the UART path.
      **Never yet run on hardware.** Enable one zone first, not three.
- [x] **Dynamics from ramps** — built (`13dcf49`) and **SHELVED** (`65f6525`).
      The two-point reaction-curve fit cannot work on closed-loop firing data:
      its output reduces analytically to `0.524·K·Δduty/ramp_rate`, a function
      of the commanded ramp rate and nothing else — identical on a plant with
      any τ whatsoever. Verified by porting the fit to Python and reproducing
      both accepted fixture fits (predicted 43 s / 22 s vs fitted 35.6 / 23.5).
      Two joint causes: the response window is capped at 15 samples (153 s at
      real telemetry rates) against a true `L+τ` of 300–320 s, so the response
      is never observed; and the detrend baseline sits inside the plant's own
      dead time, so it removes nothing and leaves the ordinary ramp climb *as*
      the response. Integrating it would have cut the climb feedforward ~9x and
      shrunk the terminal ease-off window ~5x.
      **No threshold change fixes this.** A two-point fit needs a held step
      observed for ≥ 2(L+τ) ≈ 600 s; this kiln never holds duty that long in
      closed loop (exactly what `STEP_NOT_HELD` reports on 21 of 27 segments),
      and duty never saturates (0.04–0.45), so "accept only saturated segments"
      has no data here. The route that would work is a **whole-segment
      output-error / ARX estimator**: simulate a FOPDT driven by the actual duty
      trace and least-squares fit τ/L to the residual. No step, no hold, uses
      all 27 segments, extends to the coupled case. The module stays in-tree,
      unwired, as a parked experiment with the artifact pinned by a test.
- [x] **Iterative tuning** — built as `iter_tune.c/.h` (`fe14ddf`, `17f7ebd`).
      Scores each firing by the `iae_normalized` the executor already records,
      perturbs gains, and keeps the change only if the next *comparable* firing
      beats the prior score. **The blocking unknown is the noise floor**: two
      firings with identical gains do not score identically, and this kiln's
      run-to-run spread has never been measured. The only candidate pair in the
      fixtures (`holdfix_clean` vs `final`, same climb mode and integral floor)
      differs by +22.5/+47.5/+28.6 % — but it is **confounded**: those runs start
      24.6 °C and 29.4 °C respectively, a 4.8 °C difference, which is the
      residual-heat trap this document already records. So that number is an
      overestimate of unknown size and must not be quoted as the floor. The
      accept threshold is a deliberately conservative 20 % pending real data, and
      the comparability window was tightened 5.0 → 2.0 °C because the old value
      would have accepted that very pair.
      **The experiment that would settle it:** N ≥ 5 firings of one profile,
      gains fixed, each from a genuinely rested start with every zone at ambient.
- [x] **One-click revert** to the last accepted gain set (`5b44403`). Snapshots
      Kp/Ki/Kd, K_dc/tau/dead_time and `ki_baseline` before each commit point and
      restores them exactly, `ki_baseline` included — restoring gains while
      leaving a baseline latched from the reverted-away value would recreate the
      stale-reference defect this layer already had. Refuses **board-wide** while
      any firing is running or paused, because coupled feedforward means
      reverting one zone changes another zone's commanded duty.
- [x] Consolidate the opt-in flag into the zone config blob (`5b44403`,
      ZONES_CFG_VERSION 13 → 14). A one-shot migration carries an upgrading
      board's old `adap_tune`/`en_mask` bits into the new home, marked by
      `en_migrated` so a later explicit opt-out is not clobbered by the stale
      byte; a silent reset to default-off would have quietly disabled a layer an
      operator had turned on.

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

### 3.5 Documentation — CLOSED 2026-09-01 (`d382b06`)

`PID_CONTROL.md` now carries the strength-scaling formula for the fuzzy layer,
a terminal ease-off section with its measured before/after, and an honest
"what is actually measured, versus what is not" table.

That table's finding is the useful part: **only SIMC has bench data.**
Cohen-Coon, Ziegler-Nichols and Tyreus-Luyben have never been run on this rig,
and the last two cannot be until relay-feedback identification completes on
hardware, which it never has. The calibrated simulator was deliberately NOT used
to fill those cells — it simulates profile tracking under the coupled PID, not
an autotune identification cycle, so any number it produced would be exactly the
"plausible value nobody measured" the deferral existed to prevent.

Also fixed: the file's top-of-file hardware-status banner still claimed no
thermocouple daughterboard or relay expander had ever been attached, which the
same document's own 2026-08-29/30 bench sections contradict.

### 3.6 Untested control paths

- [~] **Relay-feedback identification has never completed on hardware — cause
      found and fixed (`c84abff`), awaiting one confirming run.**
      `relay_law_tick()` decides the bang-bang branch every tick (1 Hz), but
      actuation went through `heater_output_duty()`, the ordinary PID
      *time-proportioning PWM* renderer, which only re-evaluates at `window_ms`
      boundaries (60 s default). A branch flip landing mid-window did not reach
      the relay for up to 60 s, at random phase — **comparable to or larger than
      this plant's entire 34–53 s dead time**, and different every cycle.
      `pid_autotune_fit_relay()` rejects above 20 % period spread
      (`RELAY_PERIOD_SPREAD_MAX`) or 35 % amplitude spread, so the runs
      completed and the *fit* refused: "cycle periods inconsistent". No guard
      was ever involved — that hypothesis was ruled out from code, since guard
      1/2's progress window resets on every duty chop below `PROGRESS_DUTY_MIN`
      and the default relay drive (0.5 ± 0.35) chops constantly.
      Fixed by `heater_output_duty_relay_step()`, which ends and restarts the
      PWM window immediately on a branch flip; the ordinary duty path is
      untouched, so PID and the STEP method are unaffected. **No guard was
      weakened.**
      **The confirming run:** `autotune_start(zone=<rested>, method="relay",
      relay_d=-1.0, relay_h_c=-1.0, rule="tl")` from a zone genuinely at
      ambient. A completed run with plausible `Ku`/`Tu` (period roughly 4–8×
      dead time) closes this and unblocks Ziegler-Nichols and Tyreus-Luyben
      together; the *same* rejection reason would mean a second contributor
      remains.
- [ ] **The fuzzy layer has never run above `strength_pct = 0`** on hardware.
      Every measurement in §2 is with it effectively off.

### 3.7 Validation gap

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
