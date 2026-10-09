# Testing candidate 3 (closed-loop replay) for the dwell-entry peak bar, 2026-09-10

`docs/audits/sim_credibility_gate_real_cause_2026-09-10.md` sec 6/8 left three
candidates for the dwell-entry overshoot PEAK bar's residual gap. Candidate 1
(second-order plant) was refuted (`docs/audits/sim_second_order_plant_investigation_2026-09-10.md`).
Candidate 2 (relay lag) was ruled out analytically (0.5s against a 60s PWM
window). This pass builds and runs candidate 3: replay the recorded
**setpoint** through the real controller instead of replaying the recorded
**duty** open-loop.

## 1. Is the mechanism capable of the observed magnitude?

`sim_credibility_gate.c` replays recorded duty open-loop: it never recomputes
a control decision, so any mismatch between the sim's trajectory and the
recorded one is attributable to the plant model alone, given identical
input. Closing the loop lets the real controller react to the SIM's own
(possibly wrong) temperature. Whether this can matter depends on loop gain:
at the recorded operating point the zone gains are Kp 0.032-0.036/C,
Kd 0.65-0.69 (`tuned_baseline_20260831.json`). A few degrees of excess
simulated temperature crossing the setpoint flips the error sign; Kp alone
then commands duty swings of several tenths against zone duties of
0.166-0.615 -- comparable in size to the duties actually being commanded, not
a negligible correction. The mechanism is plausible in principle and worth
building rather than dismissing on paper.

## 2. What was built

`firmware/KilnFW/App/test/sim_credibility_gate_closedloop.c`, a new
standalone host executable (same "own executable, tiny fake accessors"
convention `test_zone_coupling_solve.c` already uses for this module) that
links, unmodified:

- `pid.c` (`pid_update_terms`/`pid_seed_bumpless`)
- `heater_output.c` (G2, the 60s PWM window)
- `zone_coupling_solve.c` (the coupled hold/climb Gaussian-elimination solve)
- `profile_executor_feedforward.c` (`zone_feedforward()`/`zone_taper_climb_rate()`
  -- the terminal ease-off, the actual mechanism under test)

`profile_executor.c` itself (and therefore `pid_family_zone_tick()`) is
deliberately NOT linked: it entangles the per-zone tick with FreeRTOS-stubbed
executor machinery this diagnostic does not need, and every function actually
linked above is a pure, non-static, directly callable entry point. The file
supplies its own `s_exec` global and a minimal `zone_runtime_t` stand-in
(field names/types matched by hand against `profile_executor_internal.h`
during this pass -- a structural risk the file's own top comment states,
since the two are never in the same translation unit and cannot be checked
by the compiler against drift).

Per tick: both zones' `actual_c` are refreshed from the simulated plant
first (matching production's sense-then-control order), then each zone's
feedforward + PID are recomputed from the recorded **setpoint**
(`target_c`/`dwelling`/`segment_index`), producing a fresh duty that drives
`heater_output_duty()` -> `sim_relay_lag_step()` -> `sim_kiln_step()`, same
additive-coupling plant `sim_credibility_gate.c` uses
(`sim_measured_zone_constants.h`, unchanged).

## 3. What this costs

1. **Not non-circular the way the open-loop gate is.** A mismatch can now
   come from the plant model, the controller's gains, or the controller's
   own (separately parameterized) model of the plant -- three confounded
   sources instead of one. Recorded `actual_c` stays the answer key for
   scoring only, never fed into anything upstream of the score -- the one
   property that must never be violated is preserved -- but the mechanism
   itself is no longer plant-only. Every number below is reported as a
   bounded diagnostic, not a gate bar.

2. **Gains provenance could not be established**, and this is the pass's
   main negative finding. The captures record no `kp`/`ki`/`kd`. Two
   candidate sources were checked and both are unreliable for the captures'
   actual timestamp (2026-09-02 19:15):
   - `tuned_baseline_20260831.json` (44h before the capture): `control_mode`
     reads 0 (OFF) for z1/z2 even though both captures ran mode 2
     throughout, and its `model_tau_s` (166.9/129.1/114.8s) is the same
     stale-preset artifact `sim_measured_zone_constants.h`'s own header
     documents as wrong by ~90-100s/zone against the live board -- an
     already-known offender in this repo's stale-preset bug class, not a
     fresh source.
   - `control_get_zones()` (live board, read-only, 2026-09-10): every zone
     reports `control_mode` 3 (PID_FUZZY), not the captures' plain PID
     (mode 2) -- a different control *algorithm* -- and Kp has moved from
     the preset's 0.0318/0.0361/0.0355 to 0.0318/0.0485/0.0631 (z1 +34%,
     z2 +78%).

   Neither endpoint is the captures' true gains and there is no logged
   middle point, so the harness runs **both** as a sensitivity bracket
   (labelled PRESET/LIVE) and no conclusion below depends on picking one.
   `model_k_dc`/`tau_s`/`dead_time_s`/coupling are NOT drawn from either
   uncertain source -- they are the one value already established as
   current (`sim_measured_zone_constants.h`), held fixed across both
   brackets.

3. Fields added to `zone_runtime_t`/`zones_cfg` after the 2026-09-02 capture
   (`approach_rate_cap_c_per_hr`, per-zone `ease_off_window_mult`, the
   coupled-solve diagonal-source flag) are left at their documented
   zero-sentinel defaults -- each introducing commit (`af07e455`,
   `d800a601`, `ef323b11`) states its default reproduces pre-existing
   behaviour bit-for-bit; verified by reading each message, not assumed.

4. Control decisions are recomputed once per capture tick (~5.2s, assumed to
   be the real control period -- the captures carry no other signal to
   derive it from) rather than every 1s substep; the plant/relay-lag ODE is
   still integrated at 1s for numerical accuracy. This loses PWM timing
   resolution within the 60s window, not PWM density.

## 4. Results: open-loop (current HEAD, unscaled) vs. closed-loop (both brackets)

Rebuilt `sim_credibility_gate.c` fresh at HEAD (`da9f3775`) for an
apples-to-apples "before": this is the checked-in additive-model gate with
**no row-scalar correction** (the scalars in the prior audit's sec 5 were a
demonstration, never adopted into the gate).

Open-loop dwell-entry peak (seg2 is unevaluable -- a single capture tick):

| | seg0 diff | seg1 diff |
|---|---|---|
| CAL z0 | 2.057 **FAIL** | 0.251 PASS |
| CAL z1 | 0.010 PASS | 1.254 PASS |
| CAL z2 | 1.840 PASS | 1.520 PASS |
| HOLD z0 | 2.790 **FAIL** | 0.979 PASS |
| HOLD z1 | 0.997 PASS | 0.411 PASS |
| HOLD z2 | 1.285 PASS | 1.500 PASS |

10 of 12 evaluable peak cells already PASS at HEAD without any loop-closing
or gain scalar -- the open-loop gate's peak-bar failure is **not** broad; it
is concentrated on **z0, segment 0** in both captures, exactly the zone the
coordinating pass flagged (smallest row scalar, largest incoming cross-gain).
Open-loop dwell offset fails broadly instead (1 of 6 pass, -1.4 to -4.8C).

Closed-loop, z0 seg0 specifically (the only failing peak cell):

| gains | CAL diff | HOLD diff |
|---|---|---|
| PRESET | 2.659 (worse) | 3.408 (worse) |
| LIVE | 1.362 (**now passes**) | 2.618 (still fails, slightly better) |

Closed-loop, dwell offset (robust across BOTH brackets):

| | open-loop | closed-loop PRESET | closed-loop LIVE |
|---|---|---|---|
| CAL z0/z1/z2 | -2.10/-2.75/-4.82 | +0.02/+0.53/+0.05 | -0.13/-0.05/-0.92 |
| HOLD z0/z1/z2 | -1.41/-1.96/-4.05 | -0.12/+0.41/-0.03 | -0.39/-0.08/-1.04 |

Ramp MAE is comparable between loop configurations (open-loop 1.3-3.4C,
mostly PASS; closed-loop 2.3-3.7C, mostly PASS/borderline) -- neither helped
nor hurt materially.

## 5. Conclusion

**Robust across both gain brackets:** closing the loop nulls almost all of
the open-loop gate's dwell-OFFSET failure (its worst bar, 1/6 -> passing in
5-6/6 across both brackets). This is the expected, mechanistic action of
integral control absorbing steady-state plant-model error, and it demonstrates
that a real part of the open-loop gate's dwell-offset failure is a replay
artifact of not letting the controller correct itself over a long dwell --
not solely evidence the plant model is unusable.

**Not robust, and the actual question this pass was asked:** for the one
peak cell that actually fails at HEAD (z0 seg0, both captures), closing the
loop makes it WORSE under the PRESET gain bracket and only partially better
under the LIVE bracket (passes in calibration, still fails in hold-out).
Candidate 3 does **not** reliably close the dwell-entry peak gap -- the
result flips sign depending on which unverifiable gain set is used, which is
itself evidence this mechanism is not the dominant explanation for that
specific residual (a real effect would not depend on which of two guesses at
the gains is used).

**Candidate 3 is not eliminated as a real, measurable effect** (it clearly
matters for dwell offset), but it does not survive as the explanation for
the dwell-entry peak bar's residual failure. That residual is concentrated
on z0 (both captures, segment 0 -- the ramp-to-first-dwell transition), the
same zone flagged as having the smallest row scalar and the largest
incoming cross-gains in the coordinating forward-gain/coupling pass. The
remaining, still-open explanation is that documented forward-gain/coupling
deficit, not a replay-methodology artifact.

## Method note

No `check_*` or assertion was added by this pass (the new harness is
informational/diagnostic, `sim_credibility_gate_closedloop.c`, not wired into
`build_host_tests.ps1`'s blocking build -- deliberately, since it reports no
pass/fail verdict, only bars for visual comparison, and `build_host_tests.ps1`
was under active edit by another session during this pass). Nothing here
loosens any bar in `sim_credibility_gate.c`; that gate is unmodified.
