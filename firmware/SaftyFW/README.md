# SaftyFW — kiln safety processor firmware

> **Status:** early implementation, first real-hardware boot succeeded, isolated link now proven end to end · **Last reviewed:** 2026-08-23
> **Keep this file current.** It is the entry point; if the document set or the
> headline facts change, update it in the same commit.

FreeRTOS firmware for the **Raspberry Pi Pico (RP2040)**, designator **A1** on
the kilnCtl main board, running in the isolated `GND_Safty` domain.

**2026-08-18: first flash to a real Pico.** Immediately double-faulted at
boot (`~DRDY` GPIO IRQ armed pre-`vTaskStartScheduler()` called FreeRTOS
ISR-safe APIs before the SMP port's cross-core scheduler state existed --
diagnosed via GDB against OpenOCD's gdbserver, fixed in
`src/tasks/thermo_task.c` by arming the IRQ from inside the task body
instead of from `main()`). After the fix, confirmed via GDB that the idle
task runs normally (thread mode, no exception). No CT transformers or
thermocouple ICs attached yet, so `current_task`/`thermo_task`'s real sensor
paths are still unverified against real hardware -- only scheduler bring-up
and the boot sequence itself are hardware-confirmed so far.

The pico-sdk + FreeRTOS-Kernel (SMP) CMake project, the boot sequence, the
task/priority/core-affinity shells and the isolation CI grep check exist under
`src/` and compile clean under the real arm-none-eabi-gcc/pico-sdk toolchain —
see `TODO.md` for what's built vs. still pending. This directory also
still holds the design documents below, written before implementation so the
thresholds, the wire protocol and the failure analysis could be argued about
while they were still cheap to change.

## What it does

The main controller (ESP32-S3, [`../KilnFW`](../KilnFW)) runs the kiln — PID,
profiles, ramps, web UI, its own seven-guard thermal-protection suite. It is
sophisticated, network-connected, and is where bugs live.

**This processor exists on the assumption that the main controller is wrong.**
It watches, independently:

- a thermocouple — its **own** on J7, or one **borrowed** from the main board,
  or both cross-checking each other,
- **three current-sense channels** fed by external current transformers,
- an **E-stop** input,
- the main controller's **setpoints, measured temperatures, relay commands and
  firing ceiling**, received over an isolated link,

and when something has gone badly wrong it **de-energizes K4**, which opens an
upstream mechanical line contactor and removes power from the elements — then
**checks that the current actually stopped**, and escalates loudly if it did not.

It does not control the kiln. It does not improve the firing. It notices, and
it cuts power.

Thirteen guards, of which two only ever warn and three escalate from warning to
trip.
There is deliberately **no over-current guard** — the current channels detect
*load active* and estimate power; fuses and breakers own over-current. The full
set, with thresholds and the argument for each, is in [`docs/SAFETY_MODEL.md`](docs/SAFETY_MODEL.md) §4.

## Three design commitments

**It must not cry wolf.** A safety system that trips spuriously gets bypassed —
reliably, by a reasonable person, eleven hours into a twelve-hour firing, with a
jumper. Every trip must clear two independent bars: a magnitude correct
operation cannot reach, *and* a duration a transient cannot sustain. Most
findings are warnings, not trips. See [`docs/SAFETY_MODEL.md`](docs/SAFETY_MODEL.md) §2.

**A frozen main processor must never hang it.** Nothing on the Pico ever waits
for the ESP: no ACKs, no retries, no blocking transmits. The safety core does
not include the link header; the link task cannot touch the relay; and the two
run on different RP2040 cores. The Pico still transmits — it must, so the main
board can prove it is alive — but it transmits the way a beacon does, not the
way a conversation does. See [`../CommonFW/docs/LINK_PROTOCOL.md`](../CommonFW/docs/LINK_PROTOCOL.md) §2.

**And the main board will not heat without it.** The ESP requests the safety
processor's build identity at boot, receives temperature and current telemetry
every 500 ms, and refuses every heater-on event if that stream stops for more
than 1.5 s — aborting a running firing after 30 s. An unpopulated, unprogrammed
or crashed safety processor means a kiln that will not fire
([`../CommonFW/docs/LINK_PROTOCOL.md`](../CommonFW/docs/LINK_PROTOCOL.md) §8).

## Documents

| Doc | Covers |
|---|---|
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | The traced board: Pico pin map, K4 and the contactor interlock, E-stop and `mainFault` polarity, ADC reference, the back-fed 3V3 rail — **and a correction to `KilnFW`'s isolated-link direction** |
| [`docs/SAFETY_MODEL.md`](docs/SAFETY_MODEL.md) | The nuisance-trip doctrine, the thermocouple-placement modes, all twelve guards with thresholds and rationale, trip semantics, and an honest list of what this does **not** protect against |
| [`../../tools/PcTools/TODO.md`](../../tools/PcTools/TODO.md) | The GUI/MCP server for **both** processors: the move out of `firmware/KilnFW/`, the three transports, and the capabilities that make it usable headlessly |
| [`../CommonFW/README.md`](../CommonFW/README.md) | **The shared link code** — what is shared, what is not, and the rules that let one source build under ESP-IDF, pico-sdk and MSVC |
| [`../CommonFW/docs/LINK_PROTOCOL.md`](../CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends: framing, the unacknowledged broadcast frame, the ESP→Pico context format, the telemetry frames, **what the web GUI should show**, and the `KilnFW` changes required |
| [`docs/CURRENT_SENSE.md`](docs/CURRENT_SENSE.md) | The analog front end (precision rectifier + 1 s peak hold), why there is no RMS sampler, calibration, and the commissioning check |
| [`docs/THERMOCOUPLE.md`](docs/THERMOCOUPLE.md) | The MAX31856 here vs on the main board, **why type K may be the wrong choice above ~1150 °C**, register configuration, the accuracy budget, and which failure modes read *low* |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Toolchain, one-task-per-interface layering, priorities and core affinity, boot order, **RP2040 flash/watchdog/ADC gotchas**, state machine and trip codes |
| [`docs/CONFIG_REFERENCE.md`](docs/CONFIG_REFERENCE.md) | Every tunable in one table with defaults, and whether getting it wrong is dangerous, a nuisance, or cosmetic |
| [`docs/GUARD_TEST_MATRIX.md`](docs/GUARD_TEST_MATRIX.md) | How each guard is provoked on host and hardware — **nuisance-rejection tests first** |
| [`docs/BOOTLOADER.md`](docs/BOOTLOADER.md) | **The RP2040 has no UART bootloader in mask ROM**, so field updates need one written: flash layout, metadata, recovery mode, and the rollback bar an image has to clear |
| [`../CommonFW/docs/UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md) | Updating **both** processors: the interlocks, the one-password authentication, ESP OTA partitioning, and the frames that carry an image over the isolated link |
| [`TODO.md`](TODO.md) | Sequenced build plan, phases 0–10, including the `KilnFW`-side blockers |

## Three things to know before touching this

**The isolated UART pin map is now a measurement, not a trace.** Measured on
the bench 2026-08-23: ESP TX is **GPIO5** (`DataToSafty`), ESP RX is **GPIO4**
(`DataFromSafty`), and `KILNCTL_SAFETY_TX_IO`/`RX_IO` default to 5/4. `SaftyFW`'s
own pins (`GP4` = TX, `GP5` = RX) never changed and were always right.

This was got wrong twice from the schematic before it was measured — once by
misreading U3's mirrored symbol, and once by trusting R15's position when R15
itself was wired to the wrong net. Do not re-derive it from the symbols; see
([`docs/HARDWARE.md`](docs/HARDWARE.md) §1) for the measurement and both
failures.

**Every relay on this board is a pilot relay, for galvanic isolation only** —
K4 included, and the schematic says so next to it. None carries element current;
they drive external contactors and SSRs. K4 must sit in series with whatever
energizes the load-switching stage, wired so that de-energized K4 = elements
dead. Loss of Pico power, a crash, a watchdog reset, and an unprogrammed Pico
must all land in that state ([`docs/HARDWARE.md`](docs/HARDWARE.md) §3).

**Flash and debug over SWD, and log over the link.** A CMSIS-DAP probe is wired
to the safety processor's SWD header, and `tools/PcTools/src/kilnctrl/debug_probe.py`
(`kiln_call(name="debug_program")`, `target/rp2040.cfg`, 5000 kHz) programs and
debugs it through the same OpenOCD substrate used for ESP flashing — reset,
halt, step and memory read/write are all exposed to an agent this way. The
Pico's console reaches the GUI as `kilnlink` LOG frames relayed by the ESP —
no extra cable ([`../../tools/PcTools/TODO.md`](../../tools/PcTools/TODO.md)).
The safety link itself now runs against the ESP (proven 2026-08-23 once the
baud rate was corrected to 9600 at the time — see `docs/HARDWARE.md` §1 for
that history and `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig`
for the current value, now that the barrier is a digital isolator rather than
an optocoupler pair), but
`ota_update_pico` still cannot be used for first-flash bring-up because its
interlock requires an already-healthy safety link — a chicken-and-egg that SWD
flashing sidesteps.

**Do not connect USB to the Pico while `12v_Safty` is applied.** A1's 3V3 pin is
back-fed from the board's regulator with VSYS and VBUS unconnected; USB would
put the on-module regulator in contention with it. Flash over SWD
([`docs/HARDWARE.md`](docs/HARDWARE.md) §7).

## Related

- [`../CommonFW`](../CommonFW) — the shared link contract and codecs, linked by both firmwares
- [`../../tools/PcTools`](../../tools/PcTools) — one GUI and MCP server for both processors
- [`../KilnFW`](../KilnFW) — ESP32-S3 main controller firmware
- [`../../hardware/mainBoard`](../../hardware/mainBoard) — KiCad project; `output/kiln.pdf` is the
  authoritative schematic. **`kiln.net` is stale — do not trace from it.**
- [`../../hardware/SaftyThermocoupleBoard`](../../hardware/SaftyThermocoupleBoard) — the single-channel
  MAX31856 daughterboard on J7


---

## Completion checklist

High-level only — each document carries its own. Phases refer to [`TODO.md`](TODO.md).

- [x] **Phase 0** safety-UART pin map settled by measurement 2026-08-23 (ESP TX=GPIO5, RX=GPIO4); remaining Phase 0 blockers still open
- [ ] **Phase 1** `CommonFW` extracted and linked by both firmwares
- [x] **Phase 2** skeleton: GPIO6 low first, watchdog, task/core layout —
      done 2026-08-16, build-verified clean under the real toolchain, never
      run on hardware
- [ ] **Phase 3** thermocouple, with `~DRDY` interrupt and silence detection
- [ ] **Phase 4** guards as a pure function, host-tested, nuisance tests first
- [ ] **Phase 5** relay authority, safe state proven four ways, interlock polarity confirmed
- [ ] **Phase 6** current sensing, CT channel mapping confirmed before S3/S4 enabled
- [ ] **Phase 7** link: context, ceiling, clear, borrowed-source guards
- [ ] **Phase 8** telemetry and the ESP web GUI panel
- [ ] **Phase 9** commissioning, guard test matrix worked end to end, gaps recorded
- [ ] `SAFETY_MODEL.md`'s summary table updated with per-row verification state
- [ ] Board-change proposals for the next revision written up, not forgotten
