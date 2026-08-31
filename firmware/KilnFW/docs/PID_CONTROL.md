# PID Control, Thermal Protection, and Autotune

This document is the map for TODO.md section 6A's design as actually built,
not as originally planned — check the code
(`App/drivers/pid.c`, `App/drivers/thermal_guard.c`, `App/drivers/heater_output.c`,
`App/drivers/pid_autotune.c`, `App/drivers/autotune_engine.c`,
`App/drivers/profile_executor.c`) if this and the code disagree, and fix
whichever one is wrong. TODO.md section 6A remains the authoritative design
doc and change log, with dated "Implemented"/"Not built" notes on every
bullet; this file is the shorter "how does this actually fit together"
companion to it.

**HARDWARE STATUS**: everything below has been built, rebuilt, reflashed via
OpenOCD, and live-verified against the absent-hardware code paths on a real
board — but no thermocouple daughterboard or relay expander has been
attached to the bench unit at any point this was written. Every control/guard
behavior described here is logic-verified, not yet verified against a real
relay driving a real heating element. Host-side testing against a plant
*model* (below) is not a substitute for that.

## Module layout

Four of the five modules are pure C — no FreeRTOS, no ESP-IDF, no logging, no
I/O, state entirely in a caller-owned struct — specifically so they can be
built and unit-tested with a host compiler (`App/test/build_host_tests.ps1`,
MSVC, no ESP-IDF) instead of only ever being exercised on-target:

| Module | Pure? | Owns |
|---|---|---|
| `pid.c` | Yes | One zone's PID math: `pid_state_t` in, `pid_update()` out |
| `thermal_guard.c` | Yes | Guards 1-8's trip/no-trip verdict: `thermal_guard_state_t` in, `thermal_guard_tick()` out (guard 8 added 2026-08-12, and disabled by default — see below) |
| `heater_output.c` | Yes | Duty/bang-bang decision -> actual relay on/off, with min-on/min-off debounce and time-proportioning |
| `pid_autotune.c` | Yes | FOPDT model fit from a step-response trace, relay-feedback `{Ku, Tu}` identification, SIMC/ZN/Tyreus-Luyben tuning-rule math, and the Relative Gain Array (all three added 2026-08-12) |
| `profile_executor.c` | No | The only module that talks to FreeRTOS, `kiln_io`, `relay_authority`, or HTTP. Runs one profile across every zone in its `zone_mask` concurrently (6A.5(a), 2026-08-11), each zone with its own instance of the three pure modules below, sharing one ramp/dwell schedule gated by ramp-lock (6A.5(d)); each PID-mode zone's time-proportioning window is phase-offset via `heater_output_seed_phase()`, and a global `max_simultaneous_relays` cap (0 = unlimited) suppresses excess simultaneous relays with a deferred-credit payback for PID-mode zones (6A.5 load-staggering, both halves, 2026-08-11) |
| `autotune_engine.c` | No | Same FreeRTOS/`kiln_io`/`relay_authority` role as `profile_executor.c`, but for one zone's open-loop step test instead of a firing profile. Mutually exclusive with `profile_executor.c` (checked both directions). While STEPPING, samples every configured zone (not just the one under test) and fits each against the same duty step to fill a row of the cross-zone coupling matrix (6A.5(b), 2026-08-11) |

`App/test/sim_plant.c` is a fifth pure module: a
first-order-lag-plus-transport-delay kiln model used to validate the other
four against known ground truth rather than trusting the math by inspection.
Since 2026-08-12 it carries a `sim_kiln` layer on top of the original
single-zone `sim_plant_step()` API (which is unchanged) — up to
`SIM_KILN_MAX_ZONES` (3) coupled zones, an inter-zone conductance matrix, an
optional radiative loss term, a read-time `sensor_map[]` for modeling swapped
connectors, deterministic sensor noise, and injectable per-zone faults. It is
still test-only by default, but it is no longer *only* a host module: with
`CONFIG_KILNCTL_SIM_PLANT` set, the same file is compiled into the firmware
behind `App/drivers/sim_backend.c` (see below).

## Control modes (per zone, `zone_control_mode_t` in `zones_http.h`)

- **OFF** — never commands heat. Safe default, and the post-fault state.
- **BANGBANG** — relay on below `setpoint - 2°C`, off above `setpoint + 2°C`
  (`PROFILE_EXECUTOR_HYSTERESIS_C`), debounced by `heater_output_bangbang()`'s
  min-on/min-off. No PID math, no tuning required — the fallback if autotune
  hasn't run or a zone's tuning is bad.
- **PID** — `pid_update()`'s `u ∈ [0,1]` rendered onto the relay by
  `heater_output_duty()` as a 60s time-proportioning window (default;
  `HEATER_DEFAULT_WINDOW_MS`/`HEATER_DEFAULT_MIN_ON_MS`/
  `HEATER_DEFAULT_MIN_OFF_MS` in `heater_output.h`, per-zone overridable via
  `zones_config_set_heater_cfg()` — TODO.md 6A.9).

  **Minimum on-time: 10 s (`HEATER_MIN_ON_MS_FLOOR`, owner request
  2026-08-28).** Once the PID path energizes the relay it stays energized for
  at least 10 s. This is a floor, not a default: `heater_output_duty()` raises
  any smaller configured `min_on_ms` to it, so no per-zone edit, HTTP write or
  backup import can schedule a shorter on-pulse. It is enforced in two places,
  because window quantization alone does not cover every case — (1) a window's
  computed on-time below the floor renders as OFF for that whole window (never
  rounded up to a minimum pulse, which is how a relay ends up chattering at low
  demand), and (2) a running hold: once the relay is on, an off decision is
  deferred until 10 s of *continuous* on-time has accumulated, across window
  boundaries if the window is shorter than 10 s.

  The floor never applies to a de-energize: every trip, halt, pause and stop
  goes through `heater_output_force_off()`, which drops the relay on the tick
  it happens and clears the hold accumulator. Delaying a safety shutoff to
  protect contact life would trade a safety property for a wear property, and
  `test_heater_output.c` carries an explicit test for it.

  **Where the floor is enforced (2026-08-28, second pass).** `min_on_ms` is an
  editable per-zone field on the zones web page ("Min on", posted as
  `z%u_minon` to `POST /api/zones`), so the floor cannot live only at the point
  of use — an operator would otherwise type 3000, get an ACK, and watch
  `GET /api/zones` read back a number the kiln is not using. Four gates now,
  and the last of them is the one that must never be removed:

  | Gate | Where | Behavior below 10 s |
  | --- | --- | --- |
  | HTTP submission | `parse_zone_fields()` (`zones_http.c`) | **Refused**, with an error naming the field and the floor. The page shows it. |
  | Any stored/imported blob | `validate_zones_cfg()` | Refused. |
  | Load from NVS or a kiln-config slot | `raise_heater_timing_to_floors()`, called by `decode_zones_blob()` | **Raised** to the floor. |
  | Point of use | `heater_output_duty()` | Raised to the floor + the running hold. Defense in depth. |

  Refusing at the HTTP door and raising on load are deliberately different,
  and the split is what makes both honest. A refusal must only ever fire on a
  number a **human typed**: a board configured before this floor existed
  legally carries the old 2000 ms default, and since the web page saves by
  posting back what it just read, refusing that on load would make an
  unrelated save fail on a value nobody chose. Raising it as it comes off
  flash means `GET /api/zones` never reports a sub-floor value in the first
  place. `0` still means "not configured" everywhere and is left alone — the
  default it selects (`HEATER_DEFAULT_MIN_ON_MS`) *is* the floor.

  All four gates are covered by tests, and each was negative-tested by being
  broken in turn: `test_zones_http.c` for the first three,
  `test_heater_output.c` for the last.

  **The window and the minimum on-time are not independent (2026-08-29).**
  The paragraph above used to end by saying a window shorter than the floor
  "is not treated as a config error and does not clamp the zone dead". That
  was wrong, and it was wrong on this bench for a day.

  Zone 0 carried `heater_window_ms = 2000` against the 10 s floor. Every
  on-time a duty could compute in a 2 s window — `0.4 * 2000 = 800 ms`,
  `0.9 * 2000 = 1800 ms` — is below 10 s, so gate (1) rendered every one of
  them as OFF. The running hold never got a chance to extend anything,
  because the relay never closed in the first place. The zone was configured
  as a PID zone, reported itself as a PID zone through `GET /api/zones`, and
  was incapable of being one. The autotune step method commanded `step_duty
  0.4` and the relay did not close once in the whole run.

  Both numbers were inside their own ranges. Only their *ratio* was wrong,
  which is the class of mistake a per-field range check cannot catch — and
  every check in `zones_http.c` was a per-field range check.

  The rule now enforced is

  ```
  heater_window_ms >= HEATER_MIN_WINDOW_MULTIPLE * max(heater_min_on_ms, HEATER_MIN_ON_MS_FLOOR)
  ```

  with `HEATER_MIN_WINDOW_MULTIPLE = 3` (`heater_output.h`). Equivalently: the
  smallest duty a zone can render is `1 / 3`. Three is a practical floor, not
  a control-theory optimum — it is the point at which a zone can express
  roughly a third, two thirds and full duty, which is enough for a PID loop to
  modulate and for an autotune step to produce a real thermal response. Real
  windows sit well above it: the 60 s default against the 10 s floor is a
  ratio of 6, giving duty steps of about 0.17.

  It is enforced at the same four gates, in the same shapes, for the same
  reasons:

  | Gate | Where | Behavior below 3x the min-on |
  | --- | --- | --- |
  | HTTP submission | `parse_zone_fields()` (`zones_http.c`) | **Refused**, with an error naming `heater_window_ms` and the relationship. |
  | Any stored/imported blob | `validate_zones_cfg()`, and `zones_config_set_heater_cfg()` (backup import's door) | Refused. |
  | Load from NVS or a kiln-config slot | `raise_heater_timing_to_floors()` | **Raised** to `3x` the *already-raised* min-on. Order matters: the bound is computed from the raised value, never the stored sub-floor one. |
  | Point of use | `heater_output_cfg_expressible()` | Reports the pairing unusable. `heater_output_duty()`'s own behaviour is unchanged and now unreachable from any policed path. |

  `heater_window_ms` was already an editable field on the zones page
  ("Window"); it now carries a `min` attribute and a hint explaining the
  relationship, because an operator who can type one half of a pair can
  misconfigure the pair. The bench preset
  (`tools/PcTools/config_presets/bench_fixture.json`) pins both halves
  explicitly rather than leaving either at 0, and `test_config_presets.py`
  asserts the preset satisfies the same rule the firmware enforces — a preset
  that did not would be rejected on apply, which is a worse way to find out.

  **Measured on the bench, 2026-08-29, immediately after the fix.** Relay 1
  under a commanded `step_duty` of 0.40 in a 60 s window, sampled at 1 s:

  | Edge | t | On-time | Period |
  | --- | --- | --- | --- |
  | ON | 12.4 s | | |
  | OFF | 36.4 s | 24.0 s | |
  | ON | 73.3 s | | 60.9 s |
  | OFF | 96.6 s | 23.3 s | |
  | ON | 132.4 s | | 59.1 s |

  Mean duty 0.394 against a commanded 0.40, on a 60.0 s mean period. Before
  the fix the same command produced **zero** relay transitions.

  And the autotune that had never once fit a model completed: `state=done`,
  `model_valid=true` after 390 s and 39 samples, PV 31.9 → 45.0 °C (peak
  45.04 °C, against a 75 °C test-side abort and the 80 °C fixture ceiling).
  Fitted FOPDT `K = 32.95 °C/duty`, `tau = 166.9 s`, `dead time = 36.9 s`;
  SIMC proposal `kp = 0.0343`, `ki = 0.000206`, `kd = 0.633`, predicted max
  ramp 425.7 °C/hr. **The gains were not accepted** — `/api/autotune/accept`
  was deliberately not posted, on the same rule the live-bench regression
  tests hold: a verification run must not retune the bench.

  New tests, each negative-tested: `test_heater_output.c` pins the predicate,
  reproduces the 2000 ms window rendering duty 0.4 as zero relay transitions
  across ten whole windows, and shows a 30 s window rendering that same duty
  as a real 12 s-on / 18 s-off cycle. `test_zones_http.c` covers the three
  config gates including the on-load raise computing its bound from the raised
  min-on.

PID form: positional, derivative-on-measurement (not on-error — a profile's
ramp steps the setpoint every tick, and derivative-on-error would spike on
every step) with a 30s low-pass filter, conditional-integration +
output-clamp anti-windup, output clamped to `[0,1]` (no active cooling),
functional-range blending (`±25°C` — outside that, full on/off, integrator
held), setpoint weighting `b=1.0`, bumpless transfer on resume, and
model-based feedforward when the zone has an identified plant model
(2026-08-12, see below). One temperature band per zone (no gain scheduling —
TODO.md 6A.4).

## Thermal guards (`thermal_guard.c`, TODO.md 6A.3)

Armed every control tick while a zone is RUNNING (profile) or
SETTLING/STEPPING (autotune). Latching: once tripped, stays tripped until
explicitly cleared (`profile_executor_halt()` / autotune finishing or being
aborted) — no auto-recovery when the underlying condition clears.

| # | Trip | Catches |
|---|---|---|
| 1 | `HEATING_FAILED` | Commanded duty ≥0.5 but temperature isn't rising at the configured sanity rate — disconnected/fallen-out TC, dead element |
| 2 | `WRONG_DIRECTION` | Heating commanded at/above setpoint, temperature falling fast — miswired/swapped TC |
| 3 | `RUNAWAY` | Duty commanded 0, temperature still rising — welded relay contact / shorted SSR |
| 4 | `DRIFT` | Settled near setpoint once, then sustained excursion for 10 minutes |
| 5 | `MAX_TEMP` / `MIN_TEMP` | Zone's configured absolute ceiling/floor, no debounce |
| 6 | `SENSOR_INVALID` | 3 consecutive bad reads (SPI failure, NaN, MAX31856 fault bits) |
| 7 | `FROZEN` | Reading identical for 10 minutes while duty > 0 |
| 8 | `CROSS_ZONE` | Built 2026-08-12, **off unless the operator arms it** — a zone whose reading disagrees with the *worst* of its peers by more than `cross_zone_max_delta_c` for a sustained `cross_zone_period_s` (600 s, not configurable). `cross_zone_max_delta_c` is a per-zone field on the zones page (added 2026-08-12) and still has no default: 0 means off, and 0 is what a zone starts at. See below |
| 9 | control-tick liveness | A *second*, independent FreeRTOS task (`watchdog_task_entry()` in `profile_executor.c`) — "a control loop cannot be its own watchdog." No matching second task for `autotune_engine.c` yet |

Escalation policy (TODO.md 6A.6, decided during this build since the design
doc left it open): guards 3/5/6 assert a **global** `safety_link` fault
**Guard 1's rate is per-zone config** (`sanity_rate_c_per_min`, Settings >
Zones, `z%u_sanity` on `POST /api/zones`, range 0–20 °C/min, 0 = the firmware
default 0.5). It is the one guard threshold whose right value is a property of
the *rig* rather than of kilns in general: too high false-trips a slow but
genuinely heating system, too low lets a real dead element run longer. The
bench fixture ran 5.0 (a kiln's figure) and killed every step-response and
autotune run at t=62 s until it was commissioned to 0.2 — see
`tools/PcTools/config_presets/bench_fixture.json`. Lowering it does not slow
guards 2–5; each reads its own threshold.

(blocks every zone, `SAFETY_FAULT_SRC_THERMAL_SANITY` for 3/5,
`SAFETY_FAULT_SRC_THERMO` for 6) — their failure modes are board-wide
(welded contact, sensor electrically faulted). Guards 1/2/4/7/8 only block
*that zone*, via `relay_authority_zone_blocked()`'s per-zone mask — their
failure modes are specific to one zone's physics.

### Guard 8 (cross-zone plausibility) — implemented, armable, off by default

Added 2026-08-12, once the executor had concurrent multi-zone execution
(TODO.md 6A.5(a)) to compare zones against each other. It catches the case
guards 1 and 2 can miss: a thermocouple that fell out of the kiln body while
that zone's own elements really are heating, so the zone looks like it's
working while its neighbors climb and it doesn't.

How it works: `profile_executor.c` hands each zone the same tick's whole-bus
snapshot (`peer_c`, `peer_ok`, `peer_count`, `peer_index_self` — raw readings,
un-averaged, from the one read every channel gets per tick). The guard compares
this zone against the **worst** disagreeing peer, not an average — with three
zones an average would let one badly wrong channel hide behind a healthy one.
Peers whose reading isn't trustworthy (`peer_ok[i] == false`) are skipped, and
the zone's own slot is excluded. Disagreement must be *sustained* past
`cross_zone_period_s` (0 → `CROSS_ZONE_PERIOD_S_DEFAULT`, 600 s); a single tick
back inside the band resets the timer, because real kilns stratify transiently
and only a sustained disagreement says a sensor has left the building. On trip
it blocks only its own zone (`relay_authority_zone_blocked()`), asserts no
global fault bit, and reports `fault_guard: 9` (the enum value appended after
`TRIP_FROZEN`).

**It is off until you arm it, and off is the default.**
`cross_zone_max_delta_c` still has no default — 0 means "no threshold
configured", and a zone starts at 0 — because the number is supposed to come
from a measured cross-gain matrix `K` (TODO.md 6A.5's last bullet) and no
hardware has produced one. Hard-coding a plausible figure and calling the
guard done is exactly what that bullet says not to do.

What changed on 2026-08-12 is that the number is now **per-zone config** you
can set: a *Cross-zone plausibility* field on Settings → Thermocouples & Zones,
persisted with the rest of the zone config, read back by
`zones_config_get_cross_zone_delta()`, and handed to the guard by
`profile_executor.c`. `cross_zone_period_s` deliberately stayed a firmware
constant (600 s) — one knob is enough to arm the guard, and a second is easier
to get wrong than to get value from. **Until you set a threshold on a kiln you
have measured, this is not protection you have** — an unarmed guard protects
nobody, and the field starts blank on purpose.

`POST /api/zones` treats `z<i>_xzone` as optional, unlike every other zone
field: a client that doesn't send it (the MCP/`pc_tools` path, older test
harnesses) is accepted rather than rejected, and leaves the threshold at 0.
Such a submit does *clear* a previously saved threshold, which is the same
whole-page-submit semantics every other field on that page already has —
so arm the guard from the page, or include the field in whatever posts the
config. Since config reload while running landed (TODO.md 6A.7, same day),
arming the guard mid-firing does take effect on the next tick — and is logged
at WARN as an operator action, like every other guard-threshold edit.

**Observed on hardware 2026-08-12** (sim plant, threshold set to a
deliberately tight 2 °C, `fault=tc_detached` on one of two zones): tripped at
600 s with `fault_guard: 9` and
`"28.5C differs from zone 1's 20.5C by 8.1C (>2.0C) for 600s"`. Note what a
two-zone kiln does here — the disagreement is mutual, so **both** zones trip on
the same tick, each naming the other, and the run escalates to `FAULTED` via
"every active zone individually faulted" even though the guard itself only ever
blocks its own zone. Isolating the bad channel needs three or more zones, where
the worst-disagreeing-peer comparison has a majority to compare against.

The host tests show why that matters concretely: in `test_sim_kiln.c`, a
detached thermocouple in a strongly coupled chamber does trip guard 8 against
its neighbor, and a healthy coupled kiln with both zones at equal duty does
not. But two *healthy* zones driven at 1.0 and 0.6 duty legitimately settle
more than 100 °C apart in the same model — which is precisely why a fixed,
hand-picked threshold is not defensible and why the firmware asks for the
number instead of inventing one.

## Autotune (`pid_autotune.c` + `autotune_engine.c`, TODO.md 6A.4)

**Step-test identification only** in this build — the relay-feedback
(Åström–Hägglund) method TODO.md 6A.4 also describes is not implemented; the
step test was built first per that section's own recommendation ("slow,
thermally abusive, and at cone temperature it is the last thing anyone wants
to do on purpose" — of the relay-feedback alternative).

Flow: `POST /api/autotune/start {zone, step_duty}` ->
`autotune_engine` state machine (`IDLE -> SETTLING (180s @ duty 0) ->
STEPPING (records the response every 5s, up to 4h) -> DONE | ABORTED`) ->
`pid_autotune_fit_fopdt()` (two-point 28.3%/63.2% method) fits `{K, tau, L}`
-> `pid_autotune_tune_from_fopdt()` computes SIMC gains (`lambda = 3*L`
default, "robust" per TODO.md 6A.4's own recommendation for a kiln) ->
`GET /api/autotune` shows the proposed `{Kp,Ki,Kd}` and predicted max ramp
rate -> operator reviews -> `POST /api/autotune/accept` is the *only* path
that writes anything, through `zones_config_set_pid()` (new setter,
`zones_http.c` stays the sole NVS owner — neither autotune module ever
touches NVS itself). `POST /api/autotune/abort` works at any point; the
engine also self-aborts on a guard trip or on hitting the 4h budget without
a fittable trace.

**Guard coverage during STEPPING with no `max_temp_c` configured (fixed
2026-08-24).** The step test has no real setpoint, so `thermal_guard`'s
`setpoint_c` is synthesized each tick: the zone's `max_temp_c` ceiling when
one is configured, else `raw_c + STEP_TEST_GUARD_HEADROOM_C` (a fixed 5 °C
headroom, not a real target — guard 1's math only ever reads the *sign* of
`setpoint_c - measurement_c`). Before the fix, the no-ceiling fallback was
bare `raw_c`, which pinned `error` at exactly 0.0 every tick and silently
disabled guard 1 (HEATING_FAILED) for the whole run — a dead element or a
flat thermocouple went undetected for up to 4h with duty legitimately on.
Guard 2 (WRONG_DIRECTION) was not actually broken by this (its math never
reads `setpoint_c`'s magnitude, only real measurement deltas), but it is
narrower than guard 1 and was the only thing running in that state, so a
stalled-but-not-yet-falling reading still passed. The fix keeps `error`
strictly positive so guard 1 — which already subsumes guard 2's case —
stays live regardless of whether a ceiling is configured. Guard 4 (DRIFT)
is unaffected by the fix either way: without a real setpoint it was already
only a best-effort check (dormant unless the zone happens to settle within
`DRIFT_HYSTERESIS_C` of the ceiling), and `autotune_engine_run()` logs a
WARN at start when no ceiling is configured, naming guard 4 specifically, so
this remaining limitation is visible rather than silent. See
`autotune_engine.c`'s `STEP_TEST_GUARD_HEADROOM_C` comment and its use site
for the full reasoning, and `test_autotune_engine_prestart.c`'s
STEPPING-loop tests for the regression coverage.

Since 2026-08-12 acceptance also persists the **model**, not just the gains:
`zones_config_set_model()` stores `{K, tau, L}` per zone next to the gains,
because the feedforward term (6A.2) is computed from `K` and `tau` and the
fit used to be discarded the moment the gains were written. 0 in any of the
three means "no model identified". The zones page shows them read-only and
echoes them back on save — a whole-page submit that omitted them would wipe a
model that took hours of real heat to measure.

**Relay-feedback identification (2026-08-12).**
`pid_autotune_fit_relay()` recovers `{Ku, Tu}` from a trace taken under relay
control, `Ku = 4d / (pi*sqrt(a^2 - h^2))` with `a` the oscillation's
*half*-amplitude and `h` the hysteresis half-band. It fits only the trailing
complete cycles, demands cycle-to-cycle consistency, and rejects `a <= h`
rather than propagating a NaN. `pid_autotune_tune_from_relay()` supplies the
ZN and Tyreus-Luyben rules (the FOPDT path still refuses them — they are not
derivable from a step model). Neither is a default anywhere: ZN targets
quarter-amplitude decay, i.e. it is *designed* to oscillate, which on a kiln
at 1200 °C costs the firing.

`autotune_engine.c` gained the matching on-target path the same day
(`AUTOTUNE_METHOD_RELAY`: `RELAY_APPROACH -> RELAY_CYCLING`), opt-in only —
`POST /api/autotune/start` still defaults to the step test. Recording begins
at the first relay switch rather than on arrival at setpoint, so the trace
carries no approach ramp to bias the midline the fitter slices cycles
against, and the relay law is evaluated every 1 Hz tick rather than per
10 s sample, because the switch instants *are* `Tu`. Accepting a relay
result writes gains but never a plant model: one frequency-response point
does not determine a FOPDT model, and a model an earlier step test measured
must survive. Note guard 3 has no off-window while cycling — the low branch
is duty 0.15 — so the welded-relay check simply has nothing to observe;
guards 4 and 5 cover that window with the real setpoint.
**This section used to say "neither method has ever completed on hardware,
because with no thermocouples attached every on-target run aborts on guard 6
first."** That stopped being true on 2026-08-29/2026-08-30: a thermocouple
daughterboard is now attached to the bench unit, the window/min-on ratio bug
above is fixed, and the step method has completed multiple times — see "Measured
on the bench, 2026-08-29" above and "First real cross-zone coupling
measurement (2026-08-30)" below. The relay-feedback method specifically has
still never completed on hardware; nothing in this file should be read as
claiming otherwise for it.

### Tuning-rule comparison

Four rules are implemented, on two different identification paths, and they
are not interchangeable — each refuses the other path's input rather than
guessing at a conversion.

**SIMC** (`pid_autotune_tune_from_fopdt()`, `AUTOTUNE_RULE_SIMC`, the
default) consumes the FOPDT step-test model `{K, tau, L}`: `Kc = tau /
(K*(lambda+L))`, `Ti = min(tau, 4*(lambda+L))`, `Td = L/2`
(`pid_autotune.c:180-192`). `lambda` defaults to `3*L` when the caller passes
0 (`pid_autotune.c:180`), which is the "robust" tuning Skogestad's own SIMC
paper recommends over the "tight" `lambda = L` — a 3x larger lambda trades
response speed for margin against exactly the kind of model error a two-point
fit on a noisy thermocouple trace is going to have. On the degenerate `L == 0`
case, lambda falls back to `tau` (or 1.0 if `tau` is also non-positive) rather
than leaving lambda at zero (`pid_autotune.c:181-183`).

**Cohen-Coon** (`AUTOTUNE_RULE_COHEN_COON`, added 2026-08-30, same FOPDT
path) is more aggressive: `Kc = (1/K) * (tau/L) * (4/3 + L/(4*tau))`, `Ti = L
* (32 + 6*(L/tau)) / (13 + 8*(L/tau))`, `Td = L * 4 / (11 + 2*(L/tau))`
(`pid_autotune.c:116-119, 152-154`). It refuses below
`AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S` (0.5 s, `pid_autotune.c:96`) rather than
clamping a small `L`: Cohen-Coon's `Kc` divides by `L` directly, so a fit
whose step response barely lagged reports `L` near zero and `1/L` blows up
right where the two-point method is least trustworthy anyway. SIMC has a
principled fallback for `L == 0` (substitute `tau` for `lambda` and keep
producing a conservative number); Cohen-Coon has none, because the whole rule
is parameterized by `L/tau` with no substitute that preserves its meaning —
clamping `L` to an epsilon would hand back an arbitrarily large `Kc` dressed
up as a real answer, so the code refuses instead (`pid_autotune.c:85-96`).

**Ziegler-Nichols** and **Tyreus-Luyben** (`pid_autotune_tune_from_relay()`,
`pid_autotune.c:444-483`) take a completely different input: `{Ku, Tu}` from
the relay-feedback test, not `{K, tau, L}` from the step test. ZN is `Kc =
0.6*Ku`, `Ti = Tu/2`, `Td = Tu/8`; Tyreus-Luyben is `Kc = Ku/3.2`, `Ti =
2.2*Tu`, `Td = Tu/6.3` — roughly a third the gain and four times the integral
time of ZN, trading settling speed for a loop that does not ring
(`pid_autotune.c:460-473`). Each tuning function refuses the other path's
rules outright: `pid_autotune_tune_from_fopdt()` returns
`AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH` for ZN/Tyreus-Luyben
(`pid_autotune.c:162-167`), and `pid_autotune_tune_from_relay()` does the
mirror-image refusal for SIMC (`pid_autotune.c:474-483`) — SIMC has no
definition in terms of a single frequency-response point, and ZN/Tyreus-Luyben
have no definition without one. This is why the UI needs two separate rule
selectors rather than one four-way dropdown: picking a rule also picks which
identification run has to precede it.

All four rules produce their gains in Kc/Ti/Td **series** form and convert to
pid.c's **parallel** form the same way, in the same two lines, at every call
site: `Kp = Kc`, `Ki = Kc/Ti`, `Kd = Kc*Td` (`pid_autotune.c:194-198,
485-490`). This conversion matters enough to call out explicitly — pid.c's
integral and derivative terms are `Ki*integral` and `Kd*d_filtered`, i.e.
already parallel form, so a Ti/Td pair plugged in directly (as if it were
already Ki/Kd) would be wrong by a factor of `Kc`, and mixing the two forms
by hand is a classic way to end up with a plausible-looking but wrong tuning.

All four rules also share the same refusal conditions rather than emitting a
nonsense gain: an invalid input model or fit (`AUTOTUNE_REFUSAL_INVALID_MODEL`),
a non-positive fitted plant gain `K` or `Ku`/`Tu`
(`AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN` — a heater that cannot cool cannot have a
negative gain, so a negative fit is always a bad fit, never a real plant), and
for the FOPDT path a non-positive `tau`
(`AUTOTUNE_REFUSAL_NONPOSITIVE_TAU`). Every refusal path returns an explicit
enum plus a human-readable reason string carrying the offending numbers
(`pid_autotune.h:83-100`), rather than the all-zero gains a refusal used to
collapse to indistinguishably from every other refusal.

**SIMC stays the default, and Cohen-Coon should be a deliberate choice, not
one.** Both are derivable from the same FOPDT model, but Cohen-Coon is the
more aggressive of the two (`pid_autotune.c:108-114`) — like ZN, it targets
roughly quarter-amplitude decay, the same design target as the
oscillation-based rules even though it is derived differently. On a kiln,
where overshoot costs the ware and elements do not enjoy being cycled hard,
the conservative SIMC/`lambda=3*L` tuning is the one that should run
unattended; Cohen-Coon is offered for an operator who specifically wants the
faster response and understands the overshoot trade, the same posture this
file already takes toward ZN and Tyreus-Luyben.

## Feedforward (TODO.md 6A.2), built 2026-08-12

`u_ff = (T_sp - T_ambient)/K_dc + (dT_sp/dt) * tau/K_dc` — the first term is
the duty the kiln needs just to *hold* the setpoint, the second the extra to
*climb* at the commanded rate. With dead time in the tens of seconds,
feedback alone always lags a ramp; this is what puts the actual curve on the
desired curve instead of a fixed offset below it, leaving the PID to correct
only model error.

Details that matter more than the formula:

- **It is off unless that zone has an identified model**, and off on a
  non-finite or non-positive `K_dc`/`tau`. A kiln driven from a fabricated
  `K` is worse than one on feedback alone.
- `dT_sp/dt` comes from the segment's **commanded** ramp, not a tick-to-tick
  difference of `target_c`: at 1 Hz a 100 °C/hr ramp moves the target by
  ~0.03 °C, and dividing that by a jittering measured `dt_s` is mostly noise
  — which then gets multiplied by tau on its way to the duty. The climb term
  is zero while dwelling, while ramp-lock holds, and on a step segment.
- **Ambient is the cold junction, sampled once at firing start.** It warms
  with the board, so re-sampling would walk the hold term down over a
  firing. No valid CJ at start falls back to 20 °C and keeps feedforward on:
  the error enters as `(delta_ambient)/K_dc`, under 2 % duty for a kiln.
- Only the *sum* is clamped to [0,1]. On a cooling ramp the climb term is
  legitimately negative and should reduce the hold duty.
- `pid_terms_t.ff` on `/api/control` reports exactly what was passed, so the
  P/I/D/FF split is visible while tuning.

**It broke bumpless transfer, and that is worth knowing if you touch this
code.** `pid_seed_bumpless()` solves `integral = (u_desired - P - ff_u)/Ki`
— seeding against `u_desired` alone would come back one whole feedforward
term high, and on a hot kiln that is the largest term in the sum. A model
appearing mid-run re-seeds too, since ff jumping from 0 to most of the duty
on top of an integral built to supply that same heat would peg the element
until the integrator unwound. One consequence: resume can no longer
reproduce `u = 0` when ff > 0 — a resumed zone comes back at exactly its
feedforward duty, the model's estimate of the hold cost with nothing
accumulated on top. **Update (2026-08-13): the subtraction now lives inside
`pid.c`** — `pid_seed_bumpless()` takes `ff_u` directly and does it itself,
so `profile_executor.c`'s wrapper just passes the term through instead of
pre-subtracting it.

**Untested against a real kiln.** A wrong `K_dc` from a bad fit now reaches
the duty directly, bounded only by the clamp and the guards.

The FOPDT fit is validated in `App/test/test_pid_autotune.c` by running it
against `App/test/sim_plant.c` with *known* ground-truth `K`/`tau` (a pure
first-order plant, no sensor delay) and checking the fit lands within 2%/5%
of the true values — this is "the only place the identification math can be
validated exactly" per TODO.md 6A.8, since a real kiln's true `K`/`tau`/`L`
are never actually known.

Not built: relay-feedback identification, gain-scheduling bands (v1 ships a
single band per zone, matching TODO.md 6A.4's own "v1 may ship a single
band"), and a distinct `AUTOTUNE` relay-ownership tag (section 0's
ownership-tag mechanism itself was never generally built). Cross-zone
coupling logging during a run (TODO.md 6A.5(b)) is now built — see the
Module layout table above and TODO.md 6A.5(b) for what it does and doesn't
cover (no real hardware has exercised it yet).

## Fuzzy adjustment (`pid_fuzzy.c`, PID_EXPANSION_PLAN.md §2b/§4 Phase 3)

An optional second control mode layered over classic PID: a fixed,
firmware-wide Mamdani fuzzy-rule table nudges the three PID gains up or down
around whatever base gains Autotune measured, so one set of tuning numbers
keeps working across a temperature range wider than the point Autotune ran
at. It does not change the base gains — selecting it lets them drift within a
caller-bounded range while firing. Pure C, no FreeRTOS, no ESP-IDF, no
globals, same host-testability discipline as `pid.c` (`pid_fuzzy.c:1-6`).

**The rule table.** Two inputs — this zone's own error (`setpoint -
measurement`, degC) and its rate of change (degC/s) — each mapped onto three
triangular membership buckets (NEG/ZERO/POS on error, FALLING/STEADY/RISING
on rate), giving a 3x3 grid of 9 rule cells (`RULE_TABLE`,
`pid_fuzzy.c:96-109`). Membership degrees are computed with plain triangular
functions centered at `-band/0/+band` (`triangular_memberships()`,
`pid_fuzzy.c:55-82`), each cell's firing strength is the product AND of its
two membership degrees (Mamdani, product AND), and the three output gain
directions are combined by weighted-average defuzzification over all 9 cells
(`pid_fuzzy.c:205-219`) — a lookup-plus-interpolation, not a fuzzy-logic
library. The rule table is a compile-time constant rather than per-zone
config, deliberately: a per-installation editable table reopens the
trial-and-error tuning problem this mode exists to avoid (`pid_fuzzy.h:17-21`).

**The band constants** (`pid_fuzzy.c:44-50`):
- `ERROR_BAND_C = 20.0` — the error axis half-width; "large" error is set
  around a single time-proportioning window's worth of visible
  overshoot/undershoot for a mid-size kiln zone.
- `RATE_BAND_C_PER_S = 0.5` — the rate axis half-width, rescaled 2026-08-30
  from an earlier `0.05`. At `0.05`, an ordinary 100-300 degC/hr profile ramp
  (0.028-0.083 degC/s) sat mid-scale on this axis for its entire duration, so
  "large rate" was measuring the commanded profile rather than a disturbance
  — exactly the hazard PID_EXPANSION_PLAN.md Phase 3 hazard 2 calls out. At
  `0.5` (30 degC/min, roughly 6x the fastest ramp this kiln's profiles
  realistically command), a legitimate firing's own ramp now sits well inside
  the STEADY bucket for its whole duration, and only a rate no ordinary ramp
  produces — a stuck-open lid, a runaway element, a thermocouple snapping
  loose toward ambient — reaches toward the RISING/FALLING extremes.
- `MAX_NUDGE_FRACTION = 0.5` — the maximum fractional adjustment any single
  gain may receive at `strength_pct=100` and full rule membership: the fuzzy
  layer may at most halve or 1.5x a base gain. A bounded adjustment, not a
  re-tune.

**The sign convention — read this carefully before touching either
file.** `pid.c` computes `raw_d = -(measurement - prev_measurement)/dt_s`, so
`d_filtered` (what `pid_fuzzy_adjust()` receives as `error_rate_c_per_s`)
carries `+d(error)/dt` in form, but it is derivative *on measurement*, never
on the setpoint — `pid.c` never differentiates the setpoint. A **climbing**
kiln therefore feeds a **negative** value on the rate axis: a kiln rising at
300 degC/hr (0.083 degC/s) feeds `-0.083` here, which leans toward the NEG
(FALLING) bucket, not POS (RISING). The magnitude tracks the profile's ramp
rate because the derivative is on measurement and the setpoint is never
differentiated in it — not because "tracking well" implies a large
d(error)/dt. The opposite is true: true `d(error)/dt` is close to zero while
a ramp is being tracked well, since error itself is roughly constant. Getting
this backwards would invert every rule in the table; `pid_fuzzy.c:9-33` and
`pid_fuzzy.h:79-94` carry the same explanation and this paragraph is meant to
stay consistent with both.

**Defaults and degenerate inputs.** `strength_pct = 0` is the safety
contract: it reproduces the base gains bit-for-bit, with no fuzzy math in the
path at all, and is the default (`pid_fuzzy.c:148-166`). A non-finite
`error_c` or `error_rate_c_per_s` (e.g. from a faulted thermocouple) also
holds the base gains rather than adjusting — an earlier version mapped a
non-finite input to `0.0` on both axes, which lands on the ZERO/STEADY cell
and *raises* Ki by up to 50% at the exact moment the temperature reading has
failed; that was replaced with "make no adjustment at all, let the thermal
guards decide" (`pid_fuzzy.c:168-188`). Every output gain is separately
clamped finite and non-negative regardless of how the inputs misbehave
(`clamp_gain()`, `pid_fuzzy.c:111-121`).

**The integral rescale this mode requires.** Because the fuzzy layer can
change Ki every tick while a zone is running, `pid.c` carries
`pid_rescale_integral_for_new_ki()` (`pid.c:25`), which rescales the
accumulated integral term proportionally whenever Ki changes so the *product*
`Ki * integral` — the actual I contribution to duty — stays continuous across
the gain change. Without it, a Ki nudge would step the I term (and therefore
the commanded duty) discontinuously the instant the fuzzy layer moved it,
exactly the kind of bump bumpless transfer exists elsewhere in this file to
avoid.

## Host-side testing (`App/test/`, TODO.md 6A.8)

`App/test/build_host_tests.ps1` builds `kilnctl_host_tests.exe` with MSVC
(`cl.exe`, no ESP-IDF) from `pid.c`/`thermal_guard.c`/`heater_output.c`/
`pid_autotune.c` plus the test files and `sim_plant.c`. 215 checks as of
2026-08-12 (96 before that day's `sim_kiln` work; 120 before guard 8's two
checks; 125 after the guard-3 rate-baseline regressions found on hardware;
154 with the relay-feedback identification checks; 215 with the RGA's 61),
all passing: per-guard trip
*and* non-trip cases, `heater_output`'s quantization rules, a
4-simulated-hour closed-loop run (sim_plant + pid + thermal_guard +
heater_output wired together the way `profile_executor.c` wires the real
hardware) that reaches setpoint with no false guard trip, and the autotune
fit-vs-ground-truth check above.

`App/test/test_sim_kiln.c` is what the 2026-08-12 checks bought: every
*implemented* guard is now provoked by a stated physical failure rather than
by hand-fed numbers — a dead element, a welded relay contact, a detached,
frozen or open thermocouple, swapped connectors, and (guard 8) a detached
thermocouple in a coupled chamber, with the test supplying the threshold the
firmware deliberately leaves unset — plus negative controls (sensor noise on a
healthy zone trips nothing; guard 8 stays silent on a healthy coupled kiln at
equal duty) and a first off-target
exercise of the 6A.5(b) cross-gain fit against a plant whose coupling is
known by construction. `docs/GUARD_TEST_MATRIX.md` is the per-guard writeup
of all of it.

Writing these tests found a real bug in `heater_output_bangbang()`: it
re-checked its min-on/min-off debounce timer only on the exact call where
the raw comparator decision changed, not on every call while a transition
was still pending, so a `want_on` that stopped flip-flopping and just held
its new value could leave the relay stuck at the old state forever no matter
how much time passed. Fixed to gate on the relay's actual state instead of
the previous call's requested state — see the function's comment for the
full trace-through.

Both of the remaining TODO.md 6A.8 items landed 2026-08-12: the
firmware-integrated N-zone/fault-injection simulator behind
`CONFIG_KILNCTL_SIM_PLANT` (next section) and the written test matrix, one
row per guard, in `docs/GUARD_TEST_MATRIX.md`.

## Simulated plant on-target (`sim_backend.c`, `CONFIG_KILNCTL_SIM_PLANT`)

The host tests prove the pure modules. They cannot walk a whole firing — HTTP
start, ramp, guard trip, escalation, fault display, operator clear — because
`profile_executor.c` links FreeRTOS and ESP-IDF. `App/drivers/sim_backend.c`
closes that gap by compiling the *same* `App/test/sim_plant.c` model into the
firmware (deliberately the same file, not a second on-target copy of a
thermal model) and substituting it for the MAX31856 channels.

Turn it on under `idf.py menuconfig` → **KilnCtrl Hardware Configuration** →
**Simulated plant (development only)** → **KILNCTL_SIM_PLANT** (default `n`).
The same menu carries the model parameters: ambient, per-zone thermal mass,
element power, linear loss coefficient, radiative coefficient (in units of
1e-12 W/K⁴, because Kconfig has no float type), inter-zone conductance,
sensor transport delay and lag, and peak-to-peak sensor noise.

With it on:

- The profile executor, autotune engine, and dashboard read simulated
  temperatures via `sim_backend_read_all()`, which fills one
  `MAX31856Reading` per configured zone using the real driver's failure
  contract (NaN temperature plus the matching `fault_status` bit for an
  injected sensor fault, never a stale-but-plausible number).
- Relay commands are fed back into the model from the *post-safety-gate*
  decision — `apply_relay()` in both `profile_executor.c` and
  `autotune_engine.c` calls `sim_backend_note_zone_relay()` with what was
  actually commanded, so the sim sees the same thing the hardware would.
- The model advances on wall-clock time (`esp_timer`) rather than once per
  call, so the 1 Hz executor, the dashboard poll, and the autotune engine can
  all read it without any of them making simulated time run fast. Each
  advance is sub-stepped at ≤1 s so the integrator sees roughly the step size
  the host tests validated it at, and a long gap (Wi-Fi reconnect, debugger
  halt) is capped at 60 s of catch-up rather than becoming one enormous
  forward-Euler step.
- `GET /api/sim` reports, per zone, the true element temperature, the
  reported reading, the relay state, and the injected fault (plus
  `"simulated": true`). `POST /api/sim` takes a form-encoded
  `zone=<n>&fault=none|element_dead|relay_welded|tc_detached|tc_frozen|tc_open`.
  Neither handler exists in a production image.

Every entry point compiles to an inline no-op when the option is off, so call
sites need no `#ifdef` and the production build is unaffected.

**This is a development feature and it is not safe near a kiln.** A sim build
reports fabricated temperatures — every page will show a healthy firing with
nothing plugged in — and relay writes still go out to a real expander if one
is attached. It must not be flashed to a board wired to elements.

Status as of 2026-08-12: both builds compile clean, production (sim off,
the default) and a sim-enabled build in a separate build directory. **Nothing
was flashed.** The sim path has never been run on the board, so nothing in
this section is verified beyond compiling.

## What's still open

See TODO.md section 6A for the itemized, dated checklist. In rough
cost/value order, the largest remaining pieces are:

- **A real filled coupling matrix, and everything downstream of it** — (a),
  (b), and (d) of TODO.md 6A.5 all landed 2026-08-11 (see
  `docs/PROJECT_STATUS.md`): concurrent multi-zone execution, ramp-lock, and
  the matrix-capture code path in `autotune_engine.c`. But no thermocouple
  hardware is attached to the bench unit, so every real autotune run so far
  aborts on guard 6 before a fit ever happens — the matrix has never
  actually been filled against a real cross-gain. The RGA (c) was **built
  2026-08-12** — `pid_autotune_rga()` computes `RGA = K .* (K^-1)^T` over the
  largest principal sub-block whose every cell is measured, refusing an
  incomplete or (scale-aware) singular matrix rather than approximating,
  served on `/api/autotune/matrix` and shown on the zones page as a verdict
  keyed to the worst diagonal element. It is host-tested (61 checks) but has
  never seen measured data, for the reason above; on hardware only its
  refusal path has been observed. The static decoupler (e) still depends on
  real matrix data and stays blocked until sensor hardware is attached.
- **A threshold for guard 8.** The guard is built, host-tested, settable from
  the zones page, and was observed tripping on the board at 600 s
  (2026-08-12). What is left is only the *number*:
  `cross_zone_max_delta_c` defaults to 0 (off), and picking a real value wants
  a measured `K`, which wants real hardware. The mechanism is no longer the
  open question; the kiln measurement is.
- **6A.9 is now fully landed** (history buffer, dashboard graph,
  `/api/control`, per-zone relay timing) except the cycle-count/simultaneous-on
  telemetry itself, which is a separate small item from the cap that now
  enforces the limit (6A.5's load-staggering, both halves built 2026-08-11 —
  see TODO.md 6A.5 for the deferred-credit design and what's still
  unverified against real hardware).
- **Relay-feedback autotune** and **gain-scheduling bands** — smaller,
  independent items, see TODO.md 6A.4. (The written test matrix, the third
  6A.8 item, landed 2026-08-12 as `docs/GUARD_TEST_MATRIX.md`.)
- **An on-target run of the simulated plant.** `CONFIG_KILNCTL_SIM_PLANT`
  builds, but has never been flashed, so the end-to-end walk it exists to
  enable — start a profile, inject a fault over `POST /api/sim`, watch the
  guard trip, escalate, display, and clear — has not been done yet.

## A lesson from building the CSV endpoints

`GET /api/history.csv` and `GET /api/autotune/trace.csv` both originally
allocated one large buffer per request (tens to ~115KB). A `idf.py size`
check of static DIRAM headroom before ever flashing looked comfortable
(137KB free), which turned out to be the wrong number to check: that's
static allocation, not the separate runtime heap Wi-Fi/lwIP/httpd already
draw from. `/api/history.csv` came back `500 out of memory` against the
real board the first time it was actually hit with data to serve. Fixed by
paginating the underlying getters and streaming both responses via
`httpd_resp_send_chunk()` in small batches instead of one big buffer — peak
allocation is now independent of how many samples exist. The general
lesson: a static-size report is not a substitute for hitting the actual
endpoint on real hardware before calling something done.

## First real cross-zone coupling measurement (2026-08-30)

The step test finally ran to completion on hardware, and the coupling matrix
`autotune_engine.c` has been filling since 2026-08-11 has its first real row.
Zone 0 driven at duty 0.4, from ~27 °C ambient, 39 samples over 390 s:

| Cell | K (°C/duty) | tau (s) | dead time (s) |
|------|-------------|---------|---------------|
| 0 -> 0 (self) | 32.648 | 163.7 | 40.2 |
| 0 -> 1        |  5.863 | 181.4 | 65.5 |
| 0 -> 2        |  2.812 | 163.9 | 84.7 |

SIMC proposal from the self-cell: `kp = 0.03123`, `ki = 0.00019`,
`kd = 0.62687`, predicted max ramp 431 °C/hr. **Not accepted** — zone 0 still
carries its hand-entered `Kp=2.0 / Ki=0.1 / Kd=1.0`.

**The zones are genuinely coupled.** Zone 1 sees 18% of zone 0's own gain and
zone 2 sees 8.6%. That is not measurement noise, and it settles the question
`PID_EXPANSION_PLAN.md` §2c was left open on: cross-zone compensation is worth
building, because a controller tuned per-zone in isolation here really is
fighting a disturbance worth ~18% of its own authority.

Two details worth keeping:

- **Dead time grows with distance** (40.2 -> 65.5 -> 84.7 s) while tau stays
  roughly constant (~164-181 s). That is the physically expected signature —
  heat takes longer to *arrive* at a further zone, but once arriving, the
  thermal mass it is charging is similar. A coupling row where the dead times
  came out equal, or shorter with distance, would be a reason to distrust the
  fit; these do not.
- **The self-cell reproduces the earlier bench run** (K = 32.95, tau = 166.9,
  L = 36.9 recorded above) to within ~1% on K and ~2% on tau. Two independent
  runs agreeing is the first evidence the identification path is repeatable
  rather than merely finishing.

**RGA is still unavailable** (`code 2`, "no 2 zones yet have every cross-gain
between them measured") — it needs zones 1 and 2 driven too, so rows 1 and 2
of the matrix are still `valid: false`. Note the ordering hazard when
filling them: a step test on zone 1 started while zone 0 is still cooling
will fit cell (1,0) against a falling baseline and can report a *negative*
cross-gain. Let the kiln settle to a flat baseline between rows rather than
chaining runs back to back.

Caveat on all of the above: this rig is range-limited to 0..80 °C, so these
numbers describe the plant near ambient. `PID_EXPANSION_PLAN.md` §2a's `T^4`
argument means K at cone temperature will not be this number.

## Full 3x3 coupling matrix and first RGA (2026-08-30, later the same night)

All three zones have now been step-tested (duty 0.4, bench rig, near-ambient
0..80 °C), each run started only after the rig settled below 0.8 °C spread —
the ordering hazard called out above, taken seriously. The coupling matrix
`autotune_engine.c` has been filling since 2026-08-11 is complete for the
first time:

| K (°C/duty), tau (s), L (s) | j=0 | j=1 | j=2 |
|---|---|---|---|
| i=0 | K=32.648 tau=163.7 L=40.2 | K=5.863 tau=181.4 L=65.5 | K=2.812 tau=163.9 L=84.7 |
| i=1 | K=10.887 tau=123.2 L=56.7 | K=20.969 tau=113.5 L=39.3 | K=3.332 tau=125.6 L=65.4 |
| i=2 | K=7.723 tau=123.1 L=93.2 | K=8.625 tau=127.8 L=83.7 | K=23.641 tau=124.7 L=40.8 |

With every cell now measured, `pid_autotune_rga()` produces its first real
answer instead of the incomplete-matrix refusal (`n=3`, `det=13700`):

```
[ 1.1131  -0.0992  -0.0140 ]
[-0.0909   1.1484  -0.0575 ]
[-0.0222  -0.0492   1.0715 ]
```

**Diagonal pairing is confirmed correct for all three zones.** All three
diagonal elements sit in 1.07-1.15 with small, negative off-diagonals — weak
interaction, and zone *i*'s own loop should drive heater *i*, not some other
assignment. This does not extend to justifying full MIMO decoupling; the RGA
says decentralized per-zone PID is the right structure here, not that
cross-zone compensation is unnecessary (the earlier 0->1/0->2 row already
established the couplings are real, ~8-18% of self-gain).

**Coupling is asymmetric in every pair, and by a lot.** 0<->1 is 5.863 vs
10.887 (zone 1's pull on zone 0 is 1.86x zone 0's pull on zone 1); 0<->2 is
2.812 vs 7.723 (2.75x); 1<->2 is 3.332 vs 8.625 (2.59x). Normalized to each
zone's own self-gain, zone 1 sends 52% of its self-gain into zone 0, while
zone 2 spreads its influence more evenly (33% into zone 0, 37% into zone 1).
Much of this asymmetry is just zone 0's much larger self-gain (32.6 against
21.0 and 23.6 for zones 1 and 2) diluting the *fraction* zones 1 and 2
receive back from it, rather than the physical coupling paths themselves
being one-directional.

**Dead time rises monotonically with distance from the driven zone in every
row** — the same signature the single-row measurement above flagged, and
seeing it hold across all three rows is the check that these fits are
tracking real heat transport through the kiln body rather than baseline
drift dressed up as a cross-gain.

**Caveat, and it is the same one as above, worth repeating because it bounds
everything in this section**: the bench rig only reaches 0..80 °C, so this
whole matrix and RGA describe the plant near ambient. It does not settle
`PID_EXPANSION_PLAN.md` §2a's `T^4` radiative-gain-variation question, which
needs a real firing to real cone temperature to answer — an RGA is a
steady-state measure at a single operating point, and this is one operating
point (near-ambient, all zones driven individually at the same duty step).
