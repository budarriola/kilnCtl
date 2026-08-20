# SimFW — First Hardware Bench Session Runbook

> **Status:** planning — no fixture hardware has ever existed · **Last
> reviewed:** 2026-08-20
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
- **The built fixture hardware itself.** `firmware/SimFW/docs/BOM.md` does
  **not exist yet** — there is no bill of materials or built harness to pick
  up. This session's realistic ceiling is therefore the fixture Pico +
  reference-master Pico + a hand-wired handful of jumpers for M-A, not a
  populated fixture board. If a breadboard/protoboard harness has been built
  since this line was last reviewed, update this paragraph and section 7.
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
  version fields populated. **Caution:** `kilnsim`'s serial autodetect
  matches on VID:PID `2E8A:000A` — this is explicitly a **placeholder**
  (Raspberry Pi's generic example-board CDC ID, `tools/PcTools/src/kilnsim/
  link.py`'s own comment), not an ID `SimFW` has claimed for itself. If more
  than one RP2040-based CDC device is plugged in (fixture Pico,
  `spi_test_master` Pico, or even the Debug Probe itself), autodetect may
  pick the wrong port or refuse with "no SimFW-looking port found." Use
  `kilnsim --port COMx state` explicitly whenever more than one Pico is
  attached — which will be most of this session.
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
`SaftyFW`.** A software cross-check (`firmware/SimFW/tools/virtual_dut/`,
compiling `SaftyFW`'s real, unmodified `safety_guards.c`/`relay_grace.c` and
ticking them against simulated fixture data) established, and this session
independently re-verified by reading `firmware/SaftyFW/src/tasks/
safety_core.c` and `link_task.c` directly, that **only guards S5, S6b, S7,
and S12 can structurally fire in today's `SaftyFW`.** The other nine are
blocked because `safety_core_build_input()` never populates the inputs they
need — not because anything is broken on the bench.

Two consequences will hit you almost immediately once real hardware is
involved (steps 6–10 below):

| Symptom you will see | This is expected, because |
|---|---|
| **K4 (the safety pilot relay) reads open/de-energized from the moment `SaftyFW` boots, and never closes, no matter what the fixture or `KilnFW` does.** The kiln will never appear to heat under DUT control. | `relay_owner_command_energize()` (`firmware/SaftyFW/src/tasks/relay_owner.c`) is fully implemented but has **zero callers anywhere in the tree** — confirmed by grep, not just at the one obvious call site. `relay_owner_task()` starts in GRACE and, once GRACE expires, nothing ever asks for an energize. This would also be the eventual caller of a `SAFETY_CMD_REQUEST_ENABLE` decode — but `link_task.c`'s own command switch (`firmware/SaftyFW/src/tasks/link_task.c`, verified this session) has cases for `ANNOUNCE_VERSION`, `FW_VERSION`, `PUSH_CONTEXT`, `CLEAR_TRIP`, `SET_CONFIG`, `ROLLBACK`, `ANNOUNCE_REBOOT`, and the four `UPDATE_*` frames — **no case for `SAFETY_CMD_REQUEST_ENABLE`** exists, even though `KilnFW`'s side of that frame (`safety_link.c`/`safety_link.h`) already sends it. K4 cannot be energized by any path in the current source tree. |
| **Roughly 120 seconds into every `SaftyFW` boot, S6b (LINK_DEAD) trips — even with a perfectly healthy link, even with nothing else happening.** | `safety_core_build_input()` never sets `link_up` true (no field for it in its struct literal — verified directly, line ~146–156 of `safety_core.c`). `safety_core.c` itself carries a standing `// TODO (Phase 7): context_snapshot_t is read here too, once link_task publishes one`. `link_task.c` is substantially built (context frames, DIAG, STATUS, TRIP_EVENT all exist) — the gap is narrower than "no link task": `safety_core` simply never asks it whether the link is alive. The elapsed-silence timer therefore accumulates from t=0 of every boot and trips the 120 s hard backstop (`link_dead_hard_s` default) unconditionally. |
| Most other guards (S1–S4, S9, S10, S11, S13) never trip or warn no matter what fault you inject through the fixture. | S2/S3/S4/S10/S13 are gated on `in->context_valid`, also never set true (same Phase 7 gap). S9 needs `in->relay_deenergized`, never computed. S11 needs `in->heat_commanded`, hardcoded `false` ("no current sense yet, Phase 6" — `safety_core.c`'s own comment). S1 needs `cfg->abs_max_temp_c` commissioned; it defaults to 0, and 0 means "not commissioned, never trip" by deliberate convention (`safety_guards.h`), not a bug — this one's a config gap, not a missing producer. S6a needs `in->main_fault_asserted`; the debounced reading already exists (`discrete_task_main_fault()`) and is simply never called from `safety_core_build_input()` — a one-line wiring omission, not a missing phase. |
| S5, S6b (once past 120 s), S7, and S12 *do* react correctly to fixture-injected faults. | These are the four guards whose inputs `safety_core_build_input()` actually populates today: `tc_c`/`cj_c`/`fault_bits`/`spi_failed` (unconditional, from `thermo_task`) and `estop_pressed` (unconditional, from `discrete_task_estop_pressed()`). Use these four to validate that the fixture→DUT signal path itself works, since they are the only guards that can currently confirm it. |

**None of the above is a fixture bug, a wiring mistake, or something this
session can fix by re-checking connections.** It is `SaftyFW`'s own Phase
6/7 incompleteness (current-sense and link-context wiring), already flagged
in `SaftyFW/TODO.md` and `SAFETY_MODEL.md`, now quantified per-guard by
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §6. Full detail and the
guard-by-guard reachability table live there and in `docs/PLAN.md`'s status
header — read those if a specific guard's non-behavior needs explaining to
someone else.

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
- **ABORT** — anything ambiguous about ground-domain continuity (step 5),
  anything that suggests the real board saw an out-of-spec signal (5 V where
  3.3 V was expected, a connector plugged in reversed), or any smoke/smell/
  unexpected heat. Safe abort action: **power off the fixture and the main
  board immediately** (bench supply off, not just USB unplugged — the main
  board's 12 V feed is a separate rail from USB), disconnect the harness
  between fixture and main board, and do not reconnect until the suspected
  cause is understood. A wrong result on step 5 or step 6 is the one place
  in this whole procedure with real hardware-damage risk (`docs/PLAN.md`
  §15's risk table: "Ground strap through the fixture defeats isolation" and
  "J6 pinout traced wrong" are both flagged Impact: possible damage).

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

**Pass (table-only, not yet M-A's full exit criterion):** every rate point
in the sweep table reports PASS, zero mismatches, zero suspected
first-byte-late events, up through 5 MHz.
**Pass (M-A's actual, literal exit criterion):** the above, *plus* a Saleae
capture of the 5 MHz run showing clean mode-1 framing and no MISO-tri-state
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
says (PLAN.md §3.2.1: "a test run with nonzero underruns is flagged invalid
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
calibration is an explicit `TODO(M-D calibration)` IDENTITY placeholder in
`wave_owner.c` today** — "5.0 A commanded" does not yet mean 5 A as any real
ADC would measure it; judge this step on waveform cleanliness (frequency,
shape, no carrier ripple bleeding through), not on absolute calibrated
amplitude.
**NO-GO:** see §5's CT row.

### Step 5 — GROUND-DOMAIN CHECK BEFORE FIRST DUT CONTACT

**Do not skip. Do not reorder. This is the step that protects the real
board.**

With the bring-up jumper (if one is fitted) **OUT**:

1. With a multimeter in continuity mode, probe fixture `GND_Main` against
   fixture `GND_Safty`. **Pass: no continuity (open circuit).** Any
   continuity here means the fixture has become an unintended ground strap
   between the main board's two isolated domains — **ABORT** per section 3,
   do not connect to the real board until this is resolved.
2. Verify isolator orientation (the 6-channel digital isolator carrying SPI
   bus B + `DRDY_SAFETY`/`FAULT_SAFETY`) and CT transformer orientation
   against `docs/HARDWARE.md` §4's isolation boundary map, if that hardware
   is populated this session.
3. **Resolve the J7 pin-1 contradiction before wiring the isolated-side
   supply.** `firmware/KilnFW/docs/HARDWARE.md` says J7 pin 1 is "no
   connect"; `firmware/SaftyFW/docs/HARDWARE.md` §8 says the same physical
   pin is `3.3v_Safty` (via R51, 0 Ω). `docs/HARDWARE.md` §0 item 5 follows
   the `SaftyFW` doc as the more recently reviewed source, **but explicitly
   flags this as unverified**. With the real main board powered but nothing
   else connected, probe J7 pin 1 with a multimeter (continuity to the
   3.3 V safety rail, or DC voltage if the rail is live) to determine which
   doc is actually correct on this board revision, and record the answer in
   `docs/HARDWARE.md` §0 item 5 in the same session. Getting this wrong
   before wiring the isolated-side supply means either back-feeding an
   unintended 3.3 V rail into a "no connect" pin, or leaving the isolator
   side unpowered.

**Pass:** step 1's continuity check reads open, and the J7 pin-1 question is
resolved (measured, not assumed) before any isolated-domain wire is landed.
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
getting this backwards risks driving 5 V into a clock line. Consider fixture
bus-A series resistors for this first plug-in, per `docs/PLAN.md` §15's
stated mitigation.

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
**NO-GO:** see §5's DUT-thermocouple row. **Remember section 2's table**
before concluding anything is broken — a lack of *heating* here is expected
(K4 never energizes, relays are never commanded by a PID with nothing to
regulate toward yet); a lack of *temperature readings at all* is not
expected and is a real fixture-path problem.

### Step 7 — Safety path (J7, isolated)

J7 mates **straight, pin-for-pin** to the safety daughterboard's J1 — no
reversal, unlike J6 (`docs/HARDWARE.md` §3.2). Wire per that table, having
already resolved the pin-1 question in step 5.

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
shows the expected fault reaction; S6b will also be counting down in the
background from the moment `SaftyFW` booted, regardless of anything you do
here (section 2).

### Step 8 — Relay sense

```powershell
kilnsim --port COMx relay states
```
Command relays via the existing DUT-side MCP tool
(`mcp__kilnctrl__io_set_relay`), then re-poll:
```powershell
kilnsim --port COMx relay edges --since-seq 0
```
**Pass:** commanding K1/K2/K3/K5/K4 produces a matching edge in the
fixture's relay-edge log within one debounce window (~24 ms worst case, per
`mcp23017.h`). **K4 specifically will never show a commanded-closed edge**
regardless of what you do here — section 2's table, `relay_owner_command_
energize()` has no caller. Confirm K1/K2/K3/K5 edges track real commands;
treat K4 staying open as expected, not a fixture defect.

### Step 9 — E-stop, fault line, DUT power relay — one at a time

```powershell
kilnsim --port COMx estop open
kilnsim --port COMx estop closed
kilnsim --port COMx io fault-line
kilnsim --port COMx power cycle --off-ms 500
```
**Pass (E-stop):** open/closed produces the expected STOP/healthy transition
on `mcp__kilnctrl__safety_get_status`.
**Pass (fault line):** a fault forced on the DUT's `Fault` GPIO shows up in
`kilnsim io fault-line`'s reading.
**Pass (DUT power):** the power cycle reboots the DUT and its telemetry
shows the gap. **First confirm the J18/J19 wiring question is resolved** —
`docs/HARDWARE.md` §0 item 6: the fixture's single MCP23017-driven relay can
only brown out one of the main board's *two* independent 12 V inputs (J18
main-domain, J19 safety-domain) unless the bench operator has deliberately
wired both from a common point downstream of that one relay. If that wiring
hasn't been done, this step only proves one domain browns out — note which
domain in your bench log (section 6).

### Step 10 — First closed-loop firing attempt

```powershell
kilnsim --port COMx run firmware\SimFW\scenarios\baseline_firing.yaml --report out.json
```
Exit code: 0 = PASS, 1 = FAIL, 2 = BLOCKED (per `cli.py`'s documented
convention — a BLOCKED-only result, expected here, is not a fixture
failure; see section 2). **Given section 2's findings, expect this run to
report BLOCKED or FAIL, not PASS** — K4 never energizes and most guards
never see their inputs, so a genuine closed-loop firing with a real PID
regulating through relay cycling almost certainly cannot happen against
today's shipping `SaftyFW`, independent of anything about the fixture. A
BLOCKED/FAIL result that traces cleanly back to section 2's table (inspect
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

| Symptom | Likely cause (PLAN.md §3.2.1 reference) |
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
| Waveform present but wildly wrong amplitude | Expected — calibration is an identity placeholder (`TODO(M-D calibration)`), not a fixture fault; do not chase this as a bug this session |
| Waveform visible on the fixture side but nothing at the safety board's ADC | Transformer not yet built/sized (`docs/PLAN.md` §11 item 2, open), or the burden resistor question — **R72/R78/R84 on the real safety board are DNP by design** (`SaftyFW/docs/CURRENT_SENSE.md` §2: "self-burdened, voltage-output CT" expected); if the fixture's transformer secondary presents as current-output instead, the safety board's clamp diodes (D12/D13) will conduct and saturate the reading regardless of what's commanded |

### Isolator / K4 / relay-sense-wetting-circuit symptoms

| Symptom | Likely cause |
|---|---|
| Safety-side SPI (bus B) dead but bus A works fine | Digital isolator not powered from J7's safety-side rail, or the 3-board→fixture/3-fixture→board channel-direction split doesn't match the specific isolator part purchased (`docs/HARDWARE.md` §3.2 flags this as unconfirmed against any specific part) |
| K4 relay sense never shows closed even when you believe you've closed it externally for a bench test | Expected per section 2 — nothing in current `SaftyFW` ever asks for an energize. This is not a wetting-circuit fault; confirm by checking whether K1/K2/K3/K5 sense correctly (they should) while only K4 stays stuck |
| K1/K2/K3/K5 relay sense never shows closed either | Now a real fixture problem — check the wetting-circuit voltage source + resistor per contact (`docs/HARDWARE.md` §5, "not sized" — verify it was actually built to a sane value) |

### I/O-expander-driven E-stop / DUT-power symptoms

| Symptom | Likely cause |
|---|---|
| E-stop reads STOP no matter what `kilnsim estop` commands | `configure_exp1()`'s idle state (de-asserted/low at boot) may present as *open* (STOP) rather than *closed* (healthy) to GPIO9 — `docs/HARDWARE.md` §3.6 flags this as something to confirm at bring-up, not assumed either way |
| DUT power cycle only browns out part of the board | J18/J19 dual-feed gap (§0 item 6) — see step 9 above |

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
   5 MHz with zero underruns over ≥10k transactions, M-A's status changes
   from NOT MET to MET, with the capture as cited evidence. Do not mark a
   milestone met on the strength of the sweep table alone — the exit
   criterion is the capture.
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
   `docs/PLAN.md` §8.2's stated convention — this is the CI-archivable
   artifact, don't let it evaporate as a stray file in your working
   directory.
6. If this session found a **new** contradiction, wiring gap, or open
   question not already tracked, add it to `docs/PLAN.md` §11 (Open
   questions) with the same "unresolved, here's what's at stake" framing the
   existing entries use, not just left as a comment in this runbook.

---

## 7. Realistic session plan

**M-A alone (the sweep to 5 MHz plus a Saleae capture) is real, non-trivial
bench time** — flashing two boards, wiring a 6-wire+GND harness correctly,
running a 10,000-transaction sweep across 7 rate points, then re-running at
5 MHz specifically while a Saleae capture is armed, then reviewing that
capture. Budget most of a first session for this alone if it's attempted at
all.

**Achievable in a first sitting**, roughly in priority order:
1. Steps 1–2 (Pico alone, expanders) — quick, low-risk, get the fixture
   Pico's basic health confirmed.
2. Step 3 (M-A) at least through the sweep table at all 7 rate points — the
   single highest-value thing this session can produce, since it retires
   (or characterizes) the project's single biggest named risk
   (`docs/PLAN.md` §15's top row).
3. The Saleae capture at 5 MHz, if the sweep passes and there's time left.
4. Step 5's ground-domain and J7 pin-1 checks, **even if no fixture harness
   exists yet to build on** — these can be done directly against the real
   main board with just a multimeter, and resolving the J7 pin-1
   contradiction on paper (`docs/HARDWARE.md` §0 item 5) unblocks safely
   wiring the isolated side whenever that hardware does get built.

**Defer to a later session:**
- Steps 6–10 (anything touching the real DUT) — these depend on fixture
  hardware existing beyond a bare Pico + jumper harness (CT transformers,
  isolators, relay-sense wetting circuits, none of which are confirmed
  built — §1.1). Attempting them before M-A is proven and the ground-domain
  check has passed risks the real board for no proportionate benefit.
- CT calibration (M-D) — explicitly gated on hardware existing to calibrate
  against.
- Any scenario run expecting a genuine PASS rather than a documented
  BLOCKED/FAIL (step 10) — not reachable until `SaftyFW` Phase 6/7 lands,
  independent of anything this bench session can do.

**If only one thing gets done this session, make it M-A's Saleae capture.**
Every later step in this runbook, and every hardware-trip row in
`GUARD_TEST_MATRIX.md` §3, depends on the PIO SPI slave actually working —
nothing else is worth risking the real board over until that's proven.
