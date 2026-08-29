# Thermal Guard Test Matrix

One row per guard: what it catches, how it is provoked deliberately, how long
it should take to trip, how it escalates, and what the operator actually sees.
This is TODO.md 6A.8's third bullet ("a test matrix, written down, one row per
guard"). `docs/PID_CONTROL.md` is the shorter narrative companion to the whole
control/guard stack; TODO.md section 6A remains the authoritative design doc
and change log. If this file and the code
(`App/drivers/thermal_guard.c`, `App/drivers/profile_executor.c`,
`App/test/sim_plant.c`) disagree, check the code and fix whichever is wrong.

**HARDWARE STATUS**: as of **2026-08-12** the matrix has been walked for the
first time on the real ESP32-S3 bench unit, running a `CONFIG_KILNCTL_SIM_PLANT`
build with the PC joined to the board's fallback AP, injecting faults over
`POST /api/sim` and reading the trip back over `GET /api/profile_exec`. **Four
guards have now tripped on-target: 6, 5, 3, and 1**, each with the observed
time and `fault_reason` recorded in the matrix below. Guards **2, 4, 7, 8 and 9
remain unobserved on hardware** — but for 2, 4 and 7 the reason is now measured
rather than assumed: two further experiments on the same day injected the
physical failures those guards are usually credited with, and **guard 1 fired
first in both cases** (481 s on a frozen sensor, 478 s on a swapped pair of
thermocouples). They are *preempted*, not broken. See "Guard precedence: which
guard actually fires" below.

Two caveats keep this short of "verified on a kiln". First, the plant is
simulated: the trips prove the guard logic, the escalation path, and the HTTP
surface on real firmware, but no thermocouple daughterboard or relay expander
has ever been attached to the bench unit, so no guard has yet seen a real
sensor or a real relay. (Guard 6's earlier 2026-08-11 trip — TODO.md 6A.3, with
no thermocouple attached so every read was invalid; see
`docs/PROJECT_STATUS.md` — remains the only trip caused by genuinely absent
hardware rather than by the sim path.) Second, a host test passing is still
evidence about the *logic*, not about the kiln.

The walk was not merely confirmatory: it **found and fixed a real guard-3 bug**
that no host test had caught — see "What the 2026-08-12 on-target walk found"
below.

Guard 8 (cross-zone plausibility) is a further exception in the other
direction: it was implemented 2026-08-12 and is host-tested both ways, but it
is **disabled until an operator arms it** — its threshold defaults to 0, which
means off, because the number belongs to a measured kiln rather than to the
firmware. It became settable from the zones page the same day and was then
**observed firing on the board at 600 s**, so the mechanism is proven; a kiln
whose field is left at 0 still has no cross-zone protection. See its row below.

## Reading the `fault_guard` number

`/api/profile_exec` reports `fault_guard` as the raw `thermal_guard_trip_t`
enum value, not the guard number. `THERMAL_GUARD_TRIP_NONE = 0` occupies slot
zero, so the numbering shifts by one past the guard numbers used everywhere
else — guard 6 (`SENSOR_INVALID`) reports `fault_guard: 7`, as documented in
TODO.md 6A.3's guard-6 note. Full mapping:

| Enum value | `thermal_guard_trip_t` | Guard |
|---|---|---|
| 0 | `TRIP_NONE` | — (not faulted) |
| 1 | `TRIP_HEATING_FAILED` | 1 |
| 2 | `TRIP_WRONG_DIRECTION` | 2 |
| 3 | `TRIP_RUNAWAY` | 3 |
| 4 | `TRIP_DRIFT` | 4 |
| 5 | `TRIP_MAX_TEMP` | 5 (ceiling) |
| 6 | `TRIP_MIN_TEMP` | 5 (floor) |
| 7 | `TRIP_SENSOR_INVALID` | 6 |
| 8 | `TRIP_FROZEN` | 7 |
| 9 | `TRIP_CROSS_ZONE` | 8 (added 2026-08-12, appended after `TRIP_FROZEN`) |

Guards 5 and 7 therefore do *not* report `fault_guard: 5` / `fault_guard: 7`
meaning "guard 5" / "guard 7". Guard 5 reports 5 or 6 depending on which limit
was crossed; guard 7 reports 8; guard 8 reports 9; and `fault_guard: 7` means
guard **6**. (Guard 9, control-tick liveness, is not a `thermal_guard_trip_t`
value at all and still reports `fault_guard: 0` — so `fault_guard: 9` means
guard 8, never guard 9.) This
is a known wart of exposing the enum directly and is the reason
`fault_reason` (free text, e.g. `"sensor invalid for 3 consecutive reads"`) is
the field a human should actually read.

## The matrix

Thresholds below are the firmware-wide **defaults** at the top of
`App/drivers/thermal_guard.c`. Since 2026-08-16/08-27 essentially every one of
them is per-zone config instead (Settings > Zones, `GET/POST /api/zones`), and
a configured zone uses its own number, not the default quoted here — read the
zone's config before concluding a guard "should have" fired at the time in this
table. The 0-means-not-configured convention applies throughout: 0 selects the
firmware default, it does **not** disable the guard. The one exception is guard
8's `cross_zone_max_delta_c`, where 0 genuinely disables the check and is the
shipped default.

**Guard 1's `sanity_rate_c_per_min` is the one that bites on a slow rig.** It
is the minimum rise rate, in °C/min, that commanded heat must produce; too
high a value false-trips a system that is genuinely — just slowly — heating,
too low a value lets a genuinely dead element run unnoticed for longer. The
bench fixture carried 5.0 (a real kiln's figure) with a 60 s
`guard_wrong_dir_window_s` and every firing died at t=62 s on `heating but rose
only 0.0C in 1.0min (need >=5.0C)`; it now runs **0.2** °C/min, commissioned
via `tools/PcTools/config_presets/bench_fixture.json`. The accepted range is
0..`ZONE_SANITY_RATE_MAX_C_PER_MIN` (20), enforced in `parse_zone_fields()`
and `validate_zones_cfg()`. Lowering it does not slow guards 2/3/4/5 — they
read their own separate thresholds (asserted in `test_thermal_guard.c`).
"Expected time to trip"
assumes the executor's control tick, i.e. one `thermal_guard_tick()` per tick
with `dt_s` equal to the tick period; the host tests use `dt_s = 10s`.

| Guard | What it catches | How it is provoked | Threshold / window | Expected time to trip | Escalation | What the operator sees |
|---|---|---|---|---|---|---|
| **1** `HEATING_FAILED` | Commanded heat producing no rise: dead/open element, thermocouple that fell out of the kiln body | Sim: `SIM_ZONE_FAULT_ELEMENT_DEAD` (relay closes, no heat) or `SIM_ZONE_FAULT_TC_DETACHED` (electrically fine, physically outside the body), duty 1.0, setpoint 300 °C. Unit: duty 1.0, setpoint 500 °C, measurement +0.01 °C/tick against `sanity_rate_c_per_min = 10` | Window starts when `commanded_duty >= PROGRESS_DUTY_MIN` (0.5) and measurement is below setpoint; after `PROGRESS_WINDOW_S` (300 s) the rise must be at least `sanity_rate_c_per_min × elapsed_min` | One full 300 s window after duty first reaches 0.5; the window then slides, so a marginal zone trips at a later 300 s boundary. `test_sim_kiln.c` asserts ≤ 600 s for a dead element. **Observed on-target 2026-08-12: 541 s** (`fault=element_dead`, 400 °C-target profile), inside that window | **Per-zone.** `relay_authority_set_zone_blocked(zone, true)`, that zone's relay forced off, run continues on the other zones. No global fault bit unless it was the last unfaulted zone | Zone row shows `— FAULTED` plus `fault_reason` `"heating but rose only X°C in Y min (need >= Z°C)"`; zone `fault_guard: 1`. Top-level `state` stays `"running"` unless every active zone has faulted. Red vertical mark on the dashboard graph at that sample |
| **2** `WRONG_DIRECTION` | Heat commanded at/above setpoint while the reading falls fast — miswired or swapped thermocouples | Sim: `sensor_map[0] = 1, sensor_map[1] = 0` (swapped connectors), zone 1 pre-heated to 500 °C and cooling, zone 0 commanded duty 1.0 against setpoint 300 °C. Unit: setpoint 500 °C, measurement 600 °C falling 2 °C/tick at duty 1.0 | Same shared progress window as guard 1, but with the shorter `WRONG_DIR_WINDOW_S` (120 s) because the error is negative; trips if the fall exceeds `WRONG_DIR_RATE_C_PER_MIN` (1.0 °C/min) | One 120 s window after duty first reaches 0.5 with the measurement at/above setpoint. **Never observed firing, on hardware or otherwise — and the swapped-connector failure it is usually credited with is in fact caught by guard 1.** On-target 2026-08-12, `POST /api/sim` with `swap=0,1` (a new sim-only injection that exchanges which physical thermocouple two zones read) tripped **guard 1 at 478 s**, `fault_guard: 1`, reason `"heating but rose only 0.1C in 5.0min"` — at the trip, zone 0's element had climbed to 33.3 °C while zone 0 was reading zone 1's untouched 20.1 °C sensor. Guard 2 needs the reading to be *above setpoint and falling* while heat is commanded, which a PID loop makes rare by construction: above setpoint it commands zero duty. Guard 2 is therefore a backstop for a narrower, forced-duty case, not the miswire guard | **Per-zone**, same mechanism as guard 1 | `fault_reason` `"heating commanded but temperature falling X °C/min"`; zone `fault_guard: 2`; zone relay off; red graph mark |
| **3** `RUNAWAY` | Welded relay contact / shorted SSR: duty 0 commanded, temperature still rising | Sim: `SIM_ZONE_FAULT_RELAY_WELDED` (full power regardless of commanded duty), zone pre-heated to 200 °C, commanded duty 0.0. Unit: duty 0.0, measurement +3 °C/tick from 200 °C | `OFF_SETTLE_S` (120 s) of grace after duty goes to 0 (a real kiln coasts), then trips if rise rate exceeds `RUNAWAY_RATE_C_PER_MIN` (1.0 °C/min) **or** total rise exceeds `RUNAWAY_MARGIN_C` (20 °C) from the baseline latched at duty-0 | 120 s settle plus however long the rise takes to clear either limit — with a genuinely welded contact, the 20 °C margin dominates and trips shortly after the settle window. **Observed on-target 2026-08-12: 160 s** (`fault=relay_welded` at a satisfied setpoint — a 21 °C-target profile just above the sim's 20 °C ambient, so the PID commands zero duty), i.e. 40 s past the settle window | **GLOBAL.** `safety_link_set_fault_source(SAFETY_FAULT_SRC_THERMAL_SANITY)`, every active zone marked faulted and forced off, run state → `FAULTED`. The failure mode is board-wide by nature | Top-level `state: "faulted"`, `fault_reason` `"heat commanded off Xs but temperature rose Y°C (rate Z°C/min) — possible welded relay"`, `fault_guard: 3`; every zone row shows FAULTED with the same reason; red graph mark |
| **4** `DRIFT` | A zone that settled once, then sustained an excursion — slow loss of control that no rate guard sees | Sim: zone pre-settled at 300 °C = setpoint, then `SIM_ZONE_FAULT_ELEMENT_DEAD` injected while commanded duty is only 0.2 (below `PROGRESS_DUTY_MIN`, so guard 1 cannot claim it first). Unit: settle exactly at setpoint 500 °C, then hold measurement at 400 °C with duty 0.3 | Latches "settled" the first tick within `DRIFT_HYSTERESIS_C` (25 °C) of setpoint; then trips after `DRIFT_PERIOD_S` (600 s) of *continuous* time outside that band. Any tick back inside the band resets the timer (but not the settled latch) | 600 s of unbroken excursion after having settled. In the sim run this is longer, since the zone has to cool 25 °C past setpoint first. **Never observed firing, and preempted in practice**: the physical scenarios that would produce a sustained excursion are each caught first by a shorter-window guard — a dead element by guard 1 (`PROGRESS_WINDOW_S` 300 s vs. this guard's `DRIFT_PERIOD_S` 600 s, and no 25 °C band to cross first), a welded relay by guard 3 (120 s settle plus a 20 °C margin). Guard 4's reachable window is what is left over: an excursion at duty below `PROGRESS_DUTY_MIN` (0.5) with no runaway, which is why the host test has to inject the dead element at duty 0.2 to see it fire at all | **Per-zone** | Zone `fault_reason` `"drifted >25°C from setpoint for 600s after settling"`, zone `fault_guard: 4`, that zone's relay blocked; red graph mark |
| **5** `MAX_TEMP` / `MIN_TEMP` | The zone's configured absolute ceiling / floor on the **raw** (uncalibrated) reading | Sim: `max_temp_c` lowered to 200 °C, healthy zone driven at duty 1.0 to setpoint 400 °C so it simply heats through the ceiling. Unit: measurement 1301 °C against `max_temp_c = 1300`; measurement −25 °C against `min_temp_c = −20` | `measurement_c >= max_temp_c` or `measurement_c <= min_temp_c`. **No debounce, no window.** `max_temp_c == 0` means "not set" and disables the ceiling entirely (unit-tested at 5000 °C) | One tick — the first over-limit reading; on-target the wait is however long the zone takes to *reach* the limit, not the guard's own latency. **Observed on-target 2026-08-12: 283 s** with `max_temp_c` lowered to 25 °C on a heating zone | **GLOBAL.** `SAFETY_FAULT_SRC_THERMAL_SANITY`, all zones off, state → `FAULTED` | `state: "faulted"`, `fault_reason` `"1301.0C >= max_temp_c 1300.0C"` (or the `<= min_temp_c` form), `fault_guard: 5` for the ceiling, **6** for the floor; red graph mark |
| **6** `SENSOR_INVALID` | Electrically bad sensor: SPI failure, NaN, or MAX31856 `OPEN`/`OVUV`/`TCRANGE` fault bits (folded into `sensor_ok` by the caller) | Sim: `SIM_ZONE_FAULT_TC_OPEN` — `sim_kiln_reading_c()` returns NaN, and the test feeds `sensor_ok = !isnan(reading)` exactly as `profile_executor.c` does. Unit: `sensor_ok = false` repeatedly, plus a good-read-in-the-middle case proving the streak resets. **Also provoked on real hardware simply by running a profile with no thermocouple attached** | `SENSOR_FAULT_DEBOUNCE_TICKS` (3) consecutive bad reads. Any good read resets the streak to 0 | 3 ticks — `test_sim_kiln.c` asserts trip at 0-indexed step 2, exactly. At the executor's tick rate this is a few seconds. **Observed on-target 2026-08-12 via the sim path: 3 s** (`fault=tc_open`) | **GLOBAL**, and the one guard using a different bit: `SAFETY_FAULT_SRC_THERMO` (not `THERMAL_SANITY`) — a faulted sensor is a sensor-subsystem fault, not a plant-behavior one | `state: "faulted"`, `fault_reason` `"sensor invalid for 3 consecutive reads"`, **`fault_guard: 7`** (see the enum-offset note above), all relays off. This exact output was observed on the real board on 2026-08-11 (no thermocouple attached) and again on 2026-08-12 via `fault=tc_open` in a sim-plant build. Red graph mark |
| **7** `FROZEN` | A sensor that still answers on SPI but has stopped converting — a bit-identical reading forever | Sim: `SIM_ZONE_FAULT_TC_FROZEN` (latches the reading at injection time), duty 0.2 — deliberately below `PROGRESS_DUTY_MIN` so guard 1 cannot claim it first. Unit: duty 1.0, measurement pinned at 300 °C; plus a non-trip case where the reading moves 0.5 °C/tick | Reading bit-identical (`!=` on the float, no tolerance) for `FROZEN_WINDOW_S` (600 s) **while `commanded_duty > 0`**. Duty going to 0 clears the window | 600 s. `test_sim_kiln.c` asserts ≤ 620 s, i.e. at the window and not later. **On-target 2026-08-12 the frozen sensor was caught by guard 1, not this guard**: injecting `fault=tc_frozen` on a heating zone tripped **guard 1 at 481 s**, `fault_guard: 1`, reason `"heating but rose only 0.0C in 5.0min"`. That is correct behaviour — a frozen reading is flat while heat is commanded, which is exactly guard 1's no-progress signature, and guard 1's window (300 s) is half this guard's (600 s). Guard 7's reachable window is therefore narrow: a zone holding at setpoint with commanded duty above 0 but below `PROGRESS_DUTY_MIN` (0.5), where guard 1 does not engage. **Guard 7 has still never been observed firing** | **Per-zone** | Zone `fault_reason` `"reading unchanged at X°C for 600s while duty > 0"`, zone `fault_guard: 8`, that zone blocked; red graph mark |
| **8** `CROSS_ZONE` | One zone's reading implausible given its neighbors' — the thermocouple that fell out of the kiln body while its own elements really are heating, so guards 1 and 2 stay satisfied | Sim: two zones with inter-zone conductance 3.0, both driven at duty 1.0, `SIM_ZONE_FAULT_TC_DETACHED` on zone 0 — zone 1 climbs, zone 0 reads near-ambient. Peer arrays come from the same tick's snapshot (`profile_executor.c` passes `raw_c` / `sensor_ok` / `MAX31856_CHANNEL_COUNT` / this zone's index straight through). **Not reachable in a shipped build** — see the threshold column | `\|measurement_c − peer_c[i]\|` against the **worst** disagreeing peer (not an average: with three zones an average would let one badly wrong channel hide behind a healthy one), sustained past `cross_zone_period_s` (0 → `CROSS_ZONE_PERIOD_S_DEFAULT` = 600 s). Any tick back inside the band resets the timer. Peers with `peer_ok[i] == false` are skipped, and the zone's own slot (`peer_index_self`) is excluded. **`cross_zone_max_delta_c` has no default — 0 means off, and 0 is what ships**: `profile_executor.c` sets it to 0 explicitly because the threshold is supposed to come from a measured cross-gain matrix that no hardware has ever produced, and nothing on the settings page edits it. The guard is also inert with `peer_count == 0` (single-zone run) | 600 s of unbroken disagreement, once armed. Not asserted by time in the host test — only *which* guard fired. **Not observed on hardware, and not observable** while the threshold ships at 0 | **Per-zone.** `relay_authority_set_zone_blocked(zone, true)`, that zone's relay forced off, run continues on the other zones. No global fault bit (`escalate_guard_trip()` lists only RUNAWAY/MAX/MIN/SENSOR_INVALID as global) | **Nothing today, because the guard ships disabled.** If a threshold were configured: zone `fault_reason` `"X°C differs from zone N's Y°C by Z°C (>W°C) for 600s"`, zone `fault_guard: 9`, that zone blocked, red graph mark |
| **9** control-tick liveness | The control task itself dying or stalling | **Not a `thermal_guard` verdict at all.** `watchdog_task_entry()` in `profile_executor.c` — a second, independent FreeRTOS task, because "a control loop cannot be its own watchdog" (TODO.md 6A.3/6A.7). Not exercised by any host test; it needs FreeRTOS, which the pure-C host build deliberately does not link | Checks every `WATCHDOG_CHECK_PERIOD_MS` (2000 ms); fires when the control task's last tick is older than `WATCHDOG_TICK_DEAD_MS` (10 000 ms) | 10–12 s after the control task stops ticking. **Not yet observed on hardware** | **GLOBAL**, but via a different path and a different bit: `kiln_io_all_relays_off()` directly, plus `safety_link_set_fault_source(SAFETY_FAULT_SRC_APP)`. It does **not** go through `escalate_guard_trip()` | `state: "faulted"` with `fault_reason` `"control task tick stale for Nms"` — but **`fault_guard` stays 0** (`TRIP_NONE`), because no `thermal_guard_trip_t` value exists for this. No red graph mark either, since the history sampler is the dead task |

## Provoking a guard on the board (simulated-plant build)

The "How it is provoked" column above is host-side. A firmware build with
`CONFIG_KILNCTL_SIM_PLANT` set (`idf.py menuconfig` → **KilnCtrl Hardware
Configuration** → **Simulated plant (development only)**) compiles the same
`App/test/sim_plant.c` model into the image behind `App/drivers/sim_backend.c`
and exposes fault injection over HTTP, which is how these same failure modes
are walked end to end on real firmware — start a profile, inject, watch the
trip escalate, reach the display, and be cleared. This is exactly how the
2026-08-12 walk was done: PC joined to the board's fallback AP, faults injected
over `POST /api/sim`, trips read back over `GET /api/profile_exec`.

Injection is a form-encoded POST:

```
POST /api/sim   zone=<n>&fault=none|element_dead|relay_welded|tc_detached|tc_frozen|tc_open
POST /api/sim   swap=<a>,<b>     exchange which physical thermocouple zones a and b read
GET  /api/sim   -> per zone: true element temperature, reported reading, relay state, injected fault
```

`swap=a,b` is a second, zone-pair form rather than a per-zone `fault=` value:
it was added 2026-08-12 because the per-zone fault list cannot express a
miswire — a miswire is a property of a *pair* of channels, not of one channel.
It exchanges the sensor mapping so zone `a` reports zone `b`'s reading and vice
versa, both readings otherwise untouched, which is the "two channels swapped at
the connector" failure. This makes guard 2's usual scenario provokable on target
for the first time; what it actually trips is guard 1 (see below).

### Preconditions — read this before blaming a guard

Anyone repeating this walk needs both of the following, or **no zone will ever
heat** and every heat-dependent guard (1, 3, 4, 5, 7) will look broken when in
fact the safety system is working correctly. The first 2026-08-12 test run
failed on exactly this and looked like a guard problem for a while.

1. **Keep the UART PC link alive.** Relay-on is blocked globally while the link
   is idle: an idle link asserts `SAFETY_FAULT_SRC_PC_LINK`, and the executor
   then commands zero duty, so nothing heats and no rise-dependent guard can
   ever be provoked.
2. **Both boot faults must be scoped to sim.** The bench unit has no SX1509
   (raises `SAFETY_FAULT_SRC_APP`) and no RP2040 peer (raises
   `fault_on_link_loss`). `App/main.c` now demotes both, but only under
   `#if CONFIG_KILNCTL_SIM_PLANT` — so this only works in a sim-plant build, by
   design. In a real build these remain hard faults, as they should.

### The provocations, and what the 2026-08-12 walk observed

| Guard | On-target provocation in a sim build | Observed on-target 2026-08-12 |
|---|---|---|
| **1** `HEATING_FAILED` | `fault=element_dead` (or `fault=tc_detached`) on a zone the running profile is driving at duty ≥ 0.5 | **Tripped, 541 s** on a 400 °C-target profile. `fault_guard: 1`, reason ending `"heating but rose only -0.5C in 5.0min"`. Run-level message `"every active zone individually faulted"` — the per-zone escalation reaching run level once the last unfaulted zone went, exactly as the escalation column describes |
| **2** `WRONG_DIRECTION` | `swap=0,1` — the new zone-pair injection above, exchanging which physical thermocouple zones 0 and 1 read while the profile drives them. (Previously this was compile-time only, needing a non-identity `sensor_map[]` and a rebuild) | **Attempted — and guard 1 fired instead, at 478 s.** `fault_guard: 1`, reason `"heating but rose only 0.1C in 5.0min"`; zone 0's element was at 33.3 °C while zone 0 read zone 1's untouched 20.1 °C. **Guard 2 remains unobserved**, and this is the expected outcome, not a failure: guard 2 needs the reading above setpoint *and falling* under commanded heat, which a PID loop rarely produces because it commands zero duty above setpoint |
| **3** `RUNAWAY` | `fault=relay_welded` on a zone, then let the profile command that zone's duty to 0. The cleanest way to get duty 0 is a profile targeting a setpoint the zone is already at — 21 °C against the sim's 20 °C ambient, so the PID commands zero duty from the start | **Tripped, 160 s.** `fault_guard: 3`, reason `"heat commanded off 160s but temperature rose 6.0C (rate 2.30C/min) -- possible welded relay"`. This is the post-fix result; the first attempt exposed a bug — see below |
| **4** `DRIFT` | `fault=element_dead` on a zone that has already settled at setpoint and is holding at low duty (below `PROGRESS_DUTY_MIN` = 0.5, so guard 1 cannot claim it first). Note the duty precondition is doing all the work here — at any normal duty this same injection is a guard 1 trip | **Not attempted — still unobserved on hardware, and preempted in practice.** The two physical failures that produce a sustained excursion are claimed first by shorter windows: a dead element by guard 1 (measured at 541 s), a welded relay by guard 3 (measured at 160 s). Guard 4 is what remains after both, not the first responder to either |
| **5** `MAX_TEMP` / `MIN_TEMP` | **Not injectable.** There is no fault that forces a limit crossing; lower the zone's `max_temp_c` on the zones page below the temperature the profile is driving it to, and it heats through the ceiling | **Tripped, 283 s** with `max_temp_c` lowered to 25 °C on a heating zone. `fault_guard: 5`, reason `"25.0C >= max_temp_c 25.0C"` |
| **6** `SENSOR_INVALID` | `fault=tc_open` — the reading goes NaN and the executor's `sensor_ok` goes false for three consecutive ticks | **Tripped, 3 s.** `fault_guard: 7` (the enum offset), reason `"sensor invalid for 3 consecutive reads"` — the same output as the 2026-08-11 absent-hardware trip, now reproduced through the sim path |
| **7** `FROZEN` | `fault=tc_frozen` on a zone held at duty > 0; the reading latches at its injection-time value. To reach guard 7 rather than guard 1, that duty must also be *below* `PROGRESS_DUTY_MIN` (0.5) — i.e. a zone holding at setpoint, not one ramping | **Attempted on a heating zone — and guard 1 fired instead, at 481 s.** `fault_guard: 1`, reason `"heating but rose only 0.0C in 5.0min"`. Correct behaviour: a frozen reading under commanded heat *is* guard 1's no-progress signature, and guard 1's 300 s window is half guard 7's 600 s one. **Guard 7 remains unobserved** |
| **8** `CROSS_ZONE` | Set `z<i>_xzone` (Settings → Thermocouples & Zones, *Cross-zone plausibility*) on every zone in the run — the guard is inert at 0 — then run a two-zone profile and `fault=tc_detached` on one of them. Since 2026-08-12 the threshold can also be armed **mid-run** — config reload while running (TODO.md 6A.7) picks it up on the next tick and logs it as an operator action — but arming it before the start is the honest test | **Tripped, 600 s** (2026-08-12, threshold deliberately tight at 2 °C so the gap opened in minutes). `fault_guard: 9` on **both** zones: `"28.5C differs from zone 1's 20.5C by 8.1C (>2.0C) for 600s"` and its mirror image. Run then escalated to `state: "faulted"` — see the two-zone note below |
| **9** control-tick liveness | Not provokable through `/api/sim`. It requires starving the control task, which nothing in the sim backend can do | **Not attempted — still unobserved on hardware** (not provokable) |

### Guard 8 on exactly two zones takes both of them down

Observed on-target 2026-08-12, and not something the host tests showed: guard 8
is symmetric. Zone 0 disagrees with zone 1 by exactly as much as zone 1
disagrees with zone 0, so on a **two-zone** kiln both zones trip on the same
tick, each naming the other:

```
zone 0: "28.5C differs from zone 1's 20.5C by 8.1C (>2.0C) for 600s"
zone 1: "20.5C differs from zone 0's 28.5C by 8.1C (>2.0C) for 600s"
```

Guard 8 is a per-zone guard and blocks only its own zone — but with no
unfaulted zone left, `escalate_guard_trip()`'s "every active zone individually
faulted" path took the whole run to `state: "faulted"`, `fault_guard: 9`. So
the *effect* on a two-zone kiln is a global stop even though the *mechanism*
stayed per-zone. That is the right outcome (with one thermocouple lying and no
way to tell which, continuing to fire would be guessing), but the guard cannot
identify the bad channel on its own until there are three or more zones, where
the worst-disagreeing-peer comparison isolates the outlier.

A second, practical note from that run: **the threshold is read once, when the
run starts.** Config reload while running is unbuilt (TODO.md 6A.7), so arming
guard 8 on the zones page mid-firing does nothing until the next start.

### Guard precedence: which guard actually fires

The matrix's "what it catches" column reads as though each guard owns a failure
mode. It does not. Several guards watch the same physical failures from
different angles, and the one that fires is simply **the one whose window
expires first**. Three measured examples from 2026-08-12:

| Physical failure | Guard usually credited | Guard that actually fired | Why |
|---|---|---|---|
| Sensor stops converting (`tc_frozen`) on a heating zone | 7 `FROZEN` (600 s) | **1**, at 481 s | A frozen reading is flat while heat is commanded — that is guard 1's exact no-progress signature, and its 300 s window is half guard 7's |
| Two thermocouples swapped at the connector (`swap=0,1`) | 2 `WRONG_DIRECTION` (120 s) | **1**, at 478 s | Guard 2 needs the reading *above setpoint and falling* under commanded heat; a PID commands zero duty above setpoint, so that state is rare by construction. The swapped zone instead reads a cold, untouched sensor and looks like no progress |
| Dead element / welded relay on a settled zone | 4 `DRIFT` (600 s) | **1** at 541 s / **3** at 160 s | Both windows are shorter than guard 4's, and neither has to wait for the reading to leave a 25 °C hysteresis band first |

Two consequences worth stating plainly:

- **A guard being unobserved is not the same as a guard being broken.** Guards
  2, 4 and 7 are implemented, host-tested with hand-fed inputs, host-tested
  against the physical model, and correct — they are simply out-competed on
  target by a faster guard watching the same event. Their reachable windows are
  the leftovers: for guard 7, a zone holding at setpoint with duty above 0 but
  below `PROGRESS_DUTY_MIN`; for guard 2, a forced-duty case a PID does not
  normally produce; for guard 4, an excursion at low duty that is neither a
  no-progress nor a runaway. This is why the host tests have to pin duty at 0.2
  to see guards 4 and 7 fire at all — that duty precondition is not incidental
  test setup, it is the guard's whole remaining territory.
- **A row claiming a guard "catches X" should name which guard catches X
  *first*.** Where this file says a guard catches a failure, read it as "this
  guard is one of the guards that would eventually catch it" unless an observed
  on-target time is recorded next to it. The recorded times are the ground truth
  about precedence; everything else is a claim about the arithmetic.

The practical reading for anyone debugging a trip: do not conclude the wrong
guard fired because the `fault_reason` names a different failure than the one
you injected. Check the windows first. Defence in depth means overlapping
coverage, and overlapping coverage means the shortest window wins.

### What the 2026-08-12 on-target walk found

The walk was not just confirmatory — it found a real guard-3 bug that no host
test had caught.

Guard 3's **first** on-target trip reported
`"rose 4.3C (rate 257.74C/min)"`. That rate is nonsense for a plant heating at
roughly 2.4 °C/min, and the cause was an interval mismatch: the guard measured
the rise from the **start of the off-window** but divided it by the time since
the **settle window ended**. On the first tick past the settle window that
divisor is a single `dt`, so an arbitrarily small rise produces an arbitrarily
large rate.

The consequence was not cosmetic. Any zone idling at duty 0 and drifting up
slightly — a dwell, a neighbour's heat arriving, ordinary coasting after a ramp
— would trip a welded-contact fault. The guard 1 test case hit exactly that:
it tripped **guard 3** instead of guard 1.

The fix in `thermal_guard.c` latches a **separate rate baseline** when the
settle window ends, so the rise and the elapsed time now cover the same
interval. The 20 °C margin check still references the original duty-0 baseline,
which is the interval that check wants. Two host regression tests were added;
the suite is now **125 checks, all passing**. Re-running the same guard 3 case
afterwards gave 2.30 °C/min — matching the simulated plant's actual 2.4 °C/min
heating rate, which is the number that says the arithmetic is now right.

## What is actually covered by an automated test

Everything below runs in `kilnctl_host_tests.exe`
(`App/test/build_host_tests.ps1`, MSVC, no ESP-IDF). The suite stands at
**125 checks, all passing**, including the two guard-3 rate-baseline
regressions added by the 2026-08-12 on-target walk.

**Guard arithmetic — hand-fed inputs** (`run_test_thermal_guard()` in
`App/test/test_thermal_guard.c`): guards 1, 2, 3, 4, 5 (ceiling and floor),
6, 7 — each with a trip case, and guards 1, 3, 4, 7 additionally with an
explicit **non-trip** case (healthy heating, normal cooldown, never-settled,
a reading that is still moving). Plus `max_temp_c == 0` disabling the ceiling,
the guard-6 streak reset, latching behavior, and `thermal_guard_clear()`.

**Failure mode → guard, via a physical model**
(`test_guard_provocations()` in `App/test/test_sim_kiln.c`): guard 1 from both
a dead element and a detached thermocouple, guard 2 from swapped connectors,
guard 3 from a welded relay, guard 4 from an element dying at low duty after
settling, guard 5 from a lowered ceiling, guard 6 from an open (NaN)
thermocouple, guard 7 from a non-converting sensor, and guard 8 from a
detached thermocouple in a *coupled* chamber (both zones at duty 1.0, inter-zone
conductance 3.0, so the healthy zone climbs away from the detached one) — with
the test supplying `cross_zone_max_delta_c = 100 °C` itself, since the firmware
supplies no threshold. Plus negative controls: 1 °C of sensor noise on a healthy
heating zone trips nothing, and guard 8 stays silent on a healthy coupled kiln
with both zones at equal duty. This is the difference between "the arithmetic
works" and "this failure mode trips that guard."

Worth recording from writing that guard-8 negative control: a first version
drove the two zones at 1.0 and 0.6 duty, and in steady state those two
*perfectly healthy* zones settle more than 100 °C apart in this model. That is
not a guard bug — it is precisely why the threshold cannot be a hand-picked
constant and must come from a measured cross-gain matrix, and why the firmware
asks the operator for the number instead of shipping one. Equal duty is the
honest "healthy" case against a fixed band this tight.

Timing is asserted, not just the trip reason, on three rows: guard 1 ≤ 600 s
for a dead element, guard 6 at exactly the third read (0-indexed step 2), and
guard 7 ≤ 620 s (i.e. at its 600 s window, not later). The other rows assert
only *which* guard fired, not when.

Supporting model checks in the same file: `test_coupling()` (a neighbor's
elements really do heat an idle zone, and with zero conductance they do not),
`test_radiative_loss()` (the radiative term costs real ceiling temperature —
the temperature-dependent plant gain behind TODO.md 6A.4's gain-scheduling
rationale), and `test_cross_gain_matrix()` (TODO.md 6A.5(b)'s cross-gain
capture, fitted off-target against a plant whose coupling is known by
construction).

**Reasoned about but not automatically tested:**

- **Escalation itself.** `escalate_guard_trip()` lives in
  `profile_executor.c`, which links FreeRTOS/ESP-IDF and is not in the host
  build, so the global-vs-per-zone column is still read off the source rather
  than asserted by any test. It is no longer unobserved, though: the 2026-08-12
  on-target walk exercised it five times — global escalation for guards 6, 5
  and 3, and per-zone escalation for guards 1 and 8, whose run-level
  `"every active zone individually faulted"` message is the per-zone path
  reaching run level once the last unfaulted zone went. Guards 2, 4, 7 and 9
  have had their escalation paths read but never run — and for 2, 4 and 7 that
  is because a faster guard escalated first, not because the scenario went
  untried.
- **Guard 9 entirely.** Same reason — it is a FreeRTOS task, and there is no
  host harness that can starve the control task.
- **Guard 8 on a kiln nobody armed.** The guard was walked on hardware
  2026-08-12 and trips exactly as designed, but only because the test set a
  threshold first. `cross_zone_max_delta_c` still defaults to 0, and at 0 the
  guard never evaluates. What remains untested is not the mechanism but the
  *number*: no measured cross-gain matrix exists, so no one yet knows what
  threshold a real kiln should carry, and a wrong one either nuisance-trips a
  firing or is too wide to catch anything.
- **The operator-facing column.** `fault_reason` strings are quoted from the
  `trip()` format strings in `thermal_guard.c`; the dashboard behavior is read
  from `drawHistoryChart()` and the exec-card renderer in
  `App/drivers/main_page.html` (the red vertical line is drawn for any history
  sample whose `guard` CSV field is non-zero). Five guards' end-to-end paths —
  trip → `state: "faulted"` (or per-zone FAULTED, for guard 1) → `fault_guard`
  and `fault_reason` over `/api/profile_exec` — have now been walked on
  hardware: guards 6, 5, 3, 1 and 8, on 2026-08-12. The observed `fault_reason`
  strings in the tables above are copied from that walk, not from the format
  strings. The dashboard rendering itself was not separately checked, and
  guards 2, 4, 7 and 9 remain quoted from source only — including the two
  whose failure modes *were* injected on target (2 and 7), since guard 1
  answered both and so guard 1's strings are what appeared.
- **Anything involving a real relay or a real thermocouple.** Every sim row is
  a statement about the model plus the guard logic — and this is still true of
  the on-target walk, which ran against `CONFIG_KILNCTL_SIM_PLANT`. The model
  was written to make these failures provokable, and it can be wrong about a
  real kiln in ways neither a host test nor a sim-plant bench run will ever
  notice.

**Coverage summary as of 2026-08-12:**

| Guard | Host test | On-target trip |
|---|---|---|
| **1** `HEATING_FAILED` | yes (arithmetic + model) | **yes — 541 s, 2026-08-12** |
| **2** `WRONG_DIRECTION` | yes (arithmetic + model) | no — **preempted**: `swap=0,1` injected on target, guard **1** fired at 478 s |
| **3** `RUNAWAY` | yes (arithmetic + model, + 2 new regressions) | **yes — 160 s, 2026-08-12** (and found a bug) |
| **4** `DRIFT` | yes (arithmetic + model) | no — **preempted in practice** by guard 1 (dead element, 541 s) and guard 3 (welded relay, 160 s); not separately attempted |
| **5** `MAX_TEMP` / `MIN_TEMP` | yes (arithmetic + model) | **yes — 283 s, 2026-08-12** |
| **6** `SENSOR_INVALID` | yes (arithmetic + model) | **yes — 3 s, 2026-08-12** (plus the 2026-08-11 absent-hardware trip) |
| **7** `FROZEN` | yes (arithmetic + model) | no — **preempted**: `fault=tc_frozen` injected on target, guard **1** fired at 481 s |
| **8** `CROSS_ZONE` | yes, but only with a threshold the test supplies | **yes — 600 s, 2026-08-12**, once the threshold became settable; still off (0) unless an operator arms it |
| **9** control-tick liveness | no — needs FreeRTOS | no — requires starving the control task |

So: five guards proven on real firmware (1, 3, 5, 6, 8); three implemented,
host-tested, and **preempted on target** by a shorter-window guard when their
failure mode was actually injected (2, 7) or when that failure mode's faster
claimant was measured (4); and one not provokable at all (9). The plant
underneath every on-target trip was simulated.

The distinction matters when reading this table: "no" in the on-target column
means two different things. For guards 2, 4 and 7 it means *another guard got
there first*, which is the safety system working. For guard 9 it means no
harness exists to starve the control task. Neither is a gap in coverage — but
guard 8 being *armable* is not the same as guard 8 being *armed*: a kiln whose
`cross_zone_max_delta_c` is left at 0 has no cross-zone protection, proven
mechanism or not.
