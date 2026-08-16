# SaftyFW — kiln safety processor firmware

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** It is the entry point; if the document set or the
> headline facts change, update it in the same commit.

FreeRTOS firmware for the **Raspberry Pi Pico (RP2040)**, designator **A1** on
the kilnCtl main board, running in the isolated `GND_Safty` domain.

**Status: planning only.** No code exists yet. This directory currently holds
the design documents below, written before implementation so the thresholds,
the wire protocol and the failure analysis can be argued about while they are
still cheap to change.

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
  firing ceiling**, received over an opto-isolated link,

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
| [`../PcTools/README.md`](../PcTools/README.md) | The GUI/MCP server for **both** processors: the move out of `KilnFW/`, the three transports, and the capabilities that make it usable headlessly |
| [`../CommonFW/README.md`](../CommonFW/README.md) | **The shared link code** — what is shared, what is not, and the rules that let one source build under ESP-IDF, pico-sdk and MSVC |
| [`../CommonFW/docs/LINK_PROTOCOL.md`](../CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends: framing, the unacknowledged broadcast frame, the ESP→Pico context format, the telemetry frames, **what the web GUI should show**, and the `KilnFW` changes required |
| [`docs/CURRENT_SENSE.md`](docs/CURRENT_SENSE.md) | The analog front end (precision rectifier + 1 s peak hold), why there is no RMS sampler, calibration, and the commissioning check |
| [`docs/THERMOCOUPLE.md`](docs/THERMOCOUPLE.md) | The MAX31856 here vs on the main board, **why type K may be the wrong choice above ~1150 °C**, register configuration, the accuracy budget, and which failure modes read *low* |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Toolchain, one-task-per-interface layering, priorities and core affinity, boot order, **RP2040 flash/watchdog/ADC gotchas**, state machine and trip codes |
| [`docs/CONFIG_REFERENCE.md`](docs/CONFIG_REFERENCE.md) | Every tunable in one table with defaults, and whether getting it wrong is dangerous, a nuisance, or cosmetic |
| [`docs/GUARD_TEST_MATRIX.md`](docs/GUARD_TEST_MATRIX.md) | How each guard is provoked on host and hardware — **nuisance-rejection tests first** |
| [`TODO.md`](TODO.md) | Sequenced build plan, phases 0–8, including the `KilnFW`-side blockers |

## Three things to know before touching this

**The isolated UART pins are swapped in `KilnFW` today.** Traced from the
current schematic, ESP TX is **GPIO4** (`DataToSafty`) and ESP RX is **GPIO5**
(`DataFromSafty`) — the opposite of `KILNCTL_SAFETY_TX_IO`/`RX_IO`'s defaults,
and the opposite of what `KilnFW/docs/SAFETY_LINK.md` "Trap 1" claims. R15's
placement confirms the trace. Nothing works until this is fixed
([`docs/HARDWARE.md`](docs/HARDWARE.md) §1, `TODO.md` 0.1).

**Every relay on this board is a pilot relay, for galvanic isolation only** —
K4 included, and the schematic says so next to it. None carries element current;
they drive external contactors and SSRs. K4 must sit in series with whatever
energizes the load-switching stage, wired so that de-energized K4 = elements
dead. Loss of Pico power, a crash, a watchdog reset, and an unprogrammed Pico
must all land in that state ([`docs/HARDWARE.md`](docs/HARDWARE.md) §3).

**Flash and debug over SWD, and log over the link.** There is no MCP for the
Pico and nothing exposes halt/step/memory access to an agent; the plan wraps
OpenOCD (already installed, already this project's ESP flashing path) to cover
both processors uniformly. The Pico's console reaches the GUI as `kilnlink` LOG
frames relayed by the ESP — no extra cable ([`../PcTools/README.md`](../PcTools/README.md)).

**Do not connect USB to the Pico while `12v_Safty` is applied.** A1's 3V3 pin is
back-fed from the board's regulator with VSYS and VBUS unconnected; USB would
put the on-module regulator in contention with it. Flash over SWD
([`docs/HARDWARE.md`](docs/HARDWARE.md) §7).

## Related

- [`../CommonFW`](../CommonFW) — the shared link contract and codecs, linked by both firmwares
- [`../PcTools`](../PcTools) — one GUI and MCP server for both processors
- [`../KilnFW`](../KilnFW) — ESP32-S3 main controller firmware
- [`../mainBoard`](../mainBoard) — KiCad project; `output/kiln.pdf` is the
  authoritative schematic. **`kiln.net` is stale — do not trace from it.**
- [`../SaftyThermocoupleBoard`](../SaftyThermocoupleBoard) — the single-channel
  MAX31856 daughterboard on J7


---

## Completion checklist

High-level only — each document carries its own. Phases refer to [`TODO.md`](TODO.md).

- [ ] **Phase 0** blockers cleared, including the swapped safety-UART pins in `KilnFW`
- [ ] **Phase 1** `CommonFW` extracted and linked by both firmwares
- [ ] **Phase 2** skeleton: GPIO6 low first, watchdog, task/core layout
- [ ] **Phase 3** thermocouple, with `~DRDY` interrupt and silence detection
- [ ] **Phase 4** guards as a pure function, host-tested, nuisance tests first
- [ ] **Phase 5** relay authority, safe state proven four ways, interlock polarity confirmed
- [ ] **Phase 6** current sensing, CT channel mapping confirmed before S3/S4 enabled
- [ ] **Phase 7** link: context, ceiling, clear, borrowed-source guards
- [ ] **Phase 8** telemetry and the ESP web GUI panel
- [ ] **Phase 9** commissioning, guard test matrix worked end to end, gaps recorded
- [ ] `SAFETY_MODEL.md`'s summary table updated with per-row verification state
- [ ] Board-change proposals for the next revision written up, not forgotten
