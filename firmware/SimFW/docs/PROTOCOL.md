# SimFW USB control protocol reference

SimFW's own command reference for the native USB CDC link, built on top of
`benchproto` (`firmware/CommonFW/include/benchproto/`, spec in
[`../../CommonFW/docs/BENCHPROTO.md`](../../CommonFW/docs/BENCHPROTO.md)).
This document is SimFW-specific numbering and payload layouts *layered on
top of* that library, exactly the split BENCHPROTO.md section 7 describes:
"`SimFW`'s own `PROTOCOL.md` ... is where its SYS/MODEL/TC/CT/RELAY/IO/
FAULT/EVT payload layouts belong."

Written with the code (docs/DESIGN_NOTES.md section 9): the numeric source of truth
is `src/tasks/cmd_ids.h`; keep the two in sync in the same commit, same
discipline `task_priorities.h`'s header comment asks of DESIGN_NOTES.md section
4.1's task table.

## 1. Transport

Native USB CDC (TinyUSB device stack), **two CDC-ACM instances on one
composite device** (2026-08-23 — a second CDC added alongside the original),
full speed only. No pico-sdk `stdio_usb` — the whole USB peripheral belongs
entirely to `usb_owner` (`src/tasks/usb_owner.c`, `src/tasks/
usb_descriptors.c`), its single owner (DESIGN_NOTES.md section 4's opening
paragraph).

- **CDC0 — protocol** (this document's entire subject from here on). Every
  byte described below — the `benchproto` framing, the command groups, the
  TELEMETRY/EVT broadcasts — travels on this interface, unchanged from the
  single-CDC build.
- **CDC1 — console/log**. A plain, unframed byte stream: every line this
  firmware's `printf()`/`simfw_fatal()` calls send to UART0 is mirrored here
  too (`src/drivers/console_sink.c`, `usb_owner_console_write()`) — not
  `benchproto`-framed, not part of this protocol, just human-readable text.
  Added because interleaving log text into CDC0's binary stream would
  corrupt its framing; a genuinely separate interface was the only option.
  Non-blocking and drop-if-nobody's-listening: see `usb_owner.h`'s own doc
  comment on `usb_owner_console_write()`.

VID/PID `0x2E8A`/`0xF00A` (`src/tasks/usb_descriptors.c` — see that file's
own header comment for the full reasoning: an informal, never-shipped reuse
of Raspberry Pi's VID with a fixture-specific PID chosen to avoid every
documented RPi PID this toolchain's checkout references). Two distinct
interface string descriptors — `"SimFW Control"` (CDC0) and `"SimFW
Console"` (CDC1) — let both a human (Device Manager, `ls /dev/serial/by-id`)
and `kilnsim`'s PC-side port discovery (`tools/PcTools/src/kilnsim/link.py`'s
`list_protocol_ports()`) tell the fixture's two same-VID/PID COM ports apart;
picking the wrong one looks like a dead link, since CDC1 never speaks
`benchproto` at all. The USB serial-number string descriptor (shared by both
interfaces, since it names the *device*, not either interface) is the
RP2040's own 64-bit flash unique id (16 hex chars), so `kilnsim`'s auto-detect
can also tell two SimFW fixtures apart on the same host.

## 2. Framing, addressing, reliability

Identical to `benchproto`'s own spec — see BENCHPROTO.md sections 2-4 for
the full detail (SLIP-style byte stuffing, CRC-16/CCITT-FALSE, DATA/ACK/
NACK/BROADCAST, sequence numbers, retry/dedup). This document only adds
SimFW's own numbers on top:

| Constant | Value | Source |
|---|---|---|
| `SIMFW_DEVICE_HOST` | 0 | `cmd_ids.h` — the PC / `kilnsim` side |
| `SIMFW_DEVICE_TARGET` | 1 | `cmd_ids.h` — this firmware |

### Reply convention (SimFW-specific, not part of `benchproto` itself)

Every command reply's payload starts with a **status byte**, byte 0:

| Value | Name | Meaning |
|---|---|---|
| `0x00` | `SIMFW_CMD_STATUS_OK` | command handled, remaining bytes (if any) are the answer |
| `0x01` | `SIMFW_CMD_STATUS_ERR_NOT_IMPL` | recognised group, but this command id has no handler yet (a reserved id, or the whole group is still a stub) |
| `0x02` | `SIMFW_CMD_STATUS_ERR_BAD_ARGS` | args failed to decode (short/overflowed payload) or a target (zone/channel/expander/pin/slot) was out of range |
| `0x03` | `SIMFW_CMD_STATUS_ERR_INTERNAL` | dispatch reached an inconsistent state (should not happen — see `cmd_task.c`'s dispatch fallthrough comment) |
| `0x04` | `SIMFW_CMD_STATUS_ERR_BUSY` | args were well-formed and in range, but the owning task could not apply the command right now (its internal command queue was full, or an immediate action such as `FAULT_FIRE_NOW` could not complete) — added this pass (section 5), see `cmd_ids.h`'s own comment on the constant |
| `0x05` | `SIMFW_CMD_STATUS_ERR_NO_SAMPLE` | args were valid, but the fixture has no reading to return yet — typically the hardware the read depends on has never responded (e.g. `IO/READ` with no MCP23017 attached to J20) or no scan tick has run since boot. Unlike `ERR_BUSY` this is not transient in the "retry in a moment" sense — the underlying hardware has to actually start answering first. Unlike `ERR_BAD_ARGS` the request itself was fine; see `cmd_ids.h`'s comment on the constant |

This is deliberately a separate layer from `benchproto`'s own ACK/NACK:
ACK/NACK say only whether the frame was *delivered* to a registered task
(BENCHPROTO.md section 4/5) — a command can be delivered perfectly well and
still fail at the application level (not implemented, bad arguments). This
build also piggybacks the actual reply payload directly in the ACK frame
itself, rather than `UnitTestFw`'s older two-frame "ACK for delivery, then a
separate DATA frame with the answer" INFO pattern
(`firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md`'s INFO section) —
simpler, and nothing in BENCHPROTO.md's request/reply description requires
the two-frame split. A command's request payload is `[cmd_id, args...]`
(byte 0 = subcommand, matching that same ancestor document's convention);
multi-byte fields inside a payload are little-endian (BENCHPROTO.md section
3's endianness rule).

## 3. Command groups and task ids

Each row registers as an addressable `benchproto` task (`benchproto_link_register_task()`,
done once at boot by `cmd_task_start()`) — DESIGN_NOTES.md section 5.1's task-registration model.

| Group | `SIMFW_TASK_ID_*` | Value | Status |
|---|---|---|---|
| SYS | `SIMFW_TASK_ID_SYS` | 1 | **implemented** (section 4) |
| MODEL | `SIMFW_TASK_ID_MODEL` | 2 | **implemented** (section 5.1) |
| TC | `SIMFW_TASK_ID_TC` | 3 | **implemented** (section 5.2), incl. `TC_GET_MASTER_CONFIG` |
| CT | `SIMFW_TASK_ID_CT` | 4 | **implemented** (section 5.3) |
| RELAY | `SIMFW_TASK_ID_RELAY` | 5 | **implemented** (section 5.4) |
| IO | `SIMFW_TASK_ID_IO` | 6 | **implemented** (section 5.5) |
| FAULT | `SIMFW_TASK_ID_FAULT` | 7 | **implemented** (section 5.6) |
| EVT | `SIMFW_TASK_ID_EVT` | 8 | **TELEMETRY + EVT broadcast implemented** (`telemetry.c`) — unsolicited BROADCAST only (DESIGN_NOTES.md sec 5.3), no inbound request handling exists or ever will |

A "stub" group *is* registered (so a request to it gets a real ACK with
`ERR_NOT_IMPL`, never a silent drop or a NACK) — only its dispatch table
(`src/tasks/cmd_task.c`'s `s_groups[]`) has no `cmd_id` entries yet. Filling
one in is exactly `s_groups[]`'s point: add rows to that group's own
`commands` array (see `s_sys_commands[]` for the shape); `cmd_task.c`'s
two-level lookup (group by `task_id`, then command by `cmd_id`) never needs
to change shape for this.

## 4. SYS group (`SIMFW_TASK_ID_SYS` = 1)

| `SIMFW_CMD_SYS_*` | Value | Status |
|---|---|---|
| `PING` | `0x01` | **implemented** |
| `GET_VERSION` | `0x02` | **implemented** |
| `RESET_SIM` | `0x03` | **implemented** (gap-closure pass) |
| `SET_TIMESCALE` | `0x04` | **implemented** |
| `SET_SEED` | `0x05` | **implemented** |
| `GET_CAPS` | `0x06` | **implemented** |
| `GET_SIM_STATE` | `0x07` | **implemented** (gap-closure pass; new id, not in DESIGN_NOTES.md sec 5's original sketch) |
| `REBOOT_BOOTLOADER` | `0x08` | **implemented** (later gap-closure pass, "flash over USB without BOOTSEL") |
| `SESSION_RESET` | `0x09` | **implemented** (bug-fix pass, "PC reconnect misclassified as duplicate traffic") |

### `PING` (request: `[0x01]`, no args)

Reply: `[status]` — no data, just confirms the round trip and that `cmd_task`
is alive and dispatching.

### `GET_VERSION` (request: `[0x02]`, no args)

Reply — the "version block" shared with `GET_CAPS` below:

```
byte0        status
byte1-2      BENCHPROTO_PROTOCOL_VERSION, u16 LE (benchproto_version.h)
byte3-4      BENCHPROTO_MIN_COMPATIBLE, u16 LE
byte5        SIMFW_FW_VERSION_MAJOR (version.h)
byte6        SIMFW_FW_VERSION_MINOR
byte7        SIMFW_FW_VERSION_PATCH
byte8        dirty flag (0=clean, 1=dirty/unknown) -- SIMFW_FW_GIT_DIRTY, build_info.h
byte9        git_hash_len (N)
N bytes      git commit, ASCII, not null-terminated -- SIMFW_FW_GIT_COMMIT, build_info.h
```

`build_info.h` is regenerated on every build (`gen_build_info.cmake`, wired
into `CMakeLists.txt` as an always-run custom target — not just on a CMake
reconfigure), same mechanism `KilnFW/App/drivers/gen_build_info.cmake` uses,
so it can never be stale relative to what's actually flashed. Always read
and compare `BENCHPROTO_PROTOCOL_VERSION` first, before trusting anything
else in the payload — same discipline `UART_PROTOCOL.md`'s own
`GET_FW_VERSION` section documents for its `UART_PROTOCOL_VERSION` field.

### `GET_CAPS` (request: `[0x06]`, no args)

Reply — the version block above, followed by capability fields (DESIGN_NOTES.md
section 5.1: "protocol version, SimFW version + git hash, zone count
limits, channel counts, and a feature bitmask"):

```
byte0..(9+N)   version block, identical layout to GET_VERSION above
byte(10+N)     zone_count_min      -- SIMFW_CAPS_ZONE_COUNT_MIN (1)
byte(11+N)     zone_count_max      -- SIMFW_CAPS_ZONE_COUNT_MAX (4)
byte(12+N)     zone_count_default  -- SIMFW_CAPS_ZONE_COUNT_DEFAULT (3), DESIGN_NOTES.md sec 4.3
byte(13+N)     tc_main_channels    -- SIMFW_CAPS_TC_MAIN_CHANNELS (3), spi_emu_a
byte(14+N)     tc_safety_channels  -- SIMFW_CAPS_TC_SAFETY_CHANNELS (1), spi_emu_b
byte(15+N)     ct_channels         -- SIMFW_CAPS_CT_CHANNELS (3), wave_owner
byte(16+N)     relay_channels      -- SIMFW_CAPS_RELAY_CHANNELS (3)
byte(17+N)..+3 feature_bitmask, u32 LE -- SIMFW_CAPS_FEATURE_BITMASK, 0 today
```

`feature_bitmask` bits (added this pass, `cmd_ids.h`'s `SIMFW_CAPS_FEATURE_BIT_*`):
"this group has a real implementation, not just a stub", per DESIGN_NOTES.md
section 5.1's own reasoning for `GET_CAPS` existing at all — so `kilnsim`
can refuse gracefully against a mismatched or partially-built firmware.

| Bit | Group | Notes |
|---|---|---|
| 0 | MODEL | |
| 1 | TC | fully implemented, incl. `TC_GET_MASTER_CONFIG` |
| 2 | CT | |
| 3 | RELAY | |
| 4 | IO | |
| 5 | FAULT | |

SYS has no bit (always fully present by construction — the link couldn't
work otherwise) and neither does EVT (not a request/reply group, section 6).
Today's value is `0x0000003F` (all six bits set).

### `RESET_SIM` (request: `[0x03, u8 keep_params]`)

Thin decode-then-call wrapper over `sim_engine_reset()` (`sim_engine.h`):
`keep_params` 1 reinitializes state from the current zone params' `T0`;
0 reloads the last-selected preset first (DESIGN_NOTES.md 6.1's `sim_reset` tool
doc: "model to T0"). Resets `sim_time_us` to 0, clears MANUAL overrides, and
resets the event ring's sequence number to 0 — does **not** clear
`fault_sched`'s armed/active slots (`FAULT_CANCEL` each explicitly, or send
fresh `FAULT_SCHEDULE`s, if a run needs a clean fault-slot pool too).
Queued, same command-queue contract as every `sim_engine.h` MODEL-group
setter (section 5.1) — a full queue reports `ERR_BUSY`, not `ERR_BAD_ARGS`.
Reply: `[status]`.

### `SET_TIMESCALE` (request: `[0x04, u32 timescale_x100 LE]`)

`sim_engine_set_timescale()` — DESIGN_NOTES.md 4.2/5.2's x100 fixed point (1000 ==
10.00x accelerated, 100 == 1.00x real time, 0 treated as 1.00x by
`sim_engine` itself). Queued, same contract as `RESET_SIM` above. Reply:
`[status]`.

### `SET_SEED` (request: `[0x05, u32 seed LE]`)

`sim_engine_set_seed()` — DESIGN_NOTES.md 4.2's determinism contract ("the same
scenario + seed => the same run, byte-for-byte"); a scenario runner sends
this (and typically `RESET_SIM`) before a fresh run's first tick. Also seeds
the TELEMETRY frame's own `seed` field (section 6) once wired — see that
section's note. Queued, same contract as `RESET_SIM` above. Reply:
`[status]`.

### `GET_SIM_STATE` (request: `[0x07]`, no args)

Read-back companion to `SET_TIMESCALE`/`SET_SEED` — neither setter's value
was otherwise readable back over the wire except via the TELEMETRY frame's
`seed` field (section 6), which says nothing about `timescale_x100`. New id
this pass adds (not in DESIGN_NOTES.md section 5's original sketch), same
"first-class getter for what a client just set" reasoning `cmd_ids.h`
already documents for `IO_ESTOP_GET`/`IO_DUT_POWER_GET`.

```
byte0        status
byte1-4      seed, u32 LE                -- sim_engine_get_seed(), 0 if never set
byte5        snapshot_valid, u8 (0/1)    -- sim_engine has published at least one
                                            tick's snapshot (sim_snapshot_read())
byte6-9      timescale_x100, u32 LE      -- sim_snapshot_t.timescale_x100;
                                            0 if snapshot_valid is 0
byte10-17    sim_time_us, u64 LE         -- sim_snapshot_t.sim_time_us;
                                            0 if snapshot_valid is 0
```

`timescale_x100`/`sim_time_us` come from the published snapshot (the same
source telemetry's TELEMETRY frame uses) rather than a standalone
`sim_engine.h` getter, since none exists for timescale alone — before the
first tick (or right after `RESET_SIM`, whose effect lands at the next tick
boundary like every other queued setter) `snapshot_valid` is 0 and both
fields read 0 rather than a stale value.

### `REBOOT_BOOTLOADER` (request: `[0x08, u32 confirm LE]`)

Drops the RP2040 into its ROM USB bootloader so a PC-side tool can reflash
over the same USB port, without the user physically pressing BOOTSEL. This
is one of **two** independent triggers for the same underlying sequence —
the other is the 1200-baud host-tooling touch convention described below —
both implemented by `src/tasks/safe_reboot.c`'s single
`safe_reboot_into_bootloader()`.

`confirm` must equal `SIMFW_CMD_SYS_REBOOT_BOOTLOADER_MAGIC`
(`0xB007B007`, `cmd_ids.h`) exactly, or the request is refused with
`ERR_BAD_ARGS` before any of the safety sequence below ever runs. This is on
top of `benchproto`'s own CRC-16/addressing (a malformed or misrouted frame
never reaches `cmd_task` at all) as defense in depth specifically for this
command: it is the only one in this whole table whose success path ends the
firmware session outright.

**Why this needs a safety sequence at all — read before touching this
code.** This fixture drives the DUT's E-stop loop and both DUT 12V power
relays (section 5.5 below) through two MCP23017 I2C expanders
(`src/tasks/i2c_owner.c`). Dropping the RP2040 into its ROM bootloader does
**not** reset those expanders — nothing in `i2c_owner.c`,
`src/drivers/mcp23017.{c,h}`, or the board docs (`HARDWARE.md` section 3.7,
`BOM.md` section 6) documents an MCP23017 RESET pin tied to the Pico's RUN
or to any Pico GPIO, and a search of all three turned up nothing. Per this
project's safe-default doctrine, an undocumented hazard is treated as
**present**, not absent: the expanders are assumed to **retain** their
output state across a Pico-only reboot. A naive reboot commanded while the
E-stop loop is closed ("healthy") and a DUT relay is on would leave the DUT
powered, its E-stop loop still reporting fine, with nothing running on the
fixture to fix that until a new image is flashed and boots.

`safe_reboot_into_bootloader()` therefore, before ever calling
`reset_usb_boot()`:

1. Queues (`i2c_owner.h`/`wave_owner.h`'s own queue-then-apply-next-tick
   contract — none of this is synchronous with the call):
   `i2c_owner_set_estop(true)` (loop OPEN/STOP, the fail-safe direction),
   `i2c_owner_set_dut_power_main(false)`, `i2c_owner_set_dut_power_safety(false)`,
   and per CT channel `ct_wave_set_amps(ch, 0.0f)` followed by
   `ct_wave_set_mode(ch, CT_WAVE_MODE_MANUAL)` (amps-then-mode so the channel
   never passes through "MANUAL at a stale nonzero amps" for even one
   zero-crossing).
2. **Polls the readback**, not just the setters' return values, for up to
   300 ms (10 ms between checks): `i2c_owner_get_estop_open()` /
   `_get_dut_power_main_on()` / `_get_dut_power_safety_on()` and
   `ct_wave_get_state()` for every channel must all confirm the commanded
   values actually landed.
3. Only once **every** condition is confirmed does it call
   `reset_usb_boot(0, 0)` — which never returns.

**If the 300 ms timeout elapses without full confirmation, this function
returns `false` and does NOT reboot.** This is deliberate: rebooting anyway
on a timeout risks exactly the hazard the sequence exists to prevent. A
refusal is recoverable (the reply reports `ERR_BUSY`, so the client can
retry or fall back to physical BOOTSEL); a reboot into an unconfirmed unsafe
state is not recoverable by software once the RP2040 is in the bootloader.

Reply: `[status]` — and only ever sent on the `ERR_BAD_ARGS` (bad/missing
magic) or `ERR_BUSY` (safe-state confirmation timed out) paths. **On the
success path there is no reply at all**: `reset_usb_boot()` never returns,
so `cmd_task_fn()`'s `usb_owner_send_reply()` call after
`cmd_task_dispatch()` simply never executes. A client must treat "no ACK
ever arrived for this one command" as success, not a failure — see
`kilnsim.link.SimLink.send_command_expect_reboot()` (`tools/PcTools/src/
kilnsim/link.py`) for the PC-side half of that contract, and `kilnsim
reboot-bootloader --yes` / the `sim_reboot_bootloader` MCP tool for the two
PC-side entry points.

### `SESSION_RESET` (request: `[0x09]`, no args)

**LINK state only — not to be confused with `RESET_SIM` above.** `RESET_SIM`
reinitializes the *simulation*; `SESSION_RESET` clears `usb_owner`'s
`benchproto` dedup ring and its per-task last-ACK cache for the requesting
`src_device`, and touches nothing else — never sim time, seed, timescale,
fault schedule/slots, relay outputs, DUT power, or E-stop state. It never
reaches `sim_engine.h`, `fault_sched.h`, `i2c_owner.h`, or `wave_owner.h`.

**The bug this exists to fix:** `kilnsim`'s `BenchprotoLink` restarts its own
`msg_index` counter at 0 on every `connect()`
(`tools/PcTools/src/kilnsim/benchproto_codec.py`, `BenchprotoLink.__init__`),
but this firmware's dedup ring (`benchproto_link_t`) is initialized exactly
**once**, at `usb_owner_start()` (MCU boot) — it has no idea a USB reconnect
ever happened. After a reconnect, the PC's first `BENCHPROTO_DEDUP_DEPTH`
(4) requests can therefore collide with leftover ring entries from the
*previous* session at the same literal `(src_device, src_task, msg_index)`
tuple, get classified `DUPLICATE_REACK`, and never reach `cmd_task` at all —
observed on real hardware as the first 2-3 commands after every reconnect
either timing out or getting back a stale, unrelated command's cached
answer (`usb_owner_resend_cached_ack()`).

**The dedup-bypass hazard.** `SESSION_RESET` is itself just another
`benchproto` DATA frame to SYS, so on an ordinary reconnect it is exactly as
likely to collide with a stale ring entry as anything else — the state it
exists to clear could swallow the very command sent to clear it.
`usb_owner_handle_raw_frame()` special-cases this one `(dst_task, cmd_id)`
pair: it peeks at `frame->payload[0]` and calls
`benchproto_link_reset_device()` + `usb_owner_reset_ack_cache()`
**unconditionally, before `benchproto_link_on_frame()` ever classifies the
frame** — so by the time dedup classification runs, there is nothing left
for this exact frame to collide with. This is not a generic "skip dedup for
this cmd_id" escape hatch: the side effect (wiping the *entire* requesting
device's dedup ring + ACK cache) is unconditional and total, so the check
cannot be used to sneak one specific duplicate frame past dedup while
leaving dedup intact for everything else — exercising this path always
costs the caller its whole session's worth of dedup/ACK-cache state. Every
other command id reaches `benchproto_link_on_frame()` completely unmodified.

Idempotent and safe to send at any time, any number of times — clearing an
already-empty ring/cache is a no-op. A client SHOULD send it once, as its
very first command after every (re)connect, before even `PING`
(`kilnsim.link`'s `_FramedSimLink.connect()` does exactly this), so `PING`
and everything after it are also protected from the same collision, not
just this command itself. Tolerates an older firmware build that predates
this id: `cmd_task_dispatch()`'s ordinary fallthrough answers `ERR_NOT_IMPL`
(SYS's group table simply has no `0x09` entry), which `kilnsim.link` treats
as "not supported, continue connecting anyway" rather than a connect
failure.

Reply: `[status]` only, always `SIMFW_CMD_STATUS_OK` — this command has no
failure mode of its own to report.

### The 1200-baud touch convention (`src/tasks/usb_owner.c`)

The second, protocol-independent trigger: the widely-supported convention
(Arduino, `picotool`, and most flashing tools that speak to an Arduino-like
board over a CDC-ACM port) where the host sets the CDC line coding to 1200
baud with DTR deasserted to request a bootloader drop, so standard tooling
can reflash this fixture without ever having to speak `benchproto` at all.

Implemented directly in `usb_owner.c`'s own `tud_cdc_line_coding_cb()` /
`tud_cdc_line_state_cb()` TinyUSB callbacks — **not** via pico-sdk's
`PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE` (that SDK option lives inside
`stdio_usb`, a separate CDC interface this project never enables,
`CMakeLists.txt`'s `pico_enable_stdio_usb(SimFW 0)`; SimFW drives TinyUSB
directly with its own CDC). Both callbacks call the exact same
`safe_reboot_into_bootloader()` described above, so the safety sequence,
timeout, and refuse-don't-reboot-anyway policy are identical regardless of
which trigger fired.

**Gated on baud AND DTR together, not baud alone** — unlike pico-sdk's own
reference implementation (`reset_interface.c`'s `tud_cdc_line_coding_cb()`),
which triggers on the magic baud rate by itself. An ordinary port open (e.g.
a `kilnsim run` scenario connecting at its usual baud) changes the CDC line
coding too, and triggering on baud alone would make an accidental reboot
mid-scenario-run one stray reconnect away. Requiring DTR deasserted *at the
same time* as the 1200 baud value is what keeps that from happening — see
`firmware/SimFW/test/test_safe_reboot_logic.c`'s bootloader-touch-gate tests
for the exercised accidental-trigger cases (baud alone, DTR alone, neither).

**Gated on CDC0 (protocol) only, not CDC1 (console)** — the second
independent, deliberate decision the dual-CDC pass (section 1 above) had to
make. `tud_cdc_line_coding_cb()`/`tud_cdc_line_state_cb()` both receive
which CDC instance changed, and CDC1's changes are ignored outright before
even reaching the baud/DTR check. Reasoning: CDC1 is exactly the port an
ordinary terminal program (PuTTY, `screen`, TeraTerm, a serial monitor) opens
to watch log text, and terminal programs are a class of tool that routinely
touches line coding and DTR on open/close for reasons that have nothing to
do with rebooting a device — some default to legacy baud rates on connect,
and DTR toggling on port open/close is close to universal behavior. Honoring
the touch on CDC1 too would mean a human plugging in a log viewer could,
occasionally and silently, drop this fixture into its bootloader — precisely
the accidental-trigger class the baud+DTR conjunction above already exists
to rule out, one interface further out. PC-side tooling that legitimately
wants to trigger a reflash (`kilnsim`, picotool-alikes) connects to CDC0
anyway (it is the protocol interface `kilnsim` uses for everything else), so
restricting the touch to CDC0 costs that use case nothing.

## 5. MODEL / TC / CT / RELAY / IO / FAULT

All six groups below call only their owning task's public API
(`sim_engine.h`, `fault_sched.h`, `i2c_owner.h`, `wave_owner.h`,
`spi_emu_a.h`/`spi_emu_b.h`) — never another task's private state (DESIGN_NOTES.md
section 4.5, `cmd_task.c`'s own file header). Every setter is a decode-then-
call wrapper over that owner's existing queue-then-apply-next-tick command
surface; nothing here adds a new peripheral access path.

### 5.1 MODEL group (`SIMFW_TASK_ID_MODEL` = 2) — `sim_engine.h`

| `SIMFW_CMD_MODEL_*` | Value | Owner call |
|---|---|---|
| `SET_ZONE_PARAMS` | `0x01` | `sim_engine_set_zone_params()` |
| `GET_ZONE_PARAMS` | `0x02` | `sim_engine_get_zone_params()` |
| `SET_AMBIENT` | `0x03` | `sim_engine_set_ambient()` |
| `LOAD_PRESET` | `0x04` | `sim_engine_load_preset()` |
| `SET_TEMP` | `0x05` | `sim_engine_force_zone_temp()` / `sim_engine_clear_zone_manual()` |
| `SET_TC_LAG` | `0x06` | get + set `sim_engine_*_zone_params()` (read-modify-write, see below) |

`SET_ZONE_PARAMS` request (args after the `0x01` cmd_id byte, all little-endian):

```
u8  zone
f32 C
f32 k_loss
f32 k_couple[THERMAL_MODEL_MAX_ZONES]  (4 entries; diag ignored, thermal_model.h)
f32 R_element
f32 element_health
f32 tc_lag_s
f32 T0
```

41 bytes of args. Reply: `[status]`. `GET_ZONE_PARAMS` request is `[zone]`;
reply is `[status, zone, <same 40 bytes as above>]`.

`SET_AMBIENT` request: `[f32 ambient_c]`. `LOAD_PRESET` request:
`[u8 preset]` (`thermal_preset_id_t`: 0 `fast_test`, 1 `small_kiln`,
2 `three_zone`, 3 `stress`, DESIGN_NOTES.md 4.3).

`SET_TEMP` request: `[u8 zone, u8 mode, f32 temp_c]` — `mode` 0 returns the
zone to MODEL (`sim_engine_clear_zone_manual()`, `temp_c` ignored), `mode` 1
forces MANUAL at `temp_c` (`sim_engine_force_zone_temp()`). One command id
covers both halves of `sim_engine.h`'s MANUAL-mode pair.

`SET_TC_LAG` request: `[u8 zone, f32 tc_lag_s]`. `sim_engine.h` has no
standalone "set just `tc_lag_s`" setter — it is one field of
`thermal_zone_params_t`, and the only writer replaces the whole struct — so
this handler does a get-then-set internally (still only calling
`sim_engine.h`'s own public API twice, not touching any private state). Not
an owner-API gap, just a convenience that saves a client round trip and a
TOCTOU window.

### 5.2 TC group (`SIMFW_TASK_ID_TC` = 3) — `spi_emu_a.h`/`spi_emu_b.h` + `sim_engine.h` + `fault_sched.h`

Channel numbering matches `tc_fault_state.h`'s `tc_fault_channel_t`: `0`..`2`
= `MAIN_0`..`MAIN_2` (`spi_emu_a`, ESP bus, index-matches zone 0..2), `3` =
`SAFETY` (`spi_emu_b`).

| `SIMFW_CMD_TC_*` | Value | Owner call | Status |
|---|---|---|---|
| `GET_REGS` | `0x01` | `spi_emu_a/b_get_stats()` + `spi_emu_a/b_get_reg_image()` + `sim_snapshot_read()` + `tc_fault_state_read()` | implemented |
| `FORCE_TEMP` | `0x02` | `sim_engine_force_zone_temp()` (MAIN) / `sim_engine_force_safety_temp()` (SAFETY) | implemented, all channels |
| `SET_MODE` | `0x03` | `sim_engine_force_zone_temp()` / `sim_engine_clear_zone_manual()` (MAIN); `sim_engine_force_safety_temp()` / `sim_engine_clear_safety_manual()` (SAFETY) | implemented, all channels |
| `INJECT_FAULT` | `0x04` | `fault_sched_schedule()` + `fault_sched_fire_now()` | implemented |
| `CLEAR_FAULT` | `0x05` | `fault_sched_cancel()` | implemented |
| `GET_MASTER_CONFIG` | `0x06` | `spi_emu_a/b_get_reg_image()` + `spi_emu_a/b_channel_configured()` | implemented |

**Register-image getters (`spi_emu_a.h`/`spi_emu_b.h`):** a previous pass
left `TC_GET_REGS`'s register-image half and all of `TC_GET_MASTER_CONFIG`
unreachable because neither header exported an accessor for a channel's live
`max31856_channel_t.regs[]` (only `*_get_stats()` for instrumentation
counters). Both headers now export:

- `spi_emu_a_get_reg_image(channel, out_regs[16])` /
  `spi_emu_b_get_reg_image(channel, out_regs[16])` — copies the channel's
  current 16-byte register image. **Coherency guarantee:** never blocks
  (DESIGN_NOTES.md 4.5) and never returns a torn image. It reads
  `max31856_pio_engine_channel_busy()` (already tracked by the PIO engine as
  "CS is low right now") immediately before *and* after the `memcpy`, plus
  the channel's transaction counter before/after, and only reports success
  if both checks agree nothing could have mutated `regs[]` mid-copy — a
  bounded (4 attempts), non-blocking, seqlock-style optimistic read, the same
  pattern the zone snapshot already uses elsewhere in this repo. Returns
  `false` (image left untouched) if the channel stayed busy across every
  retry; the caller must treat that as "ask again," never as a torn read.
- `spi_emu_a_channel_configured(channel)` / `spi_emu_b_channel_configured(channel)`
  — `true` once the master has ever written any byte to this channel (any
  register address, including a write attempt at a read-only one).
  Backed by a new `max31856_channel_t.master_has_written` field
  (`src/sim/max31856_regs.h`, set in `apply_write_rule()`): a monotonic,
  single-byte flag, so it needs none of the busy/retry machinery above —
  a torn read of one bool by another core is not possible the way a
  16-byte struct copy could be.

**`GET_REGS`** request: `[u8 channel]`. Reply:

```
byte0      status
byte1      channel (echo)
byte2      flags: bit0 reg_image_valid, bit1 snapshot_valid,
           bit2 stuck_ltcb, bit3 spurious_fault_pin, bit4 force_sr_open,
           bit5 force_sr_ovuv
byte3-18   reg_image[16] -- the channel's live register image (CR0..SR,
           MAX31856_REG_* addressing). Only meaningful when reg_image_valid
           (bit0) is set; all-zero otherwise.
byte19-22  f32 shadow_true_tc_c       -- uncorrupted thermal-model truth
byte23-26  f32 shadow_reported_tc_c   -- post-TC-lag, pre-corruption signal
byte27     dead_mode (max31856_dead_mode_t: 0 NONE, 1 ALL_ZERO, 2 ALL_ONE, 3 HIGH_Z)
byte28-31  f32 noise_sigma_c
byte32-35  f32 bit_error_rate
byte36-39  u32 spi transactions   (spi_emu_a/b_get_stats().transactions)
byte40-43  u32 spi protocol_errors
byte44-47  u32 spi first_byte_late (TX FIFO underrun count)
```

Together, `byte3-18` (when valid) and `byte19-26` are DESIGN_NOTES.md 5.2's full "the
DUT was lied to, this is the truth" pair: the register image the DUT
actually reads over SPI, plus the shadow truth it was never shown.

**`reg_image_valid` (flags bit0) semantics:** clear means the getter found
the channel mid-transaction (CS low) across its whole bounded retry budget —
`byte3-18` is all zero, and this is not an error, just a transient "the DUT
was mid-read/write when you asked," the same as a real SPI collision would
produce. A client should simply ask again; at the fixture's task-loop scan
cadence (~20 ms) and real SPI transaction durations (microseconds), a busy
result on a genuinely idle channel should be rare.

**`GET_MASTER_CONFIG`** request: `[u8 channel]`. Reply:

```
byte0  status
byte1  channel (echo)
byte2  flags: bit0 configured (spi_emu_a/b_channel_configured() --
       "has the master EVER written anything to this channel", not
       "is it configured correctly"), bit1 reg_image_valid (same
       busy/retry semantics as GET_REGS's identically-named bit)
byte3  CR0 -- 0 if reg_image_valid is clear
byte4  CR1 -- TC TYPE[3:0] is bits[3:0], AVGSEL[2:0] is bits[6:4]
       (DESIGN_NOTES.md 3.2's CR1 row); 0 if reg_image_valid is clear
byte5  MASK -- 0 if reg_image_valid is clear
```

`configured` (bit0) is always trustworthy regardless of `reg_image_valid`
(bit1) — it is backed by `master_has_written`, a plain monotonic flag with
no busy/retry gate of its own (see the getter description above), not by the
register image. A channel can therefore be reported `configured` (bit0 set,
from a *past* write) while the *current* request happens to land
mid-transaction (bit1 clear, `byte3-5` all zero) — callers must check bit1
before trusting `byte3-5`, but bit0 alone already answers "did the DUT ever
configure this channel at all," independent of whether this particular poll
caught it mid-transaction. This is exactly the "configured wrong" vs. "never
configured" distinction DESIGN_NOTES.md 5.2 calls out as "a different and equally
important failure" from a bad TC-type value: `configured=0` means never
configured; `configured=1` with `reg_image_valid=1` and a wrong CR1 means
configured wrong.

**`FORCE_TEMP`** request: `[u8 channel, f32 temp_c]`. For `channel` `0`..`2`
(MAIN), calls `sim_engine_force_zone_temp(channel, temp_c)`. For `channel`
`3` (SAFETY), calls `sim_engine_force_safety_temp(temp_c)` — the safety-side
MANUAL override, independent of every zone's own MAIN-side override
(`sim_engine.h`'s blend/lag+MANUAL safety-TC API). `channel >= 4` returns
`ERR_BAD_ARGS`.

**`SET_MODE`** request: `[u8 channel, u8 mode, f32 manual_temp_c]` — `mode`
0/1 same meaning as MODEL's `SET_TEMP`. For MAIN channels, `mode` 0 calls
`sim_engine_clear_zone_manual(channel)` and `mode` 1 calls
`sim_engine_force_zone_temp(channel, manual_temp_c)`. For the SAFETY channel,
`mode` 0 calls `sim_engine_clear_safety_manual()` and `mode` 1 calls
`sim_engine_force_safety_temp(manual_temp_c)`, returning the safety channel
to blend/lag-driven reporting or pinning it, without touching any zone's
MAIN-side override. `channel >= 4` or `mode` outside `{0, 1}` returns
`ERR_BAD_ARGS`.

A `main_safety_skew`-style scenario (DESIGN_NOTES.md section 8, test 8) can now pin
the SAFETY channel's reported temperature independently of every zone's own
truth/MAIN-reported values via either command above — the owner-API gap this
section used to document here is closed (`sim_engine.h`'s
`sim_engine_force_safety_temp()` / `sim_engine_clear_safety_manual()`).

**`INJECT_FAULT`** request: `[u16 slot_id, u8 channel, u8 fault_kind, f32 param0]`
— `fault_kind` is `fault_sched_fault_type_t`'s TC-only subset (`0`
`TC_DISCONNECTED` .. `5` `TC_SPURIOUS_FAULT_PIN`). Deliberately routes
through `fault_sched_schedule()` (`MANUAL` trigger, `PERMANENT` duration,
`ONCE` repeat) + `fault_sched_fire_now()` rather than writing
`tc_fault_state_write()` directly: `tc_fault_state.h`'s own header names
`fault_sched.c` as the state's **sole writer**, which "recomputes it from
scratch every tick from the active fault slot set" — a direct write from
`cmd_task` would be silently clobbered on `fault_sched`'s very next
evaluation pass. Reply: `[status, u16 slot_id]` (echoed so the client can
`TC_CLEAR_FAULT` it later). **`CLEAR_FAULT`** request: `[u16 slot_id]` →
`fault_sched_cancel()`.

### 5.3 CT group (`SIMFW_TASK_ID_CT` = 4) — `wave_owner.h`

Channel is `0..CT_WAVE_NUM_CHANNELS-1` (3).

| `SIMFW_CMD_CT_*` | Value | Owner call |
|---|---|---|
| `SET_MODE` | `0x01` | `ct_wave_set_mode()` |
| `SET_AMPS` | `0x02` | `ct_wave_set_amps()` |
| `SET_DISTORTION` | `0x03` | `ct_wave_set_distortion()` |
| `GET_STATE` | `0x04` | `ct_wave_get_state()` |
| `SET_PHASE` | `0x05` | `ct_wave_set_phase()` — not in DESIGN_NOTES.md 5's original sketch; wired up because `wave_owner.h` exposes it as a first-class public setter |

`SET_MODE` request: `[u8 channel, u8 mode]` (`ct_wave_mode_t`: 0 MODEL, 1
MANUAL). `SET_AMPS`: `[u8 channel, f32 amps]`. `SET_PHASE`:
`[u8 channel, f32 phase_deg]`. `SET_DISTORTION`:
`[u8 channel, f32 dc_offset, f32 clip_fraction, u8 dropout_half_cycle,
u8 dropout_negative_half, u8 apply_immediately]`. `GET_STATE` request:
`[u8 channel]`; reply:

```
byte0      status
byte1      mode (ct_wave_mode_t)
byte2-5    f32 amps
byte6-9    f32 phase_deg
byte10-13  f32 distortion.dc_offset
byte14-17  f32 distortion.clip_fraction
byte18     distortion.dropout_half_cycle (0/1)
byte19     distortion.dropout_negative_half (0/1)
byte20     distortion.apply_immediately (0/1)
byte21-24  f32 last_pwm_scale
byte25     valid (0/1)
```

### 5.4 RELAY group (`SIMFW_TASK_ID_RELAY` = 5) — `i2c_owner.h`

| `SIMFW_CMD_RELAY_*` | Value | Owner call |
|---|---|---|
| `GET_STATES` | `0x01` | `i2c_owner_get_relay_states()` |
| `GET_EDGES` | `0x02` | `i2c_owner_get_relay_edges()` |

`GET_STATES` request: none. Reply:
`[status, k1_closed, k2_closed, k3_closed, k5_closed, k4_closed,
fault_line_asserted, u64 sample_time_us, valid]` (each flag a 0/1 byte).

`GET_EDGES` request: `[u32 since_seq, u8 max_count]` — `max_count` is
clamped server-side to `SIMFW_RELAY_EDGES_MAX_PER_REPLY` (8, so a full reply
of `status + count + 8 * 14-byte entries` = 114 bytes always fits the
128-byte frame). Reply: `[status, u8 returned_count, returned_count *
{u32 seq, u8 signal, u8 level, u64 time_us}]` — `signal` is
`i2c_owner_signal_t` (0 K1, 1 K2, 2 K3, 3 K5, 4 K4, 5 FAULT_LINE). A client
wanting more than 8 polls again with `since_seq` set to the last entry's
`seq` (`i2c_owner.h`'s own documented pagination contract).

**`RELAY_SET_CONTACT_FAULT`** (DESIGN_NOTES.md section 5's sketch) is **not
allocated**: `i2c_owner.h` exposes no such setter by design (relay sense is
read-only from this task's perspective; a "welded contact" is modeled at
`sim_engine`'s duty-override level). The FAULT group's `WELDED_RELAY` /
`STUCK_OPEN_RELAY` types (`FAULT_SCHEDULE`, section 5.6) are the real path.

### 5.5 IO group (`SIMFW_TASK_ID_IO` = 6) — `i2c_owner.h`

| `SIMFW_CMD_IO_*` | Value | Owner call |
|---|---|---|
| `SET_DIR` | `0x01` | `i2c_owner_io_set_dir()` |
| `WRITE` | `0x02` | `i2c_owner_io_write()` |
| `READ` | `0x03` | `i2c_owner_io_read()` |
| `ESTOP_SET` | `0x04` | `i2c_owner_set_estop()` |
| `FAULT_LINE_GET` | `0x05` | `i2c_owner_get_relay_states()` (`.fault_line_asserted`) |
| `DUT_POWER_SET` | `0x06` | `i2c_owner_set_dut_power()` (deprecated alias for `_set_dut_power_main()`) |
| `ESTOP_GET` | `0x07` | `i2c_owner_get_estop_open()` |
| `DUT_POWER_GET` | `0x08` | `i2c_owner_get_dut_power_on()` (deprecated alias for `_get_dut_power_main_on()`) |
| `DUT_POWER_SAFETY_SET` | `0x09` | `i2c_owner_set_dut_power_safety()` |
| `DUT_POWER_SAFETY_GET` | `0x0A` | `i2c_owner_get_dut_power_safety_on()` |

`DUT_POWER_SET` is DESIGN_NOTES.md section 3.4's addition (the DUT 12 V power relay
was added to the plan after section 5's original command table was written)
— placed in the IO group per this pass's instructions. `ESTOP_GET`/
`DUT_POWER_GET` are not in DESIGN_NOTES.md 5's original sketch either, but
`i2c_owner.h` exposes both getters as first-class public API and a client
otherwise has no way to read back what it last commanded, so they are wired
up too.

**Two independent DUT-power relays (resolved 2026-08-20, `docs/HARDWARE.md`
section 3.7 / `docs/BOM.md` section 6):** the main board has two electrically
separate 12 V inputs — J18 (main domain) and J19 (safety domain) — with no
shared copper downstream, so a single relay switching both would bond
`GND_Main` and `GND_Safty` through the shared return path and defeat the
isolation the rest of the fixture preserves. The fixture therefore drives two
independent relays from two independent MCP23017 output bits
(`EXP1_PIN_DUT_POWER_MAIN` = exp1 pin 7, `EXP1_PIN_DUT_POWER_SAFETY` = exp1
pin 10). `DUT_POWER_SET`/`GET` (`0x06`/`0x08`) are kept exactly as they were
before relay #2 existed — **main domain only**, not "both relays" — because
redefining a single legacy command to gang both domains by default would
quietly reintroduce the exact bonding failure two relays exist to prevent.
`DUT_POWER_SAFETY_SET`/`GET` (`0x09`/`0x0A`) are the new, explicit commands
for the safety-domain relay. **There is no "set both" command**; a scenario
that wants both domains powered issues both commands, so the decision to
power two domains together is always visible at the call site, never
implicit in one command's default behavior.

`SET_DIR` request: `[u8 exp, u8 pin, u8 input, u8 pullup]` (`exp`: 0 =
`I2C_OWNER_EXP_1` (0x20), 1 = `I2C_OWNER_EXP_2` (0x21); `pin` 0..15, 0..7 =
port A, 8..15 = port B). `i2c_owner_io_set_dir()` reports which of two
distinct causes made it return `false` via its `out_status` param
(`i2c_owner_io_set_status_t`, `i2c_owner.h`): a reserved fixed-role exp1 pin
(relay sense / fault-line sense / E-stop drive / DUT-power relay main, pins
0..7 on `I2C_OWNER_EXP_1`, plus DUT-power relay safety on pin 10) maps to
`ERR_BAD_ARGS` (permanent — retrying never helps); a transiently full
command queue maps to `ERR_BUSY` (i2c_owner's task drains it on its next
~8 ms scan tick, so an immediate retry will likely succeed). `WRITE`:
`[u8 exp, u8 pin, u8 level]`, same `ERR_BAD_ARGS`/`ERR_BUSY` distinction via
`i2c_owner_io_write()`'s `out_status`. `READ`: `[u8 exp, u8 pin]`, reply
`[status, level]`. `i2c_owner_io_read()` reports `ERR_BAD_ARGS` for an
out-of-range/reserved pin exactly as above, but `ERR_NO_SAMPLE` (section 4's
Reply-convention table) if the pin itself is fine and no expander has ever
ACKed a scan on that bus yet — the common bench case with no MCP23017
attached to J20. (`READ`'s third case is `NO_SAMPLE`, not `QUEUE_FULL`,
because reads don't go through the command queue at all — they're a direct
mutex-guarded snapshot read, so `SET_DIR`/`WRITE` and `READ` need different
status codes for their respective third cases.)

`ESTOP_SET`: `[u8 open]` (1 = loop opened/tripped). `ESTOP_GET`: no args,
reply `[status, open]`. `FAULT_LINE_GET`: no args, reply
`[status, asserted, u64 sample_time_us, valid]`. `DUT_POWER_SET`: `[u8 on]`
(main domain only). `DUT_POWER_GET`: no args, reply `[status, on]` (main
domain only). `DUT_POWER_SAFETY_SET`: `[u8 on]`. `DUT_POWER_SAFETY_GET`: no
args, reply `[status, on]`.

### 5.6 FAULT group (`SIMFW_TASK_ID_FAULT` = 7) — `fault_sched.h`

| `SIMFW_CMD_FAULT_*` | Value | Owner call |
|---|---|---|
| `SCHEDULE` | `0x01` | `fault_sched_schedule()` |
| `CANCEL` | `0x02` | `fault_sched_cancel()` |
| `LIST` | `0x03` | `fault_sched_list()` |
| `FIRE_NOW` | `0x04` | `fault_sched_fire_now()` |
| `SET_UNTIL_TRIGGER` | `0x05` | `fault_sched_schedule()` (frame 2 of the `UNTIL_TRIGGER` two-frame design, see below) |

**`SCHEDULE`** request is a compact re-encoding of `fault_engine.h`'s
`fault_trigger_t`/`fault_duration_t`/`fault_repeat_t` — **not** DESIGN_NOTES.md
5.2's original `{u8 kind, f32 a, f32 b, u8 zone/relay}` sketch, which
predates `fault_engine.h` and cannot address an `AFTER_FAULT` slot id (needs
16 bits, PLAN.md's sketch only offers 8) or carry `ON_EVENT`'s name string
at all:

```
u16      slot_id
u8       fault_type        (fault_sched_fault_type_t, 0..22 -- the full
                             catalog, fault_sched.h; up through
                             FAULT_SCHED_TYPE_DUT_POWER_CUT)
u16      target             (zone / tc_fault_channel_t / system-target,
                             per fault_type -- fault_sched_schedule() itself
                             validates the pairing)
-- ARM trigger (fault_trigger_kind_t, 0..6) --
u8       trigger_kind
f64      trigger_a          (AT_SIM_TIME: at_sim_time_s; AT_ZONE_TEMP: temp_c;
                             ON_RELAY_EDGE/ON_EVENT/AFTER_FAULT: delay_s;
                             RANDOM_IN: random_t0_s; else unused)
f64      trigger_b          (RANDOM_IN: random_t1_s; else unused)
u16      trigger_ref         (AT_ZONE_TEMP: zone; ON_RELAY_EDGE: relay;
                              AFTER_FAULT: after_fault_slot; else unused)
u8       trigger_edge         (AT_ZONE_TEMP: temp_edge 0=rising/1=falling;
                               ON_RELAY_EDGE: relay_edge 0=close/1=open; else 0)
char[24] event_name           (ON_EVENT only, NUL-padded ASCII; else ignored)
-- duration (fault_duration_kind_t) --
u8       duration_kind         (0 PERMANENT, 1 FOR, 2 UNTIL_TRIGGER -- see
                                the two-frame design below; duration_for_s
                                is ignored when duration_kind == 2)
f64      duration_for_s
-- repeat (fault_repeat_kind_t, 0..2) --
u8       repeat_kind
f64      repeat_period_s
f64      repeat_jitter_s
u16      repeat_n
-- params --
f32      param0, param1, param2, param3
```

94 bytes total (incl. the `0x01` cmd_id byte), comfortably under the
128-byte frame payload cap. Reply: `[status, u16 slot_id echo]`.

`fault_type`'s valid range was previously documented (and enforced) as
`0..12`, matching the catalog's size before the sim_engine/fault_sched
gap-closure pass added `MAIN_SAFETY_DISAGREE`, `WELDED_K4_CURRENT_PERSIST`,
`THERMAL_MASS_SURPRISE`, `TC_LAG_STRESS`, and `DUT_POWER_CUT`. `cmd_task.c`'s
bound check was never updated to match and used
`FAULT_SCHED_TYPE_AMBIENT_SHIFT` (value 19) as the assumed last value, so
`THERMAL_MASS_SURPRISE`/`TC_LAG_STRESS`/`DUT_POWER_CUT` (20/21/22) were
silently rejected via `FAULT_SCHEDULE` despite being fully implemented and
reachable through `fault_sched_schedule()` directly. Fixed this pass: the
bound is now `FAULT_SCHED_TYPE_DUT_POWER_CUT`, the catalog's actual last
value.

**`UNTIL_TRIGGER` — two-frame design (gap-closure pass):** `duration_kind ==
2` needs `fault_duration_t.until_trigger`, a second, full `fault_trigger_t`
(`fault_engine.h`: "evaluated the same way as a top-level trigger") — the
same ~44-byte shape the ARM trigger above already spends. Doubling that
inside one `FAULT_SCHEDULE` frame does not fit `BENCHPROTO_FRAME_MAX_PAYLOAD`
(128 bytes: 94 already spent on the `PERMANENT`/`FOR` case, +44 more = 138),
and reserving that headroom in every request just to serve the rare
`UNTIL_TRIGGER` case would tax the common one. Instead, `UNTIL_TRIGGER` is
split across two frames:

1. **`FAULT_SCHEDULE`** with `duration_kind == 2`: `fault_type`/`target`/the
   ARM trigger/`repeat`/`params` are captured immediately (firmware-side,
   `cmd_task.c`'s `s_pending_until[slot_id]`, indexed directly by `slot_id`
   — valid range `0..FAULT_ENGINE_MAX_SLOTS-1` (31), the same range the slot
   pool itself uses) but the slot is **not armed yet** in `fault_sched`.
   `duration_for_s` is ignored. Reply: `[status, u16 slot_id echo]`, same as
   any other `FAULT_SCHEDULE` — a client cannot yet distinguish "armed" from
   "parked, awaiting its release trigger" from this reply alone; poll
   `FAULT_LIST` (state stays `IDLE` until step 2 completes) if that
   distinction matters.
2. **`SET_UNTIL_TRIGGER`** (request: `[0x05, u16 slot_id, <trigger encoding,
   byte-identical to `FAULT_SCHEDULE`'s own ARM-trigger fields above: u8
   trigger_kind, f64 trigger_a, f64 trigger_b, u16 trigger_ref, u8
   trigger_edge, char[24] event_name>]`, 47 bytes total incl. cmd_id) —
   supplies the RELEASE trigger and performs the actual
   `fault_sched_schedule()` call combining it with the parked fields from
   step 1. Only now does the slot become `ARMED`. Reply: `[status, u16
   slot_id echo]`. `ERR_BAD_ARGS` if `slot_id` has no pending `UNTIL_TRIGGER`
   schedule (step 1 was never sent for it, it was already consumed by a
   prior `SET_UNTIL_TRIGGER`, or `FAULT_CANCEL` discarded it — see below).

A `PERMANENT`/`FOR` `FAULT_SCHEDULE` on a `slot_id` that still has a pending,
uncompleted `UNTIL_TRIGGER` parked on it discards the stale pending entry.
`FAULT_CANCEL` on a `slot_id` with a pending `UNTIL_TRIGGER` also discards
it (nothing is armed in `fault_sched` yet to cancel at the engine level for
that case, but the parked intent should not survive an explicit cancel).

**PC-side follow-up needed:** `tools/PcTools/src/kilnsim/payloads.py`'s
`FAULT_SCHEDULE` encoder is unaffected for `PERMANENT`/`FOR` (identical wire
shape, `fault_type`'s valid range widened per above). A new
`FAULT_SET_UNTIL_TRIGGER` encoder (cmd_id `0x05` on the FAULT group, request
shape above) is needed to actually drive `UNTIL_TRIGGER` from the PC side —
out of scope for this pass (`payloads.py` is owned separately).

**`CANCEL`** request: `[u16 slot_id]` → `fault_sched_cancel()` (also
discards a pending `UNTIL_TRIGGER` on that slot, see above). **`FIRE_NOW`**
request: `[u16 slot_id]` → `fault_sched_fire_now()`.

**`LIST`** request: `[u8 start_index, u8 max_count]` — pagination over the
fixed 32-slot pool (`fault_sched_list()` itself always returns from index 0);
`max_count` clamped to `SIMFW_FAULT_LIST_MAX_PER_REPLY` (8). Reply:
`[status, u8 returned_count, returned_count * {u16 slot_id, u8 state,
u16 fault_type, u16 target, u32 fire_count, f32 active_since_s}]` — `state`
is `fault_slot_state_t` (0 IDLE, 1 ARMED, 2 ACTIVE, 3 EXPIRED).

**Extending this file:** the same discipline section 4's closing note asked
of SYS applies here — when a new command id lands, add it to `cmd_ids.h`,
its handler + `commands[]` entry to `cmd_task.c`, and update the relevant
subsection above in the same commit.

## 6. EVT group (`SIMFW_TASK_ID_EVT` = 8) — TELEMETRY + EVT broadcast frames

DESIGN_NOTES.md section 5.3 describes two unsolicited frame shapes — a periodic
TELEMETRY frame and a per-event EVT frame — both driven by `telemetry.c`
(this pass's real body), both sent as `BENCHPROTO_MSG_BROADCAST` frames
(never ACKed, NACKed, or deduped, BENCHPROTO.md sec 4) with `src_task =
SIMFW_TASK_ID_EVT`, `dst_device = SIMFW_DEVICE_HOST`, `dst_task = 0` (no
specific registered receiver task for unsolicited traffic — see
`usb_owner_send_broadcast()`'s own doc comment, `usb_owner.h`). This is not
a request/reply command group and never will be: `SIMFW_TASK_ID_EVT` is
still registered as a `benchproto` task purely so a stray inbound frame
addressed to it gets a defined NACK (unroutable) rather than silently
vanishing.

Both frame shapes share one source task id and are told apart by **byte 0
of the payload**, `SIMFW_EVT_FRAME_KIND_*` (`cmd_ids.h`):

### TELEMETRY frame (`SIMFW_EVT_FRAME_KIND_TELEMETRY` = `0x01`)

Periodic, default `TELEMETRY_DEFAULT_RATE_HZ` = 2 Hz, rate settable via
`telemetry_set_rate_hz()` (`telemetry.h`) — no `SIMFW_CMD_*` sets it over
the wire yet (a future SYS or MODEL command calls that function once one
exists, per DESIGN_NOTES.md sec 5.3 "rate settable"). All multi-byte fields
little-endian (section 2's convention), `zone_count` copied verbatim from
`sim_snapshot_t` (`src/sim/sim_snapshot.h`, 1–`SIM_SNAPSHOT_MAX_ZONES`):

```
byte0        frame_kind = SIMFW_EVT_FRAME_KIND_TELEMETRY
byte1-8      sim_time_us, u64 LE          -- sim_snapshot_t.sim_time_us
byte9-12     timescale_x100, u32 LE       -- sim_snapshot_t.timescale_x100
byte13-16    seed, u32 LE                 -- sim_engine_get_seed(); 0 until a client
                                              sends SYS_SET_SEED (section 4)
byte17       zone_count, u8               -- sim_snapshot_t.zone_count
byte18..+16N per-zone, N = zone_count, 16 bytes each:
               f32 LE T_true_c
               f32 LE T_tc_reported_c
               f32 LE T_safety_reported_c
               f32 LE I_amps (current_a)
next+0..1    relay_mask, u16 LE           -- sim_snapshot_t.relay_mask
next+2       estop_open, u8 (0/1)         -- sim_snapshot_t.estop_open
next+3       fault_line_asserted, u8 (0/1)-- i2c_owner_get_relay_states().fault_line_asserted
next+4..5    active_fault_count, u16 LE   -- count of fault_sched_list() slots in FAULT_STATE_ACTIVE
next+6..9    spi_transactions_total, u32 LE -- sum of spi_emu_a/b_get_stats().transactions, all 4 channels
next+10..13  spi_underruns_total, u32 LE    -- sum of ...get_stats().first_byte_late, all 4 channels
next+14..17  evt_ring_high_water_mark, u32 LE -- telemetry_get_evt_ring_high_water_mark()
next+18..21  evt_seq_gap_count, u32 LE       -- telemetry_get_evt_seq_gap_count()
next+22..25  evt_send_drop_count, u32 LE     -- telemetry_get_send_drop_count()
```

Fixed overhead (everything but the per-zone blocks) is 44 bytes; at
`SIM_SNAPSHOT_MAX_ZONES` = 4 the frame is 108 bytes, comfortably under
`BENCHPROTO_FRAME_MAX_PAYLOAD` (128).

### EVT frame (`SIMFW_EVT_FRAME_KIND_EVENT` = `0x02`)

One frame per `sim_event_ring_drain()` entry (`src/sim/sim_snapshot.h`),
forwarded promptly — drained on every pass of `telemetry`'s internal poll
loop (`TELEMETRY_EVT_POLL_MS`, `telemetry.c`), independent of and much
faster than the TELEMETRY frame's own rate, so an event is never held back
behind a slow-rate telemetry period (DESIGN_NOTES.md sec 5.3: "unsolicited,
immediate"):

```
byte0        frame_kind = SIMFW_EVT_FRAME_KIND_EVENT
byte1-4      seq, u32 LE          -- sim_event_t.seq
byte5-12     sim_time_us, u64 LE  -- sim_event_t.sim_time_us
byte13       event_type, u8       -- sim_event_t.type verbatim (sim_event_type_t,
                                      sim_snapshot.h's own 1:1 wire-mapping guarantee)
byte14       a, u8                -- sim_event_t.a
byte15       b, u8                -- sim_event_t.b
byte16-19    f0, f32 LE           -- sim_event_t.f0
```

20 bytes total, fixed size.

### Loss visibility (DESIGN_NOTES.md sec 5.3: "the PC's report generator refuses to
certify a run with a sequence gap")

Every EVT frame's own `seq` is enough for a PC client to detect loss on its
own (a gap between consecutive received `seq` values) — the TELEMETRY
frame's `evt_seq_gap_count` and `evt_send_drop_count` fields above exist so
that visibility does not depend on a client reconstructing it from the EVT
stream alone:

- `evt_seq_gap_count` increments whenever `sim_event_ring_drain()` itself
  reports the ring wrapped past what `telemetry` last drained (the ring
  outran the drain loop) — a real, unrecoverable loss of event data.
- `evt_send_drop_count` increments whenever `usb_owner_send_broadcast()`
  fails for a TELEMETRY or EVT frame (CDC TX path busy/short) — the event
  data existed and was built into a frame, but the frame itself never made
  it onto the wire.

### Known gaps

None outstanding for this frame. (Previously: "`seed` is always 0" —
`telemetry.c` now calls `sim_engine_get_seed()`, wired in the same
gap-closure pass that implemented `SYS_SET_SEED`, section 4.)

## 7. Known simplifications (this pass)

- **Reply-in-ACK, one outstanding request at a time.** `usb_owner` enqueues
  a delivered `DATA` frame to `cmd_task`'s inbox and returns immediately
  (it does not block waiting for the reply) — `cmd_task` calls
  `usb_owner_send_reply()` once it has an answer, from its own task
  context. There is no per-request matching beyond `cmd_task`'s inbox being
  a plain FIFO: since every SYS handler today is a small, synchronous,
  non-blocking computation, replies are for all practical purposes
  processed in the order requests arrive. A future group with a
  slower/blocking handler must preserve that property (DESIGN_NOTES.md section
  4.5's "nothing ever blocks" rule extends to `cmd_task`'s own handlers) or
  this ordering assumption needs revisiting.
- **Duplicate-ACK cache is one slot per task id**, not per outstanding
  request. If a single task id ever needs more than one request in flight
  at a time (it doesn't today — the host's own single-outstanding-per-link
  discipline, BENCHPROTO.md section 3, already limits this in practice), a
  `BENCHPROTO_LINK_ACTION_DUPLICATE_REACK` could resend the wrong cached
  answer. Documented in `usb_owner.c`'s own comment on
  `USB_OWNER_ACK_CACHE_SLOTS`.
- **No inbound BROADCAST consumer.** `usb_owner_handle_deliver()` drops any
  BROADCAST frame delivered to a registered task (there is nothing to hand
  it to yet); outbound BROADCAST (TELEMETRY/EVT, section 6, `telemetry.c`)
  is the only BROADCAST traffic this build actually sends or expects.
