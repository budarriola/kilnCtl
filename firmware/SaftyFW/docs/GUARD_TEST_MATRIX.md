# Guard Test Matrix

> **Status:** planning · **Last reviewed:** 2026-08-21
> **Keep this file current.** Add a row whenever a guard is added or a threshold
> moves, and record results as they are obtained — this file is the evidence
> that the safety case is real. Checklist at the bottom.

How each guard gets provoked, on the host and on real hardware, and what
"verified" means for it.

Modelled on `firmware/KilnFW/docs/GUARD_TEST_MATRIX.md`. **A guard that has only ever
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
| S5 | A 900 ms sensor dropout; a single failed SPI transfer; three non-consecutive bad reads; any reading inside the plausibility band that applies given commissioning status (2026-08-24, see note below); a `max31856_configure()` CR1 readback that confirms the intended type (2026-08-24, see note below) |
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

**S5's per-`tc_type` plausibility band (2026-08-24, TODO.md Phase 3)** is
tested one layer upstream of `safety_guards.c` itself, so it does not appear
in that file's own host-test scenario suite (§2 below): `thermo_task.c`
downgrades `thermo_snapshot_t.valid` to false *before* the reading ever
reaches `safety_guard_input_t.tc_valid`, using `max31856_tc_range_policy.c`.
**Updated 2026-08-24:** `config_store`'s `tc_type` now carries its own
commissioning bit (`CONFIG_STORE_SET_TC_TYPE`), so which band applies
depends on `config_store_is_tc_type_set()`: a genuinely commissioned type
gets `max31856_tc_range_is_plausible()` (its own exact datasheet band); an
uncommissioned one gets `max31856_tc_range_is_plausible_uncommissioned()`
(the union of all eight types' ranges, a garbage floor). Both functions
(all 8 real types, inclusive boundaries, NaN, unrecognised type, the union
band's own bounds, and the specific behavioural difference between the two —
a value the union band accepts that a single type's band would reject) are
exhaustively host-tested in `test/test_max31856_tc_range_policy.c`, against
ranges taken from `firmware/KilnFW/Datasheets/MAX31856.pdf` page 12, Table 1
"Supported Thermocouples and Temperature Ranges" (TEMP RANGE column) — see
that policy file's own header comment for the full citation and argument.
Not yet hardware-verified — no MAX31856/RP2040 on any bench this was built
on.

**S5's CR1 readback verification (2026-08-24, same pass)** is also tested
one layer upstream: `max31856_configure()` (`max31856.c`) reads CR1 back
once, right after writing it, and caches whether TC TYPE[3:0] matched what
it wrote; `thermo_task.c` checks that cached result
(`max31856_tc_type_verified()`) on every reading, downgrading
`thermo_snapshot_t.valid` on a mismatch exactly like the plausibility band.
The pure classification (`max31856_cr1_readback_check()`) — MATCH for every
real type at the exact byte this driver writes, MISMATCH for a different
real type, and DEAD_BUS for a readback of `0x00`/`0xFF` (checked to take
priority over MISMATCH even when the intended type's own low nibble
happens to be `0x0`) — is exhaustively host-tested alongside the band
above, in the same file. Also not yet hardware-verified.

---

## 2. Host tests

Pure `safety_guards.c`, MSVC, no SDK, no hardware — same setup as
`firmware/KilnFW/App/test/`. These are cheap, so they should be exhaustive.

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
| S8 | Rate above threshold with `max_rate_c_per_min` = 0 | **No trip** — disabled means disabled. Host-tested 2026-09-03 (`test_s8()`, `test/test_safety_guards.c`) |
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

Drive the thermal ones from `firmware/KilnFW/App/test/sim_plant.c` — it already models
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

**Extended 2026-09-03** (see the note at the end of §6, "§3.4 scope note"): the
table below predated the S6a/S9/S11 reachability wiring and §6a's reachability
re-derivation, and never covered S10, S11, S13 or S14 at all. §6c's
recomputation puts 11 of 14 implemented guards structurally reachable
(S8 was unimplemented at that recomputation's own time; S1/S13/S14
deliberately configured off by default, §6 category (d)) — this table now has
a row, or an explicit refusal, for every one of those 11.

**S8 gained a pure-module implementation the same day** (`safety_guards.c`,
this file's own §3.4 row below is updated accordingly) but is **still not
integrated**: `safety_core_load_guard_cfg()` does not yet copy
`max_rate_c_per_min`/`rate_window_s` out of `config_store` into the guard
config the pure module reads, so on real hardware today it still evaluates
against the zero-initialised default regardless of what an operator
commissions — the guard stays inert exactly as it did before this pass,
just for a slightly different reason (uncopied config rather than absent
code). This is the same "built and host-tested" vs. "integrated" distinction
`SAFETY_MODEL.md`'s completion checklist already tracks for every other
guard, applied to a brand-new one instead of a regression in an old one.

**Read each row's Precondition column before touching anything.** Several
guards are deliberately shipped inert and must be armed first — restore every
value listed there to its prior state afterwards, in the same way the
existing restore rule below already requires for a temporarily-lowered
threshold. Do not attempt any row below while the live A/B firing experiment
is running: every stimulus here either drops K4 or requires a commissioning
write, both of which end that run.

| Guard | Precondition | Stimulus | Expected observable | Safe order / restore |
|---|---|---|---|---|
| S1 | `abs_max_temp_c` must already be commissioned (non-zero) — it defaults to 0 = "never trip" | Temporarily lower `abs_max_temp_c` below the current reading | Trip (`SAFETY_TRIP_OVERTEMP`) on the 3rd consecutive over-ceiling reading (~300 ms), K4 drops, **S9 confirms current stopped** | K4 must be the only thing energized when the threshold is lowered — no other guard mid-provocation. Restore `abs_max_temp_c` afterwards and confirm via the config CRC in telemetry |
| S3 | `ct_installed = yes` and the CT mapping check (§3.3) already passed for the channel under test | Command a relay on directly at the contactor while KilnFW reports all relays off (or physically weld/jumper a contactor closed) | Trip (`SAFETY_TRIP_LOAD_STUCK_ON`) after `stuck_on_time_s` (default 20 s) of current with nothing commanded, operator-clearable once current stops | De-energize the forced relay **before** issuing `CLEAR_TRIP`, or the guard's own immediate-recheck (`guard_condition_still_immediate()`) correctly refuses the clear |
| S6a | None — always active, no commissioning gate | Momentarily short the mainFault opto output (SaftyFW GPIO10, active low, R8 pull-up) to ground, simulating the ESP asserting its own fault line | Trip (`SAFETY_TRIP_MAIN_FAULT`) within ~200 ms (debounce + one tick), K4 drops immediately; **not** software-clearable without the short first being removed | This is the one guard `virtual_dut`/`SimFW` never could exercise (no I2C-expander/opto emulation) — real hardware is the only way to observe it at all, per §6/§6a. Confirm the short is fully removed before `CLEAR_TRIP` |
| S6b | None for the hard backstop; the soft (current-gated) tier additionally needs `ct_installed = yes` | Unplug the ESP mid-firing | Soft tier: trip (`SAFETY_TRIP_LINK_DEAD`) once link silence exceeds `link_timeout_s` (default 10 s) **with current present**; hard backstop: unconditional trip at `link_dead_hard_s` (default 120 s) regardless of current | Reconnect the ESP only after confirming K4 dropped — reconnecting early can mask whether the guard actually fired |
| S7 | None | Press the E-stop | Immediate trip (`SAFETY_TRIP_ESTOP`), no debounce beyond the existing 50 ms | Release the E-stop before `CLEAR_TRIP` — same immediate-recheck shape as S3 |
| S9 | `ct_installed = yes`, current sensing **commissioned** (`current_sensing_commissioned` true — real calibration loaded, not just CT fitted), and some other guard already tripped so K4 has been commanded open | **Bypass the contactor** so current continues after K4 opens | `SAFETY_TRIP_INEFFECTIVE` once `trip_verify_s` (default 10 s) has elapsed **and** 3 consecutive ticks (~300 ms) of `any_current_present` are seen — **never operator-clearable**, refused unconditionally by both `safety_guards_try_clear()` and `link_frame_decide_clear_trip()`; confirm the GUI/log renders this distinctly from every other trip code | **This is the guard the task brief flagged as a known hard case, and it holds on inspection**: `any_current_present` is a real analog CT reading gated behind `context_valid`, `!current_sensing_disabled` and `current_sensing_commissioned` (`safety_guards.c:363-389`) — it cannot be faked digitally, and SimFW/kilnsim (the only software path that ever synthesized this input) were deleted 2026-08-28. **Provoking this on the bench needs a jig that injects real AC current through the CT loop while independently confirming K4's coil drive line is de-energized** (e.g. an ammeter or scope probe on the K4 coil itself, not just on the software status) — the bypass step already implies exactly this jig, since "bypass the contactor" means routing mains current around the open K4 contacts on purpose. Treat the bypass wiring as live mains for the whole test: de-energize and remove the bypass jumper immediately after the observable is recorded, before any restore step below, and before touching the breaker |
| S10 | `tc_placement_mode = CHAMBER_AGREED`, a live KilnFW context with `zone_count > 0` | Physically decouple the safety thermocouple from the chamber — e.g. clamp its junction to an external, independently-heated mass (heat gun on the junction alone, away from the elements) so it disagrees with the nearest zone's chamber reading by more than `tc_disagreement_c` (default 200 °C) | **WARN only** — `s10_warn` sets after `tc_disagreement_time_s` (default 300 s) of sustained disagreement; there is no `SAFETY_TRIP_*` for S10, so K4 is never affected and nothing needs clearing | Safe by construction (WARN-only, no relay effect). Return the safety TC to its normal chamber position afterwards and confirm the WARN clears on its own — it is non-latching |
| S11 | `ct_installed = yes` and current sensing commissioned, so `heat_commanded` (= `any_current_present`) can go true for real | Physically isolate the safety thermocouple's junction in a large ambient thermal mass (wrapped away from the elements, or clamped in an unheated metal block) so it reports a genuinely constant, valid reading while a normal firing runs and current actually flows | Trip (`SAFETY_TRIP_FROZEN_SENSOR`) once the identical reading persists for `frozen_window_s` (default 600 s = 10 min) with heat commanded throughout; **do not** try to provoke this by stalling the MAX31856's conversions (halting `~CS`/`DRDY`) — that reads as `spi_failed`/stale and trips S5 first, never reaching S11's own condition | Requires a genuine ~10 minute hold with heat on — budget bench time accordingly. Remove the thermal isolation and confirm the reading tracks the chamber again before `CLEAR_TRIP`; the clear is refused (`guard_condition_still_immediate()`) while the reading is still frozen at the value it tripped on |
| S13 | **Commissioning gap, must be armed first**: `tc_source` set to `SAFETY_TC_SOURCE_BORROWED_ZONE` (or `BOTH`) and `borrowed_zone_index` set to the specific KilnFW zone (0..2) under test — both default off (`OWN_J7`, category (d) in §6c) | Stall or unplug **that specific zone's** thermocouple on the main board (same physical technique as §3.2's "Stopped converting" row), while the link and every other zone stay healthy so `context_valid` stays true | Graduated like S5: `s13_warn` at `borrowed_stale_s` (default 10 s), trip (`SAFETY_TRIP_BORROWED_STALE`) at `borrowed_stale_trip_s` (default 60 s) once KilnFW's own `sample_counter` for that zone stops incrementing (`safety_link_frames.c`: increments only when a fresh, non-stale conversion is consumed) | Reconnect/unstall only the one zone's thermocouple used for the test. Restore `tc_source` and `borrowed_zone_index` to their prior (or intended production) values afterwards and confirm via the config CRC in telemetry, same as any other temporarily-changed commissioning field |
| S14 | Per channel: `i_normal_a[ch]` commissioned (`i_normal_valid[ch]` true, a real measured baseline) and `ct_installed = yes`; inert on a `ct_installed = no` board (§9) | With the channel's relay commanded on, add a known extra load in the same CT-monitored leg (a second heater or resistive load clamped in parallel through the same loop) to push measured current above `overcurrent_pct` (default 150 %) of the commissioned normal | **WARN only**, per channel — `s14_warn[ch]` sets after `overcurrent_time_s` (default 30 s) of sustained overcurrent; no relay effect, non-latching | Safe by construction. Remove the extra load and confirm the per-channel WARN clears on its own |

**S8 is now integrated (2026-09-03) but still cannot be provoked on the
bench — the reason changed again, to the one this document's other rows
share.** `safety_guards.c` implements the pure guard (two-window
average-rate design, `SAFETY_MODEL.md` §4's S8 section has the full
reasoning) and is host-tested (`test_s8()`). `safety_core_load_guard_cfg()`
(`safety_core.c`) now copies `max_rate_c_per_min`/`rate_window_s` from
`config_store` into the guard config — the same "producer without consumer"
shape S1's 2026-08-27 audit found is fixed for S8 too, and
`test_safety_core_s8_wiring.c` proves the whole chain (a value staged
through config_store's real `SET_PARAM` path changes `safety_guards_tick()`'s
verdict) and the still-inert default (an uncommissioned board runs the same
implausible ramp and does not trip). What remains before a bench provocation
is purely the design's own precondition, unchanged by this wiring pass: this
repo has no logged full-power ramp on any real kiln to set
`max_rate_c_per_min` from, so the guard ships genuinely off
(`config_store_default()`'s `0.0f`) until an operator measures one and
commissions a threshold at roughly 2× it, per `SAFETY_MODEL.md` §4's own
guidance.

Restore every temporarily-lowered threshold or commissioning field afterwards,
and **re-read the config CRC from telemetry to prove it** — a test threshold
left in place is the most plausible way this system ends up quietly
unprotected.

---

## 4. Recording results

For each guard: date, firmware commit, config CRC, what was done, what happened,
and pass/fail. `firmware/KilnFW/docs/PROJECT_STATUS.md` is the model — it distinguishes
"built", "compiles", "live-verified on hardware", and it is scrupulous about
which is which.

Be equally scrupulous here. `SAFETY_MODEL.md`'s summary table should carry a
verification state per row, and a guard that is implemented but unprovoked
should say so rather than being listed as coverage.


---

## Completion checklist

**Host**
- [ ] Nuisance-rejection tests written **before** trip tests, all of §1.
      Audited row by row against `test/test_safety_guards.c` (2026-09-04).
      Twelve of §1's fourteen rows were already covered, several of them
      exactly (S1, S2's 40C/overshoot_margin_c-implied-by-90s-decay case
      via the "worst case is the sustained value" argument, S5's five
      sub-cases verbatim, S6b's soft/hard backstops, S8's legitimate-ramp
      case, S9's post-trip nuisance case, S11, S12, S13's implied stability).
      Two genuine gaps were found and closed, in a new file --
      `test/test_guard_nuisance.c` (registered in `test_main.c` and
      `build_host_tests.ps1`, `safety_guards.c/.h` and
      `test_safety_guards.c` untouched, per a concurrent session holding
      those): S3/S4's "a 60s heater window at 15% duty, for an hour" --
      this document's own words for "the single most important nuisance
      test in the suite" -- run verbatim as a real 3,600s tick-by-tick
      simulation (dt_s=0.1s) with a genuine 60s on/off cycle, a
      current-decay tail (CURRENT_SENSE.md section 5's tau~=1s), and a
      correlation-window computation matching LINK_PROTOCOL.md section 4's
      own description, swept across every duty from 5-95%; and S10's
      stated 150C magnitude (the existing test used 50C) held for a
      simulated 8-hour firing, in both `CHAMBER_AGREED` and
      `EXTERNAL_OVERHEAT`. Negative-tested per
      `feedback_negative_test_every_check.md`: with the S3/S4 harness's
      recency computation deliberately forced to `false` (simulating the
      real regression class -- a dropped or inverted `relay_commanded_
      recently` computation) it failed loud, naming the guard:
      `FAIL test_guard_nuisance.c:298: INTENTIONALLY-BROKEN recency
      (relay_commanded_recently forced false) must still be reported as a
      failure by this check, not silently pass`; with S10's threshold
      dropped from 200C to 100C (so the 150C stratification legitimately
      clears it) it also failed loud: `FAIL test_guard_nuisance.c:272: 150C
      is still under tc_disagreement_c(200C) -- no WARN either`. Both
      injections reverted, suite clean again (2101/2101, up from 2077/2077
      before this pass -- the 24 added checks are `test_guard_nuisance.c`'s
      own; the payload-fuzz binary added the same day is unaffected and
      still separately reported ALL PASS).
      **Left unchecked, deliberately, because three rows remain genuinely
      uncovered on host, not vacuously "passing":** S6a ("mainFault
      glitching for 100ms") and S7 ("30ms of contact bounce on both
      edges") both need to exercise real glitch/bounce rejection, but
      `safety_guards_tick()` only ever receives an *already-debounced*
      level for both (`test_s6()`/`test_s7()`'s own comments say so). The
      actual debounce is `discrete_task.c`'s `static bool
      debounce_update(...)` (200ms window for mainFault, 50ms for E-stop) --
      `static`, called only from `discrete_task_fn()` which is gated on
      FreeRTOS + RP2040 GPIO headers, not declared in any header, and not
      referenced from anywhere under `test/`. There is no host-reachable
      entry point for it at all; giving it one is a real (and reasonable)
      refactor of `discrete_task.c`'s public surface -- the same treatment
      `relay_grace.c`/`link_frame.c` already got -- but it is a
      behavior-preserving change to a file nothing in this pass otherwise
      touches, not a side effect of a test-only pass, so it is left as a
      named follow-up rather than done implicitly here. S9 ("current
      decays with the 1s peak-hold time constant") is hardware-only by
      this document's own §3.4 S9 row: `any_current_present` is a real
      analog CT reading behind a physical peak-hold circuit, and the only
      software path that ever synthesized it (SimFW/kilnsim) was deleted
      2026-08-28 -- there is no decay model left to host-test against.
      S6b's "one dropped telemetry frame; three dropped frames with no
      current" row was also audited: `link_task_link_up()` is a pure
      elapsed-time check, not a frame-count check, so a few dropped frames
      are indistinguishable at `safety_guards_tick()`'s boundary from
      "link quiet for under a second" -- already covered by the existing
      115s-quiet-link case, no new test needed for that row.
- [x] §2's full provocation table implemented and passing, for every row that
      is a pure function of `safety_guard_input_t`/`safety_guard_cfg_t`
      (2026-08-19). Audited `test/test_safety_guards.c` row by row against
      this table. Found and closed two real gaps: S2's exact 119s/121s
      boundary (`overshoot_time_s` = 120s) wasn't pinned anywhere -- existing
      coverage proved the sustained-excess trip and the "brief excursion
      resets the timer" nuisance case, but not the boundary itself -- and
      S5's "9 bad reads, then a good one" case, which is the near-threshold
      version of the streak-reset property (one read short of the 10-read
      count bar) rather than the arbitrary single-bad-read case already
      covered. Both added as new sub-tests in `test_s2()`/`test_s5()`.
      Every other row was already covered on inspection: S1 (3rd-consecutive,
      firing_max_c tightening/clamping/EXTERNAL_OVERHEAT-ignoring), S2
      (stale-context inactivity, EXTERNAL_OVERHEAT disables), S3 (21s stuck,
      recently-commanded no-trip -- `relay_recent_mask` arrives pre-windowed
      as a boolean from the ESP, so "100s ago inside the window" and "any
      time inside the window" are the same input to this module), S4
      (WARN-never-TRIP; this guard cannot touch the relay at all -- it has no
      relay-output field, only `is_tripped`/`reason`, so "relay state
      untouched" is structurally guaranteed by S4 never setting either), S5
      (fast-burst count-without-time, graduated WARN-then-TRIP), S6b (soft
      10s-with-current trip, unconditional 120s backstop, no-current-ever
      nuisance -- this module has no separate S6b WARN flag, so "WARN only"
      at 11s-no-current is exactly the existing "never trips" assertion),
      S8 (correctly out of scope -- see below), S9 (TRIP_INEFFECTIVE
      escalation and its inverse; "current decays with tau=1s" and "current
      persists" are the same boolean `any_current_present` at this module's
      boundary, so both matrix rows collapse to the existing present/absent
      tests), S10 (EXTERNAL_OVERHEAT disables), S11 (601s frozen with/without
      heat), all guards' latching and `CLEAR_TRIP`-refused-while-still-true
      (via `test_try_clear`), and bit-identical TX-stubbed determinism (via
      `test_independence_invariant`).
      **Two matrix rows were flagged as genuinely untested** in the prior
      pass and are now closed (2026-08-19, extraction follow-up): the
      GRACE-timeout decision was extracted from `relay_owner.c`'s
      `relay_owner_task()` into `src/tasks/relay_grace.c`'s pure
      `relay_grace_tick()` (the exact `(now - grace_start) >= grace_ticks`
      comparison, unchanged) and `relay_trip_transition()` (the unconditional
      TRIPPED latch, also unchanged -- confirmed by inspection that the
      original code really does trip from every state, not just ARMED/GRACE,
      so this is documented as intentional rather than "fixed"), both
      host-tested in `test_relay_grace.c`. The `CLEAR_TRIP`
      trip_mask-mismatch / nothing-tripped refusal was extracted from
      `link_task.c`'s `link_task_handle_clear_trip()` into
      `link_frame.c`'s pure `link_frame_decide_clear_trip()`, host-tested in
      `test_link_frame.c`'s `test_decide_clear_trip()`. Both task files now
      only gather inputs, call the pure function, and act on the FreeRTOS
      side (GPIO/log/queue) -- no behavior change, same pattern
      `link_frame_trip_mask_for_reason()` already established. 452/452 host
      checks pass (434 before this pass, 409 before that).
- [x] Property tests: ceiling monotonicity over the float range incl. NaN/Inf
      (2026-08-19, `test_safety_guards.c`'s `test_s1_ceiling_properties()`).
      Found and fixed a real hole while writing this: S1's ceiling clamp used
      a bare `requested < abs_max_temp_c` comparison — NaN degraded safely by
      luck of IEEE754 comparison semantics, but `requested = -Infinity`
      (a garbled/hostile `firing_max_c` off the link) made the comparison
      true, latching `ceiling = -Infinity` and tripping S1 on every
      subsequent tick forever (permanent nuisance-trip, not a missed-trip
      risk, but still a real availability bug). Fixed with an `isfinite()`
      guard in `safety_guards.c`. Also added `test_context_gating()` for
      this section's "no guard reads a disabled input" row: with
      `context_valid=false`, deliberately provocative context fields
      (huge setpoint disagreement, current with nothing commanded, stalled
      sample counter) are confirmed to never trip/warn S2/S3/S4/S10/S13.
      409/409 host checks pass; RP2040 target build clean.
- [x] Fuzz over every decoder (`firmware/CommonFW/test`). Done 2026-09-04:
      `test_fuzz.c` (2026-08-16) already covered the two framing-layer
      decoders (`kilnlink_frame_decode`, `kilnlink_unstuff`); this pass adds
      `test_fuzz_payloads.c`, covering the other 27 -- every
      `kilnlink_*_decode` in `firmware/CommonFW/include/kilnlink/`
      (context/status/power/announce/diag/trip/ceiling/clear_trip/
      set_config/rollback/get_fw_version/set_clock/announce_reboot/
      set_ct_cal/get_ct_cal/ct_cal/set_log_level/set_param/commit_config/
      commit_config_rejected/inject_tc/get_param/param/get_config_page/
      config_page/rollback_result/fw_version). "Every decoder" is now
      actually every decoder, not just the framing layer underneath them.
      Per decoder: a fixed corpus (empty, 1 byte, min/max length -1/exactly/
      +1, all-zero and all-0xFF at several lengths, a real valid frame built
      with the matching `_encode()`/`_pack()` with every byte individually
      bit-flipped, that same frame truncated at every offset, and that frame
      plus 8 trailing garbage bytes) plus randomized bytes (uniform and
      structurally-biased) at every length from 0 through past the longest
      legal frame. Deterministic: fixed-seed xorshift32 (independent from
      test_fuzz.c's own state), seed printed every run and overridable via
      `KILNLINK_FUZZ_SEED`, iteration count via `KILNLINK_FUZZ_ITERS` for a
      longer soak. 14,483 calls across 27 decoders, ~0.4s.
      **Property asserted, not just "didn't crash":** every truncated or
      length-lied-about copy of a real valid frame must be REJECTED, never
      decoded OK -- this is the actual documented hazard ("an offset
      mismatch silently misdecodes temperatures"), and a decoder that
      crashes is a much smaller risk than one that returns success on data
      it never fully validated. Output structs are canary-guarded (64
      sentinel bytes before/after, poisoned before each call, checked after)
      as an explicit-bounds-assertion fallback, since the default `cl` build
      here has no sanitizer flag (matching every other host test binary in
      this repo).
      **Sanitizers: available and used, not just a documented gap.** This
      MSVC Build Tools install (VS "18") does ship clang-cl's `/fsanitize=
      address` -- confirmed by a real build+run (`firmware/CommonFW/test/
      run_fuzz_payloads_asan.ps1`, a separate opt-in script, ASan-clean
      across the same 14,483 calls). Not wired into the default CI build:
      that would require copying `clang_rt.asan_dynamic-x86_64.dll` next to
      a monolithic multi-file binary that also links plenty of SaftyFW/
      KilnFW code never built with `/fsanitize` elsewhere, for a benefit the
      canary-guard fallback above already delivers for this specific
      "decoder writes past its output struct" property. Run the ASan script
      by hand after touching any `kilnlink_*.c` for the deeper check.
      **Negative test (mandatory, per feedback_negative_test_every_check.md):**
      removed the final `len != announce_wire_len(commit_len, datetime_len)`
      guard from `kilnlink_announce_decode()` (`kilnlink_announce.c`) --
      exactly the documented hazard class, an offset/length check silently
      dropped. First attempt at a truncation assertion false-positived on
      `kilnlink_status_decode` (both its V1 23-byte and V2 24-byte lengths
      are independently legitimate, so "truncated by exactly 1 byte" isn't
      always a real defect there -- fixed the harness's `build_valid_status`
      to encode the V1-length frame so its own truncation corpus can't
      collide with a second valid length). With the fix in place and the
      defect still injected, the harness failed loud and named the right
      decoder:
      `FUZZ FAIL: kilnlink_announce_decode reported OK for a frame truncated
      to 15 of its real 24 bytes -- it decoded past the end of what actually
      arrived, exactly the silent-misdecode hazard this harness exists to
      catch`. Reverted (`git diff` on `kilnlink_announce.c` empty); rerun
      clean, 2077/2077 SaftyFW host checks plus the fuzz binary both pass.
      **No real defect found** in the shipping 27 decoders themselves --
      the only bug this pass produced was the deliberately-injected one
      above, reverted before commit.
      **Wired into CI**: `firmware/SaftyFW/test/build_host_tests.ps1` builds
      and runs `test_fuzz_payloads.c` (linked against every `kilnlink_*.c`)
      as a second, separately-labeled binary after the main 2077-check
      suite -- a fuzz failure throws with the seed needed to reproduce it,
      distinct from the main check count so it can never be silently folded
      into (or silently absent from) that number. KilnFW's own host build
      and `build_kilnfw` unaffected (no shared file touched besides the two
      new/edited files above).

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

---

## 6. Guard reachability in current SaftyFW (added 2026-08-20)

**This section asks whether the guard can actually fire at all** against
today's shipping `SaftyFW`, independent of any scenario or fixture —
because a guard's pure logic being implemented and
host-tested (section 2 above) says nothing about whether the caller
(`safety_core.c`) ever populates the input fields that logic depends on.

This was established two ways, cross-checked against each other:

1. **Direct source reading** — `safety_core_build_input()`
   (`firmware/SaftyFW/src/tasks/safety_core.c`) builds one tick's
   `safety_guard_input_t` with a C99 designated-initializer struct literal.
   Any field the literal does not name is zero-initialized. Reading which
   fields it does and does not name, against `safety_guards.c`'s own gating
   logic for each guard, gives a guard-by-guard reachability verdict
   directly from the code.
2. **`firmware/SimFW/tools/virtual_dut`** — a new host-side tool (this pass)
   that compiles `safety_guards.c` and `relay_grace.c` **verbatim,
   unmodified**, ticks them against `firmware/SimFW`'s `virtual_simfw`
   fixture at the real 100 ms cadence, and records which guards actually
   transition. Its full run against all 19 scenarios was recorded in
   `firmware/SimFW/tools/virtual_dut/results/SCENARIO_RESULTS.md` — gone
   along with the rest of SimFW's removal 2026-08-28, see the "Re-checking
   this table" note in §6 below — and its
   own README's "Findings" section reached the same verdicts below
   independently. **This is a software cross-check, not hardware
   verification** — no real SPI bus, no real relay coil, no real ESP link,
   no FreeRTOS jitter — see that tool's own README for exactly what it does
   and does not reproduce. It is real code, though, not a synthetic host
   test with invented inputs, which is why it is worth citing here as a
   second, independent confirmation rather than trusting the source reading
   alone.

> **Revised 2026-08-20 (commit `f304392`, fixture caught up in the commit
> this revision ships with).** The table below was written against a
> `safety_core_build_input()` whose C99 struct literal simply never named
> `context_valid`, `link_up`, `any_current_present`,
> `relay_commanded_recently`/`_continuously`, `zone_count`,
> `max_zone_setpoint_c` or `nearest_zone_measured_c`. `f304392` wired all of
> them to real producers (`link_task`'s published `context_snapshot_t`,
> `link_task_link_up()`, `link_task_get_relay_on_continuous_ms()`,
> `current_task`'s ADC snapshot, via the new pure `context_reduce_zones()`/
> `current_any_present()` helpers in `src/snapshots.h`) and gave
> `relay_owner_command_energize()` its first caller
> (`SAFETY_CMD_REQUEST_ENABLE` (0x02) → `safety_core_request_enable()`).
> **S2, S3, S4 and S10 are reachable as of that commit; S6b no longer trips
> unconditionally.** The rows below are updated in place; the struck reasons
> are kept as history because they are what the `blocked_on:` annotations in
> `firmware/SimFW/scenarios/*.yaml` were written against.
>
> The `virtual_dut` re-run confirming this was recorded in
> `firmware/SimFW/tools/virtual_dut/results/SCENARIO_RESULTS.md` (removed
> with SimFW 2026-08-28, see the "Re-checking this table" note in §6). Read its
> **"reachable" vs "provokable by this fixture"** distinction carefully: a
> guard being reachable on the RP2040 does not mean `virtual_dut` can
> currently drive it (the fixture has no setpoint producer for S2, no
> current for S3/S4 because nothing closes a relay, and no operator enable
> step for K4).

**Bottom line as of `f304392` (superseded — see the 2026-08-21 update below
the reachability table further down: S6a/S9/S11 have since been wired too,
and only S1/S6a/S13 remain blocked): S2, S3, S4, S5, S6b, S7, S10 and S12
can structurally fire; S1, S6a, S9, S11 and S13 remain blocked.** (Before
`f304392`: only S5, S6b, S7, S12 — and S6b only in the degenerate sense of
tripping unconditionally.) Each remaining block is a specific,
individually-verified reason — not "the same reason" repeated:

| Guard | Reachable today? | Blocking input | Why the input is absent |
|---|---|---|---|
| S1 | **No** | `cfg->abs_max_temp_c` | Defaults to 0, and `safety_guards.h`'s own convention is 0 = "not commissioned, never trip" (a deliberate safety choice, not a bug). Guard logic is otherwise fully wired to real `tc_c`; will trip correctly the instant a real ceiling is commissioned via `config_store` (Phase 9, already built — see M3 above). **This is a config gap, not a missing-producer gap** — different in kind from every row below it. |
| S2 | **Yes** (since `f304392`) | — | `context_valid` is now computed from `link_task_get_context_snapshot()` + `link_task_get_degraded_no_context()` + a `CONTEXT_MAX_AGE_MS` (5 s) staleness test, and `zone_count`/`max_zone_setpoint_c` come from the pure `context_reduce_zones()`. ~~Never set true by `safety_core_build_input()` — no field for it in the struct literal.~~ ~~Reachable, but `virtual_dut` cannot provoke it: the fixture has no setpoint producer at all (no ESP, no PID), so it sends `setpoint_c = NaN`.~~ **2026-08-21: now provokable.** `run_dut_scenarios.py` gained a `dut.zone_setpoints:` scenario field (a static, per-zone declared setpoint — the honest stand-in for a real ESP's PID target, same category as `operator_actions:`), and `s2_setpoint_overshoot.yaml` uses it: `GUARD_TRIP {S2}` measured 267.0 s after K4 closes. |
| S3 | **Yes** (since `f304392`) | — | Same `context_valid` wiring as S2; `any_current_present` now comes from `current_task`'s real ADC snapshot via the pure `current_any_present()`, and `relay_commanded_recently` from `ctx.relay_recent_mask`. ~~Same as S2.~~ ~~Not provokable by `virtual_dut` today: `sim_engine.c` gates heater duty/CT current on K4, and K4 never closes there because nothing issues the operator-initiated `SAFETY_CMD_REQUEST_ENABLE`.~~ **Provokable since the operator-actions pass** (a `dut.operator_actions:` scenario field that can issue `request_enable`/`command_relay`): `stuck_load_no_command_s3.yaml` trips it for real (19.4 s after an uncommanded weld) and `enabled_firing_healthy.yaml` is the anti-nuisance control (current commanded, S3 stays quiet for the whole run). `runaway_zone.yaml` still cannot reach it — that file schedules no `operator_actions:` of its own. |
| S4 | **Yes** (since `f304392`) | — | Same as S3, plus `relay_commanded_continuously`, computed here from `link_task_get_relay_on_continuous_ms()` against `correlation_window_s` (an AND over the window, which neither wire mask alone answers). ~~Same as S2.~~ ~~Same fixture-side non-provokability as S3.~~ **Provokable since the operator-actions pass**: `commanded_no_current_s4.yaml` warns for real (150.8 s after K1 closes, K4 stays closed — the WARN-only property proven positively) and `enabled_firing_healthy.yaml` is the anti-nuisance control. `broken_element.yaml`/`welded_ssr_midfire.yaml`'s own S4 clauses stay `BLOCKED` — those specific files schedule no `operator_actions:`. |
| S5 | **Yes** | — | `tc_valid`/`tc_c`/`fault_bits`/`spi_failed` all come from `thermo_task`'s real snapshot, unconditionally, no gating field at all. |
| S6a | **Yes** (since the `main_fault_asserted` wiring) | — | `main_fault_asserted` now comes from `discrete_task_main_fault()`, called directly in `safety_core_build_input()` on the line immediately after `.estop_pressed` — the same shape as S7's row below. Polarity is straight through, no inversion: `discrete_task` samples `!gpio_get(SAFTYFW_PIN_MAIN_FAULT)` (active low, R8 pull-up) and debounces it 200 ms, so `true` already means "mainFault asserted", which is exactly what S6a trips on. ~~Never set by `safety_core_build_input()` — the producer already exists and works … a one-line wiring omission, not a missing Phase.~~ ~~Reachable, but no `virtual_dut` scenario asserts it: the fixture senses the Fault line, it does not drive the Pico's GPIO10 — the harness now carries the level on the TICK line's optional trailing `<main_fault>` field (default 0), so it is wire-reachable rather than hardcoded false in C.~~ **2026-08-21: confirmed genuinely NOT provokable through this harness, at all, and precisely why (deeper than the wire-reachability note above suggested).** Two independent gaps, both fixture-side: (1) `virtual_simfw.c`'s `fault_line_asserted` — the thing this fixture *senses* on the Fault line — is written ONLY by real hardware's `i2c_owner.c` (a real I2C GPIO-expander pin wired to the ESP's GPIO6 → U1 opto); `virtual_simfw.c` has no I2C-expander emulation of that path at all (grepped: read in three places, written in none), so it is zero-initialized and stays false for the life of the process regardless of what TC fault is injected — there is no KilnFW anywhere in `virtual_simfw`/`virtual_dut` to execute the "ESP decides to assert its own fault output" step `mainfault_tc_disconnect.yaml`'s `disconnected_tc` fault is meant to provoke indirectly. (2) Independently, `run_dut_scenarios.py`'s poll loop never reads `fault_line_asserted` from telemetry and forwards it as `dut_core.exe`'s optional TICK `<main_fault>` field — it always sends the parameter's default (`False`). SaftyFW's own input is fully wired and correct; this is a harness limitation in what it chooses to emulate, not a wiring gap in SaftyFW, and not closable by another scenario. |
| S6b | **Yes** (since `f304392`) — no longer unconditional | — | `link_up` now comes from `link_task_link_up()` (a CRC-valid frame decoded within `LINK_UP_RECENCY_MS` = 1000 ms; recorded before the BROADCAST filter, so any well-formed frame counts). ~~Never set true — no field for it in the struct literal … the elapsed-silence timer accumulates from the first tick of every boot and trips the hard backstop (`link_dead_hard_s`, default 120 s) regardless of any other condition, roughly 2 minutes into every boot.~~ **That nuisance trip is gone and measured gone**: it fired in 16 of 19 `virtual_dut` scenarios before this fix and in 0 of 19 after, with no other change to the scenarios. Anti-nuisance half provoked (`power_blip.yaml`'s `mainfault_reads_healthy_during_blip` PASSes); the trip-side half stays `BLOCKED` on an unrelated K4-boot-open clause-form issue, not on link_up itself. |
| S7 | **Yes** | — | `estop_pressed` comes from `discrete_task_estop_pressed()`, a real, debounced (50 ms) GPIO9 reading, called directly in `safety_core_build_input()`. **2026-08-24: the input was wired but its POLARITY was inverted, so S7 was non-functional in both directions.** `discrete_task.c` read `!gpio_get(SAFTYFW_PIN_ESTOP)` under an "active low" comment, but GPIO9 is active HIGH for stop (HARDWARE.md section 5: R10 1k pull-up, normally-closed contact to GND_Safty). A real button press -- and equally a cut cable or an unwired board, the cases the normally-closed wiring exists to catch -- read as HEALTHY and never tripped; conversely a correctly-jumpered healthy board latched S7 permanently and could never arm. Root cause was `discrete_task.h`'s comment asserting both discretes were active-low; only `mainFault` (GPIO10) is. Fixed by dropping the negation for GPIO9 only. Nothing in the host suite or this fixture could have caught it, per the S7 row in the provocation table above -- a GPIO polarity is only observable against real hardware or a test that stubs `gpio_get()` itself. **2026-08-24, same pass: that gap is now closed.** The raw-level-to-logical-meaning mapping for both discretes was split out of `discrete_task.c` into `src/discrete_pin_policy.{c,h}`, a pure function taking the raw bool `gpio_get()` would have returned -- no `gpio_get()` stub needed, since the function never calls it. `test/test_discrete_pin_policy.c` asserts GPIO9 HIGH -> asserted for the pressed/cut-wire/unfitted cases individually (electrically identical, asserted separately so a regression in one cannot hide behind the other two), GPIO9 LOW -> healthy, GPIO10's opposite polarity both ways, and a direct cross-check that the two pins decode the SAME raw level oppositely -- the exact property the original bug's "both discretes active-low" comment got wrong. Verified to actually fail: temporarily reintroducing the `!` inversion on the E-stop path failed all four E-stop assertions plus both cross-check assertions (6/1628) while both mainFault assertions kept passing, then the fix was restored and the suite returned to 1628/1628. |
| S9 | **Yes** (since the `relay_deenergized` wiring) | — | `relay_deenergized` now comes from `!relay_owner_is_energized()`, named in `safety_core_build_input()` right after `.reboot_grace_active` (the struct's own field order). ~~Never computed — nothing plumbs `relay_owner_is_energized()`'s inverse into the input struct. This is the one row `f304392` did not move at all.~~ **Polarity is one deliberate negation, cross-checked three ways**: `relay_owner.h` publishes the affirmative ("true only while GPIO6 is actually being driven high right now"), `safety_guards.h` asks for the negative ("set by the caller once relay_owner has actually de-energized K4"), and `safety_core_get_output_status()` — twenty lines below, same file, same getter — reads it with *no* `!` for its `out_relay_energized`; the two call sites differ by exactly one negation because the two field names are exact opposites. **No GRACE suppression, and none is needed**: K4 genuinely reads de-energized for the whole 60 s startup GRACE window, but `safety_guards_tick()` only evaluates S9 inside its `if (state->is_tripped)` branch and re-zeros `s9_verify_elapsed_s` at the bottom of every untripped tick, so this is *not* the free-running-timer shape that made S6b nuisance-trip every boot before `f304392`; and if a trip does land during GRACE, "K4 open yet current still flowing for `trip_verify_s`" is a real welded contactor regardless of GRACE. ~~K4 is never energized in the first place today (`relay_owner_command_energize()` has zero callers anywhere in the tree) … no scenario models the operator enable step.~~ **Provoked end to end since the operator-actions pass**: `welded_contactor_s9.yaml` issues `request_enable`, lets S3 trip on an uncommanded weld, then a second, persisted-current fault survives K4 opening — `TRIP_INEFFECTIVE_LATCHED` measured 9.6 s after K4 opens. |
| S10 | **Yes** (since `f304392`) | — | Same `context_valid` wiring as S2; `nearest_zone_measured_c` comes from `context_reduce_zones()`'s nearest-match search (never the mean). ~~Same as S2.~~ **And genuinely exercised, both directions**: `main_safety_skew`'s `s10_stays_quiet` (an +80 °C skew must stay under `tc_disagreement_c` = 200 °C) is a real anti-nuisance PASS, and `main_safety_disagree_s10.yaml` (2026-08-21) is the genuine-WARN case at +250 °C: `GUARD_WARN {S10}` measured 294.0 s after the fault fires. S10 is WARN-only (no `SAFETY_TRIP_*` case exists for it), so this is as far as "trip" can mean for this guard. |
| S11 | **Yes** (since the `heat_commanded` wiring) | — | `heat_commanded` now comes from the same `any_current_present` value S3/S4/S6b already read (`current_task`'s real ADC0/1/2 snapshot via `current_any_present()`), named in `safety_core_build_input()` right after `.main_fault_asserted`. ~~Hardcoded `false` in `safety_core_build_input()` — its own comment: "no current sense yet, Phase 6."~~ Deliberately **not** wired from `relay_commanded_recently`/`_continuously` even though both also approximate "heat commanded": `safety_guards.h`'s own header comment requires this field stay link-independent (no `context_snapshot_t`-derived fact), matching `SAFETY_MODEL.md` §6's own S11/S13 audit note ("S11 reads neither [`link_up` nor `context_valid`]") and S11's place on the "keeps running with authority over K4 even when the main controller is unknown" list (§6) — `current_any_present` is real, independent hardware (its own ADC), never context-gated, exactly like S6b's own unconditional use of the same value. ~~Not yet provokable by `virtual_dut`: `sim_engine.c` gates CT current on K4, which never closes there.~~ **Provoked both directions since 2026-08-21**: `safety_tc_frozen.yaml` (fixed this pass, gained `operator_actions:`) trips for real — `GUARD_TRIP {S11}` measured 664.0 s after the freeze fault, with K4 closed and current flowing continuously from ~61 s onward. `safety_healthy_reading_s11.yaml` (new) is the anti-nuisance control: identical setup, no fault, run for a comparable ~650 s span — S11 never trips, because a live, unfaulted ADC reading is never bit-identical between ticks (measured, not asserted: `virtual_simfw`'s own MAX31856 emulation carries real noise/quantization). |
| S12 | **Yes** | — | `cj_c` comes from the same real `thermo_task` snapshot as S1/S5/S11, with **no** `context_valid` or `link_up` gating at all — the guard's own code puts it before the context-gated block. ~~Not observed firing … `cj_fault.yaml` has no numeric fault offset~~ — that scenario-file gap was closed (`params: [70.0]`) and S12 has since been observed genuinely warning *and* tripping in `cj_fault`. |
| S13 | **No** | `in->sample_counter_advancing` + `cfg->tc_source` | ~~Same as S2 (S13 is additionally gated on `cfg->tc_source` …).~~ `context_valid` no longer blocks it, but the two remaining blocks are both **commissioning gaps, not producer gaps** — the same category as S1's row above, and deliberately left that way by `f304392`. Deciding whether a zone's `sample_counter` advanced requires a commissioned `borrowed_zone_index` (0..2) naming *which* context zone is the borrowed channel; that field is documented (`SAFETY_MODEL.md` §3, `CONFIG_REFERENCE.md`) and **does exist in `config_store`** (`config_store.h:355`, `uint8_t borrowed_zone_index`, range-validated 0..2 and gated by `CONFIG_STORE_SET_BORROWED_ZONE_INDEX` in `config_params.c`) — **corrected 2026-08-24; this line previously said it existed nowhere, which would send a reader to build the whole field from scratch.** What is genuinely missing is one plumbing step: it has no `safety_guard_cfg_t` member (`grep borrowed_zone_index safety_guards.h` = 0 hits) and no guard reads it, so `safety_core_build_input()` leaves `sample_counter_advancing` false rather than hardcoding zone 0 and being silently wrong on any installation whose borrowed zone is not zone 0. Independently, `cfg->tc_source` defaults to `OWN_J7`, which `safety_guards.c` gates the whole S13 block on. |

**2026-08-24 caveat on the S3/S9/S11/S6b `any_current_present` rows above:
their "Yes" verdicts were established through `virtual_dut`, which
synthesizes `current_snapshot_t` directly for its scenarios rather than
running current samples through `current_sense.c`'s own calibration
pipeline (`cs_counts_to_amps()`/`current_sense_set_cal()`/`current_task_
reload_cal()`) — so those PASSes never exercised, and therefore never could
have caught, the fact that `current_sense_set_cal()` was never called
anywhere in `src/` until this date. That gap left `k_ct_v_per_a` permanently
`0.0f` on real hardware, which (before this date's presence-decoupling fix,
`current_presence_policy.h`) silently disabled all four rows' presence
detection outright — a fail-open on real hardware that these "reachable
today" verdicts did not, and structurally could not, transfer to. Treat
every "Yes" above as "reachable given the input `virtual_dut` hands
`safety_core_build_input()` directly", not as "verified against the real
producer chain that turns ADC counts into that input" — the two coincided
for `any_current_present` by accident of `virtual_dut`'s own architecture,
not by design, and nothing currently in this repo's test suite would catch
a similar gap in a different producer the same way.**

**Read this table honestly, not as a verdict on guard quality.** Every guard
above's *logic* passed its host-test row in section 2 — the code that
decides "should this trip" is correct against the inputs it is given. What
this table adds is which of those inputs are actually given.

**Three distinct states, easy to conflate — keep them apart (revised
2026-08-21 — the picture below is CURRENT, not the `f304392`-era one two
paragraphs up, which is kept for history):**

1. **Not reachable** — `safety_core.c` never produces the input, so the
   guard's branch cannot run at all. **As of the S6a/S9/S11 wiring commits
   (`5375bca`/`5f90325`/`6e98ae3`), this category is empty.** Every guard
   input `safety_core_build_input()` is capable of producing, it produces.
2. **Reachable but not commissioned** — the input path is complete; a
   per-kiln config value that has no default, and deliberately must not be
   guessed, keeps the guard quiet. **S1** (`abs_max_temp_c`) and **S13**
   (`borrowed_zone_index` + `tc_source`) are the only two guards left in
   this state, and both are deliberate safety choices, not bugs or gaps to
   close.
3. **Reachable and commissioned, but this specific fixture cannot generate
   the stimulus** — the firmware is done; `virtual_dut`/`virtual_simfw`
   lacks the emulation to provoke it. **As of 2026-08-21, this category is
   also empty for every guard except S6a.** S2/S3/S4/S9/S11 were in this
   category through most of this document's history and are not any more
   (see their rows above for exactly what closed each one — a
   `dut.zone_setpoints:`/`dut.operator_actions:` scenario capability in
   every case, not a firmware change). **S6a is the one guard that stays
   here permanently, for a reason specific to it**: it needs
   `virtual_simfw.c` to emulate the I2C-expander/opto Fault-line sensing
   path, which does not exist and is out of this scenario library's
   ownership to add (`firmware/SimFW/tools/virtual_simfw/src/**`).

**Bottom line, current as of 2026-08-21: every guard except S1, S6a and S13
now has real `virtual_dut` evidence — a genuine PASS or FAIL against
present-day SaftyFW, not just a "should be reachable" source-level verdict.**
S1 and S13 are commissioning gaps (deliberate, not fixable by a scenario or
a fixture change). S6a is the one guard this harness cannot provoke at all,
for the fixture-emulation reason given in its row above — bench hardware is
the only way to exercise it. The exact measured numbers behind every
"provoked" claim above were in
`firmware/SimFW/tools/virtual_dut/results/SCENARIO_RESULTS.md`, removed
with SimFW 2026-08-28 — see the "Re-checking this table" note just below.

**Re-checking this table:** the `virtual_dut` host cross-check this table's
"Yes"/"No" verdicts cite was part of the SimFW bench-fixture tooling, which
has been removed from this repo. Re-establish reachability by direct source
reading (method 1 above) any time `link_task`/`current_task` wiring changes
in `safety_core.c`, or any time a guard's own inputs change, and update this
table's "Reachable today?" column in the same change, per this file's own
"keep this file current" rule at the top.

## 6c. Reachability re-established (2026-09-03)

ROADMAP.md flagged this: the E-stop polarity fix and the
`current_sense_set_cal()` fix (both 2026-08-24) landed **after** the last
top-line reachability count in this file was computed, and nobody had
recomputed the count itself since — only the individual guard rows above got
hand-patched with 2026-08-24 notes. This section is that recomputation, done
fresh against today's `src/` by method 1 (direct source reading: for every
guard, is there a real writer for every input its `safety_guards.c` gate
reads, named file + symbol, not just a reader that a host test could be
feeding by hand).

**Old count (the one ROADMAP.md said to stop trusting): 6 of 13 guards
structurally reachable**, computed against `src/` as it stood *before* the
2026-08-24 fixes (S8 not implemented; S14 did not exist yet):

| Reachable (6) | Blocked (7) | Why blocked |
|---|---|---|
| S2, S4, S5, S6b (hard backstop only), S10, S12 | S1 | commissioning gap (`abs_max_temp_c` = 0), unrelated to either fix |
| | S3 | `any_current_present` permanently false — no calibration ever loaded |
| | S6a | `main_fault_asserted` not yet wired into `safety_core_build_input()` at all |
| | S7 | E-stop polarity inverted — a real press read as healthy |
| | S9 | same `any_current_present` gap as S3 |
| | S11 | `heat_commanded` was hardcoded `false` (own comment: "no current sense yet") |
| | S13 | commissioning gap (`tc_source` = `OWN_J7`), plus `sample_counter_advancing` had no producer at all yet |

**New count, verified against `src/` today: 11 of 14 guards structurally
reachable** (S8 still has zero implementation — `grep S8 safety_guards.c` =
0 hits beyond the enum comment — so it is excluded from both the numerator
and the denominator the same way the old count excluded it; S14 is a new
guard, added 2026-08-28, after both counts above, and is treated separately
below rather than folded into either one):

| Guard | Input(s) | Producer (file : symbol) | Verdict |
|---|---|---|---|
| S1 | `cfg->abs_max_temp_c` | `safety_core_load_guard_cfg()` (`safety_core.c:333`), gated on `CONFIG_STORE_SET_ABS_MAX_TEMP_C` | **(d) deliberately configured off** — no default by design, not a bug |
| S2 | `context_valid`, `zone_count`, `max_zone_setpoint_c` | `link_task_get_context_snapshot()`/`context_reduce_zones()` (`safety_core.c:778-836`, `snapshots.h`) | **(a) reachable** |
| S3 | `any_current_present`, `relay_commanded_recently` | `current_task_get_snapshot()`+`current_any_present()` (`safety_core.c:849-910`); `ctx.relay_recent_mask` (`safety_core.c:919`) | **(a) reachable** — confirmed `current_sense_set_cal()` is called (`current_task.c:197`) |
| S4 | same as S3 plus `relay_commanded_continuously` | `link_task_get_relay_on_continuous_ms()` (`safety_core.c:923-924`) | **(a) reachable** |
| S5 | `tc_valid`/`tc_c`/`fault_bits`/`spi_failed` | `thermo_task_get_snapshot()` (`safety_core.c:662`), unconditional | **(a) reachable** |
| S6a | `main_fault_asserted` | `discrete_task_main_fault()` (`safety_core.c:1073`) | **(a) reachable in source; (b) not exercisable by any host fixture** — `SimFW`/`virtual_dut` are gone, so this is bench-hardware-only, same conclusion §6a already reached |
| S6b | `link_up` | `link_task_link_up()` | **(a) reachable** — both the soft current-gated tier (via S3's `any_current_present` fix) and the unconditional hard backstop |
| S7 | `estop_pressed` | `discrete_task_estop_pressed()` → `discrete_pin_policy_estop_asserted()` (`discrete_task.c:98`) | **(a) reachable** — confirmed polarity is correct: `gpio_get(SAFTYFW_PIN_ESTOP)` HIGH decodes to asserted, per `discrete_pin_policy.h` and `test_discrete_pin_policy.c` |
| S8 | `max_rate_c_per_min`, `rate_window_s` | `safety_core_load_guard_cfg()` (`safety_core.c`), gated on `CONFIG_STORE_SET_MAX_RATE_C_PER_MIN`, wired 2026-09-03 | **(d) deliberately configured off** — implemented and integrated (`test_safety_core_s8_wiring.c` proves the whole chain), same "no default by design" shape as S1, not a bug |
| S9 | `relay_deenergized`, `any_current_present` | `!relay_owner_is_energized()` (`safety_core.c:1109`); current as S3 | **(a) reachable** |
| S10 | same context reduction as S2 | same as S2 | **(a) reachable** |
| S11 | `heat_commanded` (= `any_current_present`) | same as S3 (`safety_core.c:622-660`) | **(a) reachable** |
| S12 | `cj_c` | `thermo_task_get_snapshot()`, ungated | **(a) reachable** |
| S13 | `sample_counter_advancing`, `cfg->tc_source` | `context_borrowed_sample_counter_advancing()` (`safety_core.c:957-960`, `snapshots.h:288`), keyed off `cfg_rec.borrowed_zone_index` (`config_store.h:399`); `cfg->tc_source` from `safety_core_load_guard_cfg()` (`safety_core.c:350-357`) | **(d) deliberately configured off** — `tc_source` defaults to `OWN_J7`. **Correction to this file's own S13 row above (§6, line ~384): that row is stale.** It says `sample_counter_advancing` "has no `safety_guard_cfg_t` member... so `safety_core_build_input()` leaves `sample_counter_advancing` false rather than hardcoding zone 0." That is no longer true — `safety_core.c:940-960`'s own comment says explicitly this was fixed *because* this matrix's old S13 row found the gap: the code now reads `cfg_rec.borrowed_zone_index` directly off the config-store record (not through `safety_guard_cfg_t`, which is why grepping that struct alone still finds nothing) and calls the real producer. The only remaining block is `tc_source`, same commissioning-gap category as S1, not a missing-producer gap any more. |

**S14 (added 2026-08-28, after both counts above — not part of the 6-of-13
or 11-of-14 figures, reported separately):** per-channel `i_normal_valid[ch]`
is fields-set-gated individually off `config_store` (`safety_core_load_guard_cfg()`,
`safety_core.c:389-394`) — **(d) deliberately configured off** until each
channel's `i_normal_a` is commissioned, same shape as S1/S13. Also forced
inert by `cts_disabled` when `ct_installed` is explicitly answered "no"
(§9) — that is case **(d)** too (a legitimate configured-off state), not a
missing producer.

**Per-guard delta, old → new:**

| Guard | Old | New | What changed |
|---|---|---|---|
| S1 | blocked | blocked | unchanged — commissioning gap both times |
| S2 | reachable | reachable | unchanged |
| S3 | **blocked** | **reachable** | `current_sense_set_cal()` fix (`current_task.c:197`) |
| S4 | reachable | reachable | unchanged |
| S5 | reachable | reachable | unchanged |
| S6a | **blocked** | **reachable (source), still needs hardware to exercise** | `main_fault_asserted` wiring landed (`safety_core.c:1073`) — a separate fix from the two ROADMAP named, same window |
| S6b | reachable (hard backstop only) | **reachable, both tiers** | current-gated soft tier unblocked by the same `current_sense_set_cal()` fix |
| S7 | **blocked** | **reachable** | E-stop polarity fix (`discrete_pin_policy.c`) |
| S9 | **blocked** | **reachable** | `current_sense_set_cal()` fix, plus `relay_deenergized` wiring |
| S10 | reachable | reachable | unchanged |
| S11 | **blocked** | **reachable** | `heat_commanded` wired to `any_current_present`, plus the calibration fix |
| S12 | reachable | reachable | unchanged |
| S13 | blocked | blocked | unchanged bottom line, but the *reason* changed: `sample_counter_advancing` now has a real producer; only `tc_source` still blocks it (previously both were missing) |
| S14 | did not exist | blocked (commissioning) | new guard, ships in the same configured-off state as S1/S13 by design |

**Four states, kept distinct (this is the conflation that produced the stale
number in the first place):**

1. **(a) Reachable and exercised** — proven by a passing host test in §2
   against the pure function, *and* a real producer confirmed above. All 11
   reachable guards satisfy the host-test half; none has fresh hardware
   evidence (see the honest gap below).
2. **(b) Reachable but never exercised on real hardware** — S6a is the clear
   case: structurally reachable in source and on this board (§6a above), and
   permanently unprovokable by any fixture (no fixture can drive its input
   at all any more) — three separate facts, all true of S6a at once, none of
   which is "hardware-verified." More broadly:
   *no* guard in this table has a post-2026-08-24-fix hardware trip on
   record — §3.4's hardware-trip rows predate both fixes and have not been
   re-run since (see the §3.4 note below). Source-reachable is not the same
   claim as hardware-verified, and this file's own top banner says so.
3. **(c) Unreachable — no producer** — **empty**, same as the pre-existing
   2026-08-21 finding. Every guard's inputs, S8's exempted, now have a real
   writer somewhere in `src/`. This is the category the two named fixes
   emptied out (S7, S3/S9/S11); S6a's wiring emptied it further.
4. **(d) Deliberately configured off** — S1, S13, S14, and (per §9)
   S3/S4/S9/S14 again on a board that has explicitly answered
   `ct_installed = no`. All are asked-and-answered commissioning states, not
   defects. Do not count these as "broken" or fold them into (c).

**§3.4 scope note — CLOSED 2026-09-03.** The hardware-trip table used to list
only S1, S3, S6b, S7 and S9 to provoke on real hardware — it predated S6a's
wiring, S10/S11's un-masking (§6a above), and S13/S14 entirely (both added
after §3.4 was last edited at the time). §3.4 now has a row, or an explicit
refusal, for all 11 structurally-reachable guards from §6c plus S8's "cannot
be provoked, unimplemented" note. No hardware was touched to write this —
there is a live A/B firing experiment running that this pass was told not to
touch — so every row above is still unexecuted; §4's "Recording results"
convention is what turns a row here into evidence once the next bench window
runs it.

**§2 host-checklist count.** The checklist item citing "452/452 host checks
pass" (2026-08-19, since grown to 409→434→452 across that pass's own edits)
is stale as a *current* figure — it was never meant to track the suite
forever, but re-running it as part of this audit is what "still accurate"
means here. `test/build_host_tests.ps1` (run via bash per this repo's own
convention — the PowerShell tool hangs on it) reports **1983/1983 checks
pass** today (2026-09-03). The suite has grown roughly 4.4x since that
checklist line was written (new coverage for the CT-optional path, the
commissioning gate, discrete-pin polarity, the MAX31856 range/CR1 policies,
and more); the *proportion* passing (452/452 → 1983/1983, both 100%) is the
number worth trusting, not the absolute count in the old checklist line.

---

## 6a. The safety thermocouple was physically fitted (2026-08-24) — re-derivation

**This section re-derives §6's table against one new fact, not against any
code change.** On 2026-08-24 the safety processor's own MAX31856 and a real
thermocouple were populated on `hardware/SaftyThermocoupleBoard/` for the
first time and verified live over the link: `link up; heating enable
granted; safety thermocouple valid | 30.20 C (CJ 28.08 C)`, status flags
`0x21` = `LINK_UP | TEMP_VALID` (see `TODO.md`'s "An uncommissioned safety
processor grants heating enable" for the full bench note — that finding is
a separate, still-open question about `request_enable`/`calibration_
missing` and is not re-litigated here). Nothing in `safety_guards.c` or
`safety_core.c` changed on this date.

**The masking mechanism §6 could not see, because it reads source, not
runtime sequence.** `safety_guards_tick()`'s very first line is
`if (state->is_tripped) { ... return false; }` — once ANY guard latches,
every tick thereafter runs *only* the S9 escalation branch; no other
guard's condition is evaluated again until a `CLEAR_TRIP` succeeds. With no
safety TC ever physically present, `thermo_task`'s snapshot was permanently
`tc_valid = false` / `spi_failed = true` from the first tick of every boot,
so `s5_bad_read_now()` (`safety_guards.c`) was permanently true.
`cfg.safety_tc_installed` defaults to `1` (installed —
`config_store_default()`, `config_store.c`) and nothing on this bench had
ever written it to `0` to declare the sensor absent, so `safety_tc_not_
installed_declared` was false and the blind-grace escape hatch did **not**
apply: S5 promoted to a real, permanently latched `SAFETY_TRIP_SENSOR_
INVALID` roughly `blind_grace_s` (default 60 s) into every boot. **From
that point on, no guard but S5 and S9 could ever be observed transitioning
on the physical board** — not because their inputs were unwired (§6 already
established most of them were), but because the tick function never
reached their code again once S5 latched. This is the "masked but
structurally reachable" state, and it is a different fact from "reachable"
— a guard can be fully wired in source and still have never once run its
own condition on real hardware.

**What today's fix does change: real-hardware guard evidence becomes
obtainable.** Before today, any bench run would have hit the same permanent
S5 latch within ~60 s of boot and observed nothing else ever transition. That
is no longer true: a real bench run against this hardware can now, for the
first time, potentially obtain real hardware evidence for S2, S3, S4, S6b,
S7, S9, S10, S11 and S12 — the same set unmasked below — subject to each
guard's own remaining gates (a live ESP context for S2/S10, current flowing
for S3/S4/S9/S11, and so on). This has not been attempted as part of this
pass (no hardware was touched); it is a now-open door, not a result.

**The reachability table, re-derived:**

| Guard | Masked by S5's permanent latch before today? | Reachable on real hardware now? | Still blocked on |
|---|---|---|---|
| S1 | Yes | **No** | `abs_max_temp_c == 0` (uncommissioned) — unrelated to the TC fit; `main_safety_skew`'s S1 clause stays `BLOCKED` in `SCENARIO_RESULTS.md` for exactly this reason |
| S2 | Yes | **Yes**, given `tc_placement_mode == CHAMBER_AGREED` and a live ESP context with `zone_count > 0` | A live KilnFW context — not a SaftyFW-side gap |
| S3 | Yes | **Yes**, given live current-sense + relay context | None on SaftyFW's side |
| S4 | Yes | **Yes** (WARN only) | None |
| S5 | — (was itself the masker) | **Yes** — now the graduated WARN→TRIP guard it was always designed to be, evaluated against a real reading instead of a permanent fault | None — this is the guard the hardware fit fixed directly |
| S6a | Yes | Structurally reachable on real hardware (wiring was already correct) — **but, per §6c's later (b) category, not yet actually exercised there**: this row records that the code path is now capable of running on this board, not that anyone has shorted the mainFault line and watched it trip. Still **not provokable through `virtual_dut`** (no I2C-expander/opto Fault-line emulation, §6 above), permanently, since that fixture is now deleted | A `SimFW` harness gap, unrelated to this fix; the hardware-exercise itself is a separate, still-open bench step (§3.4) |
| S6b | Yes | **Yes** | None |
| S7 | Yes | **Yes** | None |
| S9 | Yes | **Yes** | Only meaningful once some other guard trips and K4 is commanded open |
| S10 | Yes | **Yes**, same context/mode gating as S2 | Same as S2 |
| S11 | Yes | **Yes**, needs `heat_commanded` (`any_current_present`) true for the full `frozen_window_s` | Current flowing — same as S3/S4/S9 |
| S12 | Yes | **Yes** — `cj_c` is read ungated by context or link | None |
| S13 | Yes | **Yes** — `sample_counter_advancing` now produced from a real context-frame comparison (`context_borrowed_sample_counter_advancing()`, `src/snapshots.h`, called from `safety_core_build_input()`); `tc_source` still defaults to `OWN_J7` until BORROWED_ZONE/BOTH + `borrowed_zone_index` are commissioned | Requires `tc_source` = BORROWED_ZONE/BOTH and `borrowed_zone_index` both commissioned on a real board — same commissioning requirement every other config-gated guard (S1, S2/S10) has |
| S8 | Would have been masked too, had it existed at the time | **N/A at the time of this audit row — implemented and integrated 2026-09-03.** `safety_guards.c` now has the S8 guard (`safety_guards.h` no longer says "NOT implemented here"), and `safety_core_load_guard_cfg()` now copies `max_rate_c_per_min`/`rate_window_s` from `config_store` (`test_safety_core_s8_wiring.c` proves the whole chain) | `max_rate_c_per_min` defaulting to 0 is still a *design* "ships disabled" choice (`SAFETY_MODEL.md` §4) — that has not changed and is not a bug. Nothing is open on the wiring any more; the threshold itself still needs a measured full-power kiln ramp before anyone should set it to a non-zero value, per that same section |

**Checked explicitly: does any other guard share S1's "0 means not
commissioned, never trip" shape?** No. `safety_guards.c`'s `effective_f()`/
`effective_u16()` substitute a real, documented default the instant any
*other* threshold field is 0 — S5/S11/S12/S2/S3/S6b/S9/S10/S13's thresholds
all do this (see the `_DEFAULT` macros at the top of that file).
**`max_rate_c_per_min` (S8) is the one other field with the same
deliberate "0 = disabled" shape**, and as of 2026-09-03 it disables an
entire *integrated* guard — `safety_core_load_guard_cfg()` copies it the
same way `abs_max_temp_c` is copied for S1 — rather than leaving one
threshold inside an otherwise-active guard unset. `tc_source` defaulting to `OWN_J7` (S13) and
`tc_placement_mode` defaulting to `CHAMBER_AGREED` (S2/S10) are enum
defaults, not the numeric "0/unset" convention, though `tc_source`'s default
has the same practical effect of leaving S13 keyed to an uncommissioned
choice.

**Bottom line.** Today's hardware fit changes exactly one guard's behaviour
(S5, which can now do its job instead of latching at ~60 s into every boot)
and, as a direct consequence, un-masks eleven others (S2, S3, S4, S6a, S6b,
S7, S9, S10, S11, S12, and S5 itself) that were always reachable in source
but had never once run their own condition on this physical board. It
commissions nothing: S1 and S13 remain exactly as blocked as
`SCENARIO_RESULTS.md` already recorded. S8 was entirely unimplemented as of
this bottom line; it gained a pure-module implementation and host tests
2026-09-03 (`SAFETY_MODEL.md` §4's S8 section, `test_s8()`) and, the same
day, `safety_core_load_guard_cfg()` was wired to copy
`max_rate_c_per_min`/`rate_window_s` from `config_store`
(`test_safety_core_s8_wiring.c`). S8 is now integrated and inert-by-default,
same as S1 — commissioning is still required to arm it, and this repo still
has no logged full-power ramp to set that threshold from, but there is no
remaining wiring gap.


## 8. The commissioning interlock (2026-08-28)

Not a guard in `safety_guards.c` — an **interlock on the energize path**,
alongside the update interlock and the `safety_tc_installed` refusal, all
three inside `safety_core_request_enable()` (`src/tasks/safety_core.c`) and
all three refusing only the ON direction.

**What it does.** An uncommissioned safety processor refuses heating enable.
`SAFETY_CMD_REQUEST_ENABLE` (0x02) with `enable = 1` never reaches
`relay_owner_command_energize()`; a disable request is never gated.

**What "commissioned" means.** `commissioning_gate_is_commissioned()`
(`src/commissioning_gate.c`, pure, host-tested) requires **both** facts, and
they must agree:

1. `rec->calibration_missing == false` — the verdict `COMMIT_CONFIG`'s
   handler persisted when a real commissioning pass succeeded.
2. `config_params_all_required_set(rec)` recomputed **now** from
   `rec->fields_set` — the eight `CONFIG_STORE_SET_*` bits for the
   no-safe-default fields (`tc_source`, `borrowed_zone_index`,
   `tc_placement_mode`, `abs_max_temp_c`, `ct_channel_map`,
   `max_rate_c_per_min`, `mains_voltage_v`, `tc_type`).

A disagreement in **either** direction refuses: a stored flag claiming
commissioned that `fields_set` does not back up (garbled byte, a writer that
forgot to recompute), and a v1→v2-migrated record whose flag is forced true
even though the bits look complete. Plausible-looking values are never
enough — only an explicit `SET_PARAM` + `COMMIT_CONFIG` write sets a bit, so
a defaulted board is uncommissioned however sensible its numbers read.

**Why it closes a real hole.** §6's reachability table already records S1 as
unreachable on an uncommissioned board (`abs_max_temp_c == 0` means "never
trip", `SAFETY_MODEL.md` §4) and S8 as shipping disabled. Before this
interlock, the board would grant heat in exactly that state: the independent
protection layer permitting a firing with **no absolute temperature ceiling
in force**. That is the bench finding written up in `TODO.md`'s "An
uncommissioned safety processor grants heating enable", now resolved.

**How the operator sees it.** No new fault source, trip code or wire field.
The condition already travels end to end as Frame B
(`SAFETY_CMD_DIAG`)'s `KILNLINK_DIAG_FLAG_CALIBRATION_MISSING`, which KilnFW
renders as `commissioned:false` on `/safety/commissioning` and as the FAIL of
the "Safety processor commissioned" readiness item — the item a firing start
is already blocked on. SaftyFW additionally logs
`request_enable: refused: safety processor not commissioned` (WARN).

**Tests.** `test/test_commissioning_gate.c`: uncommissioned refuses ON and
still allows OFF; a NULL record refuses ON; a full `SET_PARAM`-per-id +
`COMMIT_CONFIG` sequence then allows ON; a one-field partial commit still
refuses; and both flag/`fields_set` disagreement directions refuse. All six
refusal checks were negative-tested — the guard was broken three ways (whole
decision stubbed `true`, flag half removed, `fields_set` half removed) and
each break was caught by the tests that cover it, then restored.

---

## 9. Running without current transformers (2026-08-28)

`ct_installed` (`0x0109`, `CONFIG_STORE_SET_CT_INSTALLED`) is an ASKED
commissioning field: *"are current transformers fitted to this board?"*
Answering **no** — explicitly, with the bit set and the value 0 — is now a
first-class state, distinct from "CTs fitted but not yet mapped".

**What it changes at the gate.** `config_params_all_required_set()` requires
`ct_installed` unconditionally and requires `ct_channel_map` only when the
answer is *yes* or absent. A CT-less board can therefore complete
commissioning and be granted heat. Unanswered behaves exactly as every build
before this field did: strict.

**What it changes at the guards.** `safety_core_build_input()` forces the
sensor's outputs to their no-information state (`any_current_present` false,
`amps_valid[ch]` false, `relay_commanded_now_for_ct[ch]` false) **and** raises
`safety_guard_input_t::current_sensing_disabled`. `safety_guards.c` then
branches on the flag itself. Both halves are required and neither is
sufficient — forcing the inputs alone would make S3/S9 silently *pass* and S4
warn permanently; the flag alone would leave the raw offset-floor reading
reachable.

| Guard | Class | With CTs declared absent |
|---|---|---|
| S3 `LOAD_STUCK_ON` | TRIP | **Inert.** Accumulator held at zero. Without this the board trips within `stuck_on_time_s` of boot: an uncalibrated channel's AD8542 offset floor reads as "current present" on every tick forever (`current_presence_policy.h`), with nothing commanded on. Not merely unprotected — actively unsafe by nuisance. |
| S4 load-inactive | WARN | **Inert.** Otherwise it asserts on every firing, and a warning that is always on trains an operator to ignore warnings. |
| S9 `TRIP_INEFFECTIVE` | TRIP (unclearable) | **Inert, and distinctly so.** Already gated by `current_sensing_commissioned`, but that gate reports `s9_uncommissioned_warn` — *"finish commissioning and this comes back"* — which is a lie on a board with no sensor to commission. The disabled flag takes precedence and suppresses that warn. |
| S14 over-current | WARN | **Inert per channel.** Accumulator held at zero. |
| S6b `LINK_DEAD` | TRIP | **Degraded, deliberately.** The soft, current-keyed tier (`link_timeout_s`) becomes unreachable; the unconditional `link_dead_hard_s` backstop still fires. **This is the known, accepted cost of running without CTs.** The only local substitute for "is heat on" that does not need the (by definition dead) link is SaftyFW's own K4 energization state, and re-keying a TRIP-class guard onto a different input changes what S6b *means* — a `SAFETY_MODEL.md` §4 decision, not this pass's. |
| everything else | — | Unchanged. S7 (E-stop) is the check that would notice a flag accidentally wired into the shared context block, since it sits before it. |

**Inactive is reported, never silent.** `safety_guard_state_t::ct_guards_disabled`
is set on **every** path through `safety_guards_tick()`, including the
already-tripped one, so "these four guards did not fire" stays distinguishable
from "these four guards are not watching".

**Tests.** `test/test_safety_guards.c::test_ct_disabled_guards()` — every
check is a **pair**: the same tick evaluated once armed and once disabled. The
armed halves prove S3 really does trip, S4 really does warn, S9 really does
raise its uncommissioned warn, and S14 really does warn on the same inputs, so
the disabled halves are not passing vacuously. `test/test_config_store.c` adds
the gate's four cases (unanswered ⇒ strict; *yes* + no map ⇒ still refused;
*no* ⇒ commissioned without a map; *yes* after *no* ⇒ the map requirement comes
back) plus the pack/unpack round trip and the legacy-byte decode (every byte
except the `0xA5` sentinel decodes as INSTALLED — with the sentinel itself
checked so the loop cannot pass by the decoder simply always returning 1).
`test/test_commissioning_gate.c` states the same thing end to end at the gate.

**Negative-tested.** Eight breaks were introduced one at a time and each was
caught by the tests that cover it, then restored: S3's inert branch, S4's
inert term, S9's disabled precedence, S14's inert term, `ct_installed` being
required, the CT-map relaxation in both directions, and the legacy-byte
decode. **One gap, recorded honestly:** breaking `safety_core.c`'s forcing of
`any_current_present` is **not** caught — `safety_core.c` is RTOS/target-only
and is not in the host-test build, the same coverage boundary every other
`safety_core` producer already sits behind. The redundant branch inside
`safety_guards.c` is what makes that gap non-fatal rather than merely
unmeasured.

## 10. `virtual_dut` above-the-polarity-layer audit (ROADMAP.md M15 A4, 2026-09-04)

No file or module named `virtual_dut` exists in this repo (`SimFW`/`kilnsim`,
which the term is probably inherited from, were deleted 2026-08-28 — see
durable memory `project_simfw_and_kilnsim_removed`). The term names a
**pattern**, not a component: `discrete_task.c`'s own comment on the shipped
S7 bug —  *"virtual_dut synthesizes `estop_pressed` directly and never
exercises a GPIO read"* — describes any host test that hands
`safety_guard_input_t` (or another already-reduced value) straight to the
code under test, bypassing whatever raw-hardware decode/polarity/scaling
layer a real board would have gone through first. This section is the
requested audit of every synthesized input against that pattern.

**Method.** For each input `safety_guard_input_t` carries (its own field-by-
field header comment in `src/safety_guards.h` already documents that the
struct's contract IS pre-reduced scalars — that is not itself the bug), trace
backward from the field to its real producer and ask: does a pure,
host-tested decode/policy function sit between the raw hardware value and
this field, or does some test synthesize the field directly with nothing
underneath it?

| Input | Real decode/policy layer | Host-tested? | Injection point audited | Bug class invisible if skipped | Severity | Status |
|---|---|---|---|---|---|---|
| `estop_pressed` (S7) | `discrete_pin_policy_estop_asserted()` | Yes — `test/test_discrete_pin_policy.c` | `test_safety_guards.c` sets the field directly (post-decode, by struct contract); `safety_core_build_input()`'s one-line, unnegated assignment had no coverage | GPIO polarity inversion at the decode call site — **this is the exact bug that shipped** (S7, fixed 2026-08-24) | Was **critical** (shipped); residual risk was the un-tested wiring line | **Fixed historically** (policy extraction + test); **wiring line now covered** — added `test/test_safety_core_polarity_wiring.c` §below |
| `main_fault_asserted` (S6a) | `discrete_pin_policy_main_fault_asserted()` | Yes — `test/test_discrete_pin_policy.c` | Same shape as `estop_pressed`: pure layer tested, `safety_core_build_input()`'s unnegated assignment previously untested | Same class as S7, opposite guard | Was **high** (S6a was found unwired in §6c, fixed 2026-08-24); residual wiring-line risk | **Wiring line now covered** — same new test |
| `tc_valid`/`tc_c`/`cj_c`/`fault_bits`/`spi_failed` (S5) | `max31856_read()` (raw SPI + register decode) → `max31856_tc_range_policy.c`, `max31856_tc_type_policy.c`, `max31856_fault_pin_policy.c` | Policy layers yes (`test_max31856_tc_range_policy.c`, `test_max31856_tc_type_policy.c`, `test_max31856_fault_pin_policy.c`, `test_max31856_decode.c`); the raw SPI/register decode inside `max31856.c` itself is not host-testable (needs the SPI peripheral) | `test_safety_guards.c` synthesizes these fields directly (contract-correct); `thermo_task.c` wires the real functions | Fault-pin/CR1 polarity bug inside `max31856.c` | High if present | **Not touched** — `max31856.c` is currently being edited by another agent (fault-pin polarity extraction); documented only per this task's own instruction, not relocated |
| `any_current_present`, `amps[]`, `amps_valid[]` (S3/S4/S9/S14) | `current_presence_policy.c`, `ct_amps_cal.c` | Yes — `test/test_current_presence_policy.c`, `test/test_ct_amps_cal.c` call the real pure functions | `test_safety_guards.c` synthesizes the reduced fields (contract-correct); `safety_core.c`'s forcing of these to the no-info state when `current_sensing_disabled` is true is RTOS-only | Uncalibrated/disabled-CT forcing silently dropped | Documented as an accepted, known gap already (§9, "One gap, recorded honestly") | **Already documented** — no new work needed |
| `link_up`, context fields (S2/S6b/S10/S13) | `kilnlink_*_decode()` wire codecs, `snapshots.c`'s `context_reduce_zones()`/`current_any_present()` | Yes — `test_link_frame.c`, `test_link_frame_wire.c`, `test_kilnlink_power.c`, `test_kilnlink_inject_tc.c`, `test_snapshots.c` | `test_safety_guards.c` synthesizes the reduced fields (contract-correct) | Wire decode / frame CRC bug | Low residual — codecs are directly host-tested | **Already fixed historically** — no action |
| `relay_deenergized` (S9) | `relay_owner_is_energized()`, negated once at the `safety_core_build_input()` call site | No pure layer exists (a one-line negation of a hardware-owned getter) — **and no test exercised the negation before this pass** | `test_safety_guards.c` synthesizes the field directly (contract-correct); the negation itself was untested | A dropped or doubled `!` makes S9 (the one guard whose entire job is "prove the trip actually worked", unclearable except at the breaker) trust a contactor that never opened | **High** — same "single bare `!`, zero coverage" shape as the shipped S7 bug | **Relocated** — added a source-text scan (`test/test_safety_core_polarity_wiring.c`), negative-tested below |
| `sample_counter_advancing` (S13) | `snapshots.c` (pure, no raw layer needed — it's a plain counter comparison) | Yes — `test/test_snapshots.c` | contract-correct | n/a | Low | No action needed |
| The whole `safety_guard_input_t` struct itself | — | — | `test/test_safety_guards.c` is the canonical injection point for every field above, **by design** (the struct's own header comment: pre-reduced scalars are its documented contract, the same "already reduced by the caller" split every guard in this file relies on) | None on its own — this is the intended seam. The risk lives entirely in whether each field's OWN producer is covered (rows above), not in this file existing | n/a | **Out of scope for this task** — `test/test_safety_guards.c` and `src/safety_guards.*` are owned by another concurrent session per this task's own instructions; left untouched |

**Relocation done this pass.** One new file,
`test/test_safety_core_polarity_wiring.c` (wired into `test/test_main.c` and
`test/build_host_tests.ps1`, same source-text-scan technique
`test/test_safety_core_s8_wiring.c` established for S8, since
`safety_core.c` itself cannot be linked into a host test — FreeRTOS/pico-sdk).
It pins the exact text of three one-line assignments inside
`safety_core_build_input()`:

```c
.estop_pressed       = discrete_task_estop_pressed()   // must NOT be negated
.main_fault_asserted = discrete_task_main_fault()       // must NOT be negated
.relay_deenergized   = !relay_owner_is_energized()      // must be negated, exactly once
```

**Negative-tested.** Each of the three lines was mutated one at a time
(drop/add a `!`) and the corresponding new check failed as expected before
the line was restored: flipping `.estop_pressed` or `.main_fault_asserted`'s
negation each dropped the suite to 2067/2068 with the matching `FAIL` line
naming the guard and the consequence; flipping `.relay_deenergized`'s
negation did the same for S9. All three were restored and the suite returned
to green.

**Not relocated, documented instead.** The S5 (thermocouple) raw-decode layer
inside `max31856.c` — this task was explicitly told another agent is
currently extracting that file's fault-pin polarity, so it was left alone
per this task's own instruction rather than risking a collision. The
`current_sensing_disabled` forcing gap was already recorded honestly in §9
before this pass and needed no new documentation. `test/test_safety_guards.c`
itself is the by-design injection seam for the whole `safety_guard_input_t`
contract and is owned by another session; not touched.
