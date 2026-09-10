# §6.5 credibility gate — first run, 2026-09-09

Evidence record for `docs/ITER_TUNE_REDESIGN_PLAN.md` sec 6.5. Harness:
`firmware/KilnFW/App/test/sim_credibility_gate.c`, a new standalone
data-generating harness (same convention as `sim_iter_tune.c` /
`sim_wide_temp_sweep.c` — not wired into `build_host_tests.ps1` or
`run_all_checks.ps1`, built and run by hand). No hardware was touched and
nothing was flashed.

## What the gate does, and why it is not circular

The harness replays two **different, real** captures from
`logs/coupling/*.jsonl` (gitignored, local-only) against **one fixed
model**: the same checked-in G1 parameters `sim_iter_tune.c` and
`sim_wide_temp_sweep.c` already use
(`tools/PcTools/config_presets/tuned_baseline_20260831.json`,
`coupling_matrix_20260831.json`, commit `78f2134`). Nothing in this harness
adjusts a model constant to make either capture match better — the only
per-capture input is the recorded PID **duty command** and **setpoint**
sequence; the recorded temperatures are used exclusively as the answer key.
This is a stronger non-circularity posture than the plan's minimum ask (fit
coupling on one capture, gate on a different one): here the model sees
neither capture's own temperature trace before being scored against it.

Replay mechanism: the capture's "duty" field is the PID's raw demand,
sampled every ~5.2 s — not what the relay physically did. The harness
zero-order-holds that recorded duty between capture ticks and steps it
through the real `heater_output.c` (G2, the 60 s PWM window) at a 1 s
substep, then `sim_relay_lag_step()` (G3, actuation lag, unmeasured 0.5 s
placeholder — same assumption `sim_iter_tune.c` uses), then
`sim_kiln_step()` (G1 plant + coupling), then
`sim_max31856_quantize_tc()` (G4). No `pid.c` and no
`zone_coupling_solve.c` are linked — this gate replays a control decision,
it never recomputes one.

Two captures used: `logs/coupling/noise_floor_p7_run1.jsonl`
(CALIBRATION — the name means "looked at first", not "fit to") and
`logs/coupling/noise_floor_p7d_run1.jsonl` (HOLD-OUT — a separate firing of
the same profile ~3.6 hours later the same day, a different rested start).
Both are among the six repeats behind `noise_floor.json`: real, independent
firings, not the same data twice.

## Pass criterion and why (plan sec 6.5, taken verbatim, not invented here)

- ramp-phase MAE (sim vs recorded) ≤ 3 °C per zone
- dwell steady-state offset within 1.5 °C per zone
- dwell-entry overshoot peak within 2 °C per zone per segment
- calibration-vs-hold-out spread on dwell-entry peak within 2× the
  corresponding `noise_floor.json` entry (model *optimistic* — smaller
  spread than the real repeat-to-repeat noise — is tolerated per sec 3.1;
  *pessimistic* beyond 2× is a fail, because it means the model has a noise
  source the plant does not)

These numbers are the plan's own, not re-derived here, and this run does
not attempt to justify them further than the plan already does: the owner's
standing rule that sub-0.5 °C differences are not worth chasing sets a
floor under all of them (3 °C, 1.5 °C and 2 °C are all comfortably above
it), and the repo's own recorded finding
(`project_iae_noise_floor_unknown.md`) that the true hardware noise floor is
NOT reliably known — the only candidate pair so far differed by 4.8 °C at
the very start of a run, which is a single anecdote, not a measured floor —
is exactly why this gate does not attempt to tighten the plan's numbers
based on that one data point. No bar in this report was invented or tuned
to make the result come out a particular way.

## Result: **GATE FAILS**

Both the calibration and hold-out capture fail every ramp-MAE and
dwell-offset check, on all three zones, by a wide margin (ramp MAE 8.5–
11.2 °C against a 3 °C bar; dwell offset −15 to −18 °C against ±1.5 °C).
Dwell-entry peak comparisons are degenerate (sim shows 0 °C overshoot
everywhere) because the simulated trajectory never gets close enough to the
target to overshoot it at all. The noise-floor spread check is vacuously
"optimistic" for the same reason — the model's calibration and hold-out
runs are numerically identical to several decimal places because the
duty/setpoint sequences it was actually driven with are nearly identical
between the two captures (same profile, same rested-start recipe), so this
sub-check is not informative on its own; the honest reading is that the
ramp/dwell bars above already fail decisively.

**Per plan sec 6.5: this stops here. `sim_iter_tune.c`'s and
`sim_wide_temp_sweep.c`'s existing results (docs/audits/
iter_tune_redesign_sim_2026-09-09.md, sim_wide_temperature_2026-09-08.md)
remain internal-consistency checks of the model against itself, not
evidence about the real kiln, until this gate closes.**

## Diagnosed cause — not a coding defect, a structural modeling gap

The harness prints each zone's own-duty-only ceiling under the literal
sec 6.2 mapping (`h = loss_coeff_w_per_c = 1.0` ⇒ steady-state ΔT at
duty = 1 forever, no coupling, equals `model_k_dc`):

| zone | ambient (this capture) | model_k_dc | ceiling |
|---|---|---|---|
| z0 | 28.6 °C | 31.96 | 60.5 °C |
| z1 | 28.6 °C | 23.48 | 52.1 °C |
| z2 | 28.6 °C | 21.74 | 50.3 °C |

The captured profile (`pv08311918`) dwells at 60 °C, at or above every
zone's own-duty ceiling. In reality the board reaches 60 °C on all three
zones because of (a) cross-zone coupling — the measured matrix shows
14.30/8.33 W conductance from z1/z2 into z0, etc. — supplying real heat the
own-duty ceiling doesn't count, and (b) the recorded duty commands the real
board used were computed by the real closed-loop controller, which
compensates for whatever the real plant actually does.

This harness deliberately does **not** replay through `pid.c`
(`sim_iter_tune.c` already exercises that path for a different purpose —
scoring the *controller*, not the plant), so it feeds the *same recorded
duty* through the *simulated* plant. The simulated final temperatures
(≈36–41 °C at the end of the calibration capture, against a recorded
≈60 °C) are well below even the own-duty ceiling table above, which means
the shortfall is not fully explained by the ceiling alone — the simulated
coupling contribution is also too weak. This is the exact gap plan sec 6.2
already flagged and left open: "the algebraic first cut... is only a
starting point" and requires an iterative numerical match (drive a
single-zone step test *inside the simulator*, compare cross-gains, adjust)
to bring the fitted matrix's cross-gains within 10% of the measured ones.
That iterative fit was **not performed** for this gate run — the harness
uses `sim_kiln_coupling_from_cross_gain()`'s one-shot algebraic estimate
only, which the header comment for that function already says is
insufficient on its own.

## What this means for iter_tune results so far

`sim_iter_tune.c`'s multi-start and Monte-Carlo results
(`iter_tune_redesign_sim_2026-09-09.md`) stay confined to 20–36 °C, well
inside every zone's own-duty ceiling and far from where this gate's failure
shows up — so this specific failure mode does not retroactively invalidate
that narrower-range evidence. It does mean nothing in this codebase yet
supports extending confidence in the decision *algorithm's* Monte-Carlo
results to the temperature range real firings actually use (up to and past
60 °C), and that the wide-temperature sweep's own stated caveat ("nothing
here is evidence about behaviour at cone temperature") is, if anything,
optimistic — the model does not yet reproduce a real firing even at 60 °C,
let alone cone temperature.

## What would close this gate

1. Perform the sec 6.2 iterative coupling fit (single-zone step tests
   inside `sim_kiln`, adjust `coupling_w_per_c` until simulated cross-gains
   match the measured `[0,27.32,21.72]/[14.30,0,22.15]/[8.33,12.42,0]` to
   within 10%, as the plan specifies) instead of the one-shot algebraic
   estimate used here.
2. Re-examine whether `h = 1.0` is the right free-scale choice, or whether
   the FOPDT gain `model_k_dc` should be interpreted differently (e.g. as a
   gain per unit *duty step* from a partial-duty step test, not as an
   absolute achievable ΔT at duty = 1) — the current literal mapping puts a
   ~50–60 °C hard ceiling on a kiln that is fired well past 60 °C in every
   capture examined, which on its face looks too low for a bench kiln
   design intended to reach cone temperature.
3. Re-run this exact gate (same two captures, same bars) after 1–2 land,
   and only then treat a PASS as license to extend confidence in any
   `sim_iter_tune.c` / `sim_wide_temp_sweep.c` result beyond the low range
   they already state.

## Reproduce

```
firmware\KilnFW\App\test\build\sim_credibility_gate.exe ^
  logs\coupling\noise_floor_p7_run1.jsonl ^
  logs\coupling\noise_floor_p7d_run1.jsonl ^
  tools\PcTools\config_presets\noise_floor.json
```

Exit 0 = pass, 1 = fail (this run), 2 = usage/parse error, 3 = SKIP (a
named input file is missing — verified by pointing the tool at a
nonexistent capture path, which reports SKIP and exits 3 rather than
silently passing).
