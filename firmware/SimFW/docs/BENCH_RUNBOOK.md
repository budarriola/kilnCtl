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
| CT transformer ratio, **1:1** (step 4/§5) | **Higher confidence than the earlier ~3:1 plan, but still not measured.** 1.06 V rms clears the board's ≈1 Vrms full-scale sense input with margin (a board property, independent of whichever CT is installed) even if the ~1.5 Vpk usable-Pico-drive estimate behind it is off; the ratio itself is now **confirmed 1:1** against the candidate part's own datasheet (2026-08-24) — but that same datasheet **disqualifies the Triad TY-300P for this role** (`Frequency Range: 300 to 3500 Hz`), so no part is currently selected. Do not populate a CT transformer this session. | `docs/BOM.md` §9 item 1, `docs/DESIGN_NOTES.md` §3.3, `docs/HARDWARE.md` §5 |
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
  `SaftyFW`/`KilnFW` already use, and still the only flashing path for
  `spi_test_master` (see below). `SimFW` itself no longer needs it for a
  first flash: `cmake --build` now produces a verified `build/SimFW.uf2` via
  `firmware/SimFW/tools/elf2uf2.py` (no picotool, no host C/C++ compiler —
  see that script's docstring), so BOOTSEL drag-and-drop works for the
  fixture Pico directly. Bring the Debug Probe anyway; it's still the only
  option for `spi_test_master`, and useful if SWD-level debugging (not just
  flashing) is ever needed on the fixture Pico too.
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

`spi_test_master` has no `.uf2` (its own `CMakeLists.txt` still disables
`pico_add_extra_outputs()` for the same picotool-gap reason `SimFW`'s used
to) — flash it over SWD/OpenOCD, per the
[**"Use OpenOCD for ESP32 flashing"**]-style convention this repo follows for
bench programming.

`SimFW` itself now has a real `.uf2` at `build/SimFW.uf2`, generated and
verified automatically by the `cmake --build` step above
(`tools/elf2uf2.py`, no picotool needed). For a first flash of a bare Pico
in BOOTSEL mode, drag that file onto the `RPI-RP2` mass-storage drive it
enumerates as. SWD/OpenOCD still works too, if the Debug Probe is already
wired up for other reasons.

Don't have both Picos attached to the same Debug Probe simultaneously
unless you know your SWD wiring supports it.

**Reflashing `SimFW` after the first flash — three routes, not two:**
Besides BOOTSEL drag-and-drop with the `.uf2` above and SWD/OpenOCD, a
fixture that is already running `SimFW` and reachable over its own USB CDC
port can be told to drop into its ROM bootloader over that same port,
without touching the board:

```powershell
kilnsim reboot-bootloader --yes
```

(or the `sim_reboot_bootloader` MCP tool, or any tool that speaks the
1200-baud "set line coding to 1200 baud with DTR deasserted" touch
convention against the fixture's port — see `docs/PROTOCOL.md`'s
`REBOOT_BOOTLOADER` section for the full spec). The fixture only reboots
after confirming on its own that the E-stop loop is open, both DUT power
relays are off, and the CT outputs are silent (`src/tasks/safe_reboot.c`) —
it refuses (reporting an error, staying on the current firmware) rather than
reboot into an unconfirmed state if that check times out. Once it drops into
the bootloader it enumerates as a `RPI-RP2` mass-storage device the same way
a physical BOOTSEL boot would; drag a `.uf2` onto it or use `picotool` as
usual. This route only exists for `SimFW` itself — `spi_test_master` has no
such command, so SWD/OpenOCD or physical BOOTSEL are still its only options.

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

### 1.3 Boot-stage LED beacon — diagnosing a dead board with no debug probe

**Added after a real bench boot failure** (first-ever flash to a bare Pico:
dark LED, no USB enumeration at all) turned out to be a `telemetry` task
stack overflow with zero visible signal — `vApplicationStackOverflowHook()`
used to just disable interrupts and spin, which is indistinguishable at the
bench from "board is fine, just hasn't lit anything yet." The fixture now
drives the onboard LED (GPIO25, no header pin) at every major boot stage and
with a distinct post-boot heartbeat, specifically so the *next* dead-board
symptom is diagnosable by eye alone, with nothing attached but USB power.

**2026-08-23, second pass:** a SECOND real dead-board failure showed a
fixture that was completely dark from power-on — not even stage 1 of the
table below (at the time, stage 1 was `spi_emu_a_start`, well into `main()`).
That is too coarse to tell "crashed before `main()` even started" apart from
"crashed somewhere in `main()`'s first few lines, before the first beacon
call." The stage count was renumbered 1→14 with four new checkpoints ahead
of the first `_start()` call — group 1 is now the literal first two
statements `main()` executes, before `stdio_init_all()` — and the flash
encoding changed from a straight tally (unreadable much past ~5) to a
base-5 long/short odometer so stage 14 is still countable at a glance.

**How to read it:** each stage number is encoded as `floor(N/5)` LONG
flashes, then (only if the remainder is non-zero) a medium gap and
`N mod 5` SHORT flashes. Recover N as `5*(long count) + (short count)`. A
long dark gap separates one stage's whole number from the next.

| Flash shape | Meaning |
|---|---|
| LONG flash: 400 ms on / 250 ms off | Counts a group of five. |
| SHORT flash: 120 ms on / 180 ms off | Counts one (used for the 0-4 remainder). |
| ~500 ms medium gap | Separates the long sub-group from the short sub-group (only present when BOTH are non-empty). |
| ~1.2 s dark gap | Separates one stage number from the next. |

| Longs | Shorts | Stage N | Checkpoint reached |
|---|---|---|---|
| 0 | 1 | **1** | Literal first statement of `main()` — before `stdio_init_all()`. If this never appears, the fault is before `main()` (crt0/pico-sdk runtime_init, boot ROM, or the flash image) — instrumenting `main()` further cannot see it; reach for a debug probe. |
| 0 | 2 | **2** | `stdio_init_all()` returned. (The proven-good `tools/boot_probe/blink_uart` control image calls this exact same function, same UART pins/baud, and blinks fine standalone — so 1-but-not-2 means something SimFW links in changes this call's behavior, not that the call is broken in general.) |
| 0 | 3 | **3** | `simfw_fatal_install_cross_core_halt()` returned (SIO_IRQ_PROC0 handler installed). |
| 0 | 4 | **4** | Beacon GPIO re-init done, last checkpoint before the first `_start()` call. 4-but-not-5 means the fault is inside `spi_emu_a_start()`. |
| 1 | 0 | **5** | `spi_emu_a_start()` |
| 1 | 1 | **6** | `spi_emu_b_start()` |
| 1 | 2 | **7** | `wave_owner_start()` |
| 1 | 3 | **8** | `sim_engine_start()` |
| 1 | 4 | **9** | `usb_owner_start()` |
| 2 | 0 | **10** | `cmd_task_start()` |
| 2 | 1 | **11** | `fault_sched_start()` |
| 2 | 2 | **12** | `i2c_owner_start()` |
| 2 | 3 | **13** | `telemetry_start()` |
| 2 | 4 | **14** | `log_task_start()` — every task started. |

| What you see | Meaning |
|---|---|
| **Stage numbers count up to some N, then nothing (dark) forever, never reaching 14** | The fixture crashed or hard-faulted *inside* the checkpoint after N — before that call/statement completed, and before anything called `simfw_fatal()`. This is the "true blank" case a debug probe would normally be needed for; report the last completed stage number from the table above. |
| **1→14 appears once, then the WHOLE 1→14 sequence repeats two more times (three passes total), then the LED goes dark and only the heartbeat follows** | Full boot success. The three-pass replay (LED-only, no repeated side-effecting calls) is `main()`'s own recount aid before it calls `vTaskStartScheduler()` — no fresh power cycle needed to double check the count. If you see fewer than 3 full replays before darkness, `vTaskStartScheduler()` itself failed (out of heap for the idle/timer tasks) or the scheduler never reached the idle task; treat as a real bug. |
| **A brief (100 ms) single flash, repeating steadily every ~2 s, forever** | Healthy steady state: the scheduler is running and core 0's idle task is getting CPU time normally (`vApplicationIdleHook()`). This is deliberately a single infrequent blink, not a cluster, so it can never be mistaken for a boot-stage group. |
| **10 fast flashes (100 ms on/100 ms off) back-to-back, then SOLID ON forever** | `simfw_fatal()` fired — a real, named resource-claim failure or (as of this pass) a caught `vApplicationStackOverflowHook()`/`vApplicationMallocFailedHook()` event. This can happen at any point, mid-stage or post-boot; SWD (if a probe is available) will show `panic()`'s formatted message with the subsystem/reason. If NOT attached, at minimum you know the kernel caught something *and named it* — very different from silent corruption. |
| **Completely dark, no flashes at all, from power-on** | The fixture never reached `main()`'s literal first two statements (stage 1) at all — boot ROM/stage-2 bootloader failure, bad flash, or a hardware fault before `main()` is ever entered. This is the one case the beacon cannot help diagnose (nothing has run yet); reflash and reseat first, then reach for a debug probe. |

**Do not confuse:** boot-stage groups (long/short odometer encoding above,
~1.2 s dark gap between stage numbers) vs. the heartbeat (single 100 ms
flash, ~2 s of dark between blinks) vs. `simfw_fatal()`'s signature (always
exactly 10 fast 100/100 ms flashes, then permanently solid) — three
different cadences by design, per `src/main.c`'s
`simfw_boot_beacon()`/`vApplicationIdleHook()` and
`src/drivers/simfw_fatal.c`'s `simfw_fatal()`.

**If a stack overflow is what you're chasing:** `firmware/SimFW/build` has
`-fstack-usage` available as a one-off `target_compile_options()` addition in
`CMakeLists.txt` (not left on by default — it emits a `.su` file per
translation unit next to each `.obj`, listing every function's own stack
frame size). Cross-reference against each task's `*_STACK_WORDS` define
(`configMINIMAL_STACK_SIZE` is 1024 bytes; most tasks use a small multiple of
that) rather than guessing from source alone — this is exactly how the real
`telemetry` overflow above was found and confirmed, not eyeballed.

### 1.4 Two Debug Probes on this bench — which one is which, verified 2026-08-23

**There are now two Raspberry Pi Debug Probes on the bench, and they are the
same model.** They are **indistinguishable by VID/PID**: both enumerate as
`VID_2E8A`/`PID_000C`, a USB composite device with `MI_00` = "CMSIS-DAP v2
Interface" (SWD) and `MI_01` = a USB Serial Device (COM port). The only thing
that tells them apart is the adapter serial number burned into each unit.

| Probe serial | Wired to | Must never be used for |
|---|---|---|
| `E66540F0A36C6E21` | `SaftyFW` safety processor (RP2040) | `SimFW` |
| `E66540F0A38EA628` | `SimFW` bench fixture Pico (RP2040), along with that Pico's UART | `SaftyFW` |

**Both targets are RP2040.** An OpenOCD session that connects successfully
proves nothing about which board it reached — the target type cannot tell
them apart, only the adapter serial can. COM port numbers (currently COM10
for the safety probe, COM11 for the SimFW probe) are assigned by Windows and
can change between sessions, so they are **not** a safe identifier either.

`SimFW`'s own USB identity (so a flashed fixture is never confused with a
probe): running, it is `VID_2E8A`/`PID_F00A` (§7's table in
`firmware/SimFW/docs/HARDWARE.md`); in BOOTSEL it is `PID_0003` and mounts as
a drive with `INFO_UF2.TXT` Board-ID `RPI-RP2`. **`PID_000C` is a Debug
Probe, not `SimFW`.** On 2026-08-23 a flashed board was misidentified on this
bench because only the VID was checked — `2E8A` covers the Pico, the probe,
and the bootloader alike, so the VID alone never distinguishes them.

**Tooling limitation, confirmed on this bench 2026-08-23:** the only OpenOCD
installed here is the ESP32 variant at
`~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260424/openocd-esp32/bin/openocd.exe`.
It ships `interface/cmsis-dap.cfg` and `target/rp2040.cfg`, but it **cannot
select these probes by serial** — it cannot read their USB string
descriptors at all. Tried `adapter serial <serial>`, the legacy
`cmsis_dap_serial <serial>`, and `cmsis-dap backend usb_bulk`; all three fail
identically:
```
Warn : could not read serial number for device 0x2e8a:0x000c: Pipe error
Error: unable to find a matching CMSIS-DAP device
```

**Do not work around this by removing the serial filter.** Unfiltered,
OpenOCD picks a probe on its own and could halt or flash the **safety
processor** instead of the fixture (or vice versa). The correct responses are
(a) install an upstream/Raspberry Pi OpenOCD build, or `picotool`, that can
actually read these probes' serials, or (b) use BOOTSEL drag-and-drop, which
needs no probe at all — this is the proven working route for `SimFW`,
verified 2026-08-23: hold BOOTSEL while plugging in, the drive mounts as
`RPI-RP2`, copy `firmware/SimFW/build/SimFW.uf2` onto it.

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

Start with the bus scan — it answers "are they there, and at the addresses the
firmware expects?" in one round trip, and is the fastest way to spot a
strapping mismatch:

```powershell
kilnsim --port COMx io scan
# configured 0x25/0x26, found 0x25/0x26 -- OK
```

Then the round-trip check. **Note the direction step:** a generic pin boots as
INPUT with pull-up (the safe non-driving default), so a bare `io write` only
updates the output latch and the pin keeps reading high. Set it to output
first, or the round trip will look broken when it is not:

```powershell
kilnsim --port COMx io dir 8 out --exp 0     # J20 IO_3 spare pin, EXP1_PIN_J20_IO3
kilnsim --port COMx io write 8 on --exp 0
kilnsim --port COMx io read 8 --exp 0
kilnsim --port COMx io dir 8 in --exp 0 --pullup   # restore the safe default
```

**Pass:** `io scan` reports `-- OK`, and the spare pin round-trips the value
just written.

**Addresses are bench-specific.** As of 2026-08-24 the two units on this bench
are strapped to **0x25 and 0x26**, not the datasheet POR default 0x20/0x21;
`i2c_owner.c`'s `MCP23017_ADDR_1`/`_2` match. If `io scan` reports MISMATCH,
believe the scan and update those two defines — do not assume 0x20/0x21.
`HAEN` (the address-pin enable bit) is MCP23S17-only; on the I2C MCP23017 the
address pins are always enabled, so any strap combination is legitimate.

**Prefer `kilnsim selftest`** over doing this by hand — it now covers both
expanders, multiple pins, pin independence, pull-up behaviour and the
reserved-pin interlock, and restores every pin it touches.
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
**The 1:1 ratio is now confirmed — and the candidate part is not.**
Checked 2026-08-24 against the Triad TY-300P datasheet itself
(`hardware/datasheets/SimFW_TY300P/TY-300P.pdf`): the part is 600 Ω primary
into two independent 600 Ω secondaries, so **1:1 per secondary is correct**,
settling the ratio question this runbook previously left open. The drive
level is fine too — the datasheet's `+7 dBm` maximum is ≈1.73 Vrms into
600 Ω, comfortably above the board's ≈1 Vrms full-scale sense input, so the
~1.5 Vpk usable-Pico-drive estimate never needed to be exact.

**But the same datasheet rules the part out of this role:** `Frequency
Range: 300 to 3500 Hz`, and the CT waveform's fundamental is 60 Hz — a
factor of 5 below the specified low-frequency limit. `Max. DC Current` is
`Pri 0 mA` besides, which a directly driven winding cannot honour without a
DC-blocking capacitor that worsens the same high-pass problem. See
`docs/BOM.md` §9 item 1 for the full table and the datasheet acceptance test
a replacement has to pass.

**Practical consequence for this step: there is no CT transformer to
populate.** Do not fit a TY-300P "to see if it works" — at 60 Hz its primary
presents a few hundred ohms, which loads the driver down and produces a
small, distorted waveform that looks like a *drive* or *calibration* fault
rather than a wrong part. Judge this step on the fixture's own filtered
output at the RC network instead, upstream of where the transformer would
go, and leave the isolated side unpopulated until a 50/60 Hz-rated part is
chosen.
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

**For a single scenario, use `kilnsim run` as below. For "confirm a whole
firmware/hardware update didn't break anything" (repeated bench sessions,
not a first bring-up), use `kilnsim testmgr` / `kilnsim testmgr --quick`
instead — it detects which hardware tier is actually attached, runs every
scenario that tier supports, and reports a guard-coverage table against
`GUARD_TEST_MATRIX.md`'s S1..S13. See `docs/TEST_MANAGER.md` for the full
account; it has never been run against real hardware as of this writing, so
treat that as untested until someone runs it here and updates that doc's
§8.**

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
| **LED completely dark, no USB enumeration at all** (2E8A:F00A never appears) | Read §1.3's boot-beacon table first — this exact symptom was a real `telemetry` task stack overflow on this bench (fixed; see git history), caught silently because the overflow hook used to spin with no signal. With no debug probe, the beacon tells you the last stage that completed; a hard dark with zero flashes from power-on instead points at the boot ROM/flash image rather than application code. |

### I2C / expander symptoms

| Symptom | Likely cause |
|---|---|
| `io write`/`io read` errors on both expanders | **Run `kilnsim io scan` first — it distinguishes these causes in one command.** If it finds nothing at all: I2C0 SDA/SCL not wired, or the MCP23017s unpowered — check `docs/HARDWARE.md` §1 pin map (GPIO4/5) and §3.7's device table. If it finds addresses that are not the configured pair, the parts are strapped differently — update `MCP23017_ADDR_1`/`_2` in `i2c_owner.c` to what the scan reports. That exact mismatch (0x25/0x26 vs an assumed 0x20/0x21) cost about an hour of SWD debugging before this command existed. |
| `IO/3: ERR_NO_SAMPLE` on a read | The arguments were fine, but no MCP23017 has ever ACKed on this bus, so there is no sample to return. Distinct from `ERR_BAD_ARGS` (which means the pin is reserved or out of range and retrying will never help) and from `ERR_BUSY` (the command queue was transiently full — retry). Run `io scan`. |
| A pin always reads high no matter what you write | Expected if the pin is still INPUT: `io write` only sets the output latch, and an input pin with its pull-up on reads high regardless. `io dir <pin> out` first. |
| A write appears not to take effect, then does a moment later | Not a fault. `io dir`/`io write` are queued and land on `i2c_owner`'s next 8 ms scan tick, while `io read` returns a snapshot of the last completed tick. A read issued immediately after a write can observe the pre-write value — poll rather than reading once. |
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
