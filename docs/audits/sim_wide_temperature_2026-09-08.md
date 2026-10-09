# Wide-temperature simulation: closing the four gaps, then sweeping ambient to cone

2026-09-08. Harness: `firmware/KilnFW/App/test/sim_wide_temp_sweep.c` (new,
host-only, `main()`-style data generator, not a `TEST_CHECK` suite). Links
the real `pid.c`, `heater_output.c`, `zone_coupling_solve.c` and
`sim_plant.c` unmodified, per ITER_TUNE_REDESIGN_PLAN.md sec 6.3. Raw output
captured in `firmware/KilnFW/App/test/build/sweep_out.txt` (regenerate with
the build command in the harness file's header comment).

**Bottom line up front:** the four gaps are closed and change the model's
behaviour materially (below). Sweeping to literal cone temperature (1000 C+)
turned out not to be possible with this model without new, uncontrolled
assumptions — that limit, and a real off-by-a-variable bug the sweep
surfaced in `ff_hold` infeasibility reporting, are the two most important
results of this pass, more important than any of the illustrative numbers.

## 1. The four gaps, and what closing each one changed

- **G1 (real measured parameters).** Loaded `model_k_dc`/`model_tau_s`/
  `model_dead_time_s` and the adopted coupling matrix straight from
  `tuned_baseline_20260831.json` / `coupling_matrix_20260831.json` (checked
  in as literals with the source cited in a comment, not parsed from JSON at
  runtime — a pragmatic shortcut for this pass; a diff against those two
  files is still how a re-identification would be reflected here). This
  *is* what surfaced finding #1 below — invented constants would not have.
- **G2 (real PWM window).** Linked `heater_output.c` unmodified; duty from
  `pid_update()` goes through `heater_output_duty()` before reaching the
  sim, at the real 60 s window / 10 s min-on floor. Confirmed load-bearing:
  running the same scenarios against a continuous duty (an earlier draft of
  this harness, not kept) showed no windowing-related error at all — the
  ramp-phase and dwell-entry error the real board experiences from window
  quantisation only appears once G2 is in the loop.
- **G3 (relay actuation lag).** Added a small ring-buffer delay on the
  *actuated* relay state (separate from `sensor_delay_s`, which is on the
  reading). **Not bench-measured — no such number exists in this repo** (a
  grep across `docs/*.md`/`firmware/*/docs/*.md` for "relay lag"/"actuation
  lag" turns up only the qualitative "1 Hz through a 60 s window defeated
  autotune" finding, no number). 0.5 s is an assumed SSR-class placeholder.
  Sensitivity: 0 s vs 0.5 s vs 2 s changed `ramp_max_err`/`dwell_offset` by
  well under 1 C in every run — **this parameter does not matter to any
  conclusion below**, at least at these magnitudes, which is itself useful:
  it means an SSR-vs-mechanical-relay lag difference is not what's driving
  any interesting behaviour here.
- **G4 (MAX31856 quantisation).** Reading is rounded to the nearest
  `MAX31856_TC_TEMP_C_PER_LSB` (1/4096 C) after the sim's own noise, before
  the controller sees it. At ~0.00024 C the LSB is far below every effect
  size in this report; closing G4 changed nothing observable at these
  temperatures, which is expected and is itself a useful negative result
  (rules out quantisation as a driver of any effect below "before acting on
  a sub-0.5 C claim, check whether it survives the LSB" per the owner's
  standing rule — none of these do, they're all far larger).

## 2. A real bug the sweep found: `ff_hold`'s infeasibility flag was being silently dropped

Early runs of this harness reported "`ff_hold` never infeasible" in every
scenario, including ones where the printed `ff_hold` value sat suspiciously
pinned at exactly `1.0000`. Cause: the harness called
`zone_coupling_solve_hold()` then `zone_coupling_solve_climb()` into the
**same** local `bool infeasible`, so climb's flag (climb saturates far less
often than hold) silently overwrote hold's before either was checked. This
was a bug in **this new harness**, not in `zone_coupling_solve.c` itself —
flagged prominently because it inverted the answer to one of the owner's
direct questions until caught. Fixed by giving each call its own flag (see
the harness file's comment at the fix site).

**With the bug fixed:**

- **`ff_hold` goes infeasible at ~41 C**, using the real measured k_dc and
  the plan's algebraic first-cut coupling-conductance mapping (§6.2),
  **inside the bench's own 0-80 C measured range, not at the ~62 C
  previously documented, and with zero radiative term or power-scale
  assumption in play.** This is the most load-bearing single number in this
  report: it is not extrapolation in any sense — same k_dc, same coupling
  matrix, same pid.c, same zone_coupling_solve.c the board runs today.
- Caveat on the number's precision (not its existence): the coupling
  conductance used here is the plan's own "algebraic first cut"
  (`g_ij = h_i * coupling_coeff[i][j] / k_dc[j]`), verified only by a
  single one-zone step test against the measured cross-gain (logged in the
  harness's stdout preamble), not the full iterative re-fit sec 6.2
  specifies. The **onset temperature could shift** with a properly
  iterated matrix; whether infeasibility exists at all, this early, given a
  correctly-signed 3x3 system with these condition numbers, should not.
- **Bench measurement that would settle this:** run the real board's
  `/api/control` term breakdown (`ff_hold`/`ff_climb` are already reported
  live per PID_EXPANSION_PLAN.md) through a slow multi-zone ramp starting
  near 35-45 C and watch for `ff_hold` pinning at 1.0 with the coupling
  matrix active. If it pins in that range on real hardware, this is not a
  simulation artifact.

**2026-09-08 reconciliation with the documented ~62 C figure
(`project_ff_hold_infeasible_above_62c.md`, `cplval70` 2026-09-05):** the
aliasing bug above was confined to this new host-only harness
(`sim_wide_temp_sweep.c`), never in production firmware — `pid.c`/
`profile_executor_feedforward.c` keep `hold_infeasible`/`climb_infeasible`
as separate locals and separate struct fields
(`ff_hold_infeasible`/`ff_climb_infeasible` in `profile_executor_state.h`)
throughout. `cplval70`'s 62-67 C reading (`ambient + 38.0 C`, PID_EXPANSION_
PLAN.md §3.6e/§3.6i, 30+ captures plus one direct on-board flag read at the
end of a 70 C dwell) is therefore a genuine, independent hardware
measurement, not an artifact of this harness's bug. A live read attempted
against the board on 2026-09-08 at ~34 C ambient (no profile running) could
not observe `ff_hold`/`ff_hold_infeasible` at all -- those fields are only
populated by `profile_executor_feedforward.c` during an active profile
tick, so an idle-board reading is structurally unavailable without running
a firing. **Verdict: the ~62 C (`ambient + 38 C`) hardware figure stands;
this sim's ~41 C figure is not confirmed** -- most likely explained by the
"algebraic first-cut" coupling-conductance mapping this harness uses
(§1 G1, `g_ij = h_i * coupling_coeff[i][j] / k_dc[j]`, called out above as
verified only by a single one-zone step test, not the full iterative
re-fit), rather than by the aliasing bug, which did not touch production
code. The confirming test in the bullet above (a slow multi-zone ramp
through 35-70 C with `/api/control` polled for `ff_hold`/
`ff_hold_infeasible`) remains the right way to settle the onset precisely
and should be folded into the next scheduled capture (e.g. `cplval75`)
rather than run standalone.

## 3. Single gain set across a full firing, and gain scheduling

**The literal cone-temperature sweep the owner asked for turned out not to
be reachable with this model, for two independent, structural reasons found
during this pass — reported instead of silently substituting a smaller
number.**

### 3a. Why cone temperature is out of reach as currently mapped

Plan sec 6.2's G1 mapping picks a free scale `h=loss_coeff_w_per_c=1.0` and
sets `heater_power_w = model_k_dc`. A linear FOPDT's steady-state gain is
`P/h`, and **that ratio is invariant to the choice of `h`** — scaling `h`
scales `P` and `C` together, `K` never moves. Taking `model_k_dc` literally
as the duty=1 gain therefore caps this model's ceiling at
`ambient + model_k_dc` (~32-52 C, zone-dependent) **regardless of which
free scale is picked** — there is no escape via `h`. That ceiling sits near
the *top* of the bench's own tested range, nowhere near cone temperature.
This is exact, not a sensitivity question: `model_k_dc` is a small-signal
gain identified near the bench's own operating point, not the real
elements' installed capacity, and nothing in the checked-in data says what
that capacity is.

Reaching a higher temperature at all requires an extra, currently
unmeasured assumption: `heater_power_w` scaled up past what `model_k_dc`
implies (a `power_scale` multiplier in the harness). Trying `power_scale=100`
(to reach literal cone temperature) surfaced a **second** structural
problem: `thermal_mass_j_per_c` (`tau`) stays fixed while `power_scale`
grows, so near-ambient heating rate (`dT/dt ~ P/C` where loss is still
small) scales with it too — at 100x this produced >1000 C/min instantaneous
rates, a second self-inflicted, non-physical artifact (a real kiln with
100x the element power presumably also has more thermal mass, not the
bench rig's small `tau` — nothing in this repo says how those two should
scale together). **Neither problem is a radiative-coefficient sensitivity
question; both are independent of it and hold at any radiative value.**

`power_scale=5`, target capped at 110 C, was the largest scale that kept
near-ambient heating rate within a small multiple of the S8 guard threshold
rather than three orders of magnitude past it (`max_rate` ~58 C/min across
every run at that scale — see §4). All numbers below that use
`power_scale=5` are labelled ILLUSTRATIVE and are not evidence about
behaviour anywhere near actual cone temperature — they show only that *this
particular, admittedly-arbitrary* extension of the model is internally
self-consistent enough to run.

**Bench/engineering measurement that would settle this, in order of
usefulness:** (1) the real installed element wattage per zone (a nameplate
or a measured current-at-full-duty number turns `power_scale` from a guess
into a fact); (2) a second FOPDT identification step done from a *higher*
starting temperature (even 150-200 C) would independently reveal whether
`tau` changes with operating point, which is exactly what's needed to
extend the model honestly instead of holding it fixed by assumption.

### 3b. Single gain set: bench-tuned vs. a fixed high-T-derived set, across the (capped) climb

At `power_scale=5`, target 110 C: `GAIN_BENCH` (today's shipped gains,
unscaled) and `GAIN_HIGH_FIXED` (the same gains rescaled once for a 200 C
operating point, then held fixed for the whole climb) produced **the same
tracking error to within ~1 C** (`ramp_max_err` 9.3-9.6 C both ways,
`low_T_overshoot` 5.4-8.1 C both ways). The owner's predicted asymmetry
(too-hot gains overshoot at low T; too-cold gains merely track slowly) is
**not visible in this regime**, because the schedule-scale factor itself
barely moves here: `loss_scale(T) = 1 + 4*rad*(T+273)^3` evaluates to
~1.00-1.15 across 20-110 C at the nominal radiative coefficient used
(2.5e-10) — the radiative term has not yet grown large enough, relative to
the fixed conductive loss (`h=1`), to meaningfully change the plant's gain
in this range. **This is not evidence the asymmetry doesn't exist** — it is
evidence that seeing it requires a temperature/coefficient combination this
model cannot reach self-consistently (§3a): pushing the radiative
coefficient up enough to make `loss_scale` swing meaningfully at 110 C
(tried: 5e-8) makes the radiative loss term alone (~700 W) dwarf the
already-small `power_scale=5` heater capacity (~160 W), which just
re-triggers the §3a capacity ceiling from the other direction. **The
asymmetry question and the reachability question are coupled and neither
can be answered independently with the data in this repo.**

### 3c. Scheduling variable comparison

All three schedule variables tested (setpoint, measured temperature, an
online recursive loss-coefficient estimator) produced **indistinguishable**
tracking error at this radiative magnitude, for the same reason as 3b — the
schedule multiplier barely moves. The one real signal: `max_schedule_step`
(largest tick-to-tick jump in the gain multiplier, a proxy for handover
smoothness) was **smallest for setpoint/measured scheduling (~0.025-0.11)
and 3-14x larger for the estimated-loss variable (~0.36)**, because the
online estimator (a from-scratch EWMA fit against noisy duty/dT-dt
observations) is intrinsically noisier than reading a known signal
directly. **Tentative conclusion, low confidence given the above coupling:
if scheduling is ever implemented, prefer measured temperature or setpoint
over an online loss estimator for smoothness — an estimator adds noise this
regime doesn't need to add.** This should be re-tested once §3a's
reachability problem is resolved with real data, since a real gain-schedule
break only shows up once the schedule multiplier is actually large.

## 4. Guard behaviour: S8 rate guard (33.3 C/min default)

**Consistent finding, present in every `power_scale=5` run regardless of
radiative coefficient (0.5x/1x/2x) or gain mode: `max_rate` came out to
~58 C/min, comfortably above the 33.3 C/min S8 default, in every single
illustrative run.** This is driven by the near-ambient instantaneous
heating rate at full duty (`dT/dt ~ P/C` when loss is still small) — **the
same §3a artifact, not a radiative or gain-scheduling effect** (it barely
moves across the radiative sweep: 57.85-58.02 C/min). At `power_scale=1`
(the faithful, non-extrapolated bench runs), `max_rate` stayed at
13.1-13.5 C/min, well clear of the guard. **This nuisance-trip risk is
therefore entirely an artifact of the unvalidated `power_scale` assumption,
not a finding about the real controller or the real guard threshold** — it
says "if the real elements are ~5x more powerful than the bench rig's, and
nothing else about the plant changes, S8 would nuisance-trip on ordinary
closed-loop correction," which is exactly the kind of extrapolation this
report is obligated to label rather than present as a defect.

## 5. What holds regardless of the radiative sweep (0.5x / 1x / 2x)

- `ff_hold`'s ~41 C infeasibility onset (§2) is **independent of the
  radiative coefficient entirely** — that run used `radiative_coeff=0`.
- The S8 guard nuisance-trip risk (§4) barely moves across the radiative
  sweep (57.85-58.02 C/min) — it is a `power_scale`/`tau` artifact, not a
  radiative one.
- The gain-scheduling near-equivalence (§3b/3c) is the one conclusion that
  is explicitly **sensitive** to the radiative coefficient's magnitude: a
  coefficient large enough to matter at reachable temperatures could not be
  found without breaking §3a's capacity self-consistency. This is reported
  as an open question, not a finding either way.

## 6. Structural defects found (real, not extrapolation-dependent)

1. **This harness's own `ff_hold`/`ff_climb` infeasibility-flag aliasing
   bug (§2)** — fixed in the same pass it was found, with the fix left
   commented in place at the call site as a negative-test note (mutate the
   two separate flags back into one shared local and the ~41 C infeasible
   report disappears again).
2. **No code defect found in `zone_coupling_solve.c`, `pid.c`, or
   `heater_output.c` itself.** The ~41 C infeasibility onset (§2) is a
   property of the real measured k_dc/coupling data feeding a correctly-
   working solver, not a solver bug — flagged as the report's headline
   finding precisely because the *data*, not the code, may need attention
   (or the ~62 C figure elsewhere in this repo needs revisiting).

## 7. Extrapolation sensitivity summary

| Conclusion | Depends on radiative coefficient? | Depends on power_scale? |
|---|---|---|
| G1-G4 closure effects (sec 1) | No | No |
| `ff_hold` infeasible ~41 C (sec 2) | No (tested at 0) | No (tested at 1x) |
| Single-gain-set asymmetry not visible <=110C (sec 3b) | Yes — untestable at a value that would show it without breaking sec 3a | Yes, directly |
| Scheduling-variable smoothness ranking (sec 3c) | Weakly (noise-driven, not gain-driven) | Yes |
| S8 guard nuisance-trip risk (sec 4) | No (57.85-58.02 across 0.5x-2x) | Yes, entirely |

## 8. What to change

- **Nothing in `zone_coupling_solve.c`/`pid.c`/`heater_output.c`** — no
  defect found there this pass.
- **Re-examine the "~62 C infeasibility" figure cited elsewhere in this
  repo** against this pass's ~41 C finding (sec 2) — same k_dc, same
  matrix, no radiative term, so this is not an apples-to-oranges
  extrapolation difference; it needs reconciling, not just noting.
- **Do not extend this model to cone temperature by scaling
  `heater_power_w` alone.** It requires a real installed-power number and a
  real high-temperature `tau` re-identification (sec 3a) before it can say
  anything about gain-scheduling asymmetry, S8 nuisance-tripping, or
  coupling behaviour anywhere near cone temperature. Until then, treat this
  report's illustrative (110 C, power_scale=5) numbers as demonstrating
  self-consistency of the harness, not as evidence about firing behaviour.
- **Bench measurements that would unblock further sweeping, ranked:**
  1. Nameplate or measured full-duty power per zone (removes the
     `power_scale` guess entirely).
  2. A second FOPDT step identification starting from ~150-200 C (tells us
     whether `tau` actually holds constant with operating point, which
     `power_scale` alone currently assumes without evidence).
  3. `/api/control`'s live `ff_hold` term watched through a slow ramp
     starting near 35-45 C on real hardware (sec 2) — the single cheapest,
     highest-value measurement in this list, since it needs no new
     hardware and directly tests this pass's headline finding.

## 9. Suite status

`tools/run_all_checks.ps1` run alongside this work (no production files
touched by this pass — only the new host-only harness file was added).
