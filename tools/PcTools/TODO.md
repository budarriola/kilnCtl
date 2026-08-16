# PcTools — one GUI and MCP server for both processors

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** If a tool, page or transport changes, update it in
> the same commit. If it disagrees with the code, **the code wins.** Checklist
> at the bottom.

**Status: proposed move, not executed.** The code exists today at
`tools/PcTools/` and works. This document plans (a) moving it out from under
one firmware, (b) making it serve both processors, and (c) the capabilities that
make it usable by an agent rather than only by a human at a Tk window.

## Why move it

`tools/PcTools/` is filed under the ESP32 firmware, but it is about to talk to
two processors. It has the same problem `LINK_PROTOCOL.md` had before it moved to
`CommonFW`: a component that serves two peers, living inside one of them, reads
as belonging to that one.

Concretely it already contains `safety.py` — a page and MCP tools for a
processor whose firmware is in a different directory.

**Proposed: `PcTools/` at the repository root**, sibling to `firmware/CommonFW/`,
`firmware/KilnFW/` and `firmware/SaftyFW/`. Under the `REPO_LAYOUT.md` reorganisation it becomes
`tools/PcTools/`, or `firmware/PcTools/` if it is felt to belong with the
firmware it mirrors — either is defensible; what is not is leaving it under
`firmware/KilnFW/`.

The Python package keeps the name `kilnctrl`: it is the system's name, not the
main board's, and renaming it would churn every import and the two console
scripts for nothing.

## Three transports, one interface

The GUI and the MCP server should not care how a processor is reached.

| Path | Reaches | Notes |
|---|---|---|
| **USB serial → ESP32** | ESP directly, **and the Pico through it** | The primary path. The `SAFETY` task (task 7) already relays; no second cable |
| **USB-TTL adapter → isolated UART** | Pico directly | For bring-up before the ESP side works, or when the ESP is the thing under suspicion. **Must invert** — see `firmware/SaftyFW/docs/HARDWARE.md` §1 |
| **SWD/RTT → Pico** | Pico directly | Development and flashing. Also the only path when the Pico will not talk |

**The Pico-through-the-ESP path is the one to build first.** It needs no new
hardware, and it is how the system will actually be operated. The direct paths
are bring-up and diagnosis tools.

A `Peer` abstraction (`ESP` | `SAFETY`) threaded through `link_hub.py`, so every
tool takes a peer argument and the GUI grows a second column rather than a
second application.

## Capabilities to add, in priority order

The ordering criterion is blunt: **how much does this unblock work that
currently cannot be done at all?**

### 1. GPIO probe — highest value, unblocks the open hardware question

A new wire task exposing, on the ESP: set a pin's mode (input / input-pullup /
input-pulldown / output), read a pin, write a pin, and read all configured pins
at once.

This is what makes the isolated-UART pin question answerable **without building
and flashing a one-off test firmware**, and it generalises: every future
"is this net actually where the schematic says" question becomes one MCP call.

**It must be gated.** Raw pin control on a board that drives relays is exactly
the hazard `firmware/KilnFW/docs/SAFETY_MODEL.md` spends its length on. The gate should
mirror the existing `SX_WRITE_REG`/`SX_SET_DIR` treatment:

- A compile-time `KILNCTL_ENABLE_GPIO_PROBE`, **default off**, so a production
  build cannot be poked at all.
- A hard deny-list of pins the probe may never touch: the SPI bus, I2C,
  `IO_Expander_RST`, and above all **`SAFETY_FAULT_IO` (GPIO6)** — driving the
  fault line from a debug tool would let a probe silently disable the safety
  processor's view of the main board.
- Writes refused while a profile is running.
- Every probe call logged.

### 1b. GPIO probe on the **Pico** too

The same capability on the safety processor, reached over SWD (see "Debug and
programming" below), because the definitive isolated-UART test needs both ends
driven and read in a coordinated way — `firmware/SaftyFW/docs/HARDWARE.md` §1.

Same gating discipline, and one absolute rule: **the probe may never write
GPIO6 (`saftyRelay`)**. A debug tool that can energize the safety relay is a
debug tool that can defeat the safety processor.

Pico-side probe writes must additionally be refused unless the safety processor
is in `INIT` or `GRACE` — never while `ARMED`.

### 2. Saleae capture as an MCP tool

`logic_capture.py` and the `logic2-automation` dependency already exist, and a
**Saleae Logic16 is attached to this machine right now** — it is the only piece
of the test rig currently plugged in.

Exposing capture as an MCP tool (arm, capture N ms, return decoded UART or raw
transitions) gives an *independent* view of the wire that does not depend on
either firmware being correct. For bringing up a link where both ends are
unwritten and the pin assignment is disputed, that is worth more than any
amount of firmware instrumentation.

Add a UART decode helper that understands the `kilnlink` framing, so a capture
comes back as frames rather than edges.

### 3. Everything reachable headlessly

An agent cannot drive a Tk window. Every capability the GUI has must also exist
as an MCP tool or a CLI subcommand, returning **JSON**.

This needs an audit rather than an assumption: walk `gui.py`'s pages against
`mcp_server.py`'s tool list and close the gaps. Anything the GUI can do that the
MCP cannot is a capability that does not exist for automated work.

### 4. One-call board snapshot

`get_board_state()` returning, in one response: firmware version and git
identity for **both** processors, protocol versions, pin configuration, all
thermocouple readings, relay and I/O state, safety status, safety diagnostics,
link statistics, and Wi-Fi status.

Most diagnosis starts by asking for all of it. One call instead of nine is a
large practical difference in how much of a session gets spent on plumbing.

### 5. A software peer that speaks the safety protocol

A stub implementing the Pico side against `CommonFW`'s codecs — answering
context frames, emitting telemetry, and able to inject faults on demand.

`firmware/KilnFW/docs/SAFETY_LINK.md` already describes this as the recommended way to
develop the RP2040 protocol layer before any RP2040 is involved. It should be a
first-class, scriptable tool, and it should run in **both** directions: a stub
Pico for testing `KilnFW`, and a stub ESP emitting context frames for testing
`SaftyFW`'s parser.

With this plus `CommonFW`'s test vectors, most of the link can be finished
before either board is on a bench.

### 6. Protocol codec exposed directly

Encode and decode any frame from JSON, with no board attached. Makes it possible
to reason about a byte sequence from a capture or a log without guessing.

### 7. Log stream access

Tail the device log (`uart_log_bridge` already forwards every `ESP_LOGx`), with
filtering, and return it as structured records. Currently the log is visible in
the GUI; it should be queryable.

### 8. Honest connection errors

`recommend_port()` returning `None` should surface as "no board detected, these
ports were seen and rejected because …", not as a timeout further down. Time
spent diagnosing an unplugged board is pure waste.

## Debug and programming — what exists, and what does not

I checked. The state of the world on this machine:

| Extension | Version | Agent-usable interface? |
|---|---|---|
| `espressif.esp-idf-extension` | 2.2.0 | **Yes** — contributes one language-model tool, `espIdfCommands`. Surfaces to me as `build_project`, `clean_project`, `flash_project`, `set_target` |
| `marus25.cortex-debug` | 1.12.1 | **No.** A DAP debug adapter, driven from the VS Code UI only |
| `raspberry-pi.raspberry-pi-pico` | 0.21.0 | **No** MCP, no language-model tools |
| `paulober.pico-w-go` | 4.3.4 | **No**, and MicroPython-oriented — not relevant to a C/FreeRTOS build |

So: **there is an ESP32 MCP and it covers build/flash/target only.** There is
**no MCP for the Pico at all**, and nothing anywhere exposes reset, halt,
single-step, or memory read/write to an agent.

### One substrate covers both: OpenOCD

`openocd-esp32 v0.12.0` is already installed under `C:/Espressif/tools/`, and it
is already this project's chosen ESP flashing path. It also supports the
RP2040 — `interface/cmsis-dap.cfg` plus `target/rp2040.cfg`, against a
debugprobe/Picoprobe.

OpenOCD exposes a **TCL/telnet command port (4444)** and a **GDB server (3333)**,
both scriptable from Python. Everything asked for falls out of that, for both
chips, from one wrapper:

| Capability | OpenOCD |
|---|---|
| Program | `program <file> verify reset` |
| Reset | `reset run` / `reset halt` / `reset init` |
| Enter debug mode | `halt`, `resume`, `step` |
| Read memory | `mdw` / `mdh` / `mdb <addr> <count>` |
| Write memory | `mww` / `mwh` / `mwb <addr> <value>` |
| Registers | `reg`, `reg <name> <value>` |
| Console over SWD, no USB | `rtt setup` / `rtt start` / `rtt server start` |

**Proposal: a `debug` module in `kilnctrl` wrapping the OpenOCD TCL port, with
one target abstraction and per-chip config**, exposed as MCP tools taking a
`peer` argument. That gives symmetric capability across both processors without
depending on either VS Code extension, and it keeps flashing on the path the
project already standardised on rather than `esptool`.

**Guard rails, since this is unrestricted memory access on a board that drives
heaters:**

- Refuse `halt` on the ESP while a profile is running — halting the control loop
  leaves relays in whatever state they were in. `profile_executor`'s guard-9
  watchdog would eventually fire, but "eventually" is not a safety argument.
- Refuse **any** write to the Pico while it is `ARMED`.
- Never write flash on a live system without an explicit confirm.
- Log every halt, reset and write.

Two practical notes:

- **Debugging the ESP over JTAG while the safety link runs works**, but halting
  the ESP stops its telemetry, which the Pico will correctly read as a dead main
  controller (`S6`). Expect a trip, and expect it to be *correct*.
- Debugging the Pico needs a probe on SWD, which is also the recommended
  flashing path — one probe covers both jobs. The exact wiring, free pins and
  the ground-bonding caveat are in `firmware/SaftyFW/docs/HARDWARE.md` §7b.
- ⚠️ **Any PC debug connection into the safety domain bonds `GND_Safty` to PC
  ground**, and if the ESP is on the same PC, bypasses the isolation barrier for
  the duration. Bench only, never with load wiring connected.

## Logging and consoles

**`KilnFW` already writes log files today** — confirmed:
`session_YYYYmmdd_HHMMSS.log` under `pc_tools/logs/`, written by
`session_log.py`, with a configurable retention count and a roll-over on the
device's boot push. `logs/saleae/` holds captures. Nothing to add there beyond
keeping it after the move.

What is missing is the equivalent for the safety processor.

### The Pico's log transports

**Decided (2026-08-16): the bench setup is the Raspberry Pi Debug Probe — SWD
plus its UART bridge on GP16/GP17. The Pico's own USB is not used.**

| Path | Needs | Verdict |
|---|---|---|
| **`kilnlink` LOG frames over the isolated link** | nothing extra | **Primary.** Task id 5 (`LOG`) already exists and the ESP already forwards every log line to the PC. The Pico emits as device `SAFETY`, task 5; the ESP relays. Zero new hardware, works in the deployed system |
| **Probe UART bridge, UART0 on GP16/GP17** | the debug probe (same cable as SWD) | **Bench primary.** An ordinary console with no USB on the Pico. Present before the link works and during early boot, which is when a console earns its keep. Wiring: `firmware/SaftyFW/docs/HARDWARE.md` §7b |
| **RTT over SWD** | the debug probe, no extra pins | **Fallback.** For assembled boards where GP16/GP17 are inaccessible, and for output from contexts too early or too broken for a UART driver |
| **USB CDC (`stdio_usb`)** | a USB cable to the Pico | **Not used.** Compile-time only, default off — see below |

> **Why the Pico's USB is not in the plan.** It replaces nothing: SWD is needed
> for flashing and debug regardless, and the same probe cable already carries a
> UART bridge. Against that it links TinyUSB into the safety processor, blocks by
> default when no host is reading, is absent during early crashes and watchdog
> loops, and drops on every reset. `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 has the full
> reasoning. It survives only as `SAFTYFW_ENABLE_USB_STDIO`, **default off,
> debug builds only** — a build option, never a runtime GUI toggle.
>
> Separately, and no longer driving any decision: A1's 3V3 pin is back-fed from
> `3.3v_Safty` with VSYS and VBUS unconnected (`firmware/SaftyFW/docs/HARDWARE.md` §7), so
> USB on a powered board puts the module regulator in contention with IC3. Recorded
> as a hardware fact for whoever ignores the above.

### What the GUI should do with it

- **A separate console window per processor**, same format, side by side —
  matching the existing device-log view rather than inventing a second style.
- **A separate log file per processor**, same naming and retention scheme:
  `session_<ts>_esp.log` and `session_<ts>_safety.log` — **plus an interleaved
  one.** For a link problem, relative ordering between the two processors is the
  whole diagnosis, and it is unrecoverable from two separate files.
- **A single monotonic timestamp source.** Both streams reach the PC over the
  same serial link, so timestamp on arrival at the PC and record each side's own
  uptime alongside. The Pico has no RTC (`SET_CLOCK` is diagnostic-only), so PC
  arrival time is the only common clock.
- **Mark the transport on each line.** A line that arrived via RTT and one that
  arrived via the ESP relay mean very different things when the link itself is
  what is under investigation.
- Per-peer level filter, and a copy-to-clipboard that preserves both.
- **A control for the safety processor's log level over the link**, per peer.
  This is a runtime setting sent as a link command, defaulting to warnings and
  errors only — see `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 for why it defaults quiet.
- **Show which transports are actually available**, greyed out when not: the USB
  console is a *compile-time* option in `SaftyFW` (`SAFTYFW_ENABLE_USB_STDIO`,
  default off), so the GUI must report "not present in this build" rather than
  offering a switch that cannot work.
- **Surface the dropped-log-frame counter** from the diagnostic frame, so a
  quiet log is distinguishable from a dropped one.

### Log frames must never be able to hurt the safety processor

Same discipline as `uart_log_bridge`: best-effort, dropped locally when the queue
is full, **never blocking the task that logged**. On the Pico this is not just
good manners — it is rule 3 of the no-hang constraint
(`firmware/CommonFW/docs/LINK_PROTOCOL.md` §2). `log_task` is the lowest priority in the
system and its output is always droppable (`firmware/SaftyFW/docs/ARCHITECTURE.md` §4).

## What this does not become

- **Not a second control path that bypasses safety.** Every relay command from
  here goes through `relay_authority_on_blocked()`, exactly as
  `firmware/KilnFW/docs/SAFETY_MODEL.md` requires of the existing tools. The GPIO probe's
  deny-list exists so it cannot become a way around that.
- **Not a place for firmware logic.** It observes and commands; it does not
  decide anything safety-relevant.
- **Not a fourth protocol implementation.** It consumes `CommonFW`'s test
  vectors so its Python codec is checked against the C one
  (`firmware/CommonFW/README.md`).

---

## Completion checklist

**Move**
- [ ] `tools/PcTools/` → `PcTools/` via `git mv` (history preserved)
- [ ] Package stays `kilnctrl`; console scripts unchanged
- [ ] `firmware/KilnFW/README.md` and `CLAUDE.md` references updated
- [ ] `.gitignore` re-rooted (`logs/`, `.venv/`, `__pycache__/`)
- [ ] `selfcheck.py` still passes after the move

**Two peers**
- [ ] `Peer` abstraction (`ESP` | `SAFETY`) threaded through `link_hub.py`
- [ ] Every tool takes a peer argument
- [ ] Pico-through-the-ESP path working (no second cable)
- [ ] Direct USB-TTL path documented, **with the inversion requirement stated**
- [ ] SWD/RTT path documented for flashing and for a Pico that will not talk
- [ ] GUI grows a safety column rather than a second application

**Capabilities**
- [ ] 1. GPIO probe on the **ESP**, with `KILNCTL_ENABLE_GPIO_PROBE` **default off**, a pin deny-list including **GPIO6**, writes refused during a profile, every call logged
- [ ] 1b. GPIO probe on the **Pico** over SWD; **GPIO6 (`saftyRelay`) never writable**; writes refused unless `INIT`/`GRACE`
- [ ] 1c. Coordinated two-board test script implementing `firmware/SaftyFW/docs/HARDWARE.md` §1 Steps A/B/C, reaching each processor by a path that is **not** the link under test
- [ ] 2. Saleae capture as an MCP tool, with `kilnlink` frame decoding
- [ ] 3. GUI-vs-MCP capability audit completed and gaps closed
- [ ] 4. `get_board_state()` one-call snapshot, both processors
- [ ] 5. Software peer stub, both directions, with fault injection
- [ ] 6. Codec encode/decode exposed with no board attached
- [ ] 7. Log stream queryable as structured records
- [ ] 8. Connection errors name the problem
- [ ] All tools return JSON

**Debug and programming (OpenOCD wrapper)**
- [ ] `kilnctrl.debug` module wrapping the OpenOCD TCL port (4444)
- [ ] One target abstraction; per-chip configs for ESP32-S3 and RP2040
- [ ] MCP tools taking a `peer` argument: `program`, `reset`, `halt`, `resume`, `step`, `read_mem`, `write_mem`, `read_reg`, `write_reg`
- [ ] `halt` refused on the ESP while a profile is running
- [ ] **Any** write refused on the Pico while `ARMED`
- [ ] Flash writes require an explicit confirm
- [ ] Every halt, reset and write logged
- [ ] Documented: halting the ESP stops telemetry, and the Pico will correctly trip S6

**Logging and consoles**
- [ ] Pico logs emitted as `kilnlink` LOG frames (device `SAFETY`, task 5), relayed by the ESP — primary path, no new hardware
- [ ] Probe UART bridge (UART0, GP16/GP17) supported as the bench console — same probe cable as SWD
- [ ] RTT-over-SWD console as the fallback path, for when the link is down or GP16/GP17 are inaccessible
- [ ] Pico USB CDC **not** offered as a transport; reported as absent unless `SAFTYFW_ENABLE_USB_STDIO` was built in
- [ ] Separate console window per processor, same format
- [ ] Separate log file per processor **plus** an interleaved one
- [ ] PC arrival time as the common clock; each side's uptime recorded alongside
- [ ] Transport marked per line (relayed / probe-UART / RTT)
- [ ] Per-peer level filter
- [ ] Runtime log-level control for the safety processor over the link, default warnings+errors
- [ ] Transport availability shown honestly; USB console reported as a build-time capability, not a toggle
- [ ] Dropped-log-frame counter surfaced from the diagnostic frame
- [ ] Pico log emission best-effort and droppable — **never blocking**, per no-hang rule 3

**Integrity**
- [ ] Python codec checked against `firmware/CommonFW/test/vectors/`
- [ ] No relay path here bypasses `relay_authority_on_blocked()`
