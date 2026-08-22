# SimFW — Design Notes

This is the reference document for **SimFW** (the kiln-simulator / bench-test
fixture firmware, a second Raspberry Pi Pico) — settled architecture,
finished work, and the reasoning behind decisions that are no longer open
questions. It exists so `docs/PLAN.md` can stay a short, current list of what
is still *undecided or unfinished*, per the working rule: if it's done and
worth keeping, it lives here; if it's done and not worth keeping, it isn't
written down anywhere (git history is the record); if it's still open, it's
in `PLAN.md`, not here.

Nothing in this document should be read as "current status" — for status,
milestone state, and open items, see `PLAN.md`. Where a decision recorded
here still has a hardware-gated loose end (e.g. "confirm the transformer
ratio before ordering"), that loose end is tracked in `PLAN.md`, not
duplicated here.

---

## 1. Purpose and scope

The kiln controller is hard to test because the interesting behavior only
happens when a multi-kilowatt kiln is heating, drifting, and failing. SimFW
replaces the kiln: a second Pico that plugs into the main board's connectors
and pretends, convincingly, to be the rest of the kiln — the thermocouple
ICs, the current transformers, the heater load, and the thermal mass — while
a PC drives it over USB from a CLI, a GUI, or an MCP server. It lets every
guard, fault path, and control loop in `KilnFW` (ESP32-S3) and `SaftyFW`
(RP2040) be exercised on the bench, repeatably, from a script — including
failure modes nobody wants to provoke on a real kiln (welded contactors,
runaway zones, broken elements).

**Scope boundary (settled, 2026-08-21): SimFW's interface to the DUT is
physical only.** It acts on the thermocouples, board discrete I/O, relays,
and the E-stop input — nothing else. It **never** injects data into the
DUT's logic, issues a safety-link command, or interacts with the UI: it is
the *kiln*, not the operator and not the PC. Anything a human operator would
do — enabling the safety relay, starting a firing — stays with the operator,
via `kilnctrl` or the panel; SimFW's job is to make the kiln heat and go
wrong, convincingly, so that operator action has something real to act on.
This is why `kilnsim`'s wire protocol has no SAFETY command group and never
will (§13 below has the provenance of that specific question) — SimFW must
never gain one.

**In scope**

- Emulate every MAX31856 thermocouple IC the main board talks to: the three
  main-side channels behind J6 (CS0/CS1/CS2, ESP32 is master) and the one
  safety-side channel behind J7 (safety Pico is master). Register-accurate
  SPI slave emulation including fault bits, DRDY timing, and configuration
  readback.
- Generate three independent 60 Hz AC waveforms with programmable amplitude
  into the CT jacks (J13/J15/J17), so the safety processor's current-sense
  channels see realistic burden voltages that track simulated heater current.
- Sense all five relay outputs (K1, K2, K3, K5 main side; K4 pilot safety
  side) at their terminal blocks and close the loop: relay states drive the
  thermal model, the thermal model drives the emulated thermocouples, and
  heater current appears on the CT outputs only when the right relays are
  closed *and* K4 permits.
- Simulate the kiln's thermal mass per zone: heating, soak, cooling,
  inter-zone coupling, ambient loss.
- Inject faults on demand or on a schedule: thermocouple, IC, relay/contactor,
  element faults, and more (§7).
- Test remaining main-board I/O: E-stop circuit, `Fault` line, J20 spare I/O,
  expander-driven signals — stimulus and measurement both.
- USB CDC serial control: MCP server, CLI, and GUI in `tools/PcTools`.
- A library of standard, versioned test scenarios with a standardized fault
  timing model.

**Out of scope**

- The fixture PCB itself. Bring-up starts on a breadboard/protoboard harness;
  a KiCad project (likely `hardware/SimFixture/`) is a follow-on once the
  netlist stabilizes.
- Mains-voltage anything. The fixture never sees line voltage; SSR/contactor
  load terminals are simulated purely at signal level.
- Keeping `firmware/UnitTestFw` alive — decommissioned per §12.

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
                                    │ registration) — `benchproto` in CommonFW
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
 [SPI A]  [SPI B]  [3x PWM   [I2C bus]    │          │         │
  6 pins   4 pins   +RC LPF]     │        │          │         │
      │   via dig.     │     ┌───┴──────────────────────────┐  │
      │   isolator     │     │ MCP23017 #1: relay sense x5, │  │
      │  (GND_Safty)   │     │  ESP Fault sense, E-stop     │  │
      │      │         │     │  drive (optoMOS), DUT 12V    │
      │      │         │     │  power relay, J20 IO_3/4     │  │
      │      │         │     │ MCP23017 #2: spares only     │  │
      │      │         │     │  (DRDY/~FAULT x8 moved to    │  │
      │      │         │     │  direct Pico GPIO, §3.4)     │  │
      │      │         │     │ PCA9685 (optional): PWM/LED  │  │
      │      │         │     │  stimulus, analog-ish tests  │  │
      │      │         │     └───┬──────────────────────────┘  │
      │      │         │         │                             │
══════╪══════╪═════════╪═════════╪═════════════════════════════╪══════════
      │      │    3x isolation   │                        (heartbeat,
      │      │    transformer    │                         debug UART,
      │      │    (§3.3)         │                         SWD to Debug
      │      │         │         │                         Probe)
      ▼      ▼         ▼         ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │                      kilnCtl MAIN BOARD                         │
   │  GND_Main domain                    GND_Safty domain            │
   │  J6 (1x20 socket)  ◄── SPI A        J7 ◄── SPI B (isolated)     │
   │   SCLK/MOSI/MISO,                    SCLK/MOSI/MISO/CS0,        │
   │   CS0/CS1/CS2,                       thermoFault, thermoDrdy    │
   │   thermoFault_0..2,                  (safety Pico is master)    │
   │   thermoDrdy_0..2                                               │
   │   (ESP32-S3 is master;              J13/J15/J17 (3.5 mm CT      │
   │   thermocouple daughter-             jacks) ◄── 3x 60 Hz AC     │
   │   board UNPLUGGED, fixture           into AD8542 stages →       │
   │   sits in its place)                 A1 ADC0/1/2                │
   │  J3  K1 NC/COM/NO ──► relay sense   J10 K4 NC/COM/NO ──► sense  │
   │  J4  K2 NC/COM/NO ──► relay sense    (pilot relay, safety)      │
   │  J8  K3 NC/COM/NO ──► relay sense                               │
   │  J11 K5 NC/COM/NO ──► relay sense   E-stop net ◄── optoMOS      │
   │                                      (fixture opens/closes it)  │
   │  J20 IO_3 / IO_4 ◄──► expander I/O                              │
   │  `Fault` (ESP GPIO6→U1) ──► sense                               │
   └─────────────────────────────────────────────────────────────────┘
```

The double line marks the fixture's isolation boundary. Everything below it
on the right (safety) side is referenced to `GND_Safty`; the main board's two
ground domains share no copper, and the fixture must not become the strap
that shorts them together (§3.5).

**Closed loops the fixture creates**

1. **Heat loop (main side):** ESP32 closes K1/K2/K3 → fixture senses contacts
   → thermal model adds heater power to the zone → zone temperature rises →
   emulated MAX31856 registers report it → ESP32's PID reacts.
2. **Current loop (safety side):** relay closed *and* K4 pilot closed *and*
   element healthy → CT output amplitude = simulated current → safety Pico's
   current-sense channels see it → guards S3/S4 (current-vs-command
   coherence) become testable, including the welded-contactor case (current
   present with no relay commanded).
3. **Safety TC loop:** the same zone temperatures feed the safety-side
   emulated MAX31856, so overtemp guards fire from a physically consistent
   story — or an *inconsistent* one, when a test deliberately skews the
   safety channel.

---

## 3. Hardware architecture

### 3.1 Why a second Pico

- **PIO** is the only realistic way to be an SPI *slave* for multiple chip
  selects with register-accurate timing. The RP2040's two PIO blocks map
  cleanly onto the two independent SPI buses (ESP-side and safety-side).
- The RP2040's USB device controller gives a zero-extra-hardware CDC control
  port.
- Same toolchain, FreeRTOS config, and Debug Probe workflow as `SaftyFW`.

### 3.2 MAX31856 emulation

Each emulated channel is a register file plus behavioral rules:

| Addr (R/W) | Register | Emulated behavior |
|---|---|---|
| 00h/80h | CR0 | R/W verbatim. `AUTOCONVERT` starts/stops conversion cadence; `1SHOT` self-clears; `OCFAULT[1:0]` selects OC-detect mode; `CJ` disable honored; `FAULT` mode changes SR/`~FAULT` clearing rules; `FAULTCLR` self-clears and clears latched faults |
| 01h/81h | CR1 | R/W verbatim. `AVGSEL[2:0]` lengthens conversion time; `TC TYPE[3:0]` recorded — temperature reporting is unaffected, but the written value is exposed via `TC_GET_MASTER_CONFIG` so a wrong-type config is a test failure |
| 02h/82h | MASK | R/W verbatim; masks which SR faults assert `~FAULT` |
| 03h/83h | CJHF | CJ high threshold vs simulated CJ |
| 04h/84h | CJLF | CJ low threshold |
| 05h–06h | LTHFTH/L | TC high threshold (16-bit) vs simulated TC temp |
| 07h–08h | LTLFTH/L | TC low threshold |
| 09h/89h | CJTO | CJ offset, signed, honored in reported CJ |
| 0Ah–0Bh | CJTH/L | Simulated CJ temperature, updated per conversion; writable when CJ sensor disabled |
| 0Ch–0Eh | LTCB H/M/L | Read-only 19-bit linearized TC temp from the thermal model, 0.0078125 °C LSB, latched while CS low |
| 0Fh | SR | Read-only fault status (OC, OVUV, TCHIGH, TCLOW, CJHIGH, CJLOW, CJRANGE, TCRANGE) from simulated conditions + injected faults + master-written thresholds |

Behavior on top of the map:

- **SPI mode 1, capped at 4 MHz** (see §3.2.1) — multi-byte auto-increment
  reads/writes exactly as the datasheet describes, so the masters' existing
  drivers need no changes to run against the fixture.
- **Conversion timing:** in auto-convert mode LTCB updates on the datasheet
  cadence (~100 ms nominal) from the thermal model; DRDY asserts per
  datasheet. One-shot mode honored.
- **Configuration readback:** whatever the master writes to CR0/CR1/mask
  reads back verbatim, so tests can assert the firmware configured TC type,
  filter, and fault masks correctly.
- **Fault machinery:** SR, `~FAULT`, and mask interactions modeled per
  datasheet, evaluated against simulated temperatures and master-written
  thresholds.
- **Cold junction:** simulated (default slow ambient drift); CJ-offset writes
  honored.
- **Per-channel corruption knobs:** stuck conversion (LTCB frozen), noisy
  reads, bit-error injection on MISO, dead channel (MISO high-Z/all-0/all-1),
  delayed DRDY, spurious `~FAULT`.

#### 3.2.1 PIO slave engine — final design and the timing budget

One engine per bus. Bus A (ESP side) has three CS lines sharing
SCLK/MOSI/MISO; bus B has one CS.

- **RX SM:** samples MOSI on SCLK's falling edge (mode 1: CPOL=0, CPHA=1 —
  matches the MAX31856 datasheet Table 5, CPOL=0 row: SDI latch on "SCLK
  falling", and both real masters' configs — `KilnFW`'s `MAX31856_SPI_MODE 1`
  and `SaftyFW`'s explicit `SPI_CPOL_0, SPI_CPHA_1`). Pushes each byte to its
  RX FIFO with the active-CS number ORed into the high bits; CS deassert
  terminates the transaction.
- **TX SM:** pulls response bytes from its TX FIFO, shifts out on MISO on
  SCLK's rising edge (holding steady through the falling edge so the master
  samples a settled bit); MISO is tri-stated whenever no CS is low, since
  three emulated chips share one physical MISO on bus A.
- **Register image coherency:** each channel keeps a 16-byte live image in
  SRAM (core-1-owned). `sim_engine` (core 0) never writes it directly; it
  posts `{channel, temp_code, cj_code, sr_bits}` updates through a
  double-buffered snapshot that core 1 commits only between transactions
  (never while that channel's CS is low), so a multi-byte LTCB read is always
  internally consistent — the same guarantee the real chip gives.
- **Write-back:** master writes land in the RX FIFO tagged with CS; the
  `spi_emu_*` task drains them after CS-rise, applies register-write rules
  (self-clearing bits, read-only masks), and raises `master_config_changed`.
- **Instrumentation:** per-channel counters — transactions, bytes, CRC-class
  errors, write conflicts, first-byte-late/underrun events. Underruns are
  counted, never silent: a run with nonzero underruns is flagged invalid.

**The two-deadline timing analysis (a live constraint, not history).** At
4 MHz a byte takes 250 ns. The address byte's *value* is only known after
clock 8's falling edge, and the first response bit must be shifted out
*before* the master samples it — giving two distinct deadlines:

- **Design deadline ≈ 125 ns** — first response bit present on MISO at
  clock 9's *rising* edge (the mode-1 shift edge).
- **Hard deadline ≈ 250 ns at 4 MHz (~200 ns at 5 MHz)** — the master's
  actual MISO *sample* point, clock 9's falling edge. Missing only the design
  deadline but meeting the hard one is fine; missing the hard deadline means
  the whole burst shifts by one position, returning a *plausible-looking
  wrong temperature*, not an obvious fault — the dangerous failure mode this
  design has to rule out.

The implemented DMA-fed response path (an ISR/FreeRTOS-task approach could
not meet either deadline reliably — RP2040 core-1 interrupt latency alone is
~1 µs) measures **~150–215 ns at the stock 125 MHz sysclk**: it misses the
125 ns design deadline but **meets the 250 ns hard deadline with margin at
4 MHz**, and only barely at 5 MHz. At 200 MHz sysclk it drops to ~95–135 ns,
meeting both deadlines at 4 MHz. All of this is RP2040-datasheet arithmetic,
not measurement — a Saleae capture against real silicon is `PLAN.md` M-A's
actual gate.

**Decided: the thermocouple SPI clock is capped at 4 MHz on both masters.**
This is not a change — `KilnFW`'s `CONFIG_KILNCTL_THERMO_SPI_CLOCK_HZ` and
`SaftyFW`'s `SPI_OWNER_BAUDRATE_HZ` were already 4 MHz — it is now an
*enforced ceiling with a recorded reason* (the analysis above), so the number
cannot drift upward without someone confronting this deadline. At 4 MHz the
implemented DMA path meets the hard deadline at stock 125 MHz sysclk, so no
fixture overclock is required for correctness; 200 MHz would only buy the
tighter design deadline as margin. **The display/LCD SPI bus is explicitly
out of scope of this cap.**

**Decided: fixture sysclk stays at the RP2040's stock 125 MHz (2026-08-21,
see §13's resolved-questions entry for the short version).** 200 MHz would
buy margin against the ~125 ns design deadline, but the ~250 ns hard deadline
— the one that actually determines correctness — is already met with margin
at stock sysclk given the 4 MHz cap above. 200 MHz is also above the
RP2040's 133 MHz spec, and first bring-up already has enough unproven
variables (PIO engine, DMA path, fixture wiring) without adding an overclock
to the list. This is datasheet arithmetic, not measurement; M-A's Saleae
capture is the thing that actually settles whether either deadline holds.

A driver access-pattern audit (`docs/SPI_ACCESS_AUDIT.md`, `46fa310`) also
confirmed structurally that neither master ever issues a write-then-read, a
repeated start, or any multi-phase transaction inside one CS assertion —
every transaction is exactly one CS-low window, one address byte, then 1–6
data bytes, direction fixed by address-byte bit 7 (six shapes total: 3 write,
3 read; neither master's read burst ever crosses register `0Fh`). This is
structural, not incidental (`esp_spi_owner.c:26-34`, `SaftyFW/src/spi_owner.c
:62-65`), so the PIO program needs no special case for it and is frozen on
this point. The same audit pass found and fixed four responder defects: MISO
permanently driven instead of tri-stated (three emulated chips share one
physical MISO), TX FIFO surplus leading the next transaction, a vacuous
`first_byte_late` counter, and a 4-bit register address space where the part
has 7.

### 3.3 CT waveform generation

- Three GPIO, each running high-carrier PWM (~250 kHz carrier, 244 kHz
  actual at 8-bit resolution / 125 MHz sysclk on a dedicated slice per
  channel) whose duty cycle is modulated by a 60 Hz sine table (256 entries/
  cycle, stepped at 15.36 kHz by a repeating timer/DMA chain), then a 2-pole
  RC low-pass (~1–2 kHz corner) on the fixture side. Carrier is far enough
  above the RC corner that residual ripple is negligible next to the AD8542
  stage's own filtering.
- **Coupling into J13/J15/J17 via a step-up audio/isolation transformer**
  (decided: transformer coupling, keeping the fixture out of the `GND_Safty`
  domain for these channels, the way a real floating CT source would be).

  **Ratio: ~3:1 step-up**, corrected from an earlier 1:1 decision. At 1:1,
  the Pico-drive ceiling caps the fixture at ~31.5 A rms on the safety
  board's 1 V/30 A CT model — fine for the 10–25 A a resistive element draws,
  but structurally incapable of reaching the ADC's clipping boundary
  (≈98 A rms, from `SaftyFW/docs/CURRENT_SENSE.md` §2: gain 0.715, clamp
  ≈4.6 V peak, 1 V/30 A CT), so a 1:1 fixture could never exercise
  `CURRENT_FLAG_CLIPPED` handling — one of the fixture's stated purposes.
  Derivation:

  ```
  V_sec,pk (clip target)      ≈ 4.6 V   (CURRENT_SENSE.md §2, high confidence)
  V_pri,pk (usable Pico drive) ≈ 1.5 V   (medium confidence, see caveat below)
  n = V_sec,pk / V_pri,pk      ≈ 3.07  →  call it 3:1
  ```

  The 1.5 V peak primary-drive figure is a **medium-confidence estimate**,
  not measured or firmware-confirmed — it assumes ~91% of the theoretical
  ±1.65 V (half the 3.3 V logic rail) swing before a DC-blocking cap. The
  real ceiling depends on whatever modulation-index cap `ct_wave_pwm.c`'s
  amplitude-to-duty mapping ends up using — that mapping is an
  `TODO(M-D calibration)` identity placeholder until M-D lands (`PLAN.md`
  §10). Candidate part: Triad Magnetics TY-300P (`docs/BOM.md` §3) — its
  exact turns/impedance ratio is unconfirmed against its datasheet; that
  document also gives a same-cost fallback (1:1 transformer + ×3 op-amp gain
  stage) if the part's real ratio doesn't hold up. Full sizing derivation and
  confidence breakdown live in `docs/BOM.md` §3; treat this paragraph as the
  decision summary.

- **Programmable per channel:** amplitude (simulated amps, via calibration
  table), phase, DC offset, clipping, dropout (half-cycle skipping, as a
  failing SSR would produce), 50 Hz option.
- Amplitude tracks the thermal model: `I_zone = V_mains / R_element` when the
  zone's relay chain is conducting, scaled by element-health faults (broken
  coil = 0 A; partial short = wrong-but-plausible current).
- Amplitude changes and relay-driven on/off apply at zero crossings only
  (like a real zero-cross SSR) unless a distortion knob overrides it —
  step-in-mid-cycle is itself a selectable distortion, for testing
  current-sense RMS math against ugly waveforms.
- Per-channel phase offsets default to 0°/120°/240° or all-in-phase
  (selectable; costs nothing to support either way).
- **Calibration procedure** (M-D exit): for each channel, sweep commanded
  amplitude across ~10 points, read back what `SaftyFW`'s own `current_task`/
  ADC reports (kilnctrl MCP path or SWD), fit gain/offset, store the table in
  fixture flash keyed by channel — this doubles as `CURRENT_SENSE.md` §5's
  commissioning check (one relay commanded → exactly one channel responds).
- **Calibration tooling and persistence, built ahead of the real bench run
  they need to produce data.** `tools/ct_calibration/` (PC side) holds the
  sweep runner (`calibrate_ct.py`/`fixture.py`/`readback.py`), the fit
  (`fit.py`), a crosstalk gate (`crosstalk.py`), and a versioned table format
  (`calibration_table.py`); `push_ct_cal.py` writes a fitted table to
  `SaftyFW` and verifies it by reading it back rather than trusting the
  write. `SaftyFW` persists the table in its own flash via `config_store`
  (`ct_amps_cal.h`, `config_store.c`/`config_store_flash.c`), addressed
  through `SET_CT_CAL`/`GET_CT_CAL` link commands (`current_task.c`,
  `link_task.c`), which `KilnFW`'s `safety_link`/`uart_bridge` relay end to
  end. The mechanism rejects a corrupted record wholesale rather than
  accepting a partially-valid one, and every channel ships flagged
  uncalibrated until a real sweep populates it. **None of this has run
  against real CT/transformer hardware** — see M-D in `PLAN.md` §10; the
  tooling and persistence chain existing does not change that status.

### 3.4 Relay sensing and discrete I/O

- Relay contacts sensed at the terminal blocks: fixture supplies a small
  wetting voltage through the NO/COM contact into an MCP23017 input
  (opto-isolated for K4, safety domain). Both NO and COM/NC sides observed
  where useful, so "relay commanded but contact stuck" is distinguishable
  from "relay never commanded."
- **E-stop:** an optoMOS/relay sits in the E-stop loop so tests can open it
  mid-firing. Default state configurable — the as-built board reads permanent
  STOP with no jumper, so the fixture *becomes* the jumper.
- **`Fault` line:** the ESP-driven fault output (GPIO6 → U1 opto) is sensed
  so tests can assert the main processor raised it.
- **J20 IO_3/IO_4** and other spare I/O: MCP23017 pins, direction settable
  per test.
- **DRDY/`~FAULT` outputs** (4 channels × 2 lines): **direct Pico GPIO**
  (decided, not I2C-expander), driven open-drain so the board's pull-ups set
  the idle level — microsecond-accurate DRDY timing with no I2C latency
  dependency. The safety-side pair crosses the digital isolator with the
  SPI B lines.
- **DUT power switch** (decided): a fixture relay/high-side switch in the
  main board's 12 V feed, driven from MCP23017 #1, for cold-boot,
  power-cycle, and brownout tests (`power_blip` scenario). **Two relays, not
  one** — the main board has two independent 12 V inputs, J18 (main domain)
  and J19 (safety domain), with no shared copper downstream (their bulk caps
  live on different schematic sheets, `/5V Regulator/` vs `/SaftyRegulator/`).
  A single relay bridging both downstream of itself would bond `GND_Main` and
  `GND_Safty` through the shared 12 V return, undermining the isolation the
  rest of the fixture preserves. Implemented (`f5cb4c3`):
  `EXP1_PIN_DUT_POWER_MAIN` (pin 7, J18) and `EXP1_PIN_DUT_POWER_SAFETY`
  (pin 10, J19), separate protocol commands, **deliberately no combined
  "set both" call anywhere in the stack** — including CLI/GUI/MCP, which is
  test-enforced (`0926213`) — so bonding the two ground domains can never be
  a silent default. **K4 must never be masked on DUT power loss** (`63acef8`,
  `virtual_simfw.c`): its coil is on the separate GND_Safty relay fed from
  J19 and driven by the safety processor, while `FT_DUT_POWER_CUT` actuates
  only the GND_Main relay — only K1/K2/K3/K5 float open when GND_Main loses
  J18.

### 3.5 Isolation discipline

The main board keeps `GND_Main` and `GND_Safty` separate; the fixture must
too, or every isolation-dependent behavior becomes untestable and a real
design error could hide behind the fixture's ground strap.

- Fixture logic ground ties to **GND_Main** (SPI bus A, main-side relay
  sense, J20, `Fault` sense).
- **SPI bus B** (J7, safety domain) plus the safety-side DRDY/`~FAULT` pair
  cross digital isolators (6 channels total, ISO7741-class parts) powered
  from the safety side's 3.3 V at J7.
- **CT channels** cross via transformer (§3.3).
- **K4 sense and E-stop** cross via optocoupler/optoMOS.
- A deliberate, labeled, removable jumper can common the grounds for early
  breadboard bring-up — the standard test library must run with it out.

### 3.6 Pico pin budget

> **The authoritative GPIO *numbers* live in `HARDWARE.md` §1, not here —
> this table budgets pin *counts* only.** Treating a count budget as
> sufficient is exactly how an earlier pass re-derived four `~DRDY` pins that
> `HARDWARE.md` §1 had already assigned differently (resolved in `1d32e84`;
> §14 below). If code claims a pin, cite `HARDWARE.md` §1 or update it in the
> same commit.

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

Total = 25 of the Pico's 26 header GPIO — tight but it fits. Documented
fallback if a pin is ever needed: the three main-side `~FAULT` lines (slow,
level-only) move to MCP23017 #2, freeing 3 pins with no timing cost.

### 3.7 I/O expander allocation

| Device | Addr | Pins used | Function |
|---|---|---|---|
| MCP23017 #1 | 0x20 | 5 in | Relay sense K1/K2/K3/K5/K4 |
| | | 1 in | `Fault` line sense |
| | | 1 out | E-stop optoMOS drive |
| | | 1 out | DUT 12 V power relay |
| | | 2 i/o | J20 IO_3/IO_4 |
| | | 6 spare | future main-board I/O tests |
| MCP23017 #2 | 0x21 | 3 out (fallback) | main-side `~FAULT` x3 if Pico pins ever run out (§3.6) |
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
| `sim_engine` | 0 | mid+ | — | Thermal model tick (10 Hz), zone temps/currents; publishes snapshot |
| `fault_sched` | 0 | mid | — | Trigger evaluation each tick; arms/fires scheduled faults |
| `wave_owner` | 1 | high | PWM slices + DMA | 60 Hz synthesis, amplitude/phase/distortion at zero-crossings |
| `i2c_owner` | 0 | mid | I2C0 | Expander traffic; relay-sense debounced scan (5–10 ms), E-stop/DUT-power outputs |
| `telemetry` | 0 | low | — | Periodic state frames to USB (temps, relay states, active faults, sim clock) |
| `log_task` | 0 | lowest | — | Deferred logging, drop-counting, never blocks producers |

Core 1 is reserved for the hard-real-time producers (SPI emulation response
pre-compute, waveform DMA feeding); core 0 does everything elastic. The SPI
emulators' CS-edge/first-byte path runs in IRQ/PIO/DMA context with
pre-staged buffers — the task half only does the slow parts (write-back,
fault-state recompute).

### 4.2 Determinism and the simulation clock

The simulator keeps its own monotonic **sim clock**, normally 1:1 with wall
time but supporting **time-scale** (e.g. 10x) so a 12-hour firing profile can
be regression-tested in minutes. Everything time-driven (thermal
integration, fault triggers, conversion cadence) runs off the sim clock.
Real-time interfaces (SPI response latency, 60 Hz) stay wall-time; the 60 Hz
carrier frequency itself never scales, only the amplitude envelope's
evolution does. Scale changes are logged and stamped into telemetry. All
stochastic knobs (noise, flaky-contact timing) run from a **seeded PRNG**;
the seed is settable per run so every "random" failure is replayable.

### 4.3 Thermal model

Zone count is a runtime parameter, **1–4 zones, default 3** (3 matches the
board's relay/TC/CT channels; a 4th zone lets future boards or asymmetric
rigs map a zone with no dedicated CT/TC). Per zone:

```
C_zone * dT/dt = P_heater(t) - k_loss*(T - T_ambient) - Σ k_couple*(T - T_neighbor)
P_heater = duty(relay chain) * V_mains² / R_element * element_health
```

- Integration: forward Euler at 10 Hz is sufficient for time constants ≥
  minutes; a "fast unit-test kiln" preset (time constants of seconds) uses 4
  Euler substeps per tick, multiplying by the time-scale N so accuracy
  doesn't degrade when runs are accelerated. All math in `float`; state in
  Kelvin-offset °C.
- A configurable **TC lag** (first-order sensor time constant) between zone
  temperature and reported TC temperature.
- The safety-side TC reads a configurable blend of zone temps (default:
  zone 0), with its own lag and fault knobs, so main-vs-safety disagreement
  scenarios are first-class.
- Model is pure C, no RTOS dependencies, host-testable with the same
  MSVC/CMake harness pattern `SaftyFW/test` uses. Golden-trace tests pin the
  integration math.

**Per-zone parameters** (settable over USB, floats unless noted):

| Parameter | Meaning | Typical (real kiln) |
|---|---|---|
| `C` (J/°C) | Thermal mass | 2e5–1e6 |
| `k_loss` (W/°C) | Loss to ambient | 5–30 |
| `k_couple[j]` (W/°C) | Coupling to zone j (symmetric matrix, diag 0) | 1–10 |
| `R_element` (Ω) | Heater element resistance | 10–30 |
| `element_health` (0..1) | 1 = good; 0 = broken; between = partial | 1.0 |
| `tc_lag_s` | TC sensor time constant | 5–60 |
| `T0` (°C) | Initial temperature | ambient |

Globals: `V_mains` (default 240), `T_ambient`, safety-TC blend weights + lag,
PRNG-driven process noise level (default 0 for byte-exact replays).

**Presets** (stored in firmware, selectable by name): `fast_test` (seconds-
scale, ~2 min "firing", default for automated regression), `small_kiln`
(single-zone dominant, ~1 h scale), `three_zone` (realistic 3-zone with
top/middle/bottom coupling), `stress` (huge lag, weak coupling, low mass —
tuning-hostile, for robustness work). Presets are starting points; scenarios
may override any parameter.

### 4.4 Reuse from the repo

- **Wire protocol:** the USB link uses `UnitTestFw`'s hardened UART protocol
  design (framing, CRC, reliability/retry/dedup, task registration), but
  consumed from `CommonFW`'s `benchproto`, not from `UnitTestFw` directly
  (§12). SimFW's owners register as addressable tasks the way DAC/AD9833/OLED
  did in `UnitTestFw`. The safety link's kilnlink compatibility floor is
  untouched — SimFW never speaks on that wire.
- **MAX31856 register semantics:** `SaftyFW`'s `max31856.c` driver and
  `KilnFW`'s driver are the authorities on what the masters actually do; the
  emulator's register model is written against them plus the datasheet.
- **Build skeleton:** `SaftyFW`'s CMake + pico_sdk + FreeRTOS import layout
  (no A/B bootloader needed here — plain single-image, BOOTSEL or SWD
  flashing is fine for a bench tool).

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
- **Event ring:** fixed-size (256 entries), sequence-numbered so the PC
  detects loss; `telemetry` drains it to USB as EVT frames. Events are the
  raw material for scenario reports.
- **Command writes** (mode switches, param sets, fault arms) go through
  `cmd_task` → owner queues; owners apply at their own tick boundary and
  emit a confirming event.
- **Queue depths and drop policy** stated per queue in code; nothing ever
  blocks `spi_emu_*` or `wave_owner` — a full queue toward them is a counted
  drop plus event, never a stall.

### 4.6 DMA ownership rule

`check_single_owner.ps1` enforces two different things: for I2C/PIO/PWM/USB,
§4's single-owner doctrine by include; for DMA the include allowlist is
**only an owner gate**, and the real invariant is acquisition
(`dma_claim_unused_channel` only, no fixed-number claims, no raw `dma_hw->`
outside owners), one exclusive handler per DMA vector in distinct files, and
the §3.6-adjacent channel budget — which is *also* a `_Static_assert` in
`src/main.c` against the SDK's real `NUM_DMA_CHANNELS`, so the lint fails if
that assertion is deleted (neither half can become the only defence).

**Adding a third DMA claimant means updating three things in the same
commit:**

1. The channel-budget table (§3.6's neighborhood in `HARDWARE.md`/this doc).
2. The script's `$dmaBudgetTerms` in `check_single_owner.ps1`.
3. `SIMFW_DMA_CHANNELS_CLAIMED` in `src/main.c`.

Currently 11 of 12 DMA channels are claimed (`a27d1b1`, `docs/HARDWARE.md`
§1b), verified from source.

### 4.6.1 Why `simfw_fatal()` halts the whole board, not just the calling core

(`13a90b0`) SimFW has no console when these failures occur — `stdio_uart`/
`stdio_usb` are both disabled, TinyUSB owns USB — so the solid-on GPIO25 LED
`simfw_fatal()` drives is the only bench-visible signal an operator without a
debugger has. A board that keeps answering USB and telemetry after a
core-1 fatal — even truthfully, naming the dead bus — undermines that signal
more than it helps. `simfw_fatal()` now pushes a sentinel over the RP2040 SIO
inter-core FIFO to a `SIO_IRQ_PROC0` handler installed from `main()` before
the scheduler starts, so a core-1 fatal halts core 0 too.

---

## 5. USB control protocol (design background)

Native USB CDC carrying the hardened protocol lifted from `UnitTestFw` into
`CommonFW`'s `benchproto` (framing, CRC, reliability/retry/dedup, task
registration). Request/response, plus unsolicited telemetry and event
frames. The byte-accurate reference is `docs/PROTOCOL.md`, written with the
code; this section is architectural background only. Command groups:

| Group | Commands (sketch) |
|---|---|
| SYS | `PING`, `GET_VERSION`, `RESET_SIM`, `SET_TIMESCALE`, `SET_SEED`, `GET_CAPS` |
| MODEL | `SET_ZONE_PARAMS`, `GET_ZONE_PARAMS`, `SET_AMBIENT`, `LOAD_PRESET`, `SET_TEMP`, `SET_TC_LAG` |
| TC | `TC_GET_REGS`, `TC_FORCE_TEMP`, `TC_SET_MODE`, `TC_INJECT_FAULT`, `TC_CLEAR_FAULT`, `TC_GET_MASTER_CONFIG` |
| CT | `CT_SET_MODE`, `CT_SET_AMPS`, `CT_SET_DISTORTION`, `CT_GET_STATE` |
| RELAY | `RELAY_GET_STATES`, `RELAY_GET_EDGES`, `RELAY_SET_CONTACT_FAULT` |
| IO | `IO_SET_DIR`, `IO_WRITE`, `IO_READ`, `ESTOP_SET`, `FAULT_LINE_GET` |
| FAULT | `FAULT_SCHEDULE`, `FAULT_CANCEL`, `FAULT_LIST`, `FAULT_FIRE_NOW` |
| EVT | unsolicited: relay edges, fault fired, guard-relevant threshold crossings, sim-clock marks |

Every mutable thing has a **mode**: `MODEL` (driven by the simulation) or
`MANUAL` (frozen at an operator-set value).

### 5.1 Addressing and framing conventions

Each command group registers as an addressable task in the protocol's
task-registration model (the same way `UnitTestFw`'s DAC/AD9833/OLED
registered), so the PC discovers the fixture's capabilities at connect time
via `GET_CAPS` rather than hard-coding them (protocol version, SimFW version
+ git hash, zone/channel limits, feature bitmask).

### 5.2 Representative payloads

Frozen byte layouts live in `PROTOCOL.md`.

- `FAULT_SCHEDULE` request: `{u16 fault_slot, u8 fault_type, u8 target,
  trigger{u8 kind, f32 a, f32 b, u8 zone/relay}, duration{u8 kind, f32 t},
  repeat{u8 kind, f32 period, f32 jitter, u16 n}, f32 param0..3}`.
- `TC_GET_REGS` reply: the channel's full 16-byte register image plus the
  emulator's shadow state (actual simulated temp before corruption, active
  fault list) — the pair is what makes "the DUT was lied to, this is the
  truth" assertions possible.
- `RELAY_GET_EDGES` reply: up to N `{u8 relay, u8 edge, u64 sim_time_us}`,
  also mirrored as EVT frames.

### 5.3 Telemetry and event frames

**Telemetry frame** (periodic, default 2 Hz): sim clock, time-scale, seed,
per-zone `{T_zone, T_tc_reported, T_safety_reported, I_amps}`, relay state
mask, E-stop/fault-line state, active fault count, SPI transaction/underrun
counters, event-ring high-water mark.

**EVT frame** (unsolicited, immediate): `{u32 seq, u64 sim_time_us, u8
event_type, payload}` for relay edges, fault fired/cleared, threshold
crossings, mode changes, DUT power switch, protocol errors. Sequence numbers
make loss visible; the report generator refuses to certify a run with a gap.

---

## 6. PC side: MCP server, CLI, GUI

`tools/PcTools/src/kilnsim/`, sibling to the existing `kilnctrl` package — a
separate MCP server, since it's a different device on a different port.
`kilnsim` is a fresh, self-contained toolset; nothing was ported from
`UnitTestFw`'s `pc_tools` (deleted with that project, §12). The link layer
was written new against the `CommonFW`-hosted protocol spec, which also
served as the extraction's independent second implementation — the same
prove-it-twice pattern the kilnlink codecs used.

**MCP tool surface:** `sim_connect`/`sim_disconnect` (auto-detect by USB
VID:PID + PING), `sim_get_state`, `sim_reset`, `sim_load_preset`,
`sim_set_zone_params`/`sim_get_zone_params`, `sim_set_timescale`/
`sim_set_seed`, `tc_get_regs`, `tc_inject_fault`/`tc_clear_fault`,
`tc_set_mode`, `ct_set_mode`/`ct_set_amps`/`ct_set_distortion`,
`relay_get_states`/`relay_get_edges`, `estop_set`/`dut_power_set`,
`io_read`/`io_write`/`io_set_dir`, `fault_schedule`/`fault_cancel`/
`fault_list`/`fault_fire_now`, `run_test_scenario` (the composite: compiles
YAML into primitive commands, arms the schedule, watches the EVT stream,
evaluates expectations, assembles the report), `get_test_report`,
`sim_raw_command` (debug escape hatch).

**CLI:** `kilnsim` console script, one subcommand per tool area — `kilnsim
state`, `kilnsim preset fast_test`, `kilnsim fault tc0 open --at-temp 600
--zone 0`, `kilnsim estop open`, `kilnsim power cycle --off-ms 200`, `kilnsim
run scenarios/welded_ssr_midfire.yaml --seed 42 --report out.json` (exit code
= pass/fail, the CI entry point), `kilnsim monitor` (`--json` for machine
consumption). Also has `io`, `ct`, `relay`, `selftest`, `fault-list`,
`fault-cancel`, `fault-fire` subcommand groups.

**GUI (Tk):** zone-temperature strip chart (truth vs reported vs
safety-reported per zone); relay lamp row with edge history; per-TC panel
(register hexdump, shadow truth, fault buttons, MODEL/MANUAL toggle); CT
panel (amps slider, distortion combo, commanded-vs-calibrated readback);
discrete I/O panel; fault scheduler timeline; scenario runner (load YAML,
progress, live expectation status, report view). Session logging to
`tools/PcTools/logs/kilnsim/`.

The crucial MCP property: **assertions live PC-side.** A scenario run
returns a machine-readable report (events with sim-clock timestamps), and
the MCP layer exposes it so Claude (or pytest) can judge pass/fail — e.g.
"after the welded contactor at t=300 s, K4 must open within 5 s."

---

## 7. Fault injection framework

### 7.1 Fault catalog

**Thermocouple / sensor faults:** disconnected TC (OC fault bit + `~FAULT`),
broken/intermittent TC (alternating connected/OC on a duty/period or seeded
random), flaky TC (Gaussian/spike noise), shorted TC (reads near-ambient/CJ),
stuck TC (LTCB frozen), drifting TC (slow calibration-drift ramp), CJ fault
(high/low bits, wrong CJ temp), dead TC IC (no MISO response), flaky SPI
(bit errors on MISO), spurious fault pin, main/safety disagree (skew safety
TC vs zone truth).

**Power path faults:** shorted (welded) SSR/relay (current flows while
sensed open), stuck-open relay (commanded closed but zero current), broken
heater coil (zero current, no heating), partially failed coil (wrong
resistance, slow heating), half-waving SSR (CT dropout distortion), welded K4
test support (current flows after K4 sensed open, for S9), phase loss (one
CT channel to zero while others run).

**System faults:** E-stop during firing, runaway zone (force `P_heater` on
regardless of relay state), thermal-mass surprise (step-change model params
mid-run), ambient shift, sensor-vs-element lag stress.

### 7.2 Standardized trigger model

Every scheduled fault is `(trigger, fault, duration, repeat)`:

- **Triggers:** `AT_SIM_TIME t`, `AT_ZONE_TEMP (zone, temp, rising|falling)`,
  `ON_RELAY_EDGE (relay, close|open, +delay)`, `ON_EVENT (named event,
  +delay)`, `AFTER_FAULT (fault-id, +delay)`, `RANDOM_IN (t0, t1)` (seeded),
  `MANUAL` (armed, fired by command).
- **Duration:** `PERMANENT`, `FOR t`, `UNTIL_TRIGGER (…)`.
- **Repeat:** `ONCE`, `EVERY t [jitter j]`, `N_TIMES`.
- All timing in sim-clock units; all randomness from the run's seed — same
  scenario + seed ⇒ same run, byte-for-byte in the event log. That
  replayability rule is the fixture's core testing contract.

### 7.3 Engine internals

Fixed pool of fault slots (32): `{slot_id, state: IDLE|ARMED|ACTIVE|EXPIRED,
fault_type, target, trigger, duration, repeat, params[4], fire_count}`. Slot
ids are stable for the run and appear in every related event.
`fault_sched` evaluates all ARMED triggers each 10 Hz tick against the zone
snapshot + event ring; firing flips the target's override + emits
`FAULT_FIRED{slot}`, expiry is symmetric with `FAULT_CLEARED{slot}`. Trigger
evaluation order is slot order — deterministic and documented. Faults
compose (e.g. `tc_noise` + `tc_drift` on the same channel); genuinely
conflicting pairs (dead IC + anything) resolve to the more severe one and log
a warning event rather than refusing. Composition rules per fault type are
part of `PROTOCOL.md`.

---

## 8. Standard test library

Scenario files (YAML) in `firmware/SimFW/scenarios/`, versioned with the
firmware they test. Each declares: preset, initial conditions, the profile
the DUT is expected to run, scheduled faults, and expectations (assertions
with tolerances/deadlines). The library has grown to 19 files (from a
16-file starting sketch); the files on disk in `scenarios/` are the living
catalog — treat this list as the founding intent, not a live count:

`baseline_firing`, `welded_ssr_midfire` (S3-class), `welded_contactor_s9`
(S9 escalation), `tc_disconnect_ramp`/`tc_disconnect_soak` (S5-class),
`tc_flaky` (anti-nuisance filtering), `tc_stuck` (S13/sample-counter class),
`tc_noise_storm`, `main_safety_skew`, `broken_element`, `partial_element`,
`estop_midfire`, `runaway_zone` (S8-relevant, S8 itself ships disabled),
`cj_fault`, `spi_flaky_tc_ic`, `estop_at_boot`, `power_blip`, plus
`mainfault_tc_disconnect` and `safety_tc_frozen` (added to close
guard-coverage gaps found while building `GUARD_TEST_MATRIX.md` §5).

Each states which guard(s) it exercises, tying the library to
`SaftyFW/docs/GUARD_TEST_MATRIX.md` §3 — the hardware-trip rows that are
otherwise blocked on "no bench hardware." This fixture is how those rows
finally get run without a kiln.

### 8.1 Scenario file shape (schema frozen at M-B/M-G)

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
*cannot* see (LCD contents, web UI) is listed under `manual_checks:`, which
the report surfaces as boxes for a human, never silently dropped.

A `forbid` clause takes optional `after`/`before` bounds (default: run
start/end) — a persisting level produces no edge, but its *ending* does, so
"current never stops after K4 opens" is expressible as a `forbid` on
`current_absent` after that edge (`d5a43b0`).

### 8.2 Run report

JSON per run: scenario name/version/hash, firmware versions (SimFW + DUT via
kilnctrl if available), seed, timescale, start time, full event list,
telemetry samples, per-expectation `{name, PASS|FAIL|SKIPPED, evidence:
[event seqs]}`, overall verdict, validity flags (SPI underruns, event-seq
gaps ⇒ run invalid, not failed). Reports land in
`tools/PcTools/logs/kilnsim/reports/` and are the artifact CI archives.

---

## 9. Repo layout

```
firmware/SimFW/
  CMakeLists.txt          pico_sdk + FreeRTOS (SaftyFW-style, no A/B bootloader)
  README.md               quick start, points to PLAN.md
  docs/
    PLAN.md               live plan — what's left
    DESIGN_NOTES.md        this file — settled architecture & history
    PROTOCOL.md            USB command reference (written with the code)
    HARDWARE.md            traced fixture wiring, pin map, isolation notes
  src/
    main.c
    tasks/                usb_owner, cmd_task, spi_emu_a/b, sim_engine,
                          fault_sched, wave_owner, i2c_owner, telemetry, log_task
    sim/                  thermal_model.c, max31856_regs.c, fault_engine.c,
                          sine_synth.c — pure, host-testable
  scenarios/              standard test YAML files
  test/                   host tests (MSVC/CMake, SaftyFW pattern)
  tools/                  check scripts (isolation grep, virtual_simfw, virtual_dut, etc.)

tools/PcTools/src/kilnsim/   protocol.py, link.py, mcp_server.py, cli.py,
                             gui.py, scenario.py, report.py
```

---

## 10. The fourth testing layer: `virtual_simfw` / `virtual_dut`

The original testing strategy (host tests, loopback, DUT integration —
`PLAN.md` §13) didn't anticipate a middle ground between "pure unit test with
synthetic input" and "real hardware," but one exists now, sitting between the
first two layers in cost, and it is genuinely useful, not a toy:

- **`firmware/SimFW/tools/virtual_simfw/`** compiles this project's own
  `src/sim/*.c` (thermal model, MAX31856 register machine, sine synth, fault
  engine) **unmodified** for the host and serves the real `benchproto` wire
  protocol over TCP — so a complete scenario runs end-to-end against the
  actual simulation logic, driven by the real `kilnsim` client code, with no
  RP2040 ever attached. See that tool's own README for its precise scope and
  limits (in particular: no `SaftyFW` guards exist inside it by itself, so
  `guard_warn`/`guard_trip` events cannot appear from `virtual_simfw` alone).
- **`firmware/SimFW/tools/virtual_dut/`** closes that gap from the other
  side: it compiles `SaftyFW`'s real, unmodified `safety_guards.c` and
  `relay_grace.c` for the host, ticks them against `virtual_simfw`'s live
  data at the real 100 ms cadence, and turns the real verdicts into events
  `kilnsim`'s own report evaluator can score. See its own README for exactly
  which `SaftyFW` files are compiled verbatim, which are deliberately out of
  scope (and why), and its "Findings" section.
- **`virtual_simfw`'s TELEMETRY broadcast is real-time-paced at 2 Hz
  regardless of time-scale** (`63acef8`) — so a scenario window shorter than
  ~500 ms of *wall* time can't be observed, producing a flaky pass/fail race
  rather than a clean failure. This is why `power_blip`'s windows widened
  from 0.2 s/1 s to 25 s.
- **The harness now composes three real firmwares' unmodified source in one
  loop** (`94f2fc3`): SimFW's `src/sim/*`, KilnFW's `thermal_guard.c` (via
  `virtual_kiln`'s `kiln_core.exe`), and SaftyFW's `safety_guards.c` (via
  `virtual_dut`'s `dut_core.exe`). The opt-in `dut.kiln_guard6:` scenario
  field spawns `kiln_core.exe`, ticks KilnFW's real guard-6 against the TC
  channel a scenario's fault targets, and asserts the fault line only when
  that real code trips for `THERMAL_GUARD_TRIP_SENSOR_INVALID` —
  SaftyFW's S6a then decides for itself. This is distinct from
  `mainfault_esp_asserted.yaml`'s `set_main_fault` path, which asserts the
  line directly and still exists to test S6a in isolation. Non-vacuity was
  confirmed by a negative control: disconnecting a *different* TC channel
  than `kiln_guard6` watches produces a genuine FAIL.

**What this layer is not:** hardware verification. Neither tool's README
claims otherwise — no real SPI bus, no real relay coil, no real ESP, no
FreeRTOS scheduling jitter. A PASS here means "the code's decision logic did
the right thing given this input," never "the board is safe."

**Boundary audit (2026-08-21): several harness conveniences deliberately sit
outside §1's physical-only scope boundary, and that is correct — for the
harness, not for real SimFW firmware.** §1 says the real fixture only acts on
thermocouples, board I/O, relays, and E-stop, and never injects data,
issues safety commands, or plays operator. This PC-only harness has no real
ESP and no real human operator at all, so it must stand in for both to
produce a non-trivial test; the following all fall in that "stands in for an
absent actor" category, are legitimate exactly because this harness has no
other way to get the input, and **must never be ported into
`firmware/SimFW/src/` or the real `benchproto`/`PROTOCOL.md` wire protocol**:

- `run_dut_scenarios.py`'s `operator_actions: request_enable` — stands in
  for the operator issuing `SAFETY_CMD_REQUEST_ENABLE` via `kilnctrl`, needed
  only because `dut_core.exe` *is* SaftyFW's logic running in-process with no
  operator present. Real SimFW must never send this command (§1, §13).
- `set_main_fault` (`63acef8`) — stands in for the ESP's own decision to
  assert its fault output, needed only because no real ESP32 exists in this
  harness. A real fixture only *senses* the fault line; it never asserts it
  on the DUT's behalf.
- `dut.kiln_guard6:` (`94f2fc3`) — runs KilnFW's real `thermal_guard.c` via
  `virtual_kiln`'s `kiln_core.exe` to drive the fault line non-vacuously,
  standing in for a physical ESP32 that isn't there. It makes the harness's
  verdict less vacuous, but a real fixture has no business running KilnFW's
  guard logic itself — the real ESP does that.
- `dut.zone_setpoints:` — supplies a setpoint nothing physical would
  provide, standing in for a full profile execution the harness doesn't run.
- `SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE` (0xF0) / `SIMFW_VIRTUAL_CMD_FAULT_LINE_SET`
  (0xF1) — virtual-only commands in `virtual_simfw.c` letting a DUT with no
  physical relay coil or sense wire report a sensed state directly. Already
  disciplined correctly: deliberately excluded from
  `firmware/SimFW/src/tasks/cmd_ids.h` and `PROTOCOL.md`, with header
  comments saying so. Keep it that way — on real hardware these signals are
  *sensed* off physical wiring, never set by fiat.

None of this is a defect in the harness; it is the harness doing its job of
approximating absent hardware and an absent human in software. The risk is
purely one of migration — a future change that lifts one of these patterns
into real `SimFW` firmware or the real protocol would cross §1's boundary
(the fixture acting as the operator or injecting data instead of sensing/
driving physical signals). No such migration exists today; this entry
exists so the next reader recognizes the pattern before repeating it.

**What it is:** the first time this project's guard-provocation logic ran
against `SaftyFW`'s real, unmodified source rather than a synthetic host-test
harness or a description of intended behavior. It surfaced a genuine,
previously-unquantified finding — see §11 below — and it is re-runnable:
anyone landing further `SaftyFW` work can re-run `virtual_dut` against the
same 19 scenarios and see exactly which guards newly become reachable,
without needing bench hardware.

`virtual_dut`'s batch-ticking approximation was removed (`3c6763a`) —
replaced by one tick per *observed* sample carrying a true `dt_s`, with no
tick when the fixture published nothing new. There is no honest filler for
skipped ticks: repeating a sample invents reads, dropping `tc_valid` invents
*bad* ones. This fixed both bias directions and turned `no_warn_storm`'s FAIL
into a PASS by removing the fabrication, not by loosening the expectation.

Firmware-side checks mirror the repo's conventions: a `tools/` grep script
enforcing the single-owner doctrine, `-Wall -Wextra -Werror` clean under
arm-none-eabi-gcc, and host tests required green before merge. Three CI
check scripts exist under `firmware/SimFW/tools/`
(`check_single_owner.ps1`, `check_sim_purity.ps1`, `check_scenarios.py`) plus
a `run_checks.ps1` runner, none yet wired into any actual CI pipeline (no
pipeline exists for this repo to join yet).

---

## 11. Guard reachability investigation (provenance)

A `virtual_dut` run against real, unmodified `safety_guards.c`/
`safety_core.c` established, empirically and reproducibly, that in
shipping `SaftyFW` at the time, **only S5, S6b, S7, and S12 could
structurally fire** — the other nine guards were blocked by specific inputs
`safety_core_build_input()` never populated (`link_up` never set,
`main_fault_asserted` never read, `relay_deenergized` never computed,
`context_valid` never set true, `heat_commanded` hardcoded false). Every one
of these was verified by reading `safety_core.c`/`safety_guards.c` directly,
then cross-checked against `virtual_dut`'s independent run of the same real
code.

This was not a discovery of regressions — `SAFETY_MODEL.md` and
`SaftyFW/TODO.md` already said Phases 6/7 (link context, current sense) were
unbuilt. What was new was the exact, guard-by-guard list, and that it could
be re-checked automatically any time those phases landed.

**The finding was acted on.** Commits `f304392` (context/`link_up`/
`REQUEST_ENABLE` wiring), `5375bca` (S6a), `5f90325` (S9), and `6e98ae3`
(S11) closed all of these gaps in `SaftyFW` itself. **Every guard input is
now produced**, with two guards left deliberately dormant, and this
distinction matters going forward:

- **S1** does not trip because `abs_max_temp_c` defaults to 0, and
  `safety_guards.c`'s own convention is that 0 means "not commissioned,
  never trip" (`safety_guards.h`'s doc comment) — a deliberate safety
  choice, not a missing producer. S1's guard logic is otherwise fully wired
  (real `tc_c` from `thermo_task`) and will trip correctly the moment a real
  ceiling is commissioned (`config_store`, Phase 9, already exists).
- **S13** does not trip because no `borrowed_zone_index` field exists in the
  input struct — again a commissioning gap, not a wiring gap.

**S1 and S13 are commissioning gaps, not wiring gaps** — a categorically
different, and much smaller, remaining item than the nine that were closed.
S6a is a related but distinct case: `SaftyFW`'s own wiring is complete and
correct, but it cannot currently be *provoked* through the `virtual_dut`
harness because that harness's fixture-side emulation has no I2C-expander/
opto model for the Fault line — a fixture-side gap, not a `SaftyFW` gap (see
`PLAN.md` §0.2 for the still-open bench-only item this leaves).

The live, authoritative reachability table is
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §6 — this section is the
provenance/history, not the source of truth for current guard status.

---

## 12. `UnitTestFw` extraction history

`UnitTestFw` (the ESP32-S3 instrument bench) was a first attempt superseded
by SimFW, and is being decommissioned — decision and full remaining-step
list in `PLAN.md` §12. The history worth keeping:

- The protocol core — framing, CRC, reliability/retry/dedup, task
  registration — was extracted from `UnitTestFw/UnitTest/App/drivers/` into
  `CommonFW` as `benchproto`, a pure, host-tested library alongside
  `kilnlink` (the two stay separate protocols; nothing touches the safety
  link's compatibility floor). `docs/UART_PROTOCOL.md`'s content moved to
  `CommonFW/docs/` (as `BENCHPROTO.md`) in the same change.
  `UnitTestFw`'s own `uart_protocol.c` was a known stale fork of `KilnFW`'s
  (ROADMAP M2 allowlist), so the extraction was written from the best of
  both rather than a blind copy of the stale one.
- `benchproto` passes its own host tests (18/18, framing+link) alongside the
  pre-existing `kilnlink` suite, confirmed untouched.
- `kilnsim`, SimFW's PC toolset, was written fresh against the extracted
  protocol spec rather than porting `UnitTestFw`'s `pc_tools` — this also
  serves as the extraction's independent second implementation (the
  prove-it-twice pattern the kilnlink codecs used).
- No tag was made for the pre-deletion state — git history is the archive,
  and the eventual deletion commit message names `PLAN.md` §12 as rationale.

---

## 13. Resolved open questions (history)

These were tracked as open questions during the design pass; each is settled
now. Kept here for the reasoning; `PLAN.md` §11 keeps only what's still
open.

- **DRDY/`~FAULT` over I2C latency** — resolved: moved to direct Pico GPIO
  (§3.4/§3.6); no I2C latency question remains.
- **Does either master ever use write-then-read within one CS assertion?**
  Resolved no, by direct audit (`docs/SPI_ACCESS_AUDIT.md`, `46fa310`) — see
  §3.2.1 above for the finding and why it lets the PIO program freeze.
- **DUT power control** — resolved: two independent fixture relays (§3.4),
  not one, to preserve ground-domain isolation.
- **Naming** — stays `SimFW`; README disambiguates vs `UnitTestFw`/
  `UnitTestFixture`.
- **`UnitTestFw` protocol reuse mechanics** — resolved: extract into
  `CommonFW` before deleting `UnitTestFw`; PC side written fresh. See §12.
- **`kilnsim` CLI missing subcommands `HARDWARE.md`'s bring-up checklist
  needs** — resolved: `io`, `ct`, `relay`, `selftest`, `fault-list`,
  `fault-cancel`, `fault-fire` groups added; `HARDWARE.md` §6 and
  `BENCH_RUNBOOK.md` use them instead of an MCP-only workaround.
  Re-verified against source (`d9d54ca`): behavior matches the CLI's own
  help text, and nothing fabricates a pass.
- **`kilnsim`'s USB auto-detect matched a placeholder VID:PID** — resolved
  (`55d81e5`): SimFW claims `2E8A:F00A`, `spi_test_master` claims
  `2E8A:F00B`, documented in `HARDWARE.md` §7. Worse than first suspected:
  `link.py` had actually been matching `2E8A:000A` while the firmware shipped
  TinyUSB's stock `0xCafe:0x4001`, so auto-detect could never have found the
  fixture at all. `BENCH_RUNBOOK.md` §1.2's `--port` warning was kept
  deliberately — `list_candidate_ports()` still matches VID:PID only and
  takes the first candidate, so two SimFW fixtures on one bench remain
  ambiguous.
- **CT input stage transfer function** — resolved via §3.3 above (target
  gain/full-scale confirmed against `CURRENT_SENSE.md` §2; transformer ratio
  corrected 1:1 → ~3:1).
- **Exact J6/J7 mating pinout and voltage levels** — substantially resolved
  by `HARDWARE.md` §3.1/3.2 (full mating table for both connectors,
  including J6's reverse-pin-order trap). The one still-open piece (the J6/
  J7 voltage-pin contradiction between two main-board docs) remains tracked
  in `PLAN.md` §11 as it is unresolved and safety-relevant.
- **Should `kilnsim` send `SAFETY_CMD_REQUEST_ENABLE`, or otherwise gain a
  SAFETY command group?** Resolved no (2026-08-21), by the scope boundary in
  §1: SimFW is the kiln, not the operator or the PC. Enabling the safety
  relay is an operator action taken via `kilnctrl`/the panel, never something
  the fixture does to itself. `kilnsim` already does the right thing here —
  it prints a diagnostic hint when a run never sees K4 close (`eac3905`),
  pointing at `safety_request_enable` and `BENCH_RUNBOOK.md` step 10, without
  ever issuing the command itself. The `virtual_dut` harness's own
  `operator_actions: request_enable` (§10 below) is a *different* thing: a
  PC-only test-harness stand-in for the operator, needed only because
  `dut_core.exe` runs SaftyFW's logic in-process with no operator present at
  all — it does not reopen this question for real SimFW.
- **Fixture sysclk: stock 125 MHz or an overclock to 200 MHz?** Resolved:
  **stay at the RP2040's stock 125 MHz** (2026-08-21). See §3.2.1 for the
  full timing analysis; in short, at the 4 MHz both masters already enforce,
  the DMA-fed path's measured-on-paper ~150–215 ns meets the ~250 ns hard
  deadline with margin at stock sysclk. 200 MHz would only buy margin against
  the tighter ~125 ns *design* deadline, which is not required for
  correctness — and it is above the RP2040's 133 MHz spec, adding an
  overclock as a variable during first bring-up, when everything else in the
  fixture is already unproven. The M-A Saleae capture, not this arithmetic,
  is the real arbiter of whether either deadline actually holds on silicon.

---

## 14. Superseded decisions — provenance kept for the reasoning

Most "an earlier draft said X, it's actually Y" corrections in this
project's history have no lasting value beyond git blame and are not
repeated here. The following are kept because the *reason* still guards
against a specific, recurring mistake:

- **PIO SPI mode bug (fixed).** The PIO slave engine originally sampled MOSI
  on SCLK's *rising* edge (textbook SPI mode 0) while labeling itself mode 1;
  corrected to sample on the *falling* edge, matching the MAX31856
  datasheet's Table 5 and both real masters' drivers. See §3.2.1 above, which
  already describes the fixed (current) behavior.
- **`~DRDY` pin contradiction, resolved in favor of `HARDWARE.md` §1**
  (`1d32e84`) — code had re-derived pins from a pre-§1 view of the tree; the
  table was right, the code was wrong. A full pin sweep afterward found no
  other collisions. The recurring mistake this guards against: **treating
  §3.6's pin-count budget as if it also gave pin numbers** — it doesn't; see
  the note at the top of §3.6.
- **PWM pacer / channel-B pin trap, closed by detection, not avoidance**
  (`3dfb4e3`) — avoidance was impossible at 25-of-26 GPIO occupancy, since no
  PWM slice is free of claimed pins. A `_Static_assert` checks each CT
  channel's GPIO against the six reachable-but-owned pins (6/7/17/19/21/22),
  plus a lint for hardcoded-literal `gpio_set_function` calls and for the
  guard block being deleted. Both layers are negative-tested. The recurring
  mistake this guards against: adding or moving a CT/PWM pin without
  re-running this check.
- **The 1.6 µs first-byte budget was wrong by ~8×** — see §3.2.1's
  two-deadline analysis above for the corrected figures (~125 ns design /
  ~250 ns hard at 4 MHz) and why the error mattered (it changed which
  implementation plan was even viable).
