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
and pass/fail. `firmware/KilnFW/docs/PROJECT_STATUS.md` is the model — it distinguishes
"built", "compiles", "live-verified on hardware", and it is scrupulous about
which is which.

Be equally scrupulous here. `SAFETY_MODEL.md`'s summary table should carry a
verification state per row, and a guard that is implemented but unprovoked
should say so rather than being listed as coverage.


---

## Completion checklist

**Host**
- [ ] Nuisance-rejection tests written **before** trip tests, all of §1
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
- [ ] Fuzz over every decoder (`firmware/CommonFW/test`)

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

## 5. SimFW scenario cross-reference (added 2026-08-20)

`firmware/SimFW` is a bench-fixture firmware — a second Raspberry Pi Pico
that plugs into the main board in place of the real thermocouple
daughterboard and the rest of the kiln, driven from a PC by `kilnsim`
(`tools/PcTools/src/kilnsim/`). Its owning plan is
`firmware/SimFW/docs/DESIGN_NOTES.md`; section 8 there defines a 17-scenario
standard test library (`firmware/SimFW/scenarios/*.yaml`), and each scenario
file declares an `exercises:` list of the guard IDs it is meant to provoke.
This section is that cross-reference in the other direction — guard → the
scenario(s) that exercise it — so anyone working this matrix's §3 rows can
find the automated test that corresponds to a given guard.

**Read this table's claim carefully: it says a scenario file exists that
targets this guard, nothing more.** `SimFW`'s software is complete and
host-tested (`firmware/SimFW/docs/PLAN.md` section 10), but **no fixture
hardware has ever been built**, so **none of these scenarios has ever run
against a real `SaftyFW` board** — a scenario existing, or even loading
cleanly through `kilnsim`'s scenario loader (which all 17 do, pytest-
verified), is not the same as it having been run, and it is absolutely not
the same as passing on hardware. Every §3 row above stays exactly as
unverified as its own checkbox says until a real run happens and gets
recorded per section 4's convention.

| Guard | Scenario(s) that declare `exercises: [<guard>]` |
|---|---|
| S1 | `main_safety_skew`, `tc_noise_storm` |
| S2 | `tc_noise_storm`, `s2_setpoint_overshoot` |
| S3 | `runaway_zone`, `welded_contactor_s9`, `welded_ssr_midfire`, `stuck_load_no_command_s3`, `enabled_firing_healthy` |
| S4 | `broken_element`, `welded_ssr_midfire`, `commanded_no_current_s4`, `enabled_firing_healthy` |
| S5 | `cj_fault`, `spi_flaky_tc_ic`, `tc_disconnect_ramp`, `tc_disconnect_soak`, `tc_flaky` |
| S6a | `mainfault_tc_disconnect` |
| S6b | `power_blip` |
| S7 | `estop_at_boot`, `estop_midfire` |
| S8 | `runaway_zone` (S8 itself ships disabled per `SAFETY_MODEL.md`; this scenario documents expected *current* behavior, ready for when S8 gets a measured threshold — `DESIGN_NOTES.md` section 8 item 12) |
| S9 | `welded_contactor_s9` |
| S10 | `main_safety_skew`, `tc_noise_storm`, `main_safety_disagree_s10` |
| S11 | `safety_tc_frozen`, `safety_healthy_reading_s11` |
| S12 | `cj_fault` |
| S13 | `tc_stuck` |

**2026-08-21 update: which of the above actually PROVOKE their guard today,
not merely declare it (25 scenario files now, `virtual_dut` re-run against
all of them).** The table above answers "does a scenario targeting this
guard exist"; it does not say whether that scenario's own `expect:` clauses
actually pass against present-day SaftyFW through this fixture. That
distinction matters enough that it gets its own accounting:

| Guard | Fires for real in `virtual_dut` today? | Which scenario, and how |
|---|---|---|
| S1 | No | Blocked on `abs_max_temp_c` commissioning (see §6) — `main_safety_skew`'s S1 clause is `BLOCKED`, not evaluable regardless of the fixture. |
| S2 | **Yes** | `s2_setpoint_overshoot` (new this pass): `GUARD_TRIP {S2}` at 267.0s after K4 closes. `tc_noise_storm`'s S2 citation is still not provoked — that file declares no `setpoint_c`, so S2 stays structurally inactive there (not "quiet because healthy"). |
| S3 | **Yes** | `stuck_load_no_command_s3` (genuine trip, 19.4s after the weld), `welded_contactor_s9` (initiating trip, 19.4s), `welded_ssr_midfire` (mid-fire trip after the 150s correlation window, 179.0s), `enabled_firing_healthy` (genuine anti-nuisance: current commanded, S3 stays quiet for the whole run). `runaway_zone` still cannot reach it — see §6. |
| S4 | **Yes** | `commanded_no_current_s4` (genuine WARN, 150.8s after K1 closes, K4 stays closed — the WARN-only property proven positively) and `enabled_firing_healthy` (genuine anti-nuisance). `broken_element`/`welded_ssr_midfire`'s own S4 clauses remain `BLOCKED` (no `operator_actions:` in those specific files). |
| S5 | **Yes** | `tc_flaky`'s `no_warn_storm`, `cj_fault`'s `s5_never_trips_on_cj_alone` — no `context_valid`/K4 gating at all on this guard. |
| S6a | **No — see §6, not provokable through this harness at all** | `mainfault_tc_disconnect`'s two S6a clauses are both `BLOCKED` on a fixture-level gap, not a SaftyFW gap. |
| S6b | Only the anti-nuisance half | `power_blip`'s `mainfault_reads_healthy_during_blip` PASSes; its trip-side clause (`s6b_stays_at_warn_not_trip`) stays `BLOCKED` for an unrelated K4-boot-open reason (see the scenario file). |
| S7 | **Yes** | `estop_at_boot`/`estop_midfire` both PASS on their observable clauses. |
| S9 | **Yes** | `welded_contactor_s9`: `TRIP_INEFFECTIVE_LATCHED` 9.6s after K4 opens, current genuinely persists through the open contact (`contactor_weld_engages_on_k4_open` PASSes via a `forbid …/after:` clause form). |
| S10 | **Yes** | `main_safety_disagree_s10` (new this pass): genuine `GUARD_WARN {S10}` at 294.0s after a +250°C skew, with `main_safety_skew`'s 80°C sub-threshold skew as the standing anti-nuisance control (`s10_stays_quiet`, unchanged, still PASSing). |
| S11 | **Yes, both directions** | `safety_tc_frozen` (fixed this pass): genuine `GUARD_TRIP {S11}` at 664.0s after the freeze fault, with `operator_actions:` now supplying the closed K4 + current the guard's `heat_commanded` gate needs. `safety_healthy_reading_s11` (new this pass): the same operator actions, no fault, run for a comparable ~650s span — S11 never trips, because a live ADC reading is never bit-identical tick to tick. |
| S12 | **Yes** | `cj_fault`'s own history (see the "2026-08-20 follow-up" note above) already established this; unchanged this pass. |
| S13 | No | Blocked on two commissioning gaps (`borrowed_zone_index`, `tc_source`) — see §6. `tc_stuck`'s clauses stay `BLOCKED`. |

Full detail, measured numbers, and exact `blocked_on:` reasoning for every
non-PASS clause: `firmware/SimFW/tools/virtual_dut/results/SCENARIO_RESULTS.md`.

Two scenarios (`baseline_firing`, `partial_element`) declare an empty
`exercises: []` — they are regression/behavior baselines (no-fault firing,
partial-power ramp handling), not guard-provocation tests, and are listed
here for completeness rather than omitted silently.

**Notes and gaps found while building this table:**
- `power_blip.yaml` declares `exercises: [S6]`, not `[S6a]`/`[S6b]`
  separately, even though this matrix and `safety_guards.c` treat S6a
  (`mainFault`) and S6b (link-silence backstop) as distinct guards with
  distinct provocation rows above. Recorded as-is rather than silently
  reinterpreted — resolve which sub-guard (or both) `power_blip` actually
  targets before treating it as S6a or S6b coverage specifically.
- **S11 has no corresponding scenario.** Every other guard in
  `SAFETY_MODEL.md` section 4 has at least one scenario file; S11 (frozen
  reading, six-hour cold-idle nuisance case per section 1's table above) does
  not. Not something this pass can fix (scenario files are outside this
  document's ownership), but worth flagging so it does not go unnoticed the
  next time the SimFW scenario library is extended.
- This table was built by reading each scenario file's `exercises:` line
  directly (`firmware/SimFW/scenarios/*.yaml`, 17 files) — not by asking
  `SimFW`'s own plan to summarize itself — so it reflects the scenarios as
  written on 2026-08-20, not an aspirational mapping.

**2026-08-20 follow-up: both gaps above closed, 19 scenario files now.**
- `power_blip.yaml`'s `exercises:` was corrected from generic `S6` to the
  specific `S6b` it actually exercises: `safety_guards.c`'s S6a block reads
  only `in->main_fault_asserted`, and an unpowered ESP (R8 pulling GPIO10
  high) has no way to set that true — confirmed against the code, not just
  re-asserting the scenario file's own prior comment. S6a genuinely cannot
  be provoked by an unannounced DUT power cut, by design (SAFETY_MODEL.md
  §4 S6: "`mainFault` cannot detect a dead ESP").
- A new scenario, `mainfault_tc_disconnect.yaml`, closes the resulting S6a
  gap: it disconnects a **main-side** TC channel (`tc:0`) while a KilnFW
  profile is actively running that zone, which (per
  `firmware/KilnFW/docs/SAFETY_MODEL.md` and `App/drivers/safety_link.h`)
  makes KilnFW's own guard 6 assert a live `SAFETY_FAULT_SRC_THERMO`,
  pulling the Pico's `mainFault` input low and tripping S6a — the only path
  this fixture has to provoke S6a at all, since the `Fault` line is
  ESP-driven and the fixture only senses it (DESIGN_NOTES.md §3.4).
- A new scenario, `safety_tc_frozen.yaml`, closes the S11 gap: it freezes
  the safety-side channel (`tc:safety`) via the existing `stuck_tc` fault
  type and expects a trip once `frozen_window_s` (600s default) elapses
  with heat commanded throughout. Its own `manual_checks` flag a real,
  separate finding made while writing it: `firmware/SaftyFW/src/tasks/
  safety_core.c` currently hardcodes `heat_commanded = false` when building
  `safety_guard_input_t` ("no current sense yet, Phase 6" per its own
  comment), so **S11 cannot actually trip against present-day SaftyFW
  regardless of what any fixture does** — the guard logic and this
  scenario's provocation both match the documented design; what's missing
  is the Phase-6 current-sense wiring already flagged as future work at
  that call site. This is a SaftyFW-side wiring gap, not a scenario-writing
  or fixture-capability gap, and is recorded here rather than papered over.
- Both new scenarios pass `firmware/SimFW/tools/check_scenarios.py`
  (schema, guard-ID, and fault-type/trigger-kind validation) and load
  cleanly through `kilnsim`'s scenario loader, same as the other 17 — see
  this repo's commit history for the exact check output. As with every
  other row in this table, **a scenario existing and loading is not the
  same as it having run against hardware**; none of the 19 have.

---

## 6. Guard reachability in current SaftyFW (added 2026-08-20)

**This section answers a different question from §5 above.** §5 says which
scenario *targets* each guard. This section says whether the guard can
*actually fire at all* against today's shipping `SaftyFW`, independent of
any scenario or fixture — because a guard's pure logic being implemented and
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
   transition. Its full run against all 19 scenarios is in
   `firmware/SimFW/tools/virtual_dut/results/SCENARIO_RESULTS.md`, and its
   own README's "Findings" section reaches the same verdicts below
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
> The `virtual_dut` re-run confirming this is in
> `firmware/SimFW/tools/virtual_dut/results/SCENARIO_RESULTS.md`. Read its
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
| S7 | **Yes** | — | `estop_pressed` comes from `discrete_task_estop_pressed()`, a real, debounced (50 ms) GPIO9 reading, called directly in `safety_core_build_input()`. |
| S9 | **Yes** (since the `relay_deenergized` wiring) | — | `relay_deenergized` now comes from `!relay_owner_is_energized()`, named in `safety_core_build_input()` right after `.reboot_grace_active` (the struct's own field order). ~~Never computed — nothing plumbs `relay_owner_is_energized()`'s inverse into the input struct. This is the one row `f304392` did not move at all.~~ **Polarity is one deliberate negation, cross-checked three ways**: `relay_owner.h` publishes the affirmative ("true only while GPIO6 is actually being driven high right now"), `safety_guards.h` asks for the negative ("set by the caller once relay_owner has actually de-energized K4"), and `safety_core_get_output_status()` — twenty lines below, same file, same getter — reads it with *no* `!` for its `out_relay_energized`; the two call sites differ by exactly one negation because the two field names are exact opposites. **No GRACE suppression, and none is needed**: K4 genuinely reads de-energized for the whole 60 s startup GRACE window, but `safety_guards_tick()` only evaluates S9 inside its `if (state->is_tripped)` branch and re-zeros `s9_verify_elapsed_s` at the bottom of every untripped tick, so this is *not* the free-running-timer shape that made S6b nuisance-trip every boot before `f304392`; and if a trip does land during GRACE, "K4 open yet current still flowing for `trip_verify_s`" is a real welded contactor regardless of GRACE. ~~K4 is never energized in the first place today (`relay_owner_command_energize()` has zero callers anywhere in the tree) … no scenario models the operator enable step.~~ **Provoked end to end since the operator-actions pass**: `welded_contactor_s9.yaml` issues `request_enable`, lets S3 trip on an uncommanded weld, then a second, persisted-current fault survives K4 opening — `TRIP_INEFFECTIVE_LATCHED` measured 9.6 s after K4 opens. |
| S10 | **Yes** (since `f304392`) | — | Same `context_valid` wiring as S2; `nearest_zone_measured_c` comes from `context_reduce_zones()`'s nearest-match search (never the mean). ~~Same as S2.~~ **And genuinely exercised, both directions**: `main_safety_skew`'s `s10_stays_quiet` (an +80 °C skew must stay under `tc_disagreement_c` = 200 °C) is a real anti-nuisance PASS, and `main_safety_disagree_s10.yaml` (2026-08-21) is the genuine-WARN case at +250 °C: `GUARD_WARN {S10}` measured 294.0 s after the fault fires. S10 is WARN-only (no `SAFETY_TRIP_*` case exists for it), so this is as far as "trip" can mean for this guard. |
| S11 | **Yes** (since the `heat_commanded` wiring) | — | `heat_commanded` now comes from the same `any_current_present` value S3/S4/S6b already read (`current_task`'s real ADC0/1/2 snapshot via `current_any_present()`), named in `safety_core_build_input()` right after `.main_fault_asserted`. ~~Hardcoded `false` in `safety_core_build_input()` — its own comment: "no current sense yet, Phase 6."~~ Deliberately **not** wired from `relay_commanded_recently`/`_continuously` even though both also approximate "heat commanded": `safety_guards.h`'s own header comment requires this field stay link-independent (no `context_snapshot_t`-derived fact), matching `SAFETY_MODEL.md` §6's own S11/S13 audit note ("S11 reads neither [`link_up` nor `context_valid`]") and S11's place on the "keeps running with authority over K4 even when the main controller is unknown" list (§6) — `current_any_present` is real, independent hardware (its own ADC), never context-gated, exactly like S6b's own unconditional use of the same value. ~~Not yet provokable by `virtual_dut`: `sim_engine.c` gates CT current on K4, which never closes there.~~ **Provoked both directions since 2026-08-21**: `safety_tc_frozen.yaml` (fixed this pass, gained `operator_actions:`) trips for real — `GUARD_TRIP {S11}` measured 664.0 s after the freeze fault, with K4 closed and current flowing continuously from ~61 s onward. `safety_healthy_reading_s11.yaml` (new) is the anti-nuisance control: identical setup, no fault, run for a comparable ~650 s span — S11 never trips, because a live, unfaulted ADC reading is never bit-identical between ticks (measured, not asserted: `virtual_simfw`'s own MAX31856 emulation carries real noise/quantization). |
| S12 | **Yes** | — | `cj_c` comes from the same real `thermo_task` snapshot as S1/S5/S11, with **no** `context_valid` or `link_up` gating at all — the guard's own code puts it before the context-gated block. ~~Not observed firing … `cj_fault.yaml` has no numeric fault offset~~ — that scenario-file gap was closed (`params: [70.0]`) and S12 has since been observed genuinely warning *and* tripping in `cj_fault`. |
| S13 | **No** | `in->sample_counter_advancing` + `cfg->tc_source` | ~~Same as S2 (S13 is additionally gated on `cfg->tc_source` …).~~ `context_valid` no longer blocks it, but the two remaining blocks are both **commissioning gaps, not producer gaps** — the same category as S1's row above, and deliberately left that way by `f304392`. Deciding whether a zone's `sample_counter` advanced requires a commissioned `borrowed_zone_index` (0..2) naming *which* context zone is the borrowed channel; that field is documented (`SAFETY_MODEL.md` §3, `CONFIG_REFERENCE.md`) but exists nowhere in the codebase — no `config_store` field, no `safety_guard_cfg_t` field — so `safety_core_build_input()` leaves `sample_counter_advancing` false rather than hardcoding zone 0 and being silently wrong on any installation whose borrowed zone is not zone 0. Independently, `cfg->tc_source` defaults to `OWN_J7`, which `safety_guards.c` gates the whole S13 block on. |

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
the only way to exercise it. See
`firmware/SimFW/tools/virtual_dut/results/SCENARIO_RESULTS.md` for the exact
measured numbers behind every "provoked" claim above.

**Re-checking this table:** re-run `firmware/SimFW/tools/virtual_dut/
run_dut_scenarios.py` against all scenarios (25 as of 2026-08-21) any time
`link_task` (Phase 7) or `current_task` (Phase 6) wiring changes in
`safety_core.c`, or any time a scenario's own `operator_actions:`/
`zone_setpoints:` change — newly-reachable/newly-provoked guards will show
real `guard_warn`/`guard_trip` events in `results/SCENARIO_RESULTS.md` where
they previously showed none. Update this table's "Reachable today?" column
in the same change, per this file's own "keep this file current" rule at
the top.

**And re-check the fixture itself in the same pass.** `f304392` is the
cautionary case: it changed `safety_core_build_input()` but not
`virtual_dut/dut_core/main.c`, which is an *independent hand-written
stand-in* for that function (the real one is FreeRTOS-shaped and cannot be
host-compiled). The re-run produced byte-identical verdicts and was briefly
recorded as "no delta", when in fact the fixture was simply still mirroring
the pre-fix code. `dut_core/main.c` now `#include`s
`firmware/SaftyFW/src/snapshots.h` and calls the **real**
`context_reduce_zones()`/`current_any_present()` rather than reimplementing
them, so that class of silent drift is limited to the FreeRTOS-shaped glue
around them. Any future change to `safety_core_build_input()` must be
mirrored there in the same commit, and any pure helper it gains should live
in `snapshots.h` (or another SDK-free header) so the fixture can compile the
real thing instead of copying it.
