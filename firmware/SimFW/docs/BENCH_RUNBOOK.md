# SimFW — First Hardware Bench Session Runbook

> **Status:** planning — no fixture hardware has ever existed · **Last
> reviewed:** 2026-08-21
> **Keep this file current.** Every command below is real (checked against
> `tools/PcTools/src/kilnsim/cli.py`) and every claim about behavior is
> either read directly from source or explicitly marked as a prediction.
> When something in this runbook disagrees with what actually happens on the
> bench, **the bench wins** — fix this file in the same session, same rule
> `docs/HARDWARE.md` and `SaftyFW/docs/HARDWARE.md` already follow.

**Read this whole document before touching anything.** It is written for one
sitting at a bench with limited patience, but it front-loads two things that
will otherwise cost hours: a list of failures you should *expect* and ignore
(section 2), and the one check that protects the real board from damage
(section 4, step 5).

**Nothing in `firmware/SimFW` has ever touched real silicon.** The software
is complete and host-tested (`docs/PLAN.md`'s status header, ~4873/4873 host
checks), but the PIO SPI slave has never been clocked by a real master, no
fixture harness has ever been wired, and no scenario has ever run against a
real `KilnFW`/`SaftyFW` pair. Today's session is very likely to be entirely
about steps 1–5 below (Pico alone, through the ground-domain check) — do not
expect to reach a closed-loop firing in one sitting. Section 7 gives a
realistic plan.

**Measure before you trust.** Several numbers below read as settled facts
but are arithmetic from an assumption, not a bench reading. None of the
following have ever been measured on this hardware:

| Figure | Status | Where it comes from |
|---|---|---|
| DUT-power inrush, ~60 A / ~190 µs (step 9) | **Derived.** Assumes ~0.2 Ω source+ESR resistance that was never measured. | `docs/BOM.md` §6 |
| CT transformer ratio, **1:1** (step 4/§5) | **Higher confidence than the earlier ~3:1 plan, but still not measured.** 1.06 V rms clears the board's ≈1 Vrms full-scale sense input with margin (a board property, independent of whichever CT is installed) even if the ~1.5 Vpk usable-Pico-drive estimate behind it is off; the candidate part's (Triad TY-300P) actual turns ratio is unconfirmed against its own datasheet. | `docs/DESIGN_NOTES.md` §3.3, `docs/HARDWARE.md` §5 |
| SPI first-byte timing budget, ~250 ns / ~1.6 µs (step 3) | **Derived.** RP2040-datasheet arithmetic; no Pico has ever been attached to confirm it. | `docs/SPI_ACCESS_AUDIT.md` §9, `docs/DESIGN_NOTES.md` §3.2.1 |
| Relay debounce, ~24 ms worst case (step 8) | Derived from `mcp23017.h`'s stated debounce constant, not bench-timed. | `mcp23017.h` |
| "Plausible, non-zero temperatures" on the DUT (step 6) | **Not proof the SPI framing is correct.** See step 6's note below — a whole-burst byte shift produces exactly this symptom. | §4 step 6, `docs/SPI_ACCESS_AUDIT.md` D2 |

Treat every "Pass" criterion below that rests on one of these figures as
provisional until the cited measurement is actually taken — a derived number
that happens to match reality on the bench is still luck, not verification.

---

## 1. Pre-flight — before anything is powered or connected

### 1.1 Hardware to have on the desk

- **Two Raspberry Pi Picos** at minimum:
  1. The fixture Pico (runs `SimFW.elf`).
  2. A second Pico to act as the M-A reference SPI master
     (`tools/spi_test_master/`) — PLAN.md explicitly allows proving the PIO
     slave against this disposable known-good master *before* risking the
     real ESP32/`KilnFW` driver against unproven PIO code. Do not skip
     straight to plugging into the real board's J6.
  3. A third Pico is needed only when you get to `SaftyFW`/`KilnFW` bring-up
     (steps 6+) — that is the **real** A1 safety Pico and ESP32-S3 already on
     the main board, not a fixture part.
- **Debug Probe** (or equivalent SWD adapter) — same bench pattern
  `SaftyFW`/`KilnFW` already use. `SimFW` has no `.uf2` yet (picotool gap,
  `firmware/SimFW/README.md`'s Build section) — SWD/OpenOCD is the only
  flashing path for the fixture Pico today. BOOTSEL drag-and-drop works for
  `spi_test_master` once you have a `.uf2`, otherwise SWD there too.
- **Saleae logic analyzer** — required for M-A's actual exit criterion (a
  capture, not just a pass/fail table). ROADMAP.md records one was available
  and used on this bench 2026-08-18, so it should already be in the kit.
- **The built fixture hardware itself.** `firmware/SimFW/docs/BOM.md` now
  exists (a first-pass, not-yet-ordered bill of materials — see its own
  status header for what's confirmed-orderable vs. still flagged) but **no
  parts have been ordered and no harness has been built from it as of this
  review.** This session's realistic ceiling is therefore still the fixture
  Pico + reference-master Pico + a hand-wired handful of jumpers for M-A, not
  a populated fixture board. If parts have since been ordered or a
  breadboard/protoboard harness built, update this paragraph and section 7.
- **Cables:** USB (fixture Pico CDC + reference-master Pico CDC + Debug
  Probe), SWD ribbon/leg wires, jumper wires for the SPI-bus-A harness
  between the two Picos (6 signals + GND, see §2 of
  `tools/spi_test_master/README.md`), a multimeter for the continuity check
  in step 5.
- **The real main board**, powered from a bench supply, *not yet connected
  to anything* — it stays disconnected until step 6.

### 1.2 What to flash where, and how to confirm each is alive first

Build both firmwares before wiring anything together. From
`firmware/SimFW/tools/spi_test_master/README.md`'s verified build steps:

```powershell
$env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
$env:FREERTOS_KERNEL_PATH = "C:\pico-tools\FreeRTOS-Kernel"

# SimFW itself (fixture Pico)
cmake -G Ninja -B firmware\SimFW\build -S firmware\SimFW -DPICO_BOARD=pico `
  "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
cmake --build firmware\SimFW\build

# spi_test_master (reference-master Pico)
cmake -G Ninja -B firmware\SimFW\tools\spi_test_master\build -DPICO_BOARD=pico `
  "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
cmake --build firmware\SimFW\tools\spi_test_master\build
```

Flash both `.elf` files over SWD/OpenOCD (per the
[**"Use OpenOCD for ESP32 flashing"**]-style convention this repo follows for
bench programming — use OpenOCD/the Debug Probe path, not a
`.uf2`/BOOTSEL drag for `SimFW.elf` since none exists). Flash one Pico at a
time; don't have both attached to the same Debug Probe simultaneously unless
you know your SWD wiring supports it.

**Confirm each board is alive, alone, before wiring them together:**

- **Fixture Pico:** plug its own USB into the PC (separately from whatever
  flashed it). It should enumerate as a USB CDC serial port. Then:
  ```powershell
  kilnsim state
  ```
  Pass: JSON telemetry snapshot, no transport error, protocol/firmware
  version fields populated. `kilnsim`'s serial autodetect now matches on
  `2E8A:F00A`, a fixture-specific PID `SimFW` claims for itself
  (`docs/HARDWARE.md`'s "USB identity" section) — it no longer shares an ID
  with the `spi_test_master` Pico (`2E8A:F00B`, also fixture-specific as of
  the same change) or the Debug Probe (`2E8A:0004`/`2E8A:000C`), so the
  original three-way collision this caution used to warn about is resolved.
  **Still pass `--port COMx` explicitly if more than one SimFW fixture is on
  the bench at once**: autodetect (`SerialSimLink.list_candidate_ports()`)
  matches on VID:PID only, not serial number, and silently picks the first
  match (`candidates[0]`) when several ports share `2E8A:F00A` — a real
  scenario the moment a second fixture exists, not a hypothetical.
- **spi_test_master Pico:** open its CDC port in any terminal, press Enter,
  confirm the `spi_test_master ready...` banner, then type `PING` and
  confirm `PONG`. This tool is *not* built into SimFW's CMake and does not
  speak `benchproto` — it is a plain-text line protocol, deliberately
  separate so it stays trivially trustworthy as a reference master.

If either board fails its solo check, stop here — do not proceed to wiring.
See section 5's troubleshooting table.

---

## 2. Known DUT incompleteness — read this before you debug anything

**This section exists so you don't spend hours debugging the fixture for
problems that are actually already-known gaps in today's shipping
`SaftyFW`.** An earlier revision of this section reported that only guards
S5, S6b, S7, and S12 could structurally fire, that K4 could never be
energized, and that S6b nuisance-tripped ~120 s into every boot regardless
of link health. **That has since been substantially closed** by a guard-
wiring pass (`safety_core_build_input()` now populates `link_up`,
`context_valid`, `heat_commanded`, `relay_deenergized`, and
`main_fault_asserted`) and by `link_task.c` gaining a real
`SAFETY_CMD_REQUEST_ENABLE` handler that forwards to
`safety_core_request_enable()` → `relay_owner_command_energize()` — verified
directly against `firmware/SaftyFW/src/tasks/safety_core.c` and
`link_task.c` this pass, not carried over from the stale claim.

**Current state: every guard's input is produced. Only S1 and S13 stay
dormant, and both are commissioning gaps, not missing producers:**

| Symptom you may still see | This is expected, because |
|---|---|
| **S1 (absolute temperature ceiling) never trips no matter how hot the fixture reports.** | `cfg->abs_max_temp_c` has no commissioned value yet (defaults to 0, and 0 means "not commissioned, never trip" by deliberate convention, `safety_guards.h`) — a config gap, not a missing producer. Guard logic itself is fully wired to real `tc_c`. |
| **S13 (borrowed-zone sample-staleness) never trips or warns.** | Needs a commissioned `borrowed_zone_index` to say which context zone is "the" borrowed channel — that config field does not exist anywhere in the codebase yet (Phase 9, same category as S1). `safety_core_build_input()` deliberately leaves `sample_counter_advancing` false rather than guess a zone index; `safety_guards.c`'s own S13 block is additionally gated on `cfg->tc_source`, which also has no default, so S13 stays correctly dormant either way. |
| K4 (the safety pilot relay) energizes only when `SAFETY_CMD_REQUEST_ENABLE` is sent AND `relay_owner`'s own interlocks (state machine, GRACE window, any latched trip) allow it — **not automatically at boot.** | This is the real, current behavior, not a gap: `relay_owner_command_energize()` refuses outright while any guard is tripped, and always accepts a disable. Send the enable request (`KilnFW`'s `safety_link_request_enable()` or the equivalent `mcp__kilnctrl__safety_request_enable` tool) and confirm it during step 8/9 rather than assuming K4 stays open forever. |
| Every other guard (S2, S3, S4, S5, S6a, S6b, S7, S9, S10, S11, S12) reacts to fixture-injected faults once its trigger condition is met. | All of their inputs are now populated unconditionally or gated only on `context_valid`/`link_up`, both of which the fixture can actually drive true via a healthy USB link and `PUSH_CONTEXT` traffic — there is no longer a producer-side blocker for these guards. |

**S6b specifically no longer nuisance-trips on a healthy link.** `link_up`
resets S6b's elapsed-silence timer every tick it is true
(`safety_guards.c`), so the 120 s hard backstop only fires on a genuinely
silent link, not unconditionally from t=0 of every boot as the earlier
revision of this document reported.

**If a guard still behaves unexpectedly on the bench, do not assume it's
this same already-known gap** — that assumption has been wrong before in
this project's own docs (an earlier pass separately, and incorrectly,
claimed "S9 is blocked by SaftyFW wiring" and "S11 waits on Phase 6" when
neither was true by the time it was written). Check
`firmware/SaftyFW/src/tasks/safety_core.c`'s `safety_core_build_input()`
directly for what a guard's inputs actually are before writing off a
surprising result as expected incompleteness. Full detail lives in
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §6 and `docs/DESIGN_NOTES.md`'s status
header — read those if a specific guard's behavior needs explaining to
someone else, but verify against the source before repeating either.

**Practical upshot for scenario runs (step 10):** `kilnsim run` has a
dedicated exit code (`2`, "BLOCKED") specifically for expectations blocked
on documented DUT incompleteness like this, distinct from exit `1`
("FAIL", a genuine problem) — see `cli.py`'s `cmd_run` docstring. Do not
treat a BLOCKED scenario run as a fixture failure.

---

## 3. Go/no-go discipline

Every numbered step in section 4 has three possible outcomes. Use these
definitions throughout, not ad hoc judgment calls:

- **GO** — the stated pass criterion is met. Proceed to the next step.
- **NO-GO, diagnose here** — the pass criterion is not met, but nothing
  suggests danger to the real board (you have not connected to it yet, or
  the failure is clearly confined to the fixture side). Stop, consult
  section 5's troubleshooting table, fix, and re-run *this* step before
  advancing. Do not skip ahead "to see if the next step reveals more" —
  each step in this bring-up order exists specifically so failures are
  caught before they can reach the real board (`docs/PLAN.md` §14's stated
  rationale).
- **ABORT** — anything ambiguous about the J7 pin-1 check or CT transformer
  orientation (step 5), anything that suggests the real board saw an
  out-of-spec signal (5 V where 3.3 V was expected, a connector plugged in
  reversed), or any smoke/smell/unexpected heat. Safe abort action: **power
  off the fixture and the main board immediately** (bench supply off, not
  just USB unplugged — the main board's 12 V feed is a separate rail from
  USB), disconnect the harness between fixture and main board, and do not
  reconnect until the suspected cause is understood. A wrong result on step 5
  or step 6 is the one place in this whole procedure with real
  hardware-damage risk (`docs/PLAN.md` §15's risk table: "J6 pinout traced
  wrong" is flagged Impact: possible damage; the fixture's ground is now
  deliberately commoned with the DUT's, so the old "ground strap defeats
  isolation" risk row no longer describes a fixture defect).

---

## 4. Ordered bring-up sequence

This expands `docs/PLAN.md` §14 / `docs/HARDWARE.md` §6's ten-step order.
Each step assumes every prior step passed. **Do not reorder these** — the
sequence is deliberately risk-ascending, and step 5 exists specifically to
run before the DUT is ever touched.

### Step 1 — Pico alone: USB CDC + protocol + heartbeat

Already covered in §1.2 above as the solo aliveness check. Re-confirm here
with the fixture Pico on its own USB, nothing else wired:

```powershell
kilnsim --port COMx state
```
**Pass:** JSON snapshot, populated version fields, no error.
**NO-GO:** see §5 "Fixture Pico symptoms" below.

### Step 2 — Expanders on I2C: read/write

`docs/HARDWARE.md` §6 flags that no `kilnsim` CLI existed for raw I/O when
that document was written — **that gap is now closed**: `cli.py` has a real
`io` subcommand group today. Use it directly rather than the MCP-only
workaround `docs/HARDWARE.md` describes (treat that line in `HARDWARE.md` as
stale and worth a follow-up correction).

```powershell
kilnsim --port COMx io write 8 on --exp 0    # J20 IO_3 spare pin, EXP1_PIN_J20_IO3
kilnsim --port COMx io read 8 --exp 0
```
**Pass:** both MCP23017s ACK on I2C0 (0x20, 0x21 — confirm no I2C error from
either `io write`/`io read` call), and the spare pin round-trips the value
just written.
**NO-GO:** see §5's I2C row.

### Step 3 — SPI A loopback against `spi_test_master` (M-A's core proof)

This is the risk-first milestone (`docs/PLAN.md` M-A) and the reason a
second, disposable Pico is on the desk. **Do not substitute the real ESP32
here** — the entire point of `spi_test_master` is proving the PIO slave
against a trivially-trustworthy master before risking unproven PIO code
against `KilnFW`'s real, unmodified driver.

Wire bus A between the two Picos per `tools/spi_test_master/README.md`'s
table (**provisional pins — re-derive from `../../src/tasks/spi_emu_a.c`'s
`SPI_EMU_A_*_GPIO` defines and this tool's `src/spi_master.h` before
trusting the numbers below if either file has changed since this was
written**):

| Signal | SimFW fixture GPIO | spi_test_master GPIO |
|---|---|---|
| SCLK | 6 | 18 |
| MOSI (fixture RX) | 7 | 19 |
| MISO (fixture TX) | 8 | 16 |
| CS0 | 9 | 20 |
| CS1 | 10 | 21 |
| CS2 | 11 | 22 |
| GND | common | common — **do not skip** |

```powershell
pip install pyserial   # not vendored anywhere in this repo
python firmware\SimFW\tools\spi_test_master\run_soak.py --port COM<master-port>
```
Default sweep: 100 kHz → 5 MHz, 10,000 transactions per rate point.
**Correction: the M-A target rate is now 4 MHz, not 5 MHz.** `docs/DESIGN_NOTES.md`
§3.2.1 decided 2026-08-20 that the thermocouple SPI clock is capped at 4 MHz
on both real masters (`KILNCTL_THERMO_SPI_CLOCK_HZ`'s Kconfig `range` on the
ESP32-S3 plus a matching `_Static_assert` on `SPI_OWNER_BAUDRATE_HZ` in
`SaftyFW/src/spi_owner.c`) — the 5 MHz figures below were superseded by that
decision and are kept in the sweep tool as extra headroom characterization,
not as the pass bar. Treat any rate point above 4 MHz in the tool's own
sweep table as informational only.

**Pass (table-only, not yet M-A's full exit criterion):** every rate point
up through 4 MHz reports PASS, zero mismatches, zero suspected
first-byte-late events.
**Pass (M-A's actual, literal exit criterion):** the above, *plus* a Saleae
capture of the 4 MHz run showing clean mode-1 framing and no MISO-tri-state
violations — see `tools/spi_test_master/README.md`'s "Where the Saleae
fits" section for the exact capture procedure (start the capture just before
issuing `SOAK`, stop it just after `SOAK DONE`). **The sweep table alone
does not satisfy M-A** — pair it with the capture before recording M-A as
met anywhere (section 6 below).
**NO-GO:** see §5's SPI symptom table (`spi_test_master/README.md`'s own
troubleshooting table is more complete than what's reproduced in §5 below —
consult it directly).

Also cross-check via the fixture's own telemetry, per `docs/HARDWARE.md`
§6 step 3's interim suggestion:
```powershell
kilnsim --port COMx state
```
and inspect the SPI transaction/underrun counters before and after the soak
— nonzero underruns invalidate the run regardless of what the soak table
says (DESIGN_NOTES.md §3.2.1: "a test run with nonzero underruns is flagged invalid
in telemetry").

### Step 4 — CT synthesis into a scope/DMM

```powershell
kilnsim --port COMx ct amps 0 5.0
kilnsim --port COMx ct state 0
```
**Pass:** a clean 60 Hz waveform at the J13 test point (or the equivalent
bench test point before the transformer, if the transformer/CT jacks aren't
wired yet — likely true this session, see §1.1's BOM note) within a
plausible voltage range for the commanded amplitude. **CT amplitude
calibration's mechanism now exists (`src/sim/ct_calibration.{c,h}`), but the
compiled-in default table is deliberately all-uncalibrated** — "5.0 A
commanded" still does not yet mean 5 A as any real ADC would measure it,
because no bench calibration run has ever populated real per-channel
constants; behavior today is identity, same as the old placeholder. Judge
this step on waveform cleanliness (frequency, shape, no carrier ripple
bleeding through), not on absolute calibrated amplitude.
**Also unmeasured: the transformer's 1:1 ratio itself.** It's built on a
~1.5 Vpk usable-Pico-drive estimate the project itself calls
medium-confidence — though the fixture's basic correctness is far less
sensitive to that estimate at 1:1 than it was under the earlier ~3:1 plan —
and the candidate part's (Triad TY-300P) actual turns ratio has never been
checked against its datasheet (`docs/DESIGN_NOTES.md` §3.3,
`docs/HARDWARE.md` §5). If the transformer is populated this session,
treat any voltage reading at the safety board's ADC as informational, not as
confirmation the ratio is right — a wrong ratio here reads as a misleading
current value on the DUT side, not damage (the safety board's clamp diodes
D12/D13 are the backstop, per §5's CT troubleshooting row below).
**NO-GO:** see §5's CT row.

### Step 5 — PRE-DUT SANITY CHECK BEFORE FIRST DUT CONTACT

**Do not skip. Do not reorder. This is the step that protects the real
board.**

**Revised 2026-08-23:** the fixture's ground is now deliberately commoned
with the DUT's (`docs/DESIGN_NOTES.md` §3.5) — no digital isolators, no K4
opto, no removable jumper. The old "no continuity between fixture `GND_Main`
and `GND_Safty`" pass criterion is retired: continuity between those two
labels is now the *expected*, correct result of the fixture's own wiring
(e.g. K4's sense resistor bonds them directly), not a fault to abort on.
This was never a check for a board-side isolation defect — it metered the
fixture's own harness before connecting to the DUT — so nothing about the
main board's own isolation was ever being verified here, and nothing about
that changes now.

What this step checks instead:

1. **CT transformer orientation and 1:1 wiring**, against
   `docs/HARDWARE.md` §4's ground-domain map, if that hardware is populated
   this session. The three CT channels are the one part of the fixture that
   still stays floating from both ground domains, same as a real CT — get
   this wrong and the isolation-transformer role those channels rely on for
   correctness is defeated.
2. **Resolve the J7 pin-1 contradiction before wiring the J7 harness.**
   `firmware/KilnFW/docs/HARDWARE.md` says J7 pin 1 is "no connect";
   `firmware/SaftyFW/docs/HARDWARE.md` §8 says the same physical pin is
   `3.3v_Safty` (via R51, 0 Ω). `docs/HARDWARE.md` §0 item 5 follows the
   `SaftyFW` doc as the more recently reviewed source, **but explicitly
   flags this as unverified**. With the real main board powered but nothing
   else connected, probe J7 pin 1 with a multimeter (continuity to the
   3.3 V safety rail, or DC voltage if the rail is live) to determine which
   doc is actually correct on this board revision, and record the answer in
   `docs/HARDWARE.md` §0 item 5 in the same session. The fixture's J7 harness
   leaves pin 1 unconnected either way — getting this wrong before wiring
   would mean back-feeding an unintended 3.3 V rail if a fixture wire
   mistakenly lands on it.

**Pass:** CT transformer orientation/ratio confirmed, and the J7 pin-1
question is resolved (measured, not assumed) before the J7 harness is wired.
**Do this with a meter every time the harness is rebuilt — not once and
trusted forever** (`docs/HARDWARE.md` §6's own wording).

### Step 6 — DUT thermocouple path (main side): first real DUT contact

Only now does the real main board enter the loop. Unplug the real
thermocouple daughterboard from J6; plug the fixture's J6 harness in its
place, **respecting the reverse-pin-order trap**: J6 is a 1×20 socket, the
real daughterboard's J5 header mates in *reverse* pin order (J6 pin 1 = J5
pin 20), and the fixture's own plug must be wired the same reversed way the
daughterboard's header is — **not** pin-for-pin straight through. Verify
against `docs/HARDWARE.md` §3.1's full mating table before applying power;
getting this backwards risks driving 5 V into a clock line.

**Every GPIO number in that table, and everywhere else in `HARDWARE.md`, is
provisional** — reconciled between four independently-written source files,
never traced against physical copper or continuity-checked
(`docs/HARDWARE.md`'s own status header). Do not treat a pin number as fact
just because it appears in a table; if anything about `spi_emu_a.c`,
`i2c_owner.c`, or `ct_wave_i2s.c` (the CT waveform driver, replacing the
deleted `ct_wave_pwm.c` — `DESIGN_NOTES.md` §3.3) has changed since this
document was last reviewed, re-derive from source before trusting the table
over what you're about to wire.

**Fit fixture bus-A series resistors for this first plug-in.** This is
`docs/PLAN.md` §15's own stated mitigation for exactly this step's risk row
("J6 pinout traced wrong … Possible damage on first plug-in") — it protects
against a mis-wired connector back-feeding 5 V into a signal line, **not**
against the separate MISO-tri-state defect (`docs/SPI_ACCESS_AUDIT.md` D1,
fixed in code but never bench-proved — see step 3's note). Given this is one
of the two steps in the whole procedure with real hardware-damage risk
(section 3), treat the resistors as something to actually fit here, not an
optional nicety to skip if you're in a hurry.

Boot `KilnFW` (real ESP32-S3 on the main board):
```powershell
kilnsim --port COMx preset fast_test
kilnsim --port COMx state
```
Cross-check on the DUT side with the `kilnctrl` MCP tools already available
in this environment: `thermo_read` / `thermo_get_reports`.

**Pass:** `KilnFW`'s own telemetry (LCD, or `thermo_read`) shows plausible,
non-zero, non-fault temperatures on all three main-side channels, tracking
what `kilnsim state` believes it is reporting.
**"Plausible temperatures" is necessary here, but it is not proof the SPI
framing is correct — do not read this step's pass as retiring M-A's risk.**
The PIO SPI slave has never been clocked by a real master, and the one
audit finding on this exact failure mode (`docs/SPI_ACCESS_AUDIT.md` D2) is
a whole response burst shifted one byte late, which reads back as a
*different, still-plausible* wrong temperature — not an obvious fault. The
only thing that actually proves the framing is step 3's Saleae capture
against `spi_test_master`, done *before* this step. If step 3 hasn't been
run and passed with a capture yet, treat any "plausible" reading here as
unverified, not confirmed.
**NO-GO:** see §5's DUT-thermocouple row. **Remember section 2's table**
before concluding anything is broken — a lack of *heating* here is expected
this early (K4 has not been asked to energize yet, and relays are not
commanded by a PID with nothing to regulate toward); a lack of *temperature
readings at all* is not expected and is a real fixture-path problem.

### Step 7 — Safety path (J7, direct GPIO)

J7 mates **straight, pin-for-pin** to the safety daughterboard's J1 — no
reversal, unlike J6 (`docs/HARDWARE.md` §3.2). Wire per that table, having
already resolved the pin-1 question in step 5. No digital isolator sits in
this path anymore — SPI bus B and `DRDY_SAFETY`/`FAULT_SAFETY` wire as
direct GPIO (`docs/DESIGN_NOTES.md` §3.5).

```powershell
kilnsim --port COMx state
```
Cross-check `mcp__kilnctrl__thermo_read` / `mcp__kilnctrl__safety_get_status`
on the DUT side.

**Pass:** the safety Pico's own thermocouple reading tracks the fixture's
zone-0 (or configured blend) temperature. **Note:** this is also the first
point where you can confirm S5/S12 are alive per section 2's table — inject
a TC fault (step-ahead preview of step 10's fault tooling:
`kilnsim fault tc:safety open_circuit`) and confirm `safety_get_status`
shows the expected fault reaction. S6b will only trip from here if the USB
link actually goes silent for the backstop window — with a healthy link
`link_up` keeps resetting its timer, so it should stay quiet in the
background rather than counting down unconditionally (section 2).

### Step 8 — Relay sense

```powershell
kilnsim --port COMx relay states
```
Command relays via the existing DUT-side MCP tool
(`mcp__kilnctrl__io_set_relay`), then re-poll:
```powershell
kilnsim --port COMx relay edges --since-seq 0
```
**Pass:** commanding K1/K2/K3/K5 produces a matching edge in the fixture's
relay-edge log within one debounce window (~24 ms worst case, per
`mcp23017.h`). **K4 needs an explicit enable request first** (section 2) —
`mcp__kilnctrl__io_set_relay` alone does not energize K4; send
`mcp__kilnctrl__safety_request_enable` (or `KilnFW`'s equivalent
`SAFETY_CMD_REQUEST_ENABLE`) and confirm no guard is currently tripped
before expecting K4's edge to appear. K4 staying open after a real enable
request, with no guard tripped, is now a real fixture/DUT problem, not
expected behavior.

### Step 9 — E-stop, fault line, DUT power relays — one at a time

**Before the first `power cycle` command: the DUT-power relay's inrush
margin is unverified.** `docs/BOM.md` §6 estimates ~60 A peak / ~190 µs
decay from the board's ~940 µF per-domain bulk capacitance, but that figure
rests on an **assumed** ~0.2 Ω source resistance — nobody has scoped or
current-probed an actual power-on of either domain. The Omron
G5LE-14-DC12 relay is rated 10 A/250 VAC *continuous*; its cold-inrush/
make-current rating has not been checked against the real number. **What to
check:** if a current probe or scope is on hand, capture the first
`power cycle` of each domain and compare against the ~60 A/~190 µs estimate
before treating repeated cycling as routine. **What a pass looks like:** no
audible/visible arcing or relay chatter at the contact, and (if captured) a
peak current within the relay's rated make-current. **What happens if you
skip this:** repeated `power cycle` scenario runs could weld or degrade the
relay contact on an under-margined part with nobody having measured whether
the margin exists — a failure that would only surface as a mysterious
"domain never comes back" fault much later, not at the moment it happens.
If welding is ever suspected, stop domain-power-cycling scenarios until a
series NTC limiter or pre-charge bleed resistor is added (`docs/BOM.md`
§6's own suggested fix).

```powershell
kilnsim --port COMx estop open
kilnsim --port COMx estop closed
kilnsim --port COMx io fault-line
kilnsim --port COMx power cycle --off-ms 500 --domain main
kilnsim --port COMx power cycle --off-ms 500 --domain safety
```
**Pass (E-stop):** open/closed produces the expected STOP/healthy transition
on `mcp__kilnctrl__safety_get_status`.
**Pass (fault line):** a fault forced on the DUT's `Fault` GPIO shows up in
`kilnsim io fault-line`'s reading.
**Pass (DUT power):** each `--domain` cycle reboots only *that* domain's
downstream load and its telemetry shows the gap for that domain — run the two
commands **separately**, not back-to-back as a single "power both" action,
and confirm each one leaves the other domain's supply undisturbed. Two
independent relays exist (`docs/HARDWARE.md` §3.7, resolved 2026-08-20:
`EXP1_PIN_DUT_POWER_MAIN`/`EXP1_PIN_DUT_POWER_SAFETY`, wire commands
`DUT_POWER_SET`/`GET` (main, `0x06`/`0x08`) and `DUT_POWER_SAFETY_SET`/`GET`
(safety, `0x09`/`0x0A`)) precisely so `GND_Main` and `GND_Safty` never get
bonded through a shared control path — there is deliberately **no
combined "power both" command** anywhere in `kilnsim` (CLI, GUI, or MCP), so
do not "fix" that by scripting the two commands together into one bench
step; note in your bench log (section 6) which domain(s) you actually
exercised.

### Step 10 — First closed-loop firing attempt

```powershell
kilnsim --port COMx run firmware\SimFW\scenarios\baseline_firing.yaml --report out.json
```
Exit code: 0 = PASS, 1 = FAIL, 2 = BLOCKED (per `cli.py`'s documented
convention — a BLOCKED-only result, if it happens, is not automatically a
fixture failure; see section 2). **Do not assume this run must report
BLOCKED/FAIL** the way an earlier revision of this document did — most guard
inputs are now populated and K4 *can* energize once a
`SAFETY_CMD_REQUEST_ENABLE` is actually sent and no guard is tripped. What
is still genuinely open: `kilnsim`'s own CLI/scenario runner does not
appear to send that enable request on its own (checked this pass — grep
found no `request_enable` call in `tools/PcTools/src/kilnsim/`), so unless
something else in the loop sends it, K4 can still stay open through a
`kilnsim run` and gate a closed-loop firing exactly the old way, just for a
different, narrower reason (a missing enable call, not a missing wiring
path). Confirm whether an enable request needs to be issued separately
(e.g. `mcp__kilnctrl__safety_request_enable`) before or during the run, and
record which is actually true on this bench session — this is exactly the
kind of claim this document has gotten wrong before and needs verifying
against real behavior, not re-asserted from an old note. A BLOCKED/FAIL
result that traces cleanly back to section 2's table (inspect
`out.json`'s `expectations[].reason` and raw `events` list — the `virtual_dut/
README.md`'s "Finding 2" explains exactly this failure signature: a `forbid`
clause reporting satisfied "for the wrong reason" because K4 was already
open before the run started) is a **successful bench session**, not a
failed one. Read `firmware/SimFW/tools/virtual_dut/results/
SCENARIO_RESULTS.md` beforehand to know what shape of result to expect.

---

## 5. Troubleshooting

### Fixture Pico symptoms (USB CDC / protocol layer)

| Symptom | Likely cause |
|---|---|
| `kilnsim state` times out / no port found | Wrong `--port`; VID:PID autodetect picked another RP2040 device (§1.2's caution) — pass `--port COMx` explicitly. Or the fixture Pico never actually flashed — reflash and power-cycle. |
| Connects but every command errors | `benchproto` framing/CRC mismatch between `kilnsim`'s Python codec and the firmware build — confirm both are from the same commit; `virtual_simfw/README.md` notes the codec is proven byte-identical against `CommonFW`'s C library's shared test vectors, so a real mismatch here means a stale build on one side. |

### I2C / expander symptoms

| Symptom | Likely cause |
|---|---|
| `io write`/`io read` errors on both expanders | I2C0 SDA/SCL not wired, or one/both MCP23017s not powered — check `docs/HARDWARE.md` §1 pin map (GPIO4/5) and §3.7's device table. |
| One expander (0x20 or 0x21) responds, the other doesn't | Address strapping wrong on the non-responding part, or that device simply isn't populated yet (0x21 is spare-only per §3.7 — it may not be a priority to stuff first). |

### SPI (M-A) symptoms — see `tools/spi_test_master/README.md`'s own table for the authoritative version; summarized:

| Symptom | Likely cause (DESIGN_NOTES.md §3.2.1 reference) |
|---|---|
| All reads `0xFF` / slave never responds | MISO not tri-stating/driving correctly, CS wiring idle-low instead of idle-high, or GND not shared between the two Picos |
| All reads `0x00` | MISO stuck low, or the RX program's CS `jmp_pin` mapping wrong |
| First byte of a multi-byte read wrong, later bytes right | Classic first-byte-late / TX-FIFO-underrun — the 1.6 µs budget in §3.2.1 is being missed at this rate; `suspected_first_byte_late` in `SOAK`/`STATS` output is exactly this signature |
| Passes at low rates, fails above some N MHz | Same family; the sweep finds N — compare against the 1.6 µs budget's implied ceiling |
| Garbage specifically after a *different* CS was active previously (bus A only) | MISO tri-state contention between channels — check `cs_gpio[0..2]` really are 3 consecutive GPIOs |
| `SEQ wrrd` always fails even at very low rates | RX program not reaching write-data bytes, or `spi_emu_a/b`'s write-back task code isn't wired up |

### CT path symptoms

| Symptom | Likely cause |
|---|---|
| No waveform at all | PWM GPIO not wired, RC filter component missing/wrong values, or `wave_owner` mode is still MODEL (not MANUAL) and the thermal model has no simulated current on that zone yet — set `ct mode <ch> manual` first |
| Waveform present but wildly wrong amplitude | Expected — the shipped calibration table is deliberately all-uncalibrated (identity behavior), not a fixture fault; do not chase this as a bug this session |
| Waveform visible on the fixture side but nothing at the safety board's ADC | Transformer not yet built (ratio decided as **1:1**, `docs/DESIGN_NOTES.md` §3.3, `docs/PLAN.md` §11 item 2, but the physical coupling network has not been built — still open), or the burden resistor question — **R72/R78/R84 on the real safety board are DNP by design** (`SaftyFW/docs/CURRENT_SENSE.md` §2: "self-burdened, voltage-output CT" expected); if the fixture's transformer secondary presents as current-output instead, the safety board's clamp diodes (D12/D13) will conduct and saturate the reading regardless of what's commanded |

### K4 / relay-sense-wetting-circuit symptoms

| Symptom | Likely cause |
|---|---|
| Safety-side SPI (bus B) dead but bus A works fine | Bus B wiring fault — as of 2026-08-23 there is no digital isolator in this path (`docs/DESIGN_NOTES.md` §3.5), so check the direct GPIO connections and series resistors instead |
| K4 relay sense never shows closed even after a `SAFETY_CMD_REQUEST_ENABLE` with no guard tripped | Now a real fixture/DUT problem, not expected (section 2 — the enable path exists and is wired end to end). First confirm the request was actually sent and no guard is latched tripped; if both check out, treat this as a real wetting-circuit or relay_owner fault, and confirm K1/K2/K3/K5 sense correctly (they should) as a baseline |
| K1/K2/K3/K5 relay sense never shows closed either | Now a real fixture problem — check the wetting-circuit voltage source + resistor per contact (`docs/HARDWARE.md` §5, "not sized" — verify it was actually built to a sane value) |

### I/O-expander-driven E-stop / DUT-power symptoms

| Symptom | Likely cause |
|---|---|
| E-stop reads STOP no matter what `kilnsim estop` commands | `configure_exp1()`'s idle state (de-asserted/low at boot) may present as *open* (STOP) rather than *closed* (healthy) to GPIO9 — `docs/HARDWARE.md` §3.6 flags this as something to confirm at bring-up, not assumed either way |
| Commanding one `--domain` power-cycles both J18 and J19 together | Two-relay wiring regression (`docs/HARDWARE.md` §3.7) — the domains must be electrically independent downstream of the relays; a shared feed reintroduces the `GND_Main`/`GND_Safty` bonding the fixture exists to prevent, so treat this as a hardware fault, not expected behavior |

---

## 6. What to record after the session

This repo is scrupulous about **planned → built → host-tested →
hardware-verified** as four distinct, non-interchangeable levels
(`ROADMAP.md`'s "What 'done' means" section). Update these files with real
evidence, not just "tried it":

1. **`ROADMAP.md`, M9 section.** Add a dated "Bench state" note in the same
   style as the existing M0 entries (e.g. `ROADMAP.md` line ~172's
   "Bench state (2026-08-19)" block): what's now Working / Broken-absent,
   citing today's date. If M-A's Saleae capture succeeded, flip its
   checkbox from `[ ]` to `[x]` in the M9 milestone bullet list and name the
   capture file/location.
2. **`firmware/SimFW/docs/PLAN.md` §10, the milestone table.** Each
   milestone (M-A through M-H) currently reads "Status: NOT MET" or
   "PARTIALLY MET" for its hardware half. Update the specific milestone(s)
   this session actually touched — e.g. if M-A's Saleae capture passed at
   4 MHz (the current cap, DESIGN_NOTES.md §3.2.1) with zero underruns over ≥10k
   transactions, M-A's status changes from NOT MET to MET, with the capture
   as cited evidence. Do not mark a milestone met on the strength of the
   sweep table alone — the exit criterion is the capture.
3. **`firmware/SimFW/docs/HARDWARE.md` §0 and its bring-up checklist §6.**
   Check off whichever bring-up steps passed. If step 5's J7 pin-1
   continuity check resolved the KilnFW-vs-SaftyFW contradiction, update §0
   item 5 with the measured answer (which doc was right) — this removes an
   open item, not just records a data point. If the J18/J19 dual-feed
   question got resolved (physically wired together, or decided to leave
   single-domain for now), update §0 item 6 and §3.7 accordingly.
4. **`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §6**, if any guard's
   reachability changed (it shouldn't, this session, since nothing in
   `SaftyFW`'s source is expected to change from a bench session alone —
   but if Phase 6/7 work landed concurrently and you re-ran scenarios
   post-landing, update the "Reachable today?" column per that section's own
   instruction to re-run `virtual_dut` and update in the same change).
5. **Any new scenario report JSON** (`--report out.json` from step 10 or any
   `kilnsim run`) belongs in `tools/PcTools/logs/kilnsim/reports/` per
   `docs/DESIGN_NOTES.md` §8.2's stated convention — this is the CI-archivable
   artifact, don't let it evaporate as a stray file in your working
   directory.
6. If this session found a **new** contradiction, wiring gap, or open
   question not already tracked, add it to `docs/PLAN.md` §11 (Open
   questions) with the same "unresolved, here's what's at stake" framing the
   existing entries use, not just left as a comment in this runbook.

---

## 7. Realistic session plan

**M-A alone (the sweep plus a Saleae capture at the capped 4 MHz rate) is
real, non-trivial bench time** — flashing two boards, wiring a 6-wire+GND
harness correctly, running a 10,000-transaction sweep across the tool's rate
points, then re-running at 4 MHz specifically (the real masters' hard cap,
`docs/DESIGN_NOTES.md` §3.2.1) while a Saleae capture is armed, then reviewing that
capture. Budget most of a first session for this alone if it's attempted at
all.

**Achievable in a first sitting**, roughly in priority order:
1. Steps 1–2 (Pico alone, expanders) — quick, low-risk, get the fixture
   Pico's basic health confirmed.
2. Step 3 (M-A) at least through the sweep table up to the 4 MHz cap — the
   single highest-value thing this session can produce, since it retires
   (or characterizes) the project's single biggest named risk
   (`docs/PLAN.md` §15's top row).
3. The Saleae capture at 4 MHz, if the sweep passes and there's time left.
4. Step 5's J7 pin-1 check, **even if no fixture harness exists yet to build
   on** — this can be done directly against the real main board with just a
   multimeter, and resolving the J7 pin-1 contradiction on paper
   (`docs/HARDWARE.md` §0 item 5) unblocks safely wiring the J7 harness
   whenever that hardware does get built.

**Defer to a later session:**
- Steps 6–10 (anything touching the real DUT) — these depend on fixture
  hardware existing beyond a bare Pico + jumper harness (CT transformers,
  relay-sense wetting circuits, none of which are confirmed built — §1.1).
  Attempting them before M-A is proven risks the real board for no
  proportionate benefit.
- CT calibration (M-D) — explicitly gated on hardware existing to calibrate
  against; the calibration *mechanism* is already in firmware, only the
  bench measurement and table generation remain.
- Any scenario run expecting a genuine PASS rather than a documented
  BLOCKED/FAIL (step 10) — Phase 6/7 guard wiring has since landed, so this
  is no longer gated on `SaftyFW` guard reachability the way it used to be;
  see step 10's own note on the one thing (an explicit
  `SAFETY_CMD_REQUEST_ENABLE`) that may still be needed and unverified.

**If only one thing gets done this session, make it M-A's Saleae capture.**
Every later step in this runbook, and every hardware-trip row in
`GUARD_TEST_MATRIX.md` §3, depends on the PIO SPI slave actually working —
nothing else is worth risking the real board over until that's proven.
