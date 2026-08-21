# SimFW — Kiln Simulator / Unit-Test Fixture Plan

> **Status:** software complete, hardware-gated · **Last reviewed:** 2026-08-20
>
> **See [section 0](#0-whats-left--index) for the short checklist of what
> remains.** Sections 10, 11, 12, 13 and 14 carry per-item checkboxes.
>
> **What is actually true right now.** Every task and every `src/sim/` module
> in this plan's architecture is implemented — no stubs remain anywhere in
> `firmware/SimFW/src/` — and the standard 17-scenario test library
> (section 8) exists as real YAML. That is **build-verified and
> host-test-verified**, not hardware-verified, and the two are not the same
> thing:
> - **Build-verified:** `firmware/SimFW` builds clean under the real
>   arm-none-eabi-gcc/pico-sdk/FreeRTOS-SMP toolchain, `-Wall -Wextra -Werror`,
>   producing `SimFW.elf`.
> - **Host-test-verified:** the pure `src/sim/` modules (thermal model,
>   MAX31856 register machine, sine synth, fault engine, TC fault state) pass
>   their MSVC/CMake host-test suite (4873/4873 checks at the landing commit,
>   `c891b72`); the new `CommonFW` `benchproto` protocol library (extracted
>   from `UnitTestFw`, section 12) passes its own host tests (18/18,
>   framing+link) alongside the pre-existing `kilnlink` suite; `kilnsim`, the
>   fresh PC toolset in `tools/PcTools/src/kilnsim/`, has its own pytest suite
>   (59 passed + 17 subtests at that commit) and the pre-existing `kilnctrl`
>   suite (104 passed) is confirmed untouched; all 17 scenario YAML files
>   (section 8) load cleanly through `kilnsim`'s loader.
> - **Genuinely hardware-gated — nothing below has ever touched real
>   silicon:** the PIO SPI slave timing proof at 5 MHz (M-A's exit
>   criterion — no Saleae capture exists, no fixture hardware has ever been
>   built or connected to a bench ESP32/Pico); the `UnitTestFw` decommission
>   (section 12 — gated on SimFW's replacement link being *proven on real
>   hardware*, so `firmware/UnitTestFw/` still exists in the tree, untouched);
>   the CT calibration procedure (section 3.3/4/M-D — `wave_owner.c`'s
>   amplitude mapping is currently an IDENTITY placeholder, explicitly marked
>   `TODO(M-D calibration)` in its own header comment, not the real
>   sweep-and-fit table); every relay-sense, E-stop, DUT-power, and
>   ground-isolation claim in section 3; and every one of the 17 scenarios
>   actually *running* against a real `KilnFW`+`SaftyFW` pair (a scenario
>   existing and loading is not the same as it having ever executed against
>   hardware — see section 10's milestone table for exactly what remains).
>
> **New this pass — a fourth, software-only verification layer exists, and it
already found real bugs, not in SimFW but in `SaftyFW`.**
`firmware/SimFW/tools/virtual_simfw/` compiles SimFW's own `src/sim/*.c`
**unmodified** and serves the real `benchproto` wire protocol over TCP, so a
complete scenario can run against the actual simulation logic with no RP2040
ever attached — see that tool's own README for exactly what it does and does
not reproduce. `firmware/SimFW/tools/virtual_dut/` goes one step further: it
compiles `SaftyFW`'s real, unmodified `safety_guards.c` and `relay_grace.c`
for the host and ticks them against `virtual_simfw`'s live data at the real
100 ms cadence, turning their real verdicts into events `kilnsim`'s own
report evaluator can genuinely PASS/FAIL instead of skipping. Neither tool
touches real silicon and neither claims to — this is **not** a substitute for
section 10's hardware-gated milestones, and every one of them stays exactly
as unmet as stated below. But running real guard code against a simulated
kiln did something no host test with synthetic inputs ever could: it
established, empirically and reproducibly, that **in today's shipping
`SaftyFW`, only S5, S6b, S7, and S12 can structurally fire.** The other nine
guards are blocked by specific inputs `safety_core_build_input()`
(`firmware/SaftyFW/src/tasks/safety_core.c`) never populates:

> **SUPERSEDED 2026-08-20 (later the same day) — read this before the list
> below.** The nine-blocked-guards finding was acted on. Commits `f304392`
> (context/`link_up`/`REQUEST_ENABLE` wiring) and `5375bca` (S6a) closed most
> of these gaps in `SaftyFW` itself. **Current state: only S9
> (`relay_deenergized`) and S11 (`heat_commanded`) still have inputs that are
> never produced.** S1 and S13 remain deliberately dormant as *commissioning*
> gaps (uncommissioned `abs_max_temp_c`; no `borrowed_zone_index` field
> exists), which is a different category from a missing producer. The list
> below is kept as written because it is the evidence trail that motivated
> those fixes — treat it as history, not as current state. The live table is
> `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §6.

- **S6b trips unconditionally ~120 s into every boot.** `link_up` is never
  set true anywhere — `safety_core_build_input()`'s struct literal doesn't
  name it, so it is `false` from the first tick, on real hardware too, not
  just this harness. Correction to an earlier draft of this finding: it is
  **not** that `link_task.c` doesn't exist — it does, and per ROADMAP M5 it
  is substantially built (context frames, DIAG, STATUS, TRIP_EVENT). The gap
  is narrower and still real: `safety_core.c` has a standing `TODO (Phase
  7): context_snapshot_t is read here too, once link_task publishes one`,
  and nothing has done that yet, so `safety_core` never asks `link_task`
  whether the link is alive.
- **K4 is never energized.** `relay_owner_command_energize()` has zero
  callers anywhere in the tree (confirmed by grep, not just the one obvious
  call site) — `relay_owner_task()` starts in GRACE and, once GRACE expires,
  nothing ever asks for an energize. K4 reads open from t=0 on every boot.
- **S11 cannot trip.** `safety_core_build_input()` hardcodes
  `heat_commanded = false` ("no current sense yet, Phase 6" per its own
  comment), and S11's trip condition requires `heat_commanded` true.
- **S2, S3, S4, S10, S13 cannot fire or warn.** All five are gated on
  `context_valid`, which `safety_core_build_input()` never sets true (same
  Phase 7 link-context gap as S6b, one level up).
- **S9 cannot escalate.** Gated on `relay_deenergized`, which nothing ever
  computes into the input struct (`relay_owner_is_energized()`'s inverse is
  never plumbed through).
- **S6a cannot fire — a narrower, more mechanical gap than the others.**
  `main_fault_asserted` is never populated in `safety_core_build_input()`,
  even though the debounced reading it needs, `discrete_task_main_fault()`,
  already exists and works (the same file calls
  `discrete_task_estop_pressed()` for S7 two lines away) — `safety_core.c`
  simply never calls it for S6a. Unlike S6b/S2-S4/S9/S10/S13, this one has
  no missing producer at all; it is a one-line wiring omission.
- **S1 does not trip today, but for a different reason than the above, and
  this correction matters: it is not `context_valid`-gated.** `abs_max_temp_c`
  defaults to 0, and `safety_guards.c`'s own convention is that 0 means "not
  commissioned, never trip" — a deliberate safety choice
  (`safety_guards.h`'s doc comment), not a missing producer. S1's guard logic
  is otherwise fully wired (real `tc_c` from `thermo_task`) and will trip
  correctly the moment a real ceiling is commissioned (`config_store`, Phase
  9, already exists per ROADMAP M3).
- **S12 (cold junction / enclosure over-temperature) is structurally
  reachable today** — depends only on the real `cj_c` reading from
  `thermo_task`, with no `context_valid` or `link_up` gating at all. It
  wasn't observed firing in this pass's `virtual_dut` run, but for an
  unrelated reason: `scenarios/cj_fault.yaml` has no numeric fault offset,
  so the simulated cold junction never actually moves — a scenario-file gap,
  not a guard defect (see `virtual_dut/README.md` Finding 6).

Every one of these was verified by reading `safety_core.c`/`safety_guards.c`
directly, then cross-checked against `virtual_dut`'s independent run of the
real, unmodified code — the two agree. **This is not a discovery of
regressions; it precisely quantifies known incompleteness.** `SAFETY_MODEL.md`
and `SaftyFW/TODO.md` already say Phases 6/7 (link context, current sense)
are unbuilt — what's new is the exact, guard-by-guard list of what that
means in practice, and the fact that it can now be re-checked automatically
any time those phases land, by re-running `virtual_dut` against the same 19
scenarios. See `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`'s new
reachability section for the same table in its natural home, and section 13
below for where this capability sits relative to the three testing layers
this plan originally described.

A real SPI-mode bug was found and fixed in this pass: the PIO slave engine
> originally sampled MOSI on SCLK's *rising* edge (textbook SPI mode 0) while
> labeling itself mode 1; it has been corrected to sample on the *falling*
> edge, matching the MAX31856 datasheet's own Table 5 and both real masters'
> drivers (`KilnFW`'s `MAX31856_SPI_MODE 1`, `SaftyFW`'s explicit
> `SPI_CPOL_0, SPI_CPHA_1`). See `max31856_spi_slave.pio`'s header comment and
> §3.2.1 below, which has already been corrected to describe the fixed
> behavior — treat that correction as done; it is not a remaining task.
>
> **Decisions locked 2026-08-20 (user-confirmed):** USB link uses
> `UnitTestFw`'s hardened UART protocol design (retry/dedup/task
> registration), not bare kilnlink framing; CT coupling via isolation
> transformers (**ratio revised same-day, see below — still a decided
> transformer-coupling approach, just not 1:1**); the 8 DRDY/`~FAULT` lines
> are direct Pico GPIO, not expander pins; the fixture switches the DUT's
> 12 V feed through its own relay (brownout tests in scope); project name
> stays **SimFW**; scenarios are YAML; GUI is Tk; the thermal model supports
> a configurable 1–4 zones (default 3).
>
> **Correction to that same locked decision, found while writing
> `docs/BOM.md` (2026-08-20):** the "small 1:1 audio/isolation transformers"
> language above was the user-confirmed decision at the time, but it is a
> **design error**, not a stylistic detail — see §3.3 below for the
> corrected ratio and the arithmetic behind it. The *decision to couple via
> transformer* stands; the *1:1 ratio* does not.
>
> **Also decided 2026-08-20: `UnitTestFw` was a first attempt and gets
> deleted** — all of it, including its embedded `UnitTestFixture.kicad_*`
> files and `hardware/UnitTestFixture`. Before deletion, the protocol core is
> lifted into `CommonFW` as a shared, host-tested library (SimFW consumes it
> from there); its `pc_tools` is **not** ported — `kilnsim` gets its own
> separate PC toolset written fresh against the extracted protocol spec.
> Deletion lands in the milestone that proves the replacement works (M-B).
> **The extraction (step 1 of section 12) is done — `benchproto` exists in
> `CommonFW`, host-tested, and `kilnsim`/SimFW's CDC link speak it — but
> step 2 ("prove the replacement" against real hardware) has not happened, so
> `UnitTestFw` has not been deleted yet.** See section 12.
> **Keep this file current.** This is the owning plan for the third firmware,
> `SimFW`: a second Raspberry Pi Pico that plugs into the main board's
> connectors and pretends, convincingly, to be the rest of the kiln — the
> thermocouple ICs, the current transformers, the heater load, and the thermal
> mass — while a PC drives it over USB from a CLI, a GUI, or an MCP server.

---

## 0. What's left — index

Checkbox legend used throughout this document: `[x]` done and verified,
`[~]` partially done (the text says which half), `[ ]` not started or not
met. **Done means verified at the level the item itself demands** — for a
milestone whose exit criterion names hardware, host tests passing is `[~]`,
never `[x]`.

### 0.1 Doable now, in software (no hardware required)

- [ ] **`~DRDY` is not implemented at all** in `firmware/SimFW/src/`, and the
      "reading CJTH/LTCB releases `~DRDY`" side effect **both** real masters
      depend on has no hook in the register machine. Found by the SPI access
      audit; this is a functional gap, not a timing one.
- [ ] **Re-plan the first-byte path.** The audit found PLAN §3.2.1's 1.6 µs
      budget is wrong by ~8× (real budget ~125 ns at 4 MHz), which likely
      rules out Plan A entirely. See `docs/SPI_ACCESS_AUDIT.md` §6 for the
      DMA-fed Plan B sketch. This is the biggest open *design* risk in the
      fixture.
- [ ] **Second DUT-power relay output bit.** The two-relay decision is made
      (§11 item 5) but `i2c_owner.c` still exposes only one
      `EXP1_PIN_DUT_POWER`. Spare expander capacity exists. (M-E)
- [ ] **S11 guard input** — `heat_commanded` is the last never-produced
      input in `safety_core_build_input()`, and it genuinely waits on Phase 6
      current sense. S9 was wired in `5f90325`.
- [ ] **`virtual_simfw` advances sim by timescale² per wall second.** Its loop
      scales the tick accumulator by timescale, then each tick advances sim by
      `100 ms × timescale`. At `timescale: 10` its 2 Hz telemetry lands one
      frame per ~50 sim-seconds — coarser than some guard windows. A real bug,
      found while writing the K4 scenarios (`virtual_dut/README.md` Finding 7).
- [ ] **`at_zone_temp: 400` is unreachable in `welded_ssr_midfire` and
      `welded_contactor_s9`.** `fast_test` asymptotes at ~505 °C with a ~200 s
      time constant, so 400 °C needs ~304 sim-seconds against a ~195 s run.
      Recorded in both files as a second, independent blocker.
- [ ] **`no_warn_storm` scenario FAIL** — pre-existing, survives a fine poll
      interval, still unexplained. Deliberately not loosened.
- [ ] **MCP tool to push calibration constants into `SaftyFW` flash** — no
      such path exists today.

### 0.2 Hardware-gated (nothing here can progress without the fixture)

- [ ] **M-A SPI slave timing proof** — Saleae capture, ≥10k transactions,
      zero underruns. *The single biggest unretired risk in the plan.*
- [ ] **CT calibration against `SaftyFW`'s real ADC readback** (M-D)
- [ ] **J7 pin 1 continuity check** — the two main-board docs contradict each
      other; getting it wrong back-feeds a rail or leaves the isolator side
      unpowered. (§11 item 9)
- [ ] **DUT-power inrush measurement** — the ~60 A / ~190 µs figure rests on
      an *assumed* source resistance. (§11 item 5)
- [ ] **Verify every provisional GPIO assignment** in `HARDWARE.md`
- [ ] **`UnitTestFw` decommission** — gated on proving the replacement link on
      real hardware. (§12)
- [ ] **Fixture hardware form factor** — breadboard vs a real
      `hardware/SimFixture/` board. Not answerable until M-E. (§11 item 6)

### 0.3 Done

- [x] All `src/sim/` modules and all FreeRTOS owner tasks implemented — no
      stubs remain in `firmware/SimFW/src/`
- [x] `benchproto` extracted into `CommonFW`, host-tested, spoken by both
      SimFW's CDC link and `kilnsim`
- [x] `kilnsim` PC toolset — CLI, Tk GUI, MCP server, scenario loader, report
      generator
- [x] 19-scenario standard library (over-delivered vs the planned 16)
- [x] PIO SPI mode-1 clocking bug found and fixed (was sampling as mode 0)
- [x] `virtual_simfw` / `virtual_dut` / `virtual_kiln` — the fourth,
      software-only verification layer (§13.4)
- [x] SaftyFW guard-input wiring: context/`link_up`/`REQUEST_ENABLE`
      (`f304392`) and S6a (`5375bca`)
- [x] CT calibration tooling (`tools/ct_calibration/`) — fit, crosstalk gate,
      versioned table
- [x] `docs/BOM.md`, `docs/BENCH_RUNBOOK.md`, `docs/HARDWARE.md`,
      `docs/PROTOCOL.md`
- [x] Transformer ratio corrected 1:1 → ~3:1 (1:1 could not reach ADC clip)
- [x] S9 `relay_deenergized` wired (`5f90325`)
- [x] CT calibration **mechanism** in firmware (`f93b2eb`) —
      `src/sim/ct_calibration.{c,h}` applies
      `clamp(gain[ch]*amps + offset[ch], 0, 1)`, with a generated compiled-in
      table (`tools/gen_ct_cal_table.py`). **The shipped table is
      all-uncalibrated**, so bench behavior is unchanged; an explicit
      `calibrated` flag keeps "nobody calibrated this" distinguishable from "this
      channel genuinely fits y=x". Constants remain hardware-gated — this does
      not close M-D
- [x] First K4-closing scenarios (`8383a3a`) — S3 **trips** and S4 **warns**
      genuinely, with a healthy control case where both stay quiet. 22
      scenarios now, and `any_current_present` is no longer false suite-wide
- [x] SPI access-pattern audit — `docs/SPI_ACCESS_AUDIT.md` (`46fa310`),
      which also found and fixed four responder defects: MISO permanently
      driven instead of tri-stated (three emulated chips share one physical
      MISO), TX FIFO surplus leading the next transaction, a vacuous
      `first_byte_late` counter, and a 4-bit register address space where the
      part has 7
- [x] USB identity claimed — `2E8A:F00A` fixture / `2E8A:F00B`
      `spi_test_master` (`55d81e5`). This also fixed a live bug: `link.py`
      was matching `2E8A:000A` while the firmware actually shipped TinyUSB's
      `0xCafe:0x4001`, so auto-detect could never have found the fixture.

---

## 1. Purpose and scope

The kiln controller is hard to test because the interesting behavior only
happens when a multi-kilowatt kiln is heating, drifting, and failing. This
fixture replaces the kiln. It lets every guard, fault path, and control loop in
`KilnFW` (ESP32-S3) and `SaftyFW` (RP2040, A1) be exercised on the bench,
repeatably, from a script — including the failure modes you never want to
provoke on real hardware (welded contactors, runaway zones, broken elements).

**In scope**

- Emulate every MAX31856 thermocouple IC the main board talks to: the three
  main-side channels behind J6 (CS0/CS1/CS2, ESP32 is master) and the one
  safety-side channel behind J7 (safety Pico A1 is master). Register-accurate
  SPI slave emulation including fault bits, DRDY timing, and configuration
  readback.
- Generate three independent 60 Hz AC waveforms with programmable amplitude
  into the CT jacks (J13/J15/J17), so the safety processor's current-sense
  channels see realistic burden voltages that track simulated heater current.
- Sense all five relay outputs (K1, K2, K3, K5 on the main side; K4 pilot on
  the safety side) at their terminal blocks and close the loop: relay states
  drive the thermal model, the thermal model drives the emulated
  thermocouples, and heater current appears on the CT outputs only when the
  right relays are closed *and* K4 permits.
- Simulate the kiln's thermal mass per zone: heating, soak, cooling, inter-zone
  coupling, ambient loss.
- Inject faults on demand or on a schedule: thermocouple faults, IC faults,
  relay/contactor faults, element faults, and more (section 7).
- Test all remaining main-board I/O: E-stop circuit, `Fault` line, J20 spare
  I/O, expander-driven signals — stimulus and measurement both.
- USB CDC serial control interface, driven by a new MCP server, a CLI, and an
  optional GUI in `tools/PcTools`.
- A library of standard, versioned test scenarios with a standardized fault
  timing model.

**Out of scope (for this plan)**

- The fixture PCB itself. Bring-up starts on a breadboard/protoboard harness;
  a KiCad project (likely `hardware/SimFixture/`) is a follow-on once the
  netlist stabilizes.
- Mains-voltage anything. The fixture never sees line voltage; SSR/contactor
  load terminals are simulated purely at signal level.
- Keeping `firmware/UnitTestFw` alive. It was a first attempt and gets
  deleted once its protocol is lifted into `CommonFW` (section 12).

---

## 2. System overview and connection diagram

```
                                 PC (Windows)
                 ┌──────────────────────────────────────────┐
                 │  tools/PcTools                           │
                 │   kilnsim CLI ── kilnsim GUI (Tk)        │
                 │        │             │                   │
                 │        └──── kilnsim MCP server ─────────┼── Claude / scripts
                 └──────────────────┬───────────────────────┘
                                    │ USB CDC (native RP2040 USB)
                                    │ hardened protocol (CRC, retry/dedup, task
                                    │ registration) — lifted from UnitTestFw into
                                    │ CommonFW before UnitTestFw is deleted (sec 12)
   ┌────────────────────────────────┴────────────────────────────────────┐
   │                     SimFW  —  Raspberry Pi Pico #2                  │
   │  FreeRTOS SMP, single-owner task per interface                      │
   │                                                                     │
   │  PIO0: SPI slave engine A (3x MAX31856 emu, ESP bus)                │
   │  PIO1: SPI slave engine B (1x MAX31856 emu, safety bus, isolated)   │
   │  PWM:  3x 60 Hz sine synthesis (high-rate PWM + RC filter)          │
   │  I2C0: MCP23017 #1, MCP23017 #2, (optional PCA9685)                 │
   └──┬──────┬───────┬─────────┬──────────┬──────────┬─────────┬────────┘
      │      │       │         │          │          │         │
      │      │       │         │          │          │         │
 [SPI A]  [SPI B]  [3x PWM   [I2C bus]    │          │         │
  6 pins   4 pins   +RC LPF]     │        │          │         │
      │   via dig.     │     ┌───┴──────────────────────────┐  │
      │   isolator     │     │ MCP23017 #1: relay sense x5, │  │
      │  (GND_Safty)   │     │  ESP Fault sense, E-stop     │  │
      │      │         │     │  drive (optoMOS), DUT 12V    │
      │      │         │     │  power relay, J20 IO_3/4     │  │
      │      │         │     │ MCP23017 #2: spares only     │  │
      │      │         │     │  (DRDY/~FAULT x8 moved to    │  │
      │      │         │     │  direct Pico GPIO, sec 3.4)  │  │
      │      │         │     │ PCA9685 (optional): PWM/LED  │  │
      │      │         │     │  stimulus, analog-ish tests  │  │
      │      │         │     └───┬──────────────────────────┘  │
      │      │         │         │                             │
══════╪══════╪═════════╪═════════╪═════════════════════════════╪══════════
      │      │    3x isolation   │                        (heartbeat,
      │      │    transformer    │                         debug UART,
      │      │    (decided,      │                         SWD to Debug
      │      │    sec 3.3)       │                         Probe)
      ▼      ▼         ▼         ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │                      kilnCtl MAIN BOARD                         │
   │                                                                 │
   │  GND_Main domain                    GND_Safty domain            │
   │  ─────────────────                  ──────────────────          │
   │  J6 (1x20 socket)  ◄── SPI A        J7 ◄── SPI B (isolated)     │
   │   SCLK/MOSI/MISO,                    SCLK/MOSI/MISO/CS0,        │
   │   CS0/CS1/CS2,                       thermoFault, thermoDrdy    │
   │   thermoFault_0..2,                  (safety Pico A1 is master) │
   │   thermoDrdy_0..2                                               │
   │   (ESP32-S3 is master;              J13/J15/J17 (3.5 mm CT      │
   │   thermocouple daughter-             jacks) ◄── 3x 60 Hz AC     │
   │   board UNPLUGGED, fixture           into AD8542 stages →       │
   │   sits in its place)                 A1 ADC0/1/2                │
   │                                                                 │
   │  J3  K1 NC/COM/NO ──► relay sense   J10 K4 NC/COM/NO ──► sense  │
   │  J4  K2 NC/COM/NO ──► relay sense    (pilot relay, safety)      │
   │  J8  K3 NC/COM/NO ──► relay sense                               │
   │  J11 K5 NC/COM/NO ──► relay sense   E-stop net ◄── optoMOS      │
   │                                      (fixture opens/closes it)  │
   │  J20 IO_3 / IO_4 ◄──► expander I/O                              │
   │  `Fault` (ESP GPIO6→U1) ──► sense                               │
   └─────────────────────────────────────────────────────────────────┘
```

The double line marks the fixture's isolation boundary. Everything below it on
the right-hand (safety) side is referenced to `GND_Safty`; the main board's two
ground domains share no copper, and the fixture must not become the strap that
shorts them together (section 3.5).

### Closed loops the fixture creates

1. **Heat loop (main side):** ESP32 closes K1/K2/K3 → fixture senses contacts →
   thermal model adds heater power to the zone → zone temperature rises →
   emulated MAX31856 registers report it → ESP32's PID reacts.
2. **Current loop (safety side):** relay closed *and* K4 pilot closed *and*
   element healthy → CT output amplitude = simulated current → safety Pico's
   current-sense channels see it → guards S3/S4 (current-vs-command coherence)
   become testable, including the welded-contactor case (current present with
   no relay commanded).
3. **Safety TC loop:** the same zone temperatures feed the safety-side emulated
   MAX31856, so overtemp guards fire from a physically consistent story — or
   an *inconsistent* one, when a test deliberately skews the safety channel.

---

## 3. Hardware architecture

### 3.1 Why a second Pico is the right part

- **PIO** is the only realistic way to be an SPI *slave* for multiple chip
  selects with register-accurate timing. The RP2040's two PIO blocks map
  cleanly onto the two independent SPI buses (ESP-side and safety-side).
- The RP2040's USB device controller gives a zero-extra-hardware CDC control
  port.
- Same toolchain, FreeRTOS config, and Debug Probe workflow as `SaftyFW` —
  the repo already knows how to build, flash (SWD/OpenOCD), and host-test this
  chip.

### 3.2 MAX31856 emulation (the heart of the fixture)

Each emulated channel is a register file plus behavioral rules. The full
register map, with the emulator's behavior per register:

| Addr (R/W) | Register | Emulated behavior |
|---|---|---|
| 00h/80h | CR0 | R/W verbatim. `AUTOCONVERT` starts/stops the conversion cadence; `1SHOT` self-clears after one conversion; `OCFAULT[1:0]` selects OC-detect mode (affects OC fault latency); `CJ` disable honored; `FAULT` mode (comparator vs interrupt) changes SR/`~FAULT` clearing rules; `FAULTCLR` self-clears and clears latched faults per datasheet |
| 01h/81h | CR1 | R/W verbatim. `AVGSEL[2:0]` lengthens conversion time per datasheet table; `TC TYPE[3:0]` recorded — the emulator reports temperature identically regardless, but the *written value* is exposed to tests via `TC_GET_MASTER_CONFIG` so a wrong-type config is a test failure |
| 02h/82h | MASK | R/W verbatim; masks which SR faults assert `~FAULT` |
| 03h/83h | CJHF | CJ high threshold; compared each conversion against simulated CJ |
| 04h/84h | CJLF | CJ low threshold; same |
| 05h–06h (85h–86h) | LTHFTH/LTHFTL | TC high threshold (16-bit); compared against simulated TC temp |
| 07h–08h (87h–88h) | LTLFTH/LTLFTL | TC low threshold; same |
| 09h/89h | CJTO | CJ offset, signed, honored in reported CJ |
| 0Ah–0Bh (8Ah–8Bh) | CJTH/CJTL | Simulated CJ temperature, updated per conversion; writable by master when CJ sensor disabled (datasheet behavior honored) |
| 0Ch–0Eh | LTCBH/LTCBM/LTCBL | Read-only 19-bit linearized TC temperature from the thermal model, quantized to 0.0078125 °C LSB; updates only on conversion boundaries, never mid-read (registers latched while CS low) |
| 0Fh | SR | Read-only fault status: OC, OVUV, TCHIGH, TCLOW, CJHIGH, CJLOW, CJRANGE, TCRANGE — computed from simulated conditions + injected faults + master-written thresholds; comparator/interrupt clearing semantics per CR0 |

Behavioral rules on top of the map:

- **SPI mode 1, up to 5 MHz**, multi-byte auto-increment reads/writes, exactly
  as the datasheet describes — the masters' existing drivers must not need a
  single change to run against the fixture.
- **Conversion timing:** in auto-convert mode, LTCB registers update on the
  datasheet cadence (~100 ms nominal) from the thermal model; DRDY asserts per
  datasheet. One-shot mode honored.
- **Configuration readback:** whatever the master writes to CR0/CR1/mask
  registers reads back verbatim. This lets tests *assert* that the firmware
  configured TC type, filter, and fault masks correctly — a real test point,
  not just a simulation nicety.
- **Fault machinery:** the SR fault-status register, `~FAULT` pin, and fault
  mask interactions are modeled: open-circuit detection (OC fault when a
  "disconnected thermocouple" fault is active), over/under-voltage, CJ
  high/low, TC high/low thresholds evaluated against the *simulated*
  temperatures and the master-written threshold registers.
- **Cold junction:** CJ temperature is simulated (default: slow ambient drift)
  and CJ-offset writes are honored, so CJ-compensation code paths run for
  real.
- **Per-channel corruption knobs** for fault injection: stuck conversion
  (LTCB frozen), noisy reads (Gaussian jitter), bit-error injection on MISO
  (flaky SPI), dead channel (MISO high-Z / all-zeros / all-ones), delayed
  DRDY, spurious `~FAULT` assertion.

#### 3.2.1 PIO slave engine design

One engine per bus. Bus A (ESP side) has three CS lines sharing
SCLK/MOSI/MISO; bus B has one CS. Design per engine:

- **RX state machine** (one per bus): samples MOSI on SCLK's **falling**
  edge (mode 1: CPOL=0, CPHA=1 — both master and slave shift/drive their
  next bit on the *leading* edge, which for CPOL=0 is the rising edge, and
  latch/sample on the *trailing* edge, the falling edge; this is not "the
  opposite edge from the master" — master and slave key off the same
  leading/trailing split of the one shared clock. Confirmed against the
  MAX31856 datasheet's own Table 5, CPOL=0 row: SDI "Data bit latch" on
  "SCLK falling", and against both real masters' SPI config: KilnFW's
  `MAX31856_SPI_MODE 1` / SaftyFW's explicit `SPI_CPOL_0, SPI_CPHA_1`),
  pushes each byte to its RX FIFO with the active-CS number ORed into the
  high bits (CS lines read via `in pins`). CS deassert is detected by a
  second tiny SM or a GPIO IRQ, which terminates the transaction.
- **TX state machine** (one per bus): pulls response bytes from its TX FIFO
  and shifts them out on MISO on SCLK's **rising** edge (the mode-1 leading
  edge, per the same derivation above — this is also what the datasheet's
  Table 5 says for SDO under CPOL=0: "Next data bit shift" on "SCLK
  rising"), holding steady through the falling edge so the master samples a
  settled bit; MISO pin is tri-stated (pindir flip in the PIO program)
  whenever no CS is low, since three emulated chips share one physical
  MISO on bus A.
- **First-byte path (the 1.6 µs problem — SEE CORRECTION BELOW):** at 5 MHz a
  byte takes 1.6 µs and the master's first clock for the *response* byte comes
  one byte-time after the address byte. A FreeRTOS task cannot bounce a queue in that window
  reliably; a core-1 ISR can (RP2040 interrupt latency ~1 µs is too tight to
  bet on alone). Two-stage plan, POC decides (M-A):
  1. **Plan A — ISR staging:** address byte triggers a PIO RX IRQ on core 1
     (FreeRTOS never disables core-1 IRQs in this design); the handler
     indexes the channel's 16-byte register image and feeds the TX FIFO with
     the auto-increment stream. Handler budget: <1 µs of straight-line code,
     register image always coherent (see below).
> **CORRECTION 2026-08-20 (`docs/SPI_ACCESS_AUDIT.md` §6) — the budget above
> is wrong by roughly 8×, and it changes which plan is viable.** The response
> is not due one byte-time after the address byte. The address *value* is only
> known after clock 8's falling edge, and the first response bit must already
> be on MISO at clock 9's rising edge — so the real budget is about **half an
> SCLK period (~125 ns at 4 MHz)**, not 1.6 µs. Plan A (ISR staging) almost
> certainly cannot meet that on an RP2040, and its failure mode is not one
> late byte but **an entire burst shifted by one position**, returning
> plausible-looking wrong temperatures rather than an obvious fault. A
> DMA-fed Plan B variant is sketched in `SPI_ACCESS_AUDIT.md` §6. Treat M-A's
> POC as deciding between Plan B variants, not between A and B.

  2. **Plan B — precomputed full-image streaming:** if Plan A misses timing
     at 5 MHz, exploit MAX31856 read behavior — the TX FIFO is pre-loaded at
     every CS-fall with the register image starting at address 0, and a
     small PIO/DMA trick skips to the addressed offset; or (fallback of the
     fallback) the fixture documents a supported max SCLK below 5 MHz and
     the masters' clock is dropped for bench builds. Dropping the DUT's
     clock is a last resort because the point is testing unmodified firmware.
- **Register image coherency:** each channel keeps a 16-byte live image in
  SRAM (core-1-owned). `sim_engine` (core 0) never writes it directly; it
  posts `{channel, temp_code, cj_code, sr_bits}` updates through a
  double-buffered snapshot that the core-1 side commits **only between
  transactions** (never while that channel's CS is low), so a multi-byte
  LTCB read is always internally consistent — same guarantee the real chip
  gives.
- **Write-back:** master writes land in the RX FIFO tagged with CS; the
  `spi_emu_*` task drains them after CS-rise, applies register-write rules
  (self-clearing bits, read-only masks), and raises a `master_config_changed`
  event so tests can assert on DUT configuration behavior.
- **Instrumentation:** per-channel counters — transactions, bytes, CRC-class
  errors (malformed transactions), write conflicts, first-byte-late events
  (TX FIFO underrun detected by PIO). Underruns are *counted, never silent*:
  a test run with nonzero underruns is flagged invalid in telemetry.

Latency budget summary: the first response byte must be committed to the TX
FIFO within ~1.6 µs of the address byte's last clock. This is the hardest
real-time constraint in the project and gets the dedicated proof-of-concept
milestone (section 10, M-A) with Saleae capture as the acceptance evidence.

### 3.3 CT waveform generation

- Three GPIO, each running high-carrier PWM (~250 kHz) whose duty cycle is
  modulated by a 60 Hz sine table (DMA-paced, per-channel phase), then a
  2-pole RC low-pass on the fixture side.
- **Coupling into J13/J15/J17 via a small step-up audio/isolation
  transformer** (decided: transformer coupling; **ratio corrected
  2026-08-20**, superseding the earlier "1:1" figure — see below). A real CT
  is an isolated, floating AC source; a transformer reproduces that honestly
  *and* keeps the fixture out of the `GND_Safty` domain for these channels.
  The AD8542 input stages on the safety board set the amplitude target — the
  burden/divider values must be read off `CurrentSense` sheets during
  implementation to size the attenuation (open question, section 11).

  **Ratio correction (2026-08-20, found while writing `docs/BOM.md`):** the
  original locked decision (status header above, user-confirmed 2026-08-20)
  named a **1:1** transformer. That was a real design error, not a wording
  slip: at 1:1, this design's own Pico-drive ceiling caps the fixture at
  roughly **31.5 A rms** on the safety board's 1 V/30 A CT model — fine for
  the typical 10–25 A a resistive kiln element draws, but structurally
  incapable of reaching the ADC's clipping boundary (**≈98 A rms**, traced
  from `SaftyFW/docs/CURRENT_SENSE.md` §2: gain 0.715, clamp ≈4.6 V peak,
  1 V/30 A CT), so the fixture could never exercise `CURRENT_FLAG_CLIPPED`
  handling — one of the fixture's own stated purposes (section 1). The
  corrected decision is **~3:1 step-up**, derived as:

  ```
  V_sec,pk (clip target)      ≈ 4.6 V   (CURRENT_SENSE.md §2, high confidence)
  V_pri,pk (usable Pico drive) ≈ 1.5 V   (medium confidence, see caveat below)
  n = V_sec,pk / V_pri,pk      ≈ 3.07  →  call it 3:1
  ```

  The 1.5 V peak primary-drive figure is a **medium-confidence estimate, not
  a measured or firmware-confirmed number** — it assumes a practical usable
  swing of ~91% of the theoretical ±1.65 V (half the 3.3 V logic rail) that
  the PWM/RC chain above can produce before a DC-blocking cap. The real
  ceiling depends on whatever modulation-index cap `ct_wave_pwm.c`'s
  amplitude-to-duty mapping ends up using — that mapping is currently an
  explicit `TODO(M-D calibration)` **IDENTITY placeholder** (M-D, section 10),
  so this number is not yet firmware-confirmed either way. Candidate part
  (also unconfirmed — its exact turns/impedance ratio has not been read off
  a datasheet in this pass): Triad Magnetics TY-300P, per `docs/BOM.md` §3 —
  confirm the ratio before ordering, and see that section for a same-cost
  fallback (1:1 transformer + a ×3 op-amp gain stage) if the part's real
  ratio doesn't hold up. Full sizing derivation, confidence breakdown, and
  the CT sizing math live in `docs/BOM.md` §3; this paragraph is the summary
  a future session should trust for the *decision*, not the show-work.
- **Programmable per channel:** amplitude (in simulated amps, fixture converts
  via calibration table), phase, plus distortion knobs — DC offset, clipping,
  dropout (half-cycle skipping, as a failing SSR would produce), and 50 Hz
  option for completeness.
- Amplitude tracks the thermal model: `I_zone = V_mains / R_element` when the
  zone's relay chain is conducting, scaled by element-health faults (broken
  coil = 0 A on that channel; partial short = wrong-but-plausible current).

**Synthesis detail:**

- Sine table: 256 entries per cycle at 60 Hz → table stepped at 15.36 kHz by
  a repeating timer/DMA chain; PWM carrier ~244 kHz (8-bit resolution at
  125 MHz sysclk on a dedicated slice per channel). Carrier is >> the RC
  corner (~1–2 kHz, 2-pole) so residual carrier ripple is negligible next to
  the AD8542 stage's own filtering.
- Amplitude changes and relay-driven on/off apply **at zero crossings only**
  (like a real zero-cross SSR) unless a distortion knob says otherwise —
  step-in-mid-cycle is itself a selectable distortion for testing the
  current-sense RMS math against ugly waveforms.
- Per-channel phase offsets default to 0°/120°/240° (three-phase-ish option)
  or all-in-phase (single-phase, default) — selectable, since which one the
  real kiln has affects nothing today but costs nothing to support.
- **Calibration procedure** (M-D exit): for each channel, sweep commanded
  amplitude across ~10 points, read back what `SaftyFW`'s own
  `current_task`/ADC reports (over the existing kilnctrl MCP path or SWD),
  fit gain/offset, store the table in fixture flash keyed by channel. This
  makes "simulate 12 A" mean 12 A *as the DUT measures it*, which is the
  definition that matters, and it doubles as the `CURRENT_SENSE.md` §5
  commissioning check (one relay commanded → exactly one channel responds).

### 3.4 Relay sensing and discrete I/O

- Relay contacts sensed at the terminal blocks: fixture supplies a small
  wetting voltage through the NO/COM contact into an MCP23017 input
  (opto-isolated for K4, which lives in the safety domain). Both NO and COM/NC
  sides observed where useful so "relay commanded but contact stuck" is
  distinguishable from "relay never commanded."
- **E-stop:** an optoMOS/relay on the fixture sits in the E-stop loop so tests
  can open it mid-firing. Default state configurable (the as-built board reads
  permanent STOP with no jumper — the fixture *becomes* the jumper).
- **`Fault` line:** the ESP-driven fault output (GPIO6 → U1 opto) is sensed so
  tests can assert the main processor raised it.
- **J20 IO_3/IO_4** and other spare I/O: MCP23017 pins, direction settable per
  test, for full main-board I/O coverage.
- **DRDY/`~FAULT` outputs** (4 channels x 2 lines): **direct Pico GPIO**
  (decided), driven open-drain (output-low / input-Hi-Z) so the board's
  pull-ups set the idle level. Microsecond-accurate DRDY timing relative to
  emulated conversions, no I2C latency dependency. The safety-side pair
  crosses the digital isolator with the SPI B lines (6-channel isolator, or
  4-ch + 2-ch parts).
- **DUT power switch** (decided): a fixture relay/high-side switch in the main
  board's 12 V feed, driven from MCP23017 #1, so tests can cold-boot,
  power-cycle, and brownout the DUT mid-fire (`power_blip` scenario). The
  bench supply feeds the fixture's power connector; the fixture feeds the
  board.

### 3.5 Isolation discipline

The main board keeps `GND_Main` and `GND_Safty` separate; the fixture must
too, or every isolation-dependent behavior becomes untestable and a real
design error could hide behind the fixture's ground strap.

- Fixture logic ground ties to **GND_Main** (shared with SPI bus A, relay
  sense on the main side, J20, `Fault` sense).
- **SPI bus B** (J7, safety domain) plus the safety-side DRDY/`~FAULT` pair
  cross digital isolators (6 channels total, ISO7741-class parts) powered
  from the safety side's 3.3 V at J7.
- **CT channels** cross via transformer (3.3).
- **K4 sense and E-stop** cross via optocoupler/optoMOS.
- A deliberate, labeled, removable jumper can common the grounds for early
  breadboard bring-up — but the standard test library must run with it out.

### 3.6 Pico pin budget (draft — verify against traced pinouts before layout)

| Function | Pins | Notes |
|---|---|---|
| SPI slave A: SCLK, MOSI, MISO, CS0, CS1, CS2 | 6 | PIO0, GND_Main |
| SPI slave B: SCLK, MOSI, MISO, CS0 | 4 | PIO1, via isolator |
| DRDY x3 + `~FAULT` x3 (main side) | 6 | direct GPIO, open-drain emulation |
| DRDY + `~FAULT` (safety side) | 2 | direct GPIO, via isolator |
| CT sine PWM x3 | 3 | + external RC and transformer |
| I2C0 SDA/SCL | 2 | both MCP23017s, optional PCA9685 |
| Debug UART (to Debug Probe) | 2 | same bench pattern as SaftyFW |
| Heartbeat LED | (GPIO25) | on-board, not a header pin |

Total = 25 of the Pico's 26 header GPIO — tight but it fits, and the
heartbeat LED costs nothing extra. Documented fallback if a pin is ever
needed: the three main-side `~FAULT` lines (slow, level-only) move to
MCP23017 #2, freeing 3 pins with no timing cost.

### 3.7 I/O expander allocation

| Device | Addr | Pins used | Function |
|---|---|---|---|
| MCP23017 #1 | 0x20 | 5 in | Relay sense K1/K2/K3/K5/K4 |
| | | 1 in | `Fault` line sense |
| | | 1 out | E-stop optoMOS drive |
| | | 1 out | DUT 12 V power relay |
| | | 2 i/o | J20 IO_3/IO_4 |
| | | 6 spare | future main-board I/O tests |
| MCP23017 #2 | 0x21 | 3 out (fallback) | main-side `~FAULT` x3 if Pico pins ever run out (3.6) |
| | | 13 spare | future main-board I/O tests |
| PCA9685 (optional) | 0x40 | — | PWM stimulus if a test needs analog-ish levels or many slow PWMs; not required for the base feature set |

---

## 4. Firmware architecture

FreeRTOS SMP on RP2040, mirroring `SaftyFW`'s doctrine: **every hardware
interface has exactly one owner task; nobody else touches that peripheral.**
Cross-task communication by queues and single-writer snapshot structs only.

### 4.1 Task map

| Task | Core | Prio | Owns | Duty |
|---|---|---|---|---|
| `usb_owner` | 0 | mid | USB CDC (TinyUSB) | Frame RX/TX, CRC, dispatch to command queue, telemetry TX |
| `cmd_task` | 0 | mid | — | Decode/validate commands, route to owners, build replies |
| `spi_emu_a` | 1 | high | PIO0 + DMA | 3-channel MAX31856 register machine, ESP bus |
| `spi_emu_b` | 1 | high | PIO1 + DMA | 1-channel MAX31856 register machine, safety bus |
| `sim_engine` | 0 | mid+ | — | Thermal model tick (10 Hz), computes zone temps, element currents; publishes snapshot |
| `fault_sched` | 0 | mid | — | Trigger evaluation each tick; arms/fires scheduled faults |
| `wave_owner` | 1 | high | PWM slices + DMA | 60 Hz synthesis, amplitude/phase/distortion updates at zero-crossings |
| `i2c_owner` | 0 | mid | I2C0 | All expander traffic; relay-sense debounced scan (5–10 ms), E-stop/DUT-power outputs |
| `telemetry` | 0 | low | — | Periodic state frames to USB (temps, relay states, active faults, sim clock) |
| `log_task` | 0 | lowest | — | Deferred logging, drop-counting, never blocks producers |

Core 1 is reserved for the hard-real-time producers (SPI emulation response
pre-compute, waveform DMA feeding); core 0 does everything elastic. The SPI
emulators' CS-edge/first-byte path runs in IRQ/PIO context with pre-staged
buffers — the task half only does the slow parts (write-back, fault-state
recompute).

### 4.2 Determinism and the simulation clock

The simulator keeps its own monotonic **sim clock**, normally 1:1 with wall
time but supporting **time-scale** (e.g. 10x) so a 12-hour firing profile can
be regression-tested in minutes. Everything time-driven (thermal integration,
fault triggers, conversion cadence) runs off the sim clock. Real-time
interfaces (SPI response latency, 60 Hz) obviously stay wall-time; the 60 Hz
frequency itself never scales, only the amplitude envelope's evolution does.
Scale changes are logged and stamped into telemetry so a test run's record is
self-describing. All stochastic knobs (noise, flaky-contact timing) run from a
**seeded PRNG**; the seed is settable per run so every "random" failure is
replayable.

### 4.3 Thermal model

Zone count is a runtime parameter, **1–4 zones, default 3** (3 matches the
board's relay/TC/CT channels; a 4th zone lets future boards or asymmetric
rigs map a zone with no dedicated CT/TC). Per zone:

```
C_zone * dT/dt = P_heater(t) - k_loss*(T - T_ambient) - Σ k_couple*(T - T_neighbor)
P_heater = duty(relay chain) * V_mains² / R_element * element_health
```

- Integration: forward Euler at the 10 Hz model tick is sufficient for
  time constants ≥ minutes; the "fast unit-test kiln" preset (time constants
  of seconds) uses 4 Euler substeps per tick instead of a fancier
  integrator. At time-scale N the substep count multiplies by N so accuracy
  does not degrade when runs are accelerated. All math in `float`; state in
  Kelvin-offset °C.
- A configurable **TC lag** (first-order sensor time constant) between zone
  temperature and reported TC temperature — several guards care about the
  difference between element temperature and sensed temperature.
- The safety-side TC reads a configurable blend of zone temps (default:
  zone 0), with its own lag and its own fault knobs, so main-vs-safety
  disagreement scenarios are first-class.
- Model is pure C, no RTOS dependencies, **host-testable** with the same
  MSVC/CMake harness pattern `SaftyFW/test` uses. Golden-trace tests pin the
  integration math.

**Per-zone parameters** (all settable over USB, floats unless noted):

| Parameter | Meaning | Typical (real kiln) |
|---|---|---|
| `C` (J/°C) | Thermal mass | 2e5–1e6 |
| `k_loss` (W/°C) | Loss to ambient | 5–30 |
| `k_couple[j]` (W/°C) | Coupling to zone j (symmetric matrix, diag 0) | 1–10 |
| `R_element` (Ω) | Heater element resistance | 10–30 |
| `element_health` (0..1) | 1 = good; 0 = broken; between = partial | 1.0 |
| `tc_lag_s` | TC sensor time constant | 5–60 |
| `T0` (°C) | Initial temperature | ambient |

Globals: `V_mains` (default 240), `T_ambient`, safety-TC blend weights +
lag, PRNG-driven process noise level (small random walk on each zone so two
"identical" runs with different seeds look organically different when
noise > 0; default 0 for byte-exact replays).

**Presets** (stored in firmware, selectable by name):

- `fast_test` — time constants of seconds; a full "firing" in ~2 min of
  wall time; the default for automated regression.
- `small_kiln` — single-zone dominant, ~1 h scale behavior.
- `three_zone` — realistic 3-zone with top/middle/bottom coupling and the
  classic bottom-zone-lags asymmetry.
- `stress` — deliberately awkward: huge lag, weak coupling, low mass — the
  preset PID tuning hates; for robustness work, not regression.

Presets are starting points; scenarios may override any parameter.

### 4.4 Reuse from the repo

- **Wire protocol:** the USB link uses `UnitTestFw`'s hardened UART protocol
  design (`UnitTestFw/UnitTest/docs/UART_PROTOCOL.md` — framing, CRC,
  reliability/retry/dedup, task registration), but **consumed from
  `CommonFW`, not from `UnitTestFw`**: the protocol core gets lifted into
  `CommonFW` as a pure, host-tested library first (M-B), the spec document
  moves with it, and `UnitTestFw` is then deleted (section 12). SimFW's
  owners register as addressable tasks the way DAC/AD9833/OLED did there.
  The safety link's kilnlink compatibility floor is untouched — SimFW never
  speaks on that wire.
- **MAX31856 register semantics:** `SaftyFW`'s `max31856.c` driver and
  `KilnFW`'s driver are the authorities on what the masters actually do; the
  emulator's register model is written against them plus the datasheet.
- **Build skeleton:** `SaftyFW`'s CMake + pico_sdk + FreeRTOS import layout is
  the template (no A/B bootloader needed here — plain single-image, BOOTSEL or
  SWD flashing is fine for a bench tool).

### 4.5 Data flow and shared state

Single-writer snapshots, same doctrine as `SaftyFW`:

```
 relay edges (i2c_owner) ──┐
 fault overrides ──────────┤
                           ▼
                      sim_engine ── zone/TC/CT snapshot ──┬─► spi_emu_a/b (reg images)
                      (10 Hz tick)                        ├─► wave_owner (amplitudes)
                           │                              └─► telemetry
                           └─► event ring (relay edges, fault fires, threshold
                               crossings; sim-clock-stamped, seq-numbered)
```

- **Zone snapshot:** one struct, `sim_engine` sole writer, double-buffered
  with a sequence counter (readers retry on torn read — no locks on the hot
  path).
- **Event ring:** fixed-size (e.g. 256 entries), sequence-numbered so the PC
  detects loss; `telemetry` drains it to USB as EVT frames. Events are the
  raw material for scenario reports, so their timestamps are sim-clock and
  their ordering is authoritative.
- **Command writes** (mode switches, param sets, fault arms) go through
  `cmd_task` → owner queues; owners apply at their own tick boundary and
  emit a confirming event. No command touches another task's state directly.
- **Queue depths and drop policy** stated per queue in code; nothing ever
  blocks `spi_emu_*` or `wave_owner` — a full queue toward them is a counted
  drop plus event, never a stall (the DUT must never see the fixture hiccup).

---

## 5. USB control protocol

Native USB CDC carrying the hardened protocol lifted from `UnitTestFw` into
`CommonFW` (decided — framing, CRC, reliability/retry/dedup, task
registration; the spec doc moves to `CommonFW/docs/` during M-B, since its
current home `firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md` gets
deleted with that project; SimFW's command groups below register as
addressable tasks). Request/response, plus unsolicited
telemetry and event frames. Command groups:

| Group | Commands (sketch) |
|---|---|
| SYS | `PING`, `GET_VERSION`, `RESET_SIM`, `SET_TIMESCALE`, `SET_SEED`, `GET_CAPS` |
| MODEL | `SET_ZONE_PARAMS`, `GET_ZONE_PARAMS`, `SET_AMBIENT`, `LOAD_PRESET`, `SET_TEMP` (force a zone temp), `SET_TC_LAG` |
| TC | `TC_GET_REGS`, `TC_FORCE_TEMP`, `TC_SET_MODE` (model/manual), `TC_INJECT_FAULT`, `TC_CLEAR_FAULT`, `TC_GET_MASTER_CONFIG` (what did the DUT write?) |
| CT | `CT_SET_MODE` (model/manual), `CT_SET_AMPS`, `CT_SET_DISTORTION`, `CT_GET_STATE` |
| RELAY | `RELAY_GET_STATES`, `RELAY_GET_EDGES` (timestamped edge log), `RELAY_SET_CONTACT_FAULT` (welded/stuck-open at the *sense* interpretation level) |
| IO | `IO_SET_DIR`, `IO_WRITE`, `IO_READ`, `ESTOP_SET`, `FAULT_LINE_GET` |
| FAULT | `FAULT_SCHEDULE`, `FAULT_CANCEL`, `FAULT_LIST`, `FAULT_FIRE_NOW` |
| EVT | unsolicited: relay edges, fault fired, guard-relevant thresholds crossed, sim-clock marks |

Every mutable thing has a **mode**: `MODEL` (driven by the simulation) or
`MANUAL` (frozen at an operator-set value). The GUI's "detailed manual
control" is exactly the ability to flip any signal to MANUAL and set it.

### 5.1 Addressing and framing conventions

Each command group above registers as an addressable task in the protocol's
task-registration model (the same way `UnitTestFw`'s DAC/AD9833/OLED
registered), so the PC discovers the fixture's capabilities at connect time
via the protocol's existing INFO/enumeration mechanism rather than
hard-coding them. `GET_CAPS` reports: protocol version, SimFW version +
git hash, zone count limits, channel counts, and a feature bitmask so the PC
tools can refuse gracefully against a mismatched firmware.

### 5.2 Representative payloads (byte layouts frozen in `PROTOCOL.md` at M-B)

- `FAULT_SCHEDULE` request: `{u16 fault_slot, u8 fault_type, u8 target,
  trigger{u8 kind, f32 a, f32 b, u8 zone/relay}, duration{u8 kind, f32 t},
  repeat{u8 kind, f32 period, f32 jitter, u16 n}, f32 param0..3}` — the four
  generic params carry fault-specific values (noise sigma, offset °C,
  bit-error rate, duty…), documented per fault type.
- `TC_GET_REGS` reply: the channel's full 16-byte register image + the
  emulator's shadow state (actual simulated temp before corruption, active
  fault list) — the pair is what makes "the DUT was lied to, this is the
  truth" assertions possible.
- `RELAY_GET_EDGES` reply: up to N `{u8 relay, u8 edge, u64 sim_time_us}`
  drained from the edge log (edge log is also mirrored as EVT frames; the
  poll form exists for simple CLI use).

### 5.3 Telemetry and event frames

- **Telemetry frame** (periodic, default 2 Hz, rate settable): sim clock,
  time-scale, seed, per-zone `{T_zone, T_tc_reported, T_safety_reported,
  I_amps}`, relay state mask, E-stop/fault-line state, active fault count,
  SPI transaction/underrun counters, event-ring high-water mark.
- **EVT frame** (unsolicited, immediate): `{u32 seq, u64 sim_time_us,
  u8 event_type, payload}` for relay edges, fault fired/cleared, threshold
  crossings, mode changes, DUT power switch, protocol errors. Sequence
  numbers make loss visible; the PC's report generator refuses to certify a
  run with a sequence gap.

---

## 6. PC side: MCP server, CLI, GUI

New package in `tools/PcTools` (working name `kilnsim`), sibling to the
existing `kilnctrl` package — **a separate MCP server**, since it is a
different device with a different port. Decided: `kilnsim` is a **fresh,
self-contained toolset** — nothing is ported from `UnitTestFw`'s `pc_tools`
(that stack is deleted with its project, section 12). The link layer is
written new against the `CommonFW`-hosted protocol spec, which also serves as
the extraction's independent second implementation — the same
prove-it-twice pattern the kilnlink codecs used.

### 6.1 MCP tool surface (sketch)

| Tool | Args | Returns |
|---|---|---|
| `sim_connect` / `sim_disconnect` | port (auto-detect by USB VID/PID + protocol PING) | link + firmware version info |
| `sim_get_state` | — | full telemetry snapshot as JSON |
| `sim_reset` | keep_params? | fresh run: model to T0, faults cleared, event seq reset |
| `sim_load_preset` | name | applied parameter set |
| `sim_set_zone_params` / `sim_get_zone_params` | zone, params | applied/current values |
| `sim_set_timescale` / `sim_set_seed` | value | ack (refused mid-scenario) |
| `tc_get_regs` | channel | register image + shadow truth |
| `tc_inject_fault` / `tc_clear_fault` | channel, fault_type, params | fault slot id |
| `tc_set_mode` | channel, MODEL\|MANUAL, manual_temp? | ack |
| `ct_set_mode` / `ct_set_amps` / `ct_set_distortion` | channel, values | ack |
| `relay_get_states` / `relay_get_edges` | since_seq? | states / edge list |
| `estop_set` / `dut_power_set` | open\|closed, on\|off | ack + resulting event |
| `io_read` / `io_write` / `io_set_dir` | pin spec | values |
| `fault_schedule` / `fault_cancel` / `fault_list` / `fault_fire_now` | per 7.2 spec | slot ids / active list |
| `run_test_scenario` | path or name, seed?, timescale? | run id (async), progress events |
| `get_test_report` | run id | full report JSON (8.2) |
| `sim_raw_command` | group, cmd, hex payload | hex reply — debug escape hatch |

`run_test_scenario` is the composite: it compiles the YAML into primitive
commands, arms the schedule in firmware, watches the EVT stream, evaluates
expectations, and assembles the report. Everything else is a thin 1:1 wrapper
over the wire protocol.

### 6.2 CLI

`kilnsim` console script, subcommand per tool: `kilnsim state`,
`kilnsim preset fast_test`, `kilnsim fault tc0 open --at-temp 600 --zone 0`,
`kilnsim estop open`, `kilnsim power cycle --off-ms 200`,
`kilnsim run scenarios/welded_ssr_midfire.yaml --seed 42 --report out.json`
(exit code = pass/fail — the CI entry point), `kilnsim monitor` (live
telemetry/event tail to stdout, `--json` for machine consumption).

### 6.3 GUI (Tk)

Panels: zone-temperature strip chart (truth vs reported vs safety-reported
per zone); relay lamp row with edge history; per-TC panel (register hexdump,
shadow truth, fault buttons, MODEL/MANUAL toggle + manual temp entry); CT
panel (per-channel amps slider, distortion combo, live commanded-vs-calibrated
readback); discrete I/O panel (E-stop, DUT power, J20, fault-line lamp);
fault scheduler timeline (armed faults on a sim-time axis, drag to re-time);
scenario runner (load YAML, progress, live expectation status, report view).
Session logging to `tools/PcTools/logs/kilnsim/` in the same spirit as the
existing console-capture conventions.

The crucial MCP property: **assertions live PC-side.** A scenario run returns
a machine-readable report (events with sim-clock timestamps: when the fault
fired, when the relay opened, when the fault line asserted), and the MCP layer
exposes it so Claude (or pytest) can judge pass/fail — e.g. "after the welded
contactor at t=300 s, K4 must open within 5 s."

---

## 7. Fault injection framework

### 7.1 Fault catalog

**Thermocouple / sensor faults**

| Fault | Mechanism in fixture |
|---|---|
| Disconnected TC | OC fault bit + `~FAULT` per datasheet; temp reads per real chip's OC behavior |
| Broken (intermittent) TC | Alternates connected/OC on a configurable duty/period, or seeded-random |
| Flaky TC (noise) | Gaussian/spike noise added to reported temperature |
| Shorted TC | Reads near-ambient/CJ regardless of zone temperature |
| Stuck TC | LTCB frozen at last value while zone keeps moving |
| Drifting TC | Slow ramp offset (calibration drift) |
| CJ fault | CJ high/low fault bits; wrong CJ temperature |
| TC IC dead | No MISO response (all 0x00/0xFF/high-Z selectable) |
| TC IC flaky SPI | Bit errors on MISO at configurable rate |
| Spurious fault pin | `~FAULT` asserted with clean SR, and vice versa |
| Main/safety disagree | Skew safety TC vs zone truth by an offset/gain |

**Power path faults**

| Fault | Mechanism |
|---|---|
| Shorted (welded) SSR/relay | CT current flows while relay sensed open / not commanded |
| Stuck-open relay | Relay commanded (contact sensed closed is suppressed at model level) but zero current, zone doesn't heat |
| Broken heater coil | Relay closes, zero current on that CT, zone doesn't heat |
| Partially failed coil | Wrong element resistance → wrong current + slow heating |
| Half-waving SSR | CT dropout distortion (missing half-cycles) |
| Welded K4 test support | Keep CT current flowing after K4 sensed open (S9 escalation) |
| Phase loss | One CT channel to zero while others run |

**System faults**

| Fault | Mechanism |
|---|---|
| E-stop during firing | Open the E-stop loop at trigger |
| Runaway zone | Force P_heater on regardless of relay state (model-level) |
| Thermal-mass surprise | Step-change model params mid-run (lid opened, load added) |
| Ambient shift | Ramp ambient |
| Sensor-vs-element lag stress | Crank TC lag to provoke overshoot |

### 7.2 Standardized trigger model (the timing spec)

Every scheduled fault is `(trigger, fault, duration, repeat)`:

- **Triggers:** `AT_SIM_TIME t`, `AT_ZONE_TEMP (zone, temp, rising|falling)`,
  `ON_RELAY_EDGE (relay, close|open, +delay)`, `ON_EVENT (named event,
  +delay)`, `AFTER_FAULT (fault-id, +delay)` (chaining), `RANDOM_IN (t0, t1)`
  (seeded), `MANUAL` (armed, fired by command).
- **Duration:** `PERMANENT`, `FOR t`, `UNTIL_TRIGGER (…)` — so intermittent
  and self-clearing faults are expressible.
- **Repeat:** `ONCE`, `EVERY t [jitter j]`, `N_TIMES`.
- All timing in sim-clock units; all randomness from the run's seed. The same
  scenario + seed ⇒ the same run, byte-for-byte in the event log. That
  replayability rule is the fixture's core testing contract.

### 7.3 Engine internals

- Fixed pool of **fault slots** (e.g. 32): `{slot_id, state:
  IDLE|ARMED|ACTIVE|EXPIRED, fault_type, target, trigger, duration, repeat,
  params[4], fire_count}`. Slot ids are stable for the run and appear in
  every related event, so a report can trace one fault's whole lifecycle.
- `fault_sched` evaluates all ARMED triggers each 10 Hz tick against the zone
  snapshot + event ring (relay edges, named events). Firing a fault =
  flipping the target's override in the owning module (TC corruption knob,
  CT distortion, model parameter, discrete output) + emitting
  `FAULT_FIRED{slot}`; expiry symmetric with `FAULT_CLEARED{slot}`.
- Trigger evaluation order is slot order — deterministic; two faults
  triggering on the same tick fire in slot order, documented so scenarios
  can rely on it.
- Faults compose: e.g. `tc_noise` + `tc_drift` on the same channel stack
  (noise around a drifting mean). Composition rules per fault type are part
  of `PROTOCOL.md`; genuinely conflicting pairs (dead IC + anything) resolve
  to the more severe one and log a warning event rather than refusing.

---

## 8. Standard test library

Scenario files (YAML) in `firmware/SimFW/scenarios/`, versioned with the
firmware they test. Each declares: preset, initial conditions, the profile the
DUT is expected to run, scheduled faults, and **expectations** (assertions with
tolerances and deadlines). Starter set:

1. `baseline_firing` — no faults; full profile completes; PID holds bands;
   regression baseline for temps/currents.
2. `welded_ssr_midfire` — S3-class: current with relay open at 40 % of soak;
   expect trip + K4 drop within deadline.
3. `welded_contactor_s9` — current persists after K4 opens; expect S9
   escalation behavior.
4. `tc_disconnect_ramp` / `tc_disconnect_soak` — OC fault at two profile
   phases; expect per-guard reaction (S5-class) and correct LCD/web surfacing.
5. `tc_flaky` — intermittent OC at 0.5 Hz-class; anti-nuisance doctrine says
   *no* instant trip: assert filtered behavior matches `SAFETY_MODEL.md`.
6. `tc_stuck` — frozen reading during ramp (S13/sample-counter class).
7. `tc_noise_storm` — heavy noise; expect filtering, no nuisance trip.
8. `main_safety_skew` — safety TC reads +80 °C vs main; expect the
   conservative side to win.
9. `broken_element` — one zone never heats; expect zone-imbalance detection /
   profile abort per doctrine.
10. `partial_element` — 60 % power coil; slow ramp; expect ramp-rate handling.
11. `estop_midfire` — E-stop opens at temp trigger; expect immediate safe
    state + latched trip requiring explicit clear.
12. `runaway_zone` — heat with relay open (model-forced); the S8-relevant
    scenario (S8 itself ships disabled; scenario documents expected *current*
    behavior and is ready for when S8 gets a measured threshold).
13. `cj_fault` — cold-junction fault bits; expect per-datasheet handling.
14. `spi_flaky_tc_ic` — MISO bit errors; expect driver CRC/sanity behavior,
    no garbage temps accepted.
15. `estop_at_boot` — E-stop open from power-on (the as-built no-jumper
    state); expect permanent STOP presentation.
16. `power_blip` — via the fixture's DUT 12 V power relay (decided, 3.4):
    brownout/restart mid-fire; expect safe resume behavior.

Each standard test also states **which guard(s) it exercises**, tying the
library to `SaftyFW/docs/GUARD_TEST_MATRIX.md` §3 (the hardware-trip rows that
are currently blocked on "no bench hardware") — this fixture is how those rows
finally get run without a kiln.

### 8.1 Scenario file shape (schema frozen at M-G)

```yaml
name: welded_ssr_midfire
version: 1
exercises: [S3, S4]           # guard cross-reference, GUARD_TEST_MATRIX.md
preset: fast_test
timescale: 10
seed: 42
overrides:                    # optional per-zone param tweaks on the preset
  zones[0].R_element: 12.0
dut:                          # what the operator/runner starts on the DUT
  profile: cone6_fast         # informational + used by runner prompts/automation
faults:
  - id: weld
    type: welded_ssr
    target: relay:K1
    trigger: { at_zone_temp: { zone: 0, temp_c: 400, edge: rising } }
    duration: permanent
expect:
  - name: safety_trips
    event: { type: fault_fired, slot: weld }
    then: { dut: K4_open, within_s: 5 }
  - name: no_early_trip
    forbid: { dut: K4_open, before: { fault: weld } }
  - name: trip_latched
    at_end: { dut: K4_open }
report_keep: [telemetry, events]     # raw streams embedded in the report
```

`expect` clauses reference DUT observations the fixture can see (relay
states, fault line, E-stop loop) and fixture events; anything the fixture
*cannot* see (LCD contents, web UI) is listed under a `manual_checks:` key
that the report surfaces as boxes for a human — never silently dropped.

### 8.2 Run report

JSON per run: scenario name/version/hash, firmware versions (SimFW +
whatever the DUT reports over kilnctrl if available), seed, timescale, start
time, full event list, telemetry samples, per-expectation
`{name, PASS|FAIL|SKIPPED, evidence: [event seqs]}`, overall verdict, and
validity flags (SPI underruns, event-seq gaps ⇒ run invalid, not failed).
Reports land in `tools/PcTools/logs/kilnsim/reports/` and are the artifact
CI archives.

---

## 9. Repo layout

```
firmware/SimFW/
  CMakeLists.txt          pico_sdk + FreeRTOS (SaftyFW-style, no A/B bootloader)
  README.md               quick start, points here
  docs/
    PLAN.md               this file
    PROTOCOL.md           USB command reference (written with the code)
    HARDWARE.md           traced fixture wiring, pin map, isolation notes
  src/
    main.c
    tasks/                usb_owner, cmd_task, spi_emu_a/b, sim_engine,
                          fault_sched, wave_owner, i2c_owner, telemetry, log_task
    sim/                  thermal_model.c, max31856_regs.c, fault_engine.c,
                          sine_synth.c — pure, host-testable
  scenarios/              standard test YAML files
  test/                   host tests (MSVC/CMake, SaftyFW pattern)
  tools/                  check scripts (isolation grep, etc.)

tools/PcTools/src/kilnsim/   protocol.py, link.py, mcp_server.py, cli.py,
                             gui.py, scenario.py, report.py
```

---

## 10. Milestones

Ordered by dependency and risk; each states its exit criterion — the thing
that must be *demonstrated*, not just built. **Software for every milestone
M-A through M-H has now been written** (skeleton, protocol, thermal model,
TC emulation, CT synthesis, relay/IO, fault engine, MCP/CLI/GUI, and the
19-scenario library — grown from 17 since this table was last written — all
exist in the tree). What follows is honest about which exit criteria that
satisfies and which it does not — **build-verified and host-tested is not
hardware-verified**, and for this fixture almost every exit criterion as
originally written specifically demands hardware evidence. None of the
milestone statuses below change because of `virtual_simfw`/`virtual_dut`
(section 13.4) — those tools run real code against a simulated fixture on a
PC, which is neither "built" nor "hardware-verified" in this table's sense,
it is a new, fourth thing. Where it matters (M-G/M-H, which talk about
scenarios "running"), that distinction is called out explicitly below rather
than left to be inferred.

- [ ] **M-A — SPI slave proof of concept.** PIO MAX31856 emulation, one channel,
  against a real master (the bench ESP32 running unmodified `KilnFW` driver
  code, or a third Pico as a scripted test master first). *This is the risk
  item; it goes first, before any framework code.*
  **Exit:** Saleae capture showing correct mode-1 multi-byte reads at the
  master's real clock with zero TX underruns over ≥10k transactions; open
  question 11.4 (driver access-pattern audit) answered in writing.
  **Status: NOT MET.** The PIO engine is written (`max31856_pio_engine.c`,
  `max31856_spi_slave.pio`) and its mode-1 clocking was corrected this pass
  (see the status header above) — but no fixture hardware has ever been
  built, so no real master has ever clocked it and no Saleae capture exists.
  This remains the single biggest unproven risk in the whole plan; software
  completeness elsewhere does not retire it. `tools/spi_test_master/` (a
  standalone SPI reference-master bench firmware + host soak runner) was
  built specifically to make this milestone achievable safely, without
  needing the real ESP32/`KilnFW` driver as the first thing ever thrown at
  the emulator — but even that tool has not yet been run against real SimFW
  hardware, because none exists.
- [~] **M-B — Protocol lift + skeleton + USB.** Protocol core extracted from
  `UnitTestFw` into `CommonFW` (pure, host-tested, spec doc moved); FreeRTOS
  task skeleton; CDC link speaking the extracted protocol; PING/VERSION/
  GET_CAPS; fresh `kilnsim` CLI talking to it.
  **Exit:** CLI round-trips against real hardware; CommonFW host tests green
  under MSVC; **`UnitTestFw` decommission executed (section 12)**.
  **Status: PARTIALLY MET.** The extraction is done (`benchproto` in
  `CommonFW`, spec in `BENCHPROTO.md`) and CommonFW host tests are green
  (18/18). The task skeleton is no longer a skeleton — every task has a real
  body. **Not met:** "CLI round-trips against real hardware" — no fixture
  exists to round-trip against, only host-side simulation of the wire format
  has been exercised. **Not met:** the `UnitTestFw` decommission has not been
  executed — section 12 step 2 ("prove the replacement on real hardware")
  is exactly the hardware gate above, so `firmware/UnitTestFw/` and
  `hardware/UnitTestFixture/` are both still in the tree, untouched.
- [~] **M-C — Thermal model + TC emulation wired.** 3+1 channels, model-driven
  temps, presets, time-scale, MODEL/MANUAL modes.
  **Exit:** host-test suite green incl. golden traces; on the bench, real
  `KilnFW` displays a plausible warming curve driven entirely by the model;
  `TC_GET_MASTER_CONFIG` shows the DUT's real register writes.
  **Status: HOST-TEST MET, HARDWARE NOT MET.** `thermal_model.c` and
  `max31856_regs.c` are implemented and host-tested with golden traces. The
  bench half — a real `KilnFW` board displaying a plausible curve — has not
  happened; no board has ever been connected to this fixture.
- [~] **M-D — CT synthesis.** 3x 60 Hz with amplitude tracking, transformer
  coupling network built.
  **Exit:** calibration table fitted per 3.3 against `SaftyFW`'s own ADC
  readback; commanded 0→N A sweep reads back within ±5 % over the usable
  range; one-relay/one-channel commissioning check passes.
  **Status: NOT MET, and honestly further from met than the others.**
  `ct_wave_pwm.c`/`sine_synth.c`/`wave_owner.c` generate the waveform in
  software and pass their host tests. **Updated 2026-08-20:** both halves of
  the calibration *machinery* now exist and are tested — the PC-side
  sweep/fit/crosstalk runner (`tools/ct_calibration/`, `13487b5`) and the
  firmware apply-path (`src/sim/ct_calibration.{c,h}` + generated table,
  `f93b2eb`). **What remains is entirely hardware-gated:** a real bench run
  against real CT/transformer/ADC hardware to produce the first JSON table,
  then regenerating the header from it. The shipped table is an explicit
  "no data" marker, not placeholder constants. No transformer coupling
  network has been built. §3.3's "store the table in fixture flash keyed by
  channel" is also still unmet and waits on a SimFW `config_store`, which
  does not exist.
- [ ] **M-E — Relay sense + discrete I/O.** Expanders, E-stop, fault line, DUT
  power switch.
  **Exit:** heat loop closes end-to-end — DUT PID actually regulates a
  simulated zone through relay cycling with no fixture intervention;
  `kilnsim power cycle` reboots the DUT and telemetry shows it.
  **Status: NOT MET.** `i2c_owner.c`/`mcp23017.c` implement the drivers and
  are host- and build-verified, and `docs/HARDWARE.md` has since reconciled
  their pin claims against the other three driver files with zero collisions
  found. The closed-loop, DUT-power-cycle, and E-stop exit behaviors are all
  bench-only demonstrations that have not been attempted — no fixture
  hardware exists to close the loop with.
- [~] **M-F — Fault engine + scheduler.** Full catalog, trigger spec, slots,
  composition rules.
  **Exit:** same scenario + seed twice ⇒ byte-identical event logs;
  every fault type demonstrated at least once with an event trace.
  **Status: HOST-TEST MET (software determinism), HARDWARE NOT MET.**
  `fault_engine.c`/`fault_sched.c` implement the full catalog against a
  simulated snapshot and are host-tested for deterministic replay from a
  seed. Nothing here has produced an event trace against a real DUT, which
  is what "every fault type demonstrated" originally meant.
- [~] **M-G — MCP server + GUI + scenario runner.** Scenario YAML schema frozen;
  reports with assertions and validity flags.
  **Exit:** `baseline_firing`, `welded_ssr_midfire`, `tc_disconnect_ramp`
  green against real `KilnFW`+`SaftyFW` with reports archived; GUI drives
  every MANUAL mode.
  **Status: SOFTWARE MET, HARDWARE NOT MET.** `kilnsim`'s MCP server, CLI,
  GUI, scenario loader, and report generator all exist
  (`tools/PcTools/src/kilnsim/`); the YAML schema is frozen and all 17
  scenario files parse cleanly through the loader (pytest-verified figures
  from an earlier commit — treat exact counts as stale; three other sessions
  are editing this area as this document is written). None of the named
  scenarios — or any of the others — has ever run against a real
  `KilnFW`+`SaftyFW` pair, so no scenario report has ever been archived from
  a live run, and the GUI's MANUAL-mode controls have never driven real
  fixture hardware. **Since this table was last written, all 19 scenarios
  have run against `virtual_simfw`+`virtual_dut` (section 13.4) — real
  `SimFW` simulation code and real `SaftyFW` guard code, on a PC, with no
  RP2040 at all.** That is a genuinely new kind of evidence, but it is not
  this milestone's exit criterion: "real `KilnFW`+`SaftyFW`" means silicon,
  and none has run. See `firmware/SimFW/tools/virtual_dut/results/
  SCENARIO_RESULTS.md` for that run's actual pass/fail pattern — mostly FAIL,
  for reasons that are themselves the headline finding of this pass (see the
  status header above), not a scenario-writing or fixture defect.
- [~] **M-H — Standard library complete.** All 16 scenarios written and run.
  **Exit:** each maps to its `GUARD_TEST_MATRIX.md` rows and that file is
  updated in the same change; `kilnsim run --all` is a one-command
  regression gate.
  **Status: LIBRARY MET (over-delivered: 19, not 16), "AND RUN" NOT MET
  AGAINST REAL HARDWARE.** The library has grown twice since this milestone
  was last written — `power_blip` first brought the count to 17, and two
  more scenarios (`mainfault_tc_disconnect`, `safety_tc_frozen`) closed
  guard-coverage gaps found while building `GUARD_TEST_MATRIX.md` §5, for 19
  files on disk today, all declaring an `exercises:` guard cross-reference.
  `GUARD_TEST_MATRIX.md` §5 cross-references guards to scenarios, and its new
  reachability subsection (§6) records, per guard, whether it can currently
  fire at all. **None of them has ever run against real hardware** — "written"
  and "run" are different verbs in this milestone's own exit criterion, and
  only the first is true against silicon today. All 19 *have* now run against
  `virtual_simfw`+`virtual_dut` (section 13.4), which is real code but not
  real hardware — see that section and `virtual_dut/README.md` for what that
  run actually found (mostly: guards whose inputs are never populated in
  current `SaftyFW`, not scenario or fixture bugs).

---

## 11. Open questions (resolve before the matching milestone)

1. [~] **Exact J6/J7 mating pinout and voltage levels** — **substantially
   resolved 2026-08-20** by `docs/HARDWARE.md` §3.1/3.2, which traces the
   full mating table for both connectors (including J6's reverse-pin-order
   trap) against `firmware/KilnFW/docs/HARDWARE.md` and
   `firmware/SaftyFW/docs/HARDWARE.md`. **Still open, and newly discovered
   while writing that document:** J6/J7's own voltage/supply pins carry a
   real, unresolved contradiction between the two main-board docs — see
   item 9 below. Nothing here has been continuity-checked against physical
   silicon; `HARDWARE.md` itself is explicit that it reconciles source
   *documents*, not hardware. (M-A)
2. [~] **CT input stage transfer function** — read the AD8542 `CurrentSense`
   sheets to size the transformer/attenuator so "N amps simulated" maps to
   the right burden voltage; then calibrate against `CURRENT_SENSE.md` §5's
   commissioning procedure. **Further resolved 2026-08-20 (`docs/BOM.md`
   §3):** the target gain/full-scale figures (gain 0.715, ≈98 A rms for a
   1 V/30 A CT) are confirmed against `CURRENT_SENSE.md` §2 directly, and
   the transformer ratio decision is corrected from 1:1 to **~3:1**
   step-up — see §3.3 above for the full arithmetic and its confidence
   breakdown. **Still open:** the ~1.5 Vpk usable-Pico-drive figure behind
   that ratio is a medium-confidence estimate, not measured or
   firmware-confirmed; the candidate part's (Triad TY-300P) exact turns
   ratio is unconfirmed against its datasheet; and `wave_owner.c`'s
   amplitude mapping is still an IDENTITY placeholder pending the real
   calibration procedure — see M-D's status in section 10. (M-D)
3. [x] ~~DRDY/`~FAULT` over I2C latency~~ — **resolved 2026-08-20:** direct Pico
   GPIO (3.4/3.6); no I2C latency question remains.
4. [x] ~~**Does the ESP's driver ever use write-then-read within one CS assertion**
   in a pattern the PIO responder must special-case?~~ **RESOLVED 2026-08-20
   — `docs/SPI_ACCESS_AUDIT.md` (`46fa310`). Answer: no.** Neither master
   ever emits a write-then-read, a repeated start, or any multi-phase
   transaction inside one CS assertion. Every transaction from either master
   is exactly one CS-low window holding one address byte then 1–6 data bytes,
   with direction fixed for the whole window by bit 7 of the address byte.
   This is structural, not incidental — both transports are
   one-CS-per-transfer by construction (`esp_spi_owner.c:26-34`,
   `SaftyFW/src/spi_owner.c:62-65`). Six distinct shapes total (3 write, 3
   read); neither master's read burst ever crosses `0Fh`. **The responder
   needs no special case and the PIO program can be frozen on this point.**
   The audit also found and fixed four real responder defects (two critical)
   and corrected §3.2.1's timing budget — see §0.1 and the correction block
   in §3.2.1. Note this satisfies only the *audit* half of M-A's exit
   criterion; the Saleae capture half remains, and now has three specific
   claims to confirm (`SPI_ACCESS_AUDIT.md` §8). (M-A)
5. [~] ~~DUT power control~~ — **resolved 2026-08-20:** yes, a fixture relay in
   the 12 V feed, MCP23017 #1-driven (3.4); `power_blip` is in scope.
   **One-vs-two-relay question further resolved 2026-08-20
   (`docs/BOM.md` §6): two relays, not one.** The main board has *two*
   independent 12 V inputs — J18 (main domain) and J19 (safety domain) —
   with no shared copper downstream (confirmed: their bulk caps live on
   different schematic sheets, `/5V Regulator/` vs `/SaftyRegulator/`). A
   single relay bridging both downstream of itself would bond `GND_Main`
   and `GND_Safty` through the shared 12 V return, undermining the
   isolation the rest of the fixture is built to preserve — so the design
   uses two independent relays, each on its own bench-supply channel, which
   also lets scenarios brown out one domain independently of the other.
   **Not yet implemented in code:** `i2c_owner.c` still exposes only
   `EXP1_PIN_DUT_POWER` (one bit); a second named MCP23017 output is needed
   (spare capacity exists, `docs/HARDWARE.md` §3.7) — a firmware follow-on,
   not a parts gap. **Still genuinely open, not just an estimate needing a
   part:** relay inrush rating vs the board's actual inrush has not been
   measured. `docs/BOM.md` §6 gives an estimate — ~60 A peak / ~190 µs decay
   from ~940 µF per-domain bulk capacitance (confirmed via
   `get_kicad_component` on C9/C10/C53/C61) and an *assumed* ~0.2 Ω total
   source+ESR resistance — but that source-resistance figure is not
   measured, so treat the 60 A/190 µs numbers as a planning estimate only,
   pending a scope/current-probe capture at first power-on. See
   `docs/HARDWARE.md` §0 item 6 and §3.7. (M-E)
6. [ ] **Fixture hardware form** — how long does the breadboard harness survive
   before a real `hardware/SimFixture/` KiCad board is worth it? Revisit
   after M-E. Unchanged — no fixture hardware, breadboard or otherwise, has
   been built yet, so this has not become answerable.
7. [x] ~~Naming~~ — **resolved 2026-08-20:** stays `SimFW`; README already
   disambiguates vs `UnitTestFw`/`UnitTestFixture`.
8. [~] ~~UnitTestFw protocol reuse mechanics~~ — **resolved 2026-08-20:** the
   protocol core is lifted into `CommonFW` before `UnitTestFw` is deleted;
   PC side is written fresh, nothing imported from its `pc_tools`. The
   extraction itself is now done (`benchproto`); the deletion is still
   pending on the hardware proof section 12 step 2 requires. See section 12.
9. [ ] **NEW — J7 pin 1 contradiction between the two main-board hardware
   docs.** `firmware/KilnFW/docs/HARDWARE.md` ("Safety thermocouple board
   (J7 -> J1)") says J7 pin 1 is "(no connect)".
   `firmware/SaftyFW/docs/HARDWARE.md` §8 says J7 pin 1 is `3.3v_Safty` (via
   R51, 0 Ω). These cannot both be true of the same physical connector.
   `docs/HARDWARE.md` §0 item 5 follows the `SaftyFW` doc as the more
   recently reviewed, more narrowly scoped safety-domain source — but flags
   this explicitly as **unverified, requiring a continuity check before the
   fixture's isolated-side power feed is wired**, since getting it wrong
   means either back-feeding an unintended 3.3 V rail or leaving the
   isolator side unpowered. Resolve at bring-up step 5–6 (section 14),
   alongside the ground-domain check that already lives there. (M-A/pre-M-A
   bring-up)
10. [x] ~~kilnsim's own CLI is missing subcommands `docs/HARDWARE.md`'s bring-up
    checklist (§6) needs~~ — **resolved 2026-08-20:** `tools/PcTools/src/
    kilnsim/cli.py` now has `io`, `ct`, `relay`, `selftest`, `fault-list`,
    `fault-cancel`, and `fault-fire` subcommand groups; `docs/HARDWARE.md`
    §6 and `docs/BENCH_RUNBOOK.md` have been updated to use them instead of
    the MCP-only workaround. §13.2's `kilnsim selftest` loopback mode is
    implemented and its own help text is explicit that hardware-only checks
    (SPI master loopback, CT→ADC loopback) report `NOT_RUNNABLE`, never
    faked — still worth re-checking its exact behavior against its own
    source before leaning on it, since it was landing concurrently with this
    pass. (pre-M-A bring-up convenience)
11. [x] ~~**NEW — `kilnsim`'s USB auto-detect matches a placeholder VID:PID, not
    one `SimFW` actually claims.** `tools/PcTools/src/kilnsim/link.py`
    defines `SIMFW_VID_PID = "2E8A:000A"` — Raspberry Pi's generic
    example-board CDC identifier, explicitly commented as a placeholder "until
    `firmware/SimFW/docs/HARDWARE.md` documents a real one." With multiple
    RP2040-based Picos on the bench at once — the fixture Pico, the
    `spi_test_master` reference Pico, and the safety processor's Debug
    Probe — auto-detection could latch onto the wrong device, silently
    talking to (or trying to flash/reset) something other than the fixture.
    `docs/BENCH_RUNBOOK.md` already warns operators to pass `--port COMx`
    explicitly whenever more than one Pico is attached (§1.2), which is most
    of a real bench session. **Not fixed here** — `tools/PcTools` is being
    worked on concurrently by another session; this item exists so a real
    VID:PID gets claimed and swapped in before it causes a bench mistake, not
    just worked around by operator discipline every time. (pre-M-A bring-up
    hazard)~~ **RESOLVED 2026-08-20 (`55d81e5`):** SimFW claims
    **`2E8A:F00A`** and `spi_test_master` claims **`2E8A:F00B`**, both
    documented in `docs/HARDWARE.md` §7 "USB identity" — which is exactly the
    condition this item set for itself. Worse than described above was found
    while fixing it: `link.py` matched `2E8A:000A` while the firmware actually
    shipped TinyUSB's stock `0xCafe:0x4001`, so auto-detect could never have
    found the fixture at all, only some other Pico. `BENCH_RUNBOOK.md` §1.2's
    `--port` warning was softened but deliberately kept, because
    `list_candidate_ports()` matches VID:PID only and takes `candidates[0]` —
    two SimFW fixtures on one bench remain genuinely ambiguous.

---

## 12. `UnitTestFw` decommission plan

`UnitTestFw` (the ESP32-S3 instrument bench) was a first attempt and is being
thrown away (decided 2026-08-20). Order matters — the protocol lives only
there today:

1. [x] **Extract first (M-B):** lift the protocol core — framing, CRC,
   reliability/retry/dedup, task registration — out of
   `UnitTestFw/UnitTest/App/drivers/` into `CommonFW` as a pure, host-tested
   library alongside kilnlink (they stay separate protocols; nothing touches
   the safety link's compatibility floor). Move
   `docs/UART_PROTOCOL.md`'s content to `CommonFW/docs/` in the same change.
   Note: `UnitTestFw`'s `uart_protocol.c` is a known stale fork of `KilnFW`'s
   (ROADMAP M2 allowlist) — extract from the best of both, don't blindly copy
   the stale one.
2. [ ] **Prove the replacement:** SimFW's CDC link and the fresh `kilnsim` PC
   link layer both speak the `CommonFW`-hosted protocol, PING/VERSION green
   on real hardware. The independent Python implementation doubles as the
   extraction's cross-check.
3. [ ] **Delete, one commit, no stragglers:**
   - `firmware/UnitTestFw/` entirely — App, pc_tools, docs, build trees, and
     the embedded `UnitTestFixture.kicad_*` files plus
     `UnitTestFixture-backups/`.
   - `hardware/UnitTestFixture/` entirely.
4. [ ] **Sweep the references in the same commit** (grep hit list as of
   2026-08-20): `CLAUDE.md`, `README.md`, `ROADMAP.md`, `docs/SETUP.md`,
   `docs/REPO_LAYOUT.md`, `kilnCtl.code-workspace` (folder/tasks entries),
   `tools/setup.ps1`, `.gitignore`, and
   `tools/check_no_duplicate_crc.ps1` — its allowlist entry for
   `UnitTestFw`'s `uart_protocol.c` dies with the file (per that script's own
   rule: deleting an allowlist entry is part of finishing a migration).
   Re-grep for `UnitTestFw|UnitTestFixture` before committing.
5. [x] **No tag needed** — git history is the archive; the deletion commit
   message names this plan section as the rationale.

---

## 13. Testing strategy

Originally three layers, cheapest first. A fourth, unplanned layer has since
appeared between 1 and 2 — see 13.4.

1. [x] **Host tests** (MSVC/CMake, `SaftyFW/test` pattern, `test/`): everything
   in `src/sim/` is pure and runs on the PC — thermal model golden traces,
   MAX31856 register machine (feed byte sequences, assert register/SR/DRDY
   behavior against datasheet-derived vectors), fault trigger evaluation
   (synthetic snapshots through every trigger kind, boundary times, slot
   ordering), sine-table generation. The register-machine vectors double as
   documentation of what the emulator claims to implement. **Real, done**:
   this is the layer the status header's 4873/4873-class numbers come from.
2. [~] **Loopback tests** (fixture alone, no DUT): a `kilnsim selftest` mode —
   PIO engines clocked by a scripted on-fixture master (spare PIO SM) to
   verify the SPI path end-to-end; CT outputs looped to a spare ADC input
   for amplitude sanity; expander read-after-write. Runs in CI-on-a-bench
   without the main board attached. **Status: in progress, not yet landed as
   of this pass** — this is being built concurrently by another session; do
   not trust a specific implementation shape here until that work lands and
   this section is updated again. It remains hardware-adjacent (needs real
   fixture GPIO/SPI/I2C, just not the main board), unlike layer 4 below.
3. [ ] **DUT integration** (the point of the project): the scenario library
   (section 8) against real `KilnFW`+`SaftyFW`. `kilnsim run` exit codes
   make it a scriptable gate; reports are the archived evidence. **Status:
   unchanged, fully hardware-gated** — see section 10's milestone table.
   Nothing about layer 4 below substitutes for this; it is real hardware or
   nothing.

### 13.4 Software-level integration (new, unplanned — `virtual_simfw` +
     `virtual_dut`)

This plan's original three layers didn't anticipate a middle ground between
"pure unit test with synthetic input" and "real hardware" — but one exists
now, sitting between layers 1 and 2 in cost, and it is genuinely useful,
not a toy:

- **`firmware/SimFW/tools/virtual_simfw/`** compiles this project's own
  `src/sim/*.c` (thermal model, MAX31856 register machine, sine synth, fault
  engine) **unmodified** for the host and serves the real `benchproto` wire
  protocol over TCP — so a complete scenario runs end-to-end against the
  actual simulation logic, driven by the real `kilnsim` client code, with no
  RP2040 ever attached. See that tool's own README for its precise scope and
  limits (in particular: no SaftyFW guards exist inside it by itself, so
  `guard_warn`/`guard_trip` events cannot appear from `virtual_simfw` alone).
- **`firmware/SimFW/tools/virtual_dut/`** closes that gap from the other
  side: it compiles `SaftyFW`'s real, unmodified `safety_guards.c` and
  `relay_grace.c` for the host, ticks them against `virtual_simfw`'s live
  data at the real 100 ms cadence, and turns the real verdicts into events
  `kilnsim`'s own report evaluator can score. See its own README for exactly
  which `SaftyFW` files are compiled verbatim, which are deliberately out of
  scope (and why), and its full "Findings" section.

**What this layer is not**: it is not hardware verification, and neither
tool's own README claims otherwise — no real SPI bus, no real relay coil, no
real ESP, no FreeRTOS scheduling jitter. Treat a PASS here as "the code's
decision logic did the right thing given this input," never as "the board is
safe," exactly as `virtual_dut/README.md` states in its own first paragraph.

**What it is**: the first time any of this project's guard-provocation logic
has run against `SaftyFW`'s real, unmodified source rather than a synthetic
host-test harness or a description of intended behavior — and it surfaced a
genuine, previously-unquantified finding (the status header above, and
`GUARD_TEST_MATRIX.md`'s reachability section): only S5/S6b/S7/S12 can
currently fire in shipping `SaftyFW`, for specific, individually-verified
reasons. That is a real capability this plan did not originally scope for,
and it is re-runnable — anyone landing Phase 6/7 work in `SaftyFW` can
re-run `virtual_dut` against the same 19 scenarios and see exactly which
guards newly become reachable, without needing bench hardware to find out.

Firmware-side checks mirror the repo's conventions: a `tools/` grep script
enforcing the single-owner doctrine (no peripheral register access outside
its owner file), `-Wall -Wextra -Werror` clean under arm-none-eabi-gcc, and
host tests required green before merge — same bar `SaftyFW` holds. Three CI
check scripts now exist under `firmware/SimFW/tools/`
(`check_single_owner.ps1`, `check_sim_purity.ps1`, `check_scenarios.py`) plus
a `run_checks.ps1` runner, none yet wired into any actual CI pipeline (none
exists for this repo to join, same caveat `check_no_duplicate_crc.ps1`
already carries elsewhere in this tree).

## 14. Bring-up order (bench checklist)

The wiring order is chosen so each step is verifiable before the next adds
risk, and the DUT is not connected until the fixture alone is proven:

1. [ ] Pico alone: USB CDC + protocol + heartbeat; `kilnsim state` works.
2. [ ] Expanders on I2C: read/write, interrupt lines if used.
3. [ ] SPI A loopback (scripted master on spare pins): register machine correct.
4. [ ] CT synthesis into a scope/DMM through the transformer: waveform + levels.
5. [ ] **Ground-domain check before first DUT contact:** with the bring-up
   jumper OUT, verify no continuity fixture-GND ↔ GND_Safty; verify isolator
   and transformer orientation.
6. [ ] DUT thermocouple path: J6 unplugged from the real daughterboard, fixture
   in its place, `KilnFW` booted — temperatures appear.
7. [ ] Safety path: J7 via isolator, `SaftyFW`'s single channel reads.
8. [ ] Relay sense: command relays via existing kilnctrl tools, fixture sees
   edges.
9. [ ] E-stop + fault line + DUT power relay, one at a time.
10. [ ] First closed-loop firing on `fast_test` preset.

Each step gets a row in `docs/HARDWARE.md`'s checklist when that doc is
written, same keep-it-current rule as `SaftyFW/docs/HARDWARE.md`.

## 15. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| 5 MHz first-byte latency not met | Core feature (TC emulation) degraded | M-A first, two-stage plan (3.2.1), documented lower-clock fallback as last resort |
| Master driver access patterns surprise the responder | Emulation subtly wrong, flaky DUT reads | Open question 11.4: audit both drivers + Saleae capture of real traffic *before* freezing PIO programs |
| J6 pinout traced wrong (reverse-order trap) | Possible damage on first plug-in | 11.1 resolved on paper *and* continuity-checked at bring-up step 5–6; series resistors on fixture bus-A lines for the first plug-in |
| Ground strap through the fixture defeats isolation | Isolation-dependent behavior untestable; masks real design errors | 3.5 discipline; bring-up step 5 explicit continuity check; standard library runs jumper-out |
| CT amplitude calibration drifts / transformer nonlinearity | Current-based guards tested against wrong magnitudes | Calibration stored per channel with date; re-cal procedure in `kilnsim` (M-D); validity flag in reports if cal older than N days |
| Protocol extraction stalls (stale-fork cleanup balloons) | M-B late, UnitTestFw lingers | Scope extraction to exactly what SimFW needs; the deletion deadline is the forcing function |
| Pin budget overruns during layout | Redesign churn | Documented 3-pin fallback (3.6) reserved before it is needed |
| Fixture bugs masquerade as DUT bugs | Wasted debugging, false confidence | Shadow-truth in every reply (5.2), validity flags (8.2), selftest mode (13.2), instrumentation counters never silent |
