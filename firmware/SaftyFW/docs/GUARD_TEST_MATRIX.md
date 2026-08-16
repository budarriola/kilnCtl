# Guard Test Matrix

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** Add a row whenever a guard is added or a threshold
> moves, and record results as they are obtained — this file is the evidence
> that the safety case is real. Checklist at the bottom.

How each guard gets provoked, on the host and on real hardware, and what
"verified" means for it.

Modelled on `KilnFW/docs/GUARD_TEST_MATRIX.md`. **A guard that has only ever
passed a host test is not commissioned** — the host tests prove the logic, the
hardware tests prove the wiring, and most of the failures worth catching here
are wiring.

---

## 1. Write the nuisance tests first

This is not a stylistic preference. `SAFETY_MODEL.md` §2's whole argument is
that a guard which fires on a healthy kiln gets bypassed, and the only way to
know a guard does not do that is to run a healthy kiln at it.

So for every guard, the **first** test written is the one that must *not* trip:

| Guard | The healthy scenario that must produce no trip |
|---|---|
| S1 | A profile soaking 5 °C below `abs_max_temp_c` for two hours |
| S2 | A 40 °C overshoot at the end of a fast ramp, decaying over 90 s |
| S3 | **A 60 s heater window at 15 % duty, for an hour** — the single most important nuisance test in the suite |
| S4 | The same, plus every duty from 5 % to 95 % |
| S5 | A 900 ms sensor dropout; a single failed SPI transfer; three non-consecutive bad reads |
| S6a | `mainFault` glitching for 100 ms |
| S6b | One dropped telemetry frame; three dropped frames with no current flowing |
| S7 | 30 ms of contact bounce on both edges |
| S8 | A legitimate full-power ramp at the measured maximum rate |
| S9 | A normal trip where current decays with the 1 s peak-hold time constant |
| S10 | A 150 °C stratification held for a whole firing (`CHAMBER_AGREED`) |
| S10 | **Any** disagreement at all, in `EXTERNAL_OVERHEAT` — the guard must be off |
| S11 | A cold idle kiln reading a constant value for six hours with no heat |
| S12 | An enclosure at 55 °C all day |

**S3's low-duty test is the one to write on day one.** It is the case the 60 s
heater window and the 1 s peak-hold conspire to break, and it is the reason
`correlation_window_s` is 150 s rather than something that "looks long enough".

---

## 2. Host tests

Pure `safety_guards.c`, MSVC, no SDK, no hardware — same setup as
`KilnFW/App/test/`. These are cheap, so they should be exhaustive.

| Guard | Provocation | Assert |
|---|---|---|
| S1 | Ramp the synthetic reading through the ceiling | Trips on the **3rd** consecutive reading above, not the 1st or 2nd |
| S1 | `firing_max_c` = 900 in `CHAMBER_AGREED` | Ceiling becomes 1000, not `abs_max` |
| S1 | `firing_max_c` = 5000 | **Clamped to `abs_max`** — the ESP cannot raise the ceiling |
| S1 | `firing_max_c` = 900 in `EXTERNAL_OVERHEAT` | Ceiling stays `abs_max` — the field is ignored |
| S2 | Hold setpoint+80 for 119 s, then 121 s | No trip, then trip |
| S2 | Same, with context aged past `context_max_age_s` | **No trip** — inactive, not pessimistic |
| S2 | Same, in `EXTERNAL_OVERHEAT` | No trip, guard disabled |
| S3 | Current present, `relay_recent_mask` = 0, for 21 s | Trip |
| S3 | Current present, relay commanded on 100 s ago (inside the window) | **No trip** |
| S4 | Relay on for the whole window, no current | **WARN, never TRIP** — assert the relay state is untouched |
| S5 | 9 bad reads, then a good one | No trip, streak resets |
| S5 | 10 bad reads over 4 s (fast polling) | **No trip** — the 5 s floor also has to be met |
| S5 | Bad reads sustained 61 s | WARN at 5 s, TRIP at 60 s |
| S6b | Link dead 11 s, no current | WARN only |
| S6b | Link dead 11 s, current present | **TRIP** |
| S6b | Link dead 121 s, no current | **TRIP** — the unconditional backstop |
| S8 | Rate above threshold with `max_rate_c_per_min` = 0 | **No trip** — disabled means disabled |
| S9 | Trip, then current persists 11 s | `TRIP_INEFFECTIVE` |
| S9 | Trip, current decays with τ=1 s | No escalation |
| S10 | Disagreement in `EXTERNAL_OVERHEAT` | Guard inactive |
| S11 | Constant reading 601 s **with heat** | Trip |
| S11 | Constant reading 601 s **with no heat** | **No trip** |
| All | Any guard, during `startup_grace_s` | Evaluated and reported, **relay never energized, no latch** |
| All | Trip, condition clears, no clear command | **Still tripped** — latching |
| All | `CLEAR_TRIP` while condition is still true | **Refused** |
| All | `CLEAR_TRIP` with a mismatched `trip_mask` | **Refused** |
| All | Guard verdicts with the TX path stubbed out | **Bit-identical** to a live-TX run |

Drive the thermal ones from `KilnFW/App/test/sim_plant.c` — it already models
element lag, sensor transport delay and radiative loss, so the traces are
realistic rather than synthetic ramps.

### Two properties worth testing directly rather than by example

**Monotonicity of the ceiling.** For any `firing_max_c`, the effective ceiling
must be ≤ `abs_max_temp_c`. Property-test it over the whole float range,
including NaN and infinities — the value arrives from another processor over a
wire and `min()` with a NaN does not do what you want in C.

**No guard reads a disabled input.** With `mains_voltage_v` unset,
`p_avg_w` is NaN; assert no verdict changes. Same for the clock.

---

## 3. Hardware tests

Ordered so nothing dangerous is attempted before the safe state is proven.

### 3.1 Safe state — before anything else

| Test | Method | Pass |
|---|---|---|
| Power-on | Apply `12v_Safty` | K4 de-energized, contactor open, from the first millisecond |
| Unprogrammed Pico | Blank flash | K4 de-energized |
| Watchdog reset | Deliberately hang a task | Reset, K4 de-energized, `boot_reason` bit 1 set |
| Halted at a breakpoint | SWD halt | K4 de-energized *or* watchdog fires — never left energized |
| Rail loss | Remove `12v_Safty` mid-firing | Contactor opens |

**Interlock polarity.** With K4 de-energized, confirm the contactor coil is
**open** on the real wiring. This is the test that catches an interlock landed
on the wrong J10 contact, and it passes every other test if you skip it
(`HARDWARE.md` §3).

### 3.2 Sensors

| Test | Method | Pass |
|---|---|---|
| Thermocouple reads | K/S-type at ambient | Within a few °C of a reference |
| Open circuit | Unplug the thermocouple | `THERMO_FAULT_OPEN`, **NaN reported, not 0** |
| Stopped converting | Hold `~CS` / halt the part | **`~DRDY` silence detected** — the failure `KilnFW` structurally cannot see |
| Type match | Known soak vs a reference instrument | Agreement. Catches a `tc_type` mismatch, which is otherwise silent (`THERMOCOUPLE.md` §2) |
| Cold junction | Warm the enclosure | S12 WARN at 60 °C |
| E-stop | Press, release | Trip in <100 ms; **and disconnect the cable** — must read as stop |
| `mainFault` | Assert from the ESP | Trip within ~200 ms |

### 3.3 Current — the mapping check gates everything

Run `CURRENT_SENSE.md` §5 in full. Step 2 — **one relay at a time, confirming
exactly one channel responds and that it is the expected one** — is the gate for
enabling S3 and S4, and skipping it produces a correlation guard that trips on
healthy firings and stays silent on the failure it exists to catch.

| Test | Method | Pass |
|---|---|---|
| Zero | All off, 10 min | Stable, small, positive |
| Mapping | One relay at a time | Exactly one channel, the right one |
| Decay | Relay off | <5 % within ~4 s (τ ≈ 1 s) |
| Low duty | 15 % duty, 60 s window, 1 h | **No S4 warning storm** |

### 3.4 Trips — provoke each one that is enabled

With the kiln empty and someone present.

| Guard | How | Watch for |
|---|---|---|
| S1 | Temporarily lower `abs_max_temp_c` below the current reading | Trip, K4 drops, **S9 confirms current stopped** |
| S3 | Command a relay on directly at the contactor while reporting all off | Trip |
| S6b | Unplug the ESP mid-firing | Heat blocked at 1.5 s, firing aborted at 30 s |
| S7 | Press the E-stop | Immediate trip |
| S9 | **Bypass the contactor** so current continues after a trip | `TRIP_INEFFECTIVE`, and confirm the GUI treats it differently from every other trip |

Restore every temporarily-lowered threshold afterwards, and **re-read the config
CRC from telemetry to prove it** — a test threshold left in place is the most
plausible way this system ends up quietly unprotected.

---

## 4. Recording results

For each guard: date, firmware commit, config CRC, what was done, what happened,
and pass/fail. `KilnFW/docs/PROJECT_STATUS.md` is the model — it distinguishes
"built", "compiles", "live-verified on hardware", and it is scrupulous about
which is which.

Be equally scrupulous here. `SAFETY_MODEL.md`'s summary table should carry a
verification state per row, and a guard that is implemented but unprovoked
should say so rather than being listed as coverage.


---

## Completion checklist

**Host**
- [ ] Nuisance-rejection tests written **before** trip tests, all of §1
- [ ] §2's full provocation table implemented and passing
- [ ] Property tests: ceiling monotonicity over the float range incl. NaN/Inf
- [ ] Fuzz over every decoder (`CommonFW/test`)

**Hardware — safe state first**
- [ ] §3.1 all five rows passed
- [ ] **Interlock polarity proven on the real wiring**

**Hardware — sensors and current**
- [ ] §3.2 all rows passed
- [ ] §3.3 mapping check passed on all three channels
- [ ] S3/S4 enabled only after the mapping check passed

**Hardware — trips**
- [ ] Every *enabled* guard provoked at least once on hardware and the result recorded
- [ ] `TRIP_INEFFECTIVE` provoked deliberately (contactor bypassed) and the GUI treatment confirmed distinct
- [ ] **All temporary test thresholds restored, verified via the config CRC in telemetry**

**Records**
- [ ] Date, commit, config CRC and outcome recorded per guard
- [ ] `SAFETY_MODEL.md`'s summary table updated with per-row verification state
