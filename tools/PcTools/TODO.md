# PcTools — one GUI and MCP server for both processors

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** If a tool, page or transport changes, update it in
> the same commit. If it disagrees with the code, **the code wins.** Checklist
> at the bottom.

**Status: the move is done.** The code lives at `tools/PcTools/`
(`docs/REPO_LAYOUT.md`, done 2026-08-16) and already serves both processors --
`safety.py`/`probe.py` talk to the RP2040 side, everything else to the ESP.
This document now tracks (b) the remaining "serve both processors evenly"
work and (c) the capabilities that make it usable by an agent rather than
only by a human at a Tk window. The "Move" checklist below is kept as a
record of what that move required, not as an open task.

## Why move it

`tools/PcTools/` is filed under the ESP32 firmware, but it is about to talk to
two processors. It has the same problem `LINK_PROTOCOL.md` had before it moved to
`CommonFW`: a component that serves two peers, living inside one of them, reads
as belonging to that one.

Concretely it already contains `safety.py` — a page and MCP tools for a
processor whose firmware is in a different directory.

**Done: `tools/PcTools/`**, sibling to `firmware/`, per the `REPO_LAYOUT.md`
reorganisation (`docs/REPO_LAYOUT.md`, `ROADMAP.md` M1/M7).

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

## Firmware updates from here

Both processors gain field-update paths — the ESP over Wi-Fi, the Pico over the
isolated link relayed by the ESP. Design:
[`../../firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](../../firmware/CommonFW/docs/UPDATE_PROTOCOL.md).

For day-to-day work the MCP tools matter more than the web page, because most
updates during development will be driven by an agent rather than a browser.
They must not become a second, weaker way in: the same interlocks and the same
challenge–response apply, and the tools call the ESP's endpoints rather than
reimplementing the transfer.

- [ ] `ota_status()` — both processors: running version, build commit, active
      slot, the version in the inactive slot, and **why an update is currently
      refused**, naming the blocker
- [ ] `ota_update_esp(image_path, password)` — streams, waits for the reboot,
      returns the version actually running afterwards
- [ ] `ota_update_pico(image_path, password)` — same, relayed over the link,
      with progress surfaced at least every 2 s
- [x] `ota_rollback(processor)` — explicit, refused under the same interlocks.
      **2026-08-19**: `ota_rollback_esp(password, host=None)`
      (`mcp_server.py`) now exists, calling `POST /api/ota/esp/rollback`
      (`firmware/KilnFW/App/drivers/ota_http.c`'s new
      `ota_esp_rollback_post_handler()`) — same challenge/HMAC auth as
      `ota_update_esp()` but over its own `"esp-rollback"` context (not a
      reuse of `"esp"`, so a plain-update MAC can't double as rollback
      authorization), same `ota_http_check_interlocks()` gate, PLUS an
      explicit `esp_ota_check_rollback_is_possible()` check that refuses
      cleanly ("no previous valid image to roll back to") rather than
      calling `esp_ota_mark_app_invalid_rollback_and_reboot()` blind. On
      success it appends an `ota_record` and reboots from a short-lived
      background task (`ota_rollback_reboot_task()`, same pattern
      `factory_reset.c`'s `reboot_task()` uses) so the HTTP response has a
      chance to reach the client first.

      **2026-08-19, Pico half now also exists**: `ota_rollback_pico()`
      (`mcp_server.py`, no arguments) sends `SAFETY_CMD_ROLLBACK` (0x17,
      new — the next free id after `SET_CONFIG`'s 0x16) over the
      already-authenticated PC→ESP→Pico safety UART bridge, not the HTTP
      OTA challenge/password path (`ota_rollback_esp()` uses that path
      because it *is* an HTTP endpoint; the Pico has no HTTP surface of its
      own — every Pico command already travels this same bridge, same
      authentication boundary as `safety_set_tc_type()`/`safety_ping()`).
      Fire-and-forget: this call cannot see the refusal, only that nothing
      changes.

      Unlike the ESP (`esp_ota_check_rollback_is_possible()`), SaftyFW's own
      bootloader (`firmware/SaftyFW/bootloader/`) has no ESP-IDF-style
      rollback API — it picks the active slot at every boot from a
      versioned/CRC'd flash metadata log
      (`firmware/SaftyFW/bootloader/metadata.h`'s `bootloader_metadata_t`).
      The Pico side is `SaftyFW/src/tasks/link_task.c`'s
      `link_task_handle_rollback()`, which decodes the frame and calls
      `update_task_request_rollback()` (`SaftyFW/src/tasks/update_task.c`)
      synchronously (same "a single metadata-record write is small enough
      to do inline" precedent `config_store_write()` already established,
      not queued the way the much larger UPDATE_BEGIN/_DATA/_END/_ABORT
      transfers are). That function refuses (logs why, sends no reply —
      this frame never ACKs on the wire, same as CLEAR_TRIP/SET_CONFIG) if:
        - the relay is currently ARMED (same gate `config_store_write()`
          uses — a rollback reboots into different code, exactly as
          disruptive as a push or a config write); or
        - **the property that matters most**: the OTHER bootloader slot is
          not currently VALID or PENDING_VERIFY. SaftyFW's bootloader has
          only two slots total, unlike the ESP's richer partition history —
          a rollback that proceeded while the other slot were
          EMPTY/STAGED/BAD would strand the board with zero bootable slots
          on the very next boot. This is checked by
          `bootloader/metadata.c`'s new `bootloader_decide_rollback()`
          *before* the current slot is ever marked `BOOTLOADER_SLOT_BAD` —
          host-tested in `test/test_bootloader_metadata.c`
          (`test_decide_rollback()`, 6 cases: both-VALID, other
          PENDING_VERIFY, other EMPTY/BAD/STAGED all refused, malformed
          `current_slot` refused defensively). On success: the current slot
          is marked `BOOTLOADER_SLOT_BAD` (the existing state, not a new
          one), `active_slot` flips to the other slot, `boot_attempts`
          resets to 0, the record is persisted via the same
          `update_task_persist_metadata()` helper `UPDATE_END` already
          uses, and `watchdog_reboot(0, 0, 0)` resets the RP2040
          immediately — this is the first thing in SaftyFW to actually
          trigger a controlled reboot; `UPDATE_END` deliberately still does
          not.

      The "which slot is actually running" fact comes from the freshly-read
      metadata's own `active_slot` field, not `update_task.c`'s `s_own_slot`
      static — that static is only kept current when a boot starts
      `PENDING_VERIFY` (`update_task_startup_confirm_check()`), so it stays
      stuck at its `BOOTLOADER_SLOT_A` default through the ordinary
      steady-state case (an already-confirmed `VALID` slot) and would name
      the wrong slot most of the time. `active_slot` is written by
      `bootloader_decide_boot()` on every single boot and switch, so it is
      correct in both cases.

      New codec: `kilnlink_rollback.{h,c}` (`firmware/CommonFW`), 1-byte
      frame (cmd only, no payload — the simplest of the three SAFETY_CMD_*
      codecs added this session), host-tested in `test/test_rollback.c`
      (round trip, NULL msg/out, byte-exact vector `{0x17}`, hostile
      too-short/too-long/wrong-cmd, undersized-buffer encode) — full
      CommonFW ctest suite: 15/15 passing. PC side:
      `devices.safety_request_rollback()` + `mcp_server.ota_rollback_pico()`,
      tested in `test_safety_rollback.py` (7 tests), full PcTools suite
      104/104 passing.

      Build-verified only: `firmware/SaftyFW/test/build_host_tests.ps1`
      516/516 checks passing (up from before this pass); real `ninja -j 24`
      clean under `-Werror` for `SaftyFW`/`SaftyFW_slotA`/`SaftyFW_slotB`
      and the bootloader itself; `idf.py -C firmware/KilnFW build`
      (via `ninja -j 24` once configured) clean. **No physical RP2040 or
      ESP32-S3 exercised** — nothing here has ever actually triggered a
      reboot/rollback on real hardware.
- [ ] Every call logged with the image's SHA-256, and refusals logged too
- [ ] The password is never written to the log or to a settings file; it is
      supplied per call or read from an environment variable
- [ ] A protocol-version mismatch between a Pico image and the running ESP is a
      hard error here, not a warning — an agent will click through a warning
- [ ] `ota_status()` reports **both** processors' protocol version and
      `min_compatible`, and says plainly whether they are compatible and which
      side is older. This is the first thing to check when the link is behaving
      oddly, and it should not require reading two frames by hand
- [x] SWD recovery documented alongside, since a bricked Pico is recovered by
      the debug probe and not by these tools. **2026-08-19**: `ota_rollback`
      (this section, above) and `ota_update_pico` both only work through a
      link that is up and a bootloader that is still answering
      `UPDATE_*`/beacon traffic (`firmware/SaftyFW/docs/BOOTLOADER.md`
      §4's recovery mode) — neither path exists for a Pico that will not
      boot into either app slot or its own bootloader at all. That case is
      SWD-only, over the same Debug Probe connection
      (`kilnctrl.debug_probe`, `mcp__kilnctrl__debug_program`/`debug_halt`/
      `debug_reset`) already used to flash `SaftyFW.elf` directly, bypassing
      the isolated link entirely — reflash whichever slot (or the
      bootloader itself, `firmware/SaftyFW/bootloader/`) is bad, per
      `BOOTLOADER.md`'s own status header and completion checklist for the
      current boundary of what's flashable this way today. No new tooling
      needed: this is a pointer between two already-documented facts
      (recovery mode's real limits, SWD's unconditional path around them),
      not a new capability.

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

**Move (done 2026-08-16 -- record, not an open task)**
- [x] `KilnFW/pc_tools/` → `tools/PcTools/` via `git mv` (history preserved)
- [x] Package stays `kilnctrl`; console scripts unchanged
- [x] `CLAUDE.md` references updated
- [x] `.gitignore` re-rooted (`logs/`, `.venv/`, `__pycache__/`)
- [x] `selfcheck.py` still runs after the move (16 pre-existing, unrelated
      failures as of 2026-08-16 -- see "GPIO probe" item above; nothing the
      move itself broke)

**Two peers**
- [ ] `Peer` abstraction (`ESP` | `SAFETY`) threaded through `link_hub.py`
- [ ] Every tool takes a peer argument
- [ ] Pico-through-the-ESP path working (no second cable)
- [ ] Direct USB-TTL path documented, **with the inversion requirement stated**
- [ ] SWD/RTT path documented for flashing and for a Pico that will not talk
- [ ] GUI grows a safety column rather than a second application

**Capabilities**
- [x] 1. GPIO probe on the **ESP**, with `KILNCTL_ENABLE_GPIO_PROBE` **default off**, a pin deny-list including **GPIO6**, writes refused during a profile, every call logged.
      Done 2026-08-16: `App/drivers/gpio_probe.{c,h}` (task 12), `Kconfig`
      option, `tools/PcTools/src/kilnctrl/probe.py` client +
      `mcp_server.py` tools (`gpio_probe_set_mode/write/read/read_all`).
      Verified: `idf.py build` clean both with the option on and off (its
      default); `selfcheck.py` still passes everything it passed before
      (16 pre-existing unrelated failures, all UART_PROTOCOL_VERSION==2/
      old-payload-size test staleness predating this change).
      **Not yet bench-tested against real hardware** -- no board attached
      this session.
- [x] 1b. GPIO probe on the **Pico** over SWD; **GPIO6 (`saftyRelay`) never writable**; writes refused unless `INIT`/`GRACE`.
      Done 2026-08-18: `tools/PcTools/src/kilnctrl/pico_gpio_probe.py`, no
      firmware agent needed (RP2040 GPIO is memory-mapped: SIO + IO_BANK0 +
      PADS_BANK0, poked directly over the existing `debug_probe.py`/OpenOCD
      SWD connection, register addresses confirmed against the RP2040
      datasheet and matching the ones `coordinated_gpio_test.py` already
      exercised against real hardware this session). `set_mode()`/`write()`
      refuse GPIO6 unconditionally, no override -- enforced in this module,
      not delegated to firmware, since SWD sees the chip's memory directly
      with no firmware in the loop to ask permission of. `read()` is allowed
      on GPIO6 (passive `SIO_GPIO_IN` read only). MCP tools
      `pico_gpio_set_mode/write/read/read_all` added to `mcp_server.py`.
      **The `INIT`/`GRACE` gate from this item's own wording is NOT
      implemented** -- SaftyFW exposes no protocol yet for the PC to query
      relay_owner's state (same honest gap `debug_probe.py`'s
      `write_memory()` already documents), so there is nothing to query;
      GPIO6's hard, unconditional deny stands in for that guard rail today,
      stricter but not equivalent.
      **Bench-verified 2026-08-18, and it found a real bug**: `set_mode()`
      does set `CTRL.FUNCSEL = GPIO_FUNC_SIO` as designed, but `write()`
      against `SaftyFW`'s live GPIO4 (`PicoTx`) did not reliably move the
      physical pad -- `set_mode`/`write` both returned success and
      `read_all()` echoed the commanded value back, but a Saleae capture on
      the actual net showed **zero transitions** across two full sessions (up
      to 82 s, writes spaced 0.7-4 s apart). Root cause not yet found; suspect
      is somewhere in `write()`'s register path (`_write_word`/OE/SIO_OUT
      addressing) rather than `set_mode()`, since funcsel is confirmed
      correct. Needs a scope/multimeter cross-check against a spare, unused
      Pico GPIO (not one `SaftyFW`'s own firmware is also touching) to rule
      out firmware contention before assuming the tool itself is at fault.
- [ ] 1c. Coordinated two-board test script implementing `firmware/SaftyFW/docs/HARDWARE.md` §1 Steps A/B/C, reaching each processor by a path that is **not** the link under test.
      **Pin/direction mapping half done manually 2026-08-18** (not by this
      script): user physically confirmed ESP board pin 4/6 as outputs and
      Pico board pin 7/14 as the corresponding inputs, Pico board pin 6 as
      output into ESP board pin 5 -- matches `HARDWARE.md` §1 exactly, see
      its Verification status table. **The electrical propagation half is
      still open** and blocked on the `write()` bug just above plus an
      unconfirmed `12v_Safty` power state -- neither ESP GPIO4->Pico GPIO5
      nor Pico GPIO4->ESP GPIO5 showed a captured edge on the far side in any
      attempt this session, and ESP GPIO6 (fault) could not be driven at all
      (hard-denied, no override, by design).
- [~] 2. Saleae capture as an MCP tool. Done 2026-08-16: `saleae_list_devices`/
      `saleae_capture` wrap `logic_capture.py`'s automation-API client;
      verified against no-Logic2-running (clean error, not a crash) since no
      Saleae hardware was attached this session. **`kilnlink` frame decoding
      NOT done** -- deliberately: building a decoder against zero real
      captures risks one nobody has validated. Left for whoever has both a
      board and the analyzer on the bench at once
- [~] 3. GUI-vs-MCP capability audit: four client classes the GUI has driven
      since they were added were never wired into `mcp_server.py` at all --
      `WifiUartClient` (task 11), `ControlClient` (8), `ProfilesClient` (9),
      `AutotuneClient` (10). Closed 2026-08-16: `_wifi`/`_control`/
      `_profiles`/`_autotune` clients added, plus `wifi_get_status/scan/
      add_network/set_mode/set_ap_identity/get_networks/forget`,
      `control_get_zones/set_zone_pid/set_zone_model`, `profiles_list/get/
      save/delete/get_exec_status/start/stop/pause/resume/ack_last_run`,
      `autotune_get_status/start/abort/accept`. `profiles_start/stop` are the
      tools that begin/end an actual firing -- they carry no gate beyond what
      the firmware itself already enforces (`relay_authority_on_blocked()`),
      same as the GUI/HTTP path, per "What this does not become" below.
      Verified: `selfcheck.py` unchanged (476 pass / 16 pre-existing fail).
      **Not yet exercised against real hardware** -- no board attached this
      session. **Audit not exhaustive** -- found by diffing `gui.py`'s client
      instantiations against `mcp_server.py`'s, not a page-by-page walk of
      every GUI control; a full pass as the TODO originally scoped is still open
- [x] 4. `get_board_state()` one-call snapshot. Done 2026-08-16: fw version,
      pin config, thermo, IO, safety status+link stats, Wi-Fi status, zone
      config, profile exec status, autotune status, as JSON. Each section
      fails independently (`_snapshot_section`) so one dead subsystem (e.g.
      no safety processor fitted) doesn't blank the rest -- verified with no
      board attached, every section reported its own honest error. **Only
      one processor** -- there is no second processor's data to add yet
      (`SaftyFW` doesn't exist); revisit once it does
- [x] 5. Software peer stub, both directions, with fault injection. Done
      2026-08-19: `kilnctrl/fake_peer.py` -- `FakeWire` (byte-queue UART
      stand-in), `FaultInjector` (drop/corrupt_crc/truncate/duplicate,
      each tied to a specific `LINK_PROTOCOL.md` rule -- reordering
      deliberately omitted, the link is a single UART byte stream and
      nothing in that doc claims frames can arrive out of order), and
      `FakeEspPeer`/`FakeSaftyPeer` modelling the sec-2 asymmetry (ESP may
      retry via `request_with_retry`; the Pico stub has no retry method at
      all, and `send_telemetry` drops-and-counts on a full TX ring instead
      of blocking). `tests/test_fake_peer.py` (14 tests, pure Python, no
      board) runs `protocol.py`'s real `Frame`/`FrameDecoder` and
      `kilnlink_codec`'s real payload encoders over the fake transport.
- [x] 6. Codec encode/decode exposed with no board attached. Done 2026-08-16:
      `codec_encode_frame`/`codec_decode_frame` (pure functions, no link
      touched), built on `protocol.py`'s existing `Frame`/`unstuff` --
      round-tripped and verified by hand, `selfcheck.py` unchanged
- [x] 7. Log stream queryable as structured records. Done 2026-08-16:
      `get_device_log_json(n, min_level)` returns a JSON array of
      `{pc_time, level, text}`, oldest first, with a severity filter.
      `_device_log_history` now stores `(pc_arrival_time, LogLine)` instead
      of bare `LogLine` -- PC arrival is the only common clock, the device
      has no RTC this tool trusts. `get_device_log` (text) kept unchanged
      for compatibility
- [x] 8. Connection errors name the problem. Done 2026-08-16:
      `serial_link.py`'s `connect()` now raises with every port seen, its
      score, and why it was rejected (JTAG / no bridge-chip hint / not
      enough signal), or says plainly that no serial ports were seen at
      all -- instead of leaving the caller to discover it as a later timeout
- [x] All tools return JSON. Done 2026-08-19: audited every `@_tool()`
      function in `mcp_server.py` (~135, `-> str` throughout). Only two
      actually build a JSON document with `json.dumps` -- `get_device_log_json`
      (plain dicts of str/int, already safe) and `get_board_state` (aggregates
      dataclasses via `_snapshot_section`/`dataclasses.asdict`, `bytes`/other
      objects already routed through `_json_default`). Everything else returns
      a plain human-readable string (f-strings, `.describe()`, `"\n".join`,
      `.hex()`) built directly, with no intermediate non-JSON-safe object --
      a `str` is itself JSON-serializable, so those needed no change. Found
      and fixed one genuine gap: `get_board_state`'s "thermo" and
      "safety_status" sections carry `ThermoReading.temperature_c` /
      `cold_junction_c` and `SafetyStatus`'s temperature/cold-junction/current
      fields, which the firmware legitimately reports as NaN (SPI-failed
      channel, safety processor never reported) -- `json.dumps` accepts NaN by
      default and emits the non-standard `NaN` token instead of raising,
      silently producing invalid JSON for any strict client. Added
      `_sanitize_nan()` (recursively replaces NaN/Infinity floats with `None`)
      and applied it to `get_board_state`'s payload before serializing,
      matching this repo's "null until valid" convention rather than a NaN
      literal. Verified by hand: imported `mcp_server` under the project's own
      `.venv`, called `_sanitize_nan` on nested dict/list/tuple NaN/Infinity
      cases, and round-tripped a `ThermoReading(temperature_c=nan, ...)`
      through `dataclasses.asdict` -> `_sanitize_nan` -> `json.dumps(...,
      default=_json_default)` -> `json.loads`, confirming no `NaN` token in
      the output and no exception. Did not exercise the other ~130 tools
      end-to-end against real hardware (no board attached this session) --
      that pass was a manual read of every function's return statement(s)
      rather than a live/mocked call-and-assert harness; no existing
      `test_mcp_server.py`-style mocking convention was found in `tests/` to
      build on (only `test_debug_probe_armed.py`, `test_ota_http_client.py`,
      `test_fake_peer.py` exist, none of which mock `mcp_server.py`'s tool
      surface), and standing one up for 130+ functions with live UART/OpenOCD
      dependencies was out of scope for an audit-and-fix pass.

**Debug and programming (OpenOCD wrapper)**
- [x] `kilnctrl.debug_probe` module wrapping OpenOCD -- done 2026-08-17 as
      one-shot `subprocess.run(openocd -c "cmd1; cmd2; exit")` calls (matching
      the project's existing `flash_firmware()` convention) rather than a
      persistent TCL port (4444) connection; shared plumbing factored into
      `openocd_util.py` (also now backing `flash_firmware()`)
- [x] One target abstraction (`PeerConfig`); per-chip configs for ESP32-S3
      (JTAG, `board/esp32s3-builtin.cfg`) and RP2040 (SWD via CMSIS-DAP,
      `interface/cmsis-dap.cfg` + `target/rp2040.cfg`)
- [x] MCP tools taking a `peer` argument: `debug_program`, `debug_reset`,
      `debug_halt`, `debug_resume`, `debug_step`, `debug_read_memory`,
      `debug_write_memory`, `debug_read_registers` -- no separate `write_reg`
      (registers aren't individually addressable this way in the current
      tool set; `debug_write_memory` covers the memory-mapped case)
- [x] `openocd.exe` path resolution: settings.json override
      (`set_openocd_path`/`get_openocd_status`) -> `OPENOCD_EXE` env var ->
      autodetect, so a machine where autodetection fails is never stuck
- [x] `debug_halt` refused on the ESP while a profile is running or paused
      (`ProfilesClient.get_exec_status()`); an unreachable ESP does not block
      the halt
- [x] **Any** write refused on the Pico while `ARMED` -- **implemented
      2026-08-19, via SWD, not the (still dead) UART link**: SaftyFW's
      `relay_owner.c` keeps its GRACE/ARMED/TRIPPED state in one file-local
      static, `s_state` (1 byte after GCC's enum packing, confirmed unique
      repo-wide). `debug_probe.resolve_symbol()` resolves that symbol's
      address fresh from `SaftyFW.elf` on every call via
      `arm-none-eabi-nm -S` (never a hardcoded address -- BSS layout can
      shift on an unrelated recompile), and `pico_armed_state()` reads it
      over the same SWD path `debug_read_memory` already uses.
      `debug_write_memory`'s MCP wrapper (`mcp_server.py`) now calls this
      before honoring `confirm=True` for `peer="pico"` and refuses the write
      if the state reads ARMED, *or* if it can't be confidently determined at
      all (missing `arm-none-eabi-nm`, missing/stale ELF, ambiguous symbol,
      failed SWD read, or a read-back value outside `relay_owner_state_t`'s
      valid 0-3 range) -- fail closed in every case, `confirm=True` alone is
      never enough for the Pico. Host-side logic (symbol resolution, value
      interpretation, all the fail-closed branches) is unit-tested against
      mocked `nm`/`read_memory` calls in
      `tools/PcTools/tests/test_debug_probe_armed.py`.
      Read-only hardware smoke test this session (Debug Probe attached):
      `resolve_symbol()` against the real `SaftyFW.elf` returned
      `(0x2000ba33, 1)` as expected, but the live SWD read against the
      attached board came back `0xb7` -- not a valid `relay_owner_state_t`
      value, i.e. the attached board is not currently running the exact ELF
      this address was resolved from (stale/different image flashed, or RAM
      not yet initialized this boot). `pico_armed_state()` correctly reported
      `(None, ...)` (fail closed) rather than a false "not armed" -- this is
      exactly the failure mode the None-handling exists for, caught for real
      rather than only in a mock. **Caveat carried forward, unchanged**: any
      SWD connection into the safety domain bonds `GND_Safty` to PC ground
      (see `debug_probe.py`'s module docstring and
      `firmware/SaftyFW/docs/HARDWARE.md` §7b) -- bench only. Not yet
      exercised against a board actually running SaftyFW with `s_state ==
      ARMED` end to end (needs a live build+flash+arm sequence, a bigger
      verification pass than this read-only smoke test); do that before
      relying on this gate for anything beyond defense-in-depth.
- [x] Flash writes require an explicit confirm -- `debug_program(confirm=True)`,
      done 2026-08-17
- [x] Every halt, reset and write logged -- `_session_log.warning(...)` in
      every `debug_*` wrapper that changes state (not on refusals)
- [x] Documented: halting the ESP stops telemetry, and the Pico will correctly
      trip S6 -- in `debug_halt`'s and `debug_probe.py`'s docstrings

**Logging and consoles**
- [ ] Pico logs emitted as `kilnlink` LOG frames (device `SAFETY`, task 5), relayed by the ESP — primary path, no new hardware
- [x] Probe UART bridge (UART0, GP16/GP17) supported as the bench console — same probe cable as SWD.
      `console_capture.py`'s `add_safety_probe_uart()` reads it as a plain
      newline-delimited ASCII port (no framing, since the wire-protocol relay
      above doesn't exist yet). Host-tested only with synthetic queued events
      (no Pico/probe attached in this environment) — real-port behavior is
      unverified.
- [ ] RTT-over-SWD console as the fallback path, for when the link is down or GP16/GP17 are inaccessible
- [ ] Pico USB CDC **not** offered as a transport; reported as absent unless `SAFTYFW_ENABLE_USB_STDIO` was built in
- [x] Separate console window per processor, same format — `console_capture.py`
      is a CLI capture (`kilnctrl-console-capture` / `python -m kilnctrl.console_capture`),
      not a Tk window; it reuses `LogClient`/`get_device_log`'s plumbing for
      ESP and opens the probe UART directly for SAFETY. ROADMAP.md M1 note below.
- [x] Separate log file per processor **plus** an interleaved one — `esp_*.log`,
      `safety_*.log`, `interleaved_*.log` under `tools/PcTools/logs/console/`,
      one timestamped set per capture run (2026-08-18)
- [x] PC arrival time as the common clock — every `ConsoleEvent.pc_time` is
      `time.time()` at capture, same reasoning as `get_device_log_json`; each
      side's own uptime is not recorded (neither LOG payload carries one today)
- [ ] Transport marked per line (relayed / probe-UART / RTT) — today only the
      source processor (`ESP`/`SAFETY`) is tagged, since only one transport
      per processor exists in code yet
- [ ] Per-peer level filter
- [ ] Runtime log-level control for the safety processor over the link, default warnings+errors
- [ ] Transport availability shown honestly; USB console reported as a build-time capability, not a toggle
- [ ] Dropped-log-frame counter surfaced from the diagnostic frame
- [ ] Pico log emission best-effort and droppable — **never blocking**, per no-hang rule 3

**Firmware updates**
- [ ] `ota_status`, `ota_update_esp`, `ota_update_pico`, `ota_rollback`
- [ ] Tools call the ESP's endpoints; no second transfer implementation
- [ ] Image SHA-256 logged on every call, refusals included
- [ ] Password never persisted to log or settings
- [ ] Protocol-version mismatch is a hard error, not a warning

**Integrity**
- [ ] Python codec checked against `firmware/CommonFW/test/vectors/`
- [ ] No relay path here bypasses `relay_authority_on_blocked()`
