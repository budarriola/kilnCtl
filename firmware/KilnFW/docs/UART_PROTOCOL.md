# Hardened UART protocol — wire format reference (protocol version 5)

The ESP32-S3 firmware and the PC-side `pc_tools` package talk to each other
over a single UART link using a custom, reliable, addressed message protocol
built on top of `uart_owner` (the raw serialized-transaction layer).

**Version 2** is the kilnCtl main board. The framing, CRC and ACK layer are
byte-identical to version 1; what changed is the task table. The unit-test
fixture's devices (MCP4728 DAC = 1, AD9833 = 2, SSD1306 OLED = 4, PCF8575 = 7)
are gone and those `task_id`s are reused for this board's hardware. INFO (3),
LOG (5) and SYSTEM (6) keep both their numbering and their payloads.

**Version 3** (2026-08-11): `UART_PROTO_MAX_PAYLOAD` raised 128 → 253 (the
LENGTH header field is one byte, so 255 is the hard ceiling; 253 keeps
`DISPLAY_BLIT_CHUNK_PIXELS` an exact pixel count). Everything else about the
frame layout is unchanged — bumped anyway so a v2 peer's smaller receive
buffer can't silently truncate a v3 sender's larger frame. Motivated by the
DISPLAY blit path: it's strict stop-and-wait (see Reliability below), so the
per-frame ACK round trip, not raw baud, dominates a full-screen fill's time;
halving the frame count roughly halves that. The PC-link baud rate was also
raised 115200 → 921600 the same day (`KILNCTL_UART_BAUD_RATE`) for the same
reason, independent of this version bump.

The *same* framing also carries the isolated link between the ESP and the
RP2040 safety processor, on a second UART with `UART_PROTO_DEVICE_SAFETY` as
the peer — see [`docs/SAFETY_LINK.md`](SAFETY_LINK.md).

**Version 4** (2026-08-13): four new task_ids — `CONTROL` (8), `PROFILES` (9),
`AUTOTUNE` (10), `WIFI` (11) — plus a `FACTORY_RESET` subcommand on `SYSTEM`
(6), so a PC-side GUI can drive everything the HTTP dashboard offers
(`App/drivers/http/dashboard_http.c`, `zones_http.c`, `rules_http.c`,
`profiles_http.c`, `wifi_provision_http.c`, `factory_reset.c`) without
needing Wi-Fi — additive to HTTP, not a replacement. Every new command
mirrors an existing HTTP endpoint's request/response *fields*, not its
HTTP-specific plumbing (JSON/form-encoding, status codes); see each task's
doc comment in `App/drivers/common/uart_task_ids.h` (the frozen contract, and the
authoritative byte-level reference — this file summarizes it) for exact
payload layouts. Two deliberate scope caps, both because the 253-byte
payload ceiling has no honest way around them:
- **Zone config** (task `CONTROL`): only the fields a live control loop
  needs to read/retune (PID gains, plant model, ramp/temp ceilings) are
  exposed. `/api/zones`' whole-page fields — zone naming, relay assignment,
  heater window timing, the cross-zone guard threshold — stay HTTP-only;
  that endpoint validates ~20 fields per zone together and does not
  decompose into a safe per-field wire write.
- **Autotune bulk data**: `/api/autotune/matrix` (the cross-zone coupling
  matrix + RGA) and `/api/autotune/trace.csv`/`history.csv` (raw sample
  dumps) are HTTP-only — table/CSV data that doesn't fit one frame and has
  no meaningful truncated form (a partial coupling matrix is misleading, not
  merely incomplete).

**Version 5** (2026-08-17): one new task_id, `TOUCH` (13), for the NS2009
touch controller on the display panel (J2) — `GET_STATE` (screen on/off +
idle milliseconds) and `INJECT` (a fire-and-forget synthetic touch). The
firmware's `screen_idle` state machine (`App/drivers/ui/screen_idle.c`) treats
an injected touch exactly like a real NS2009 press: it resets the auto-blank
idle timer (`CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS`, default 60s) and wakes
the panel if it's currently blanked — this is how `pc_tools`' MCP server can
drive/test the UI ("send a touch as if from the screen") without physical
hardware. Existing task_ids 1-12 and their payloads are unchanged.

Full profile-execution history stays HTTP-only: it is a 2880-sample ring
buffer with no fixed-size UART encoding. (The relay rule DSL this paragraph
used to mention alongside it — `GET`/`POST /api/rules` — was removed
2026-08-27 along with `rules_http.c`; see `docs/PROFILES.md` and
`docs/WEB_UI.md`.)

Source of truth:
- Firmware: `App/drivers/owners/uart_protocol.h` / `.c`,
  `App/drivers/common/uart_task_ids.h` (the frozen contract),
  `App/drivers/bridge/uart_bridge.c` (the bridges that implement it)
- PC: `pc_tools/src/`

## Physical layer

UART0, 921600 baud (both Kconfig-configurable — `KILNCTL_UART_*`), 8N1, no
flow control, on GPIO43/44. Those are the ESP32-S3-DevKitC's own UART0 pins,
routed through the module's dedicated USB-UART bridge and its "UART" USB-C
port — **not** the native USB-Serial-JTAG port used for flashing/debugging.
The main board leaves both unconnected, so this link belongs to the dev board
alone (see `docs/HARDWARE.md`).

## Framing (SLIP-style byte stuffing)

```
DELIM ( ...stuffed bytes... ) DELIM
```

- `DELIM = 0x7E` marks both the start and end of a frame. Back-to-back
  delimiters are just empty-frame noise and are ignored, which is also how a
  receiver resyncs after garbage on the line: any byte before the first
  `DELIM` is discarded.
- `ESC = 0x7D`, `ESC_XOR = 0x20`. Any raw byte equal to `DELIM` or `ESC` is
  stuffed as `ESC (byte ^ ESC_XOR)`.

## Raw (unstuffed) frame layout

All **header** multi-byte fields are big-endian.

| Offset | Field | Size | Notes |
|---|---|---|---|
| 0 | `MSG_TYPE` | 1 | `DATA=0x01`, `ACK=0x02`, `NACK=0x03` |
| 1–2 | `MSG_INDEX` | 2 (BE) | sender-assigned; reused verbatim on retransmit |
| 3 | `SRC_DEVICE` | 1 | `ESP=0`, `HOST=1`, `SAFETY=2` |
| 4 | `SRC_TASK` | 1 | |
| 5 | `DST_DEVICE` | 1 | |
| 6 | `DST_TASK` | 1 | |
| 7 | `LENGTH` | 1 | payload length, 0–253 (255 is the byte's hard ceiling; `UART_PROTO_MAX_PAYLOAD` caps it at 253) |
| 8..8+LEN-1 | `PAYLOAD` | LEN | |
| 8+LEN..+1 | `CRC16` | 2 (BE) | CRC-16/CCITT-FALSE over bytes `[0, 8+LEN)` |

CRC-16/CCITT-FALSE: poly `0x1021`, init `0xFFFF`, no reflection, no xorout.
Check value: `crc16(b"123456789") == 0x29B1`.

Note the deliberate endianness split: header fields are big-endian, but the
multi-byte fields *inside* command payloads (below) are little-endian —
that's the natural layout for a `memcpy` straight into an ESP32 `float`, or a
Python `struct.pack("<...")`. Don't "fix" this asymmetry. (The SX1509's own
registers are big-endian pairs, but that conversion lives inside `SX1509.c`
and never reaches the wire.)

## Reliability

- A sender picks the next `MSG_INDEX`, transmits a `DATA` frame, and blocks
  waiting for a reply (`ACK`/`NACK`) whose `SRC_DEVICE`/`SRC_TASK` matches
  the `DST_DEVICE`/`DST_TASK` it sent to, and whose `MSG_INDEX` matches.
- On timeout it retransmits the **same** `MSG_INDEX`, up to
  `UART_PROTO_MAX_RETRIES = 10` total attempts.
- Only one outstanding send-and-await-ack cycle is allowed at a time per
  side (serialized with a lock), so replies can never be cross-matched
  between concurrent senders.
- The receiver dedups: a retransmitted `DATA` frame (the receiver's own
  prior `ACK` was lost) is recognized via a small ring of recently-accepted
  `(src_device, src_task, msg_index)` tuples per registered task, and gets
  re-ACKed **without** being delivered to the application a second time.
- If the destination `task_id` isn't registered, the receiver replies
  `NACK` ("undeliverable") instead of silently dropping the frame.
- The dedup ring lives as long as the receiver does, and has no notion of the
  peer having restarted. A host that began every session at `MSG_INDEX` 0
  would therefore have its first sends mistaken for retransmits of the
  *previous* session's and re-ACKed **without delivery** — which on a query
  task looks like "request was ACKed but no reply arrived". `pc_tools`
  randomizes its starting `MSG_INDEX` per connection to avoid this; the
  firmware's counter starts at 0 at boot, which is safe because a boot is also
  when its peer's rings are meaningless anyway.
- If a registered task's inbox is full, the receiver withholds the `ACK`
  (does not record the dedup entry either) so the sender's retry gives the
  task time to drain its queue — this doubles as natural backpressure.

Return values callers can expect from a send:

| Result | Meaning |
|---|---|
| `OK` | ACKed — delivered to the destination task's inbox |
| `UNDELIVERABLE` (NACK) | destination task not registered there |
| `TIMEOUT` | no reply after all retries — link or peer down |

**ACK means queued, not done.** The transport `ACK` above is generated by
`uart_protocol.c`'s RX task the instant a `DATA` frame lands in the
destination task's inbox — *before* that task's `switch (subcmd)` ever
runs. It proves delivery only. Do not treat `OK` from a send as proof a
mutating command executed, and do not add a new subcommand handler that
relies on the transport ACK being that proof: it has never meant that, and
a rejection path that falls through without an application-level reply
makes "queued" and "executed" indistinguishable to any caller that only
checks the transport ACK (`ROADMAP.md`'s "KilnFW PC-link command
acknowledgement" / `firmware/KilnFW/TODO.md` §11 — the fix that closed the
worst instance of this, a refused relay command looking identical to one
that switched, was commit `5df2190`). Every mutating subcommand handler in
`uart_bridge.c`/`uart_bridge_ext.c` must reply `{subcmd, ok, [reason]}` via
`bridge_reply_reject()`/`bx_reply_ok_err()` on every path that refuses or
fails, not just the happy path — see those functions' doc comments for the
exact wire shape. A handler that replies nothing at all on success (most
`SET_*` subcommands below still do, to keep the common case a single short
round trip) is fine as long as every *rejection* path from that same
handler does reply; what's not fine is a rejection path with no reply of
any kind.

## Task registration

Any task on either side can register a `task_id` (1 byte) to get an inbox
queue for messages addressed to it. Task IDs only need to be unique within
their own device (ESP task 1 and HOST task 1 are unrelated).

| task_id | Owner | Purpose | Driver |
|---|---|---|---|
| 1 | `UART_TASK_ID_THERMO` | 3x MAX31856 thermocouple channels on J6 | [`docs/MAX31856.md`](MAX31856.md) |
| 2 | `UART_TASK_ID_IO` | SX1509 expander: relays, digital I/O, `~DRDY`, raw registers | [`docs/SX1509.md`](SX1509.md) |
| 3 | `UART_TASK_ID_INFO` | device info queries (pin config, FW version) | — |
| 4 | `UART_TASK_ID_DISPLAY` | ILI9488 480x320 TFT on J2 | [`docs/ILI9488.md`](ILI9488.md) |
| 5 | `UART_TASK_ID_LOG` | firmware console output (`ESP_LOGx`), forwarded unsolicited | — |
| 6 | `UART_TASK_ID_SYSTEM` | link-recovery commands (restart) | — |
| 7 | `UART_TASK_ID_SAFETY` | isolated link to the RP2040 safety processor | [`docs/SAFETY_LINK.md`](SAFETY_LINK.md) |
| 8 | `UART_TASK_ID_CONTROL` | zone config (PID/model/read-back) + manual relay control | `uart_task_ids.h` |
| 9 | `UART_TASK_ID_PROFILES` | fire profile CRUD + execution control | `uart_task_ids.h` |
| 10 | `UART_TASK_ID_AUTOTUNE` | PID autotune (step/relay methods) | `uart_task_ids.h` |
| 11 | `UART_TASK_ID_WIFI` | Wi-Fi status/scan/provision/forget | `uart_task_ids.h` |
| 13 | `UART_TASK_ID_TOUCH` | NS2009 touch controller on J2 + screen auto-blank state | `uart_task_ids.h` |

The PC side registers the same numeric IDs for symmetry.

Manual relay control (mirroring `POST /api/diagnostics/danger/relay`,
formerly `POST /api/relay` before its 2026-08-27 removal) is **not** a
CONTROL subcommand — use IO's `SET_RELAY`/`SET_RELAY_MASK` (task 2, above),
which already gate through `relay_authority_on_blocked()`, the exact same
check that HTTP endpoint uses.

### Queries and unsolicited pushes

Two mechanisms sit on top of plain DATA frames, and both matter for how a PC
client must be structured:

- **Queries.** The subcommands marked *query* below are ACKed as usual
  (delivery confirmation only, no payload), and the actual answer arrives as a
  *separate* `DATA` frame from `(ESP, that task_id)` back to whichever
  `(device, task_id)` sent the request. **The requester must itself be
  registered on a `task_id` to receive it.** Every query response except the
  INFO ones echoes its subcommand in byte0 and is therefore self-describing.
- **Unsolicited pushes.** THERMO `SET_AUTO_REPORT` (0x08) and IO
  `SET_AUTO_REPORT` (0x06) switch on a periodic push of exactly the payload
  the matching `READ` query returns, addressed back to whoever sent the
  `SET_AUTO_REPORT`. IO additionally pushes immediately on every SX1509 `~INT`
  edge, so an input change is reported without waiting out the period. LOG is
  push-only. INFO pushes its version payload once at boot.

A failed operation is always logged on the device and forwarded over task
`LOG`. Whether it also produces a reply frame depends on the subcommand and
the task:

- Every mutating subcommand on THERMO/IO/SAFETY/DISPLAY/TOUCH that can be
  *refused before* touching hardware (truncated payload, out-of-range
  argument, relay refused for ownership/safety/OTA reasons) replies
  `{subcmd, ok=0, [reason]}` per the "ACK means queued, not done" rule
  above, so the PC does not have to wait out a timeout to learn a command
  was rejected.
- Every one of those five tasks additionally replies `{subcmd, ok=0,
  "driver error"}` when a subcommand reaches its driver call and fails
  *there* (e.g. a SPI/I2C transfer error, or a failed round trip to the
  RP2040 on SAFETY) — each task's bottom `if (err != ESP_OK)` block.
  `SAFETY_CMD_GET_CT_CAL`/`GET_PARAM`/`GET_CONFIG_PAGE` used to share their
  id with their own success reply, which structurally forbade this refusal
  (the host's exact-length check would misread it as a malformed success);
  moving those three requests onto their own ids (0x22/0x23/0x24,
  `KILNLINK_PROTOCOL_VERSION` 7) removed that block, so every subcommand on
  every task now replies the same way on a driver-call failure
  (`firmware/KilnFW/TODO.md` §11).
- `THERMO_CMD_READ_FAULTS` and `IO_CMD_SX_SCAN` reply `{subcmd, count}`
  where `count` can legitimately be 0 (an honest empty result). To keep
  that indistinguishable from nothing else, `bridge_reply_unsupported()`
  (the `default:` case for an unrecognized subcommand) replies with a real
  reason, `{subcmd, ok=0, len, "unsupported"}`, rather than the bare
  2-byte `{subcmd, 0}` it used to send — the two shapes can no longer
  collide (`firmware/KilnFW/TODO.md` §11).

## Command payloads

### THERMO (task 1) — `uart_bridge.c: thermo_bridge_task`

Three MAX31856 cold-junction-compensated thermocouple front ends on the J6
daughterboard, on the shared SPI bus with `CS0`/`CS1`/`CS2` = channels 0/1/2.
Each part's `~FAULT` is an ESP32-S3 GPIO; each part's `~DRDY` is an **SX1509
pin**, so DRDY state is only observable through the IO task's `READ`.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `CONFIG_CHANNEL` | byte1=channel(0-2), byte2=tc_type, byte3=avg_mode, byte4=filter(0=60Hz,1=50Hz), byte5=conv_mode(0=one-shot,1=auto) |
| `0x02` | `SET_THRESHOLDS` | byte1=channel, bytes2..5=tc_high `f32` LE °C, bytes6..9=tc_low `f32` LE, byte10=cj_high `i8` °C, byte11=cj_low `i8` |
| `0x03` | `SET_CJ_OFFSET` | byte1=channel, bytes2..5=offset `f32` LE °C (±8 °C range) |
| `0x04` | `ONE_SHOT` | byte1=channel — result is **not** returned; poll with `READ` |
| `0x05` | `READ` | byte1=channel(0-2) or `0xFF` for all — **query** |
| `0x06` | `READ_FAULTS` | byte1=channel or `0xFF` — **query** |
| `0x07` | `CLEAR_FAULTS` | byte1=channel |
| `0x08` | `SET_AUTO_REPORT` | byte1=channel mask (bit N = channel N), bytes2..3=period_ms `u16` LE (0 = off) |
| `0x09` | `READ_REG` | byte1=channel, byte2=reg_addr, byte3=len(1-16) — **query**, debug |
| `0x0A` | `WRITE_REG` | byte1=channel, byte2=reg_addr, byte3=value |

`tc_type` is `CR1.TC[3:0]`: `B=0x00 E=0x01 J=0x02 K=0x03 N=0x04 R=0x05 S=0x06
T=0x07`, plus the raw voltage modes `VMODE_G8=0x08`, `VMODE_G32=0x0C`.
`avg_mode` is `CR1.AVGSEL`: `1=0x00 2=0x01 4=0x02 8=0x03 16=0x04` samples.

The 50/60 Hz filter bit may only be changed while conversions are off, so
`CONFIG_CHANNEL` stops auto conversion, writes, and restores `conv_mode`.

**`READ` / auto-report response** — identical layouts, so a client parses one
thing:

```
byte0    0x05
byte1    count (N)
N * 12 bytes, one per channel:
  [0]     channel
  [1..4]  thermocouple temperature, f32 LE, degC (linearized, 19-bit)
  [5..8]  cold-junction temperature, f32 LE, degC
  [9]     fault status register (SR)
  [10]    flags: bit0 ~FAULT pin asserted (low)
                 bit1 SPI read failed
                 bit2 reading is stale (no conversion since last read)
  [11]    reserved, 0
```

A channel whose SPI read failed — or that never came up at boot — still
appears, with flags bit1 set and both temperatures NaN (`00 00 C0 7F`). A
missing channel is more confusing than an explicitly-bad one.

`SR` bits: `OPEN=0x01 OVUV=0x02 TCLOW=0x04 TCHIGH=0x08 CJLOW=0x10 CJHIGH=0x20
TCRANGE=0x40 CJRANGE=0x80`.

```
READ_FAULTS response:
  byte0    0x06
  byte1    count (N)
  N * 3 bytes: [0] channel, [1] SR, [2] MASK register

READ_REG response:
  byte0    0x09
  byte1    channel
  byte2    reg_addr
  byte3    len (N)
  N bytes  register contents
```

Example — configure ch0 as type K, 1 sample, 60 Hz, automatic; then read all
three:

```
01 00 03 00 00 01
05 FF
```

### IO (task 2) — `uart_bridge.c: io_bridge_task`

The SX1509 (U5, 0x3E) through the `kiln_io` board layer. Both a board-level
view (relays and I/O by their schematic names, **1-based**) and raw register
access are exposed; the board-level commands are what a GUI or control loop
should use. `~INT` → GPIO7, `~RESET` ← GPIO10.

Relay numbering is the schematic's `Relay1..Relay4`, which is the expander's
bit order and **not** the K-designator order: `Relay1`→K3/J8, `Relay2`→K1/J3,
`Relay3`→K2/J4, `Relay4`→K5/J11. See `docs/HARDWARE.md`.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `SET_RELAY` | byte1=relay(1-4), byte2=on(0/1) |
| `0x02` | `SET_RELAY_MASK` | byte1=mask (bits0-3 = relay1-4, which to change), byte2=value (bits0-3) — one atomic register write |
| `0x03` | `SET_IO` | byte1=io(1-7), byte2=level(0/1); only meaningful for an output |
| `0x04` | `SET_IO_DIR` | byte1=io(1-7), byte2=dir(0=output, 1=input), byte3=pullup(0/1, input only) |
| `0x05` | `READ` | none — **query** |
| `0x06` | `SET_AUTO_REPORT` | bytes1..2=period_ms `u16` LE (0 = off) |
| `0x07` | `ALL_RELAYS_OFF` | none — unconditional |
| `0x10` | `SX_WRITE_REG` | byte1=reg_addr, byte2=value |
| `0x11` | `SX_READ_REG` | byte1=reg_addr, byte2=len(1-16) — **query**, debug |
| `0x12` | `SX_SET_DIR` | bytes1..2 `u16` LE (bit N: 1 = input, matching `RegDir`) |
| `0x13` | `SX_SET_PULLUP` | bytes1..2 `u16` LE |
| `0x14` | `SX_SET_OPENDRAIN` | bytes1..2 `u16` LE |
| `0x15` | `SX_SET_DEBOUNCE` | bytes1..2=enable mask `u16` LE, byte3=config(0-7; 0.5 ms << config) |
| `0x16` | `SX_SET_INT_MASK` | bytes1..2=mask `u16` LE (1 = masked), bytes3..4=sense `u16` LE |
| `0x17` | `SX_LED_DRIVER` | byte1=pin(0-15), byte2=enable(0/1), byte3=intensity(0-255) |
| `0x18` | `SX_RESET` | byte1=hard(0 = `RegReset` software reset, 1 = pulse `~RESET`) |
| `0x19` | `SX_SCAN` | none — **query**: probes 0x3E/0x3F/0x70/0x71 |

`ALL_RELAYS_OFF` is kept as its own subcommand rather than a special case of
`SET_RELAY_MASK` precisely so it is one short frame that cannot be misparsed
as anything else. It is also the state the firmware falls back to on link loss
or a safety fault.

**`SET_RELAY` / `SET_RELAY_MASK` are refused, silently on the wire, if they
would turn any relay ON while a safety fault is asserted** (see
[`docs/SAFETY_MODEL.md`](SAFETY_MODEL.md)). "Silently on the wire" means: the
frame is still ACKed at the transport level (it was delivered to the task),
but no state changes and, like every other `SET_*` in this task, there is no
task-level reply either way. The only way to see that a command was refused
is that the next `READ` / auto-report still shows the relay off. Turning a
relay OFF, and `ALL_RELAYS_OFF`, are never refused.

The `sense` field of `SX_SET_INT_MASK` is 16 bits for 16 pins, i.e. **2 bits
per pin pair**: bits `[2p+1:2p]` give the mode for pins `2p` and `2p+1`
together (`0` none, `1` rising, `2` falling, `3` both). The firmware expands
each pair onto both of its pins before writing the part's four `RegSense`
registers, which are 2 bits per *pin*. Per-pin sense is therefore not
expressible over the wire; use `SX_WRITE_REG` against 0x14–0x17 if you need
it.

**`READ` / auto-report response** — again identical layouts:

```
byte0    0x05
bytes1-2 RegData, u16 LE, raw pin states (bit N = expander pin N)
bytes3-4 RegDir,  u16 LE (1 = input)
byte5    relay shadow, bits0-3 = relay1-4 as last commanded
byte6    digital I/O levels, bits0-6 = io1-io7
byte7    DRDY bits: bit0-2 = channel 0-2 ~DRDY asserted (pin is LOW)
byte8    flags: bit0 = ~INT currently asserted
                bit1 = last I2C transfer failed
```

The `relay shadow` is what was last *commanded*, kept separately from the pin
states so that a failed write, or a direct poke at `RegData` over the debug
subcommands, shows up as a divergence rather than being hidden.

```
SX_READ_REG response:
  byte0    0x11
  byte1    reg_addr
  byte2    len (N)
  N bytes  register contents

SX_SCAN response:
  byte0    0x19
  byte1    count (N)
  N bytes  the addresses that ACKed
```

Example — energize Relay1 (K3/J8), then turn Relay2 and Relay4 on and Relay3
off in a single register write, then start 250 ms reporting:

```
01 01 01
02 0E 0A
06 FA 00
```

### DISPLAY (task 4) — dead on the firmware side (removed 2026-08-27)

`uart_bridge.c`'s `display_bridge_task`/`uart_bridge_start_display_task()`
were confirmed dead code (never called from `main.c`; LVGL owns the panel
now) and deleted, along with the PC-side MCP `display_*` tools. The
subcommand layout below is kept for reference: `DISPLAY_CMD_*` and
`UART_TASK_ID_DISPLAY` (`uart_task_ids.h`) are still defined and still used
by kilnctrl's `gui.py`/`actions.py` Display panel, even though nothing on
this side answers any more — every call just times out.

ILI9488 480x320 SPI TFT on J2. `SCK`/`MOSI`/`MISO` are the shared SPI bus and
`CS3` is a real GPIO, but D/C and `~RESET` hang off the SX1509 — so every
command/data transition costs an I2C transfer. The driver batches hard: one
D/C toggle per command, then all of that command's data in one SPI
transaction. Do not expect per-pixel throughput; full-screen work should go
through `FILL_RECT`/`BLIT` rather than repeated small writes.

Colours are **RGB565 `u16` LE** on the wire; the driver expands to the 18-bit
RGB666 format the panel requires over SPI (see [`docs/ILI9488.md`](ILI9488.md)
for why RGB565 is not an option on this interface).

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `RESET` | byte1=hard(0 = software reset, 1 = pulse `~RESET` via the expander) |
| `0x02` | `SET_POWER` | byte1=on(0/1) — display off + sleep-in when 0 |
| `0x03` | `SET_ROTATION` | byte1=rotation(0-3); 0/2 portrait 320x480, 1/3 landscape 480x320 |
| `0x04` | `SET_INVERT` | byte1=invert(0/1) |
| `0x05` | `CLEAR` | bytes1..2=colour `u16` LE (whole screen) |
| `0x06` | `FILL_RECT` | bytes1..2=x, 3..4=y, 5..6=w, 7..8=h, 9..10=colour, all `u16` LE |
| `0x07` | `DRAW_RECT` | same args as `FILL_RECT` — 1 px outline |
| `0x08` | `DRAW_LINE` | bytes1..2=x0, 3..4=y0, 5..6=x1, 7..8=y1, 9..10=colour |
| `0x09` | `SET_TEXT_CURSOR` | bytes1..2=x, 3..4=y (pixels, glyph top-left) |
| `0x0A` | `SET_TEXT_STYLE` | bytes1..2=fg, 3..4=bg, byte5=size(1-8), byte6=opaque_background(0/1) |
| `0x0B` | `PRINT` | bytes1..(length-1)=ASCII, **not** null-terminated; cursor advances and wraps |
| `0x0C` | `BLIT_BEGIN` | bytes1..2=x, 3..4=y, 5..6=w, 7..8=h — opens a pixel window |
| `0x0D` | `BLIT_DATA` | bytes1..(length-1)=RGB565 pixels, `u16` LE, row-major, continuing where the last chunk stopped |
| `0x0E` | `BLIT_END` | none — closes the window |
| `0x0F` | `READ_ID` | none — **query** |

Coordinates are bounds-checked, never clipped: an out-of-range rectangle is
rejected outright rather than partially drawn. An odd `BLIT_DATA` length (a
split pixel), more pixels than the window holds, or anything other than
`BLIT_DATA`/`BLIT_END` while a blit is open is an error **and aborts the
blit** — a desynchronized stream would otherwise smear the rest of the image.

```
READ_ID response:
  byte0     0x0F
  byte1     ok (0/1) -- 0 if the read failed or the panel answered all-zero
  bytes2..4 the three ID bytes from RDDID (0x04)
  bytes5..6 width  u16 LE, as currently rotated
  bytes7..8 height u16 LE
```

`READ_ID` always answers, even for a panel that is not responding (`ok = 0`),
so it never times out on the PC side.

### SAFETY (task 7) — `uart_bridge.c: safety_bridge_task`

The PC's window onto the isolated link to the RP2040 safety processor.
The ESP polls the Pico over that link and caches the last good answer;
`GET_STATUS` returns **the cache**, never a blocking round trip, so a dead
link shows up as stale/invalid status rather than a hung request. Until the
Pico firmware exists, `link_up` is 0 and `age` is 65535.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `GET_STATUS` | none — **query** |
| `0x02` | `REQUEST_ENABLE` | byte1=enable(0/1) — advisory; the Pico's own interlocks always win |
| `0x03` | `PING` | none — forces an immediate poll instead of waiting for the next tick |
| `0x04` | `GET_LINK_STATS` | none — **query** |
| `0x05` | `SET_POLL_PERIOD` | bytes1..2=period_ms `u16` LE (0 = stop polling) |
| `0x06` | `SET_FAULT_OUT` | byte1=assert(0/1) — drives the isolated fault line (GPIO6) |

`SET_FAULT_OUT` is a manual override of a line the firmware otherwise asserts
on its own (a thermocouple fault, a watchdog, and — only if
`KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT` is turned on, default off — loss of the
PC link; see `docs/SAFETY_MODEL.md` §3). The line is high whenever *any*
source is set, so clearing the manual source cannot clear an automatic one.

```
GET_STATUS response:
  byte0       0x01
  byte1       flags: bit0 link_up (a valid reply within 3 poll periods)
                     bit1 Fault line currently asserted by this firmware
                     bit2 estop asserted (as reported by the Pico)
                     bit3 safety relay K4 energized
                     bit4 heating enable currently granted
                     bit5 safety thermocouple reading valid
  bytes2..5   safety thermocouple temperature, f32 LE, degC
  bytes6..9   safety cold-junction temperature, f32 LE, degC
  byte10      safety thermocouple fault status (same bits as THERMO's SR)
  bytes11..14 current sense 1, f32 LE, amps
  bytes15..18 current sense 2, f32 LE, amps
  bytes19..22 current sense 3, f32 LE, amps
  bytes23..24 age of this data, u16 LE, ms (65535 = never received)

GET_LINK_STATS response:
  byte0       0x04
  bytes1..4   frames sent,          u32 LE
  bytes5..8   frames received,      u32 LE
  bytes9..12  CRC/framing errors,   u32 LE
  bytes13..16 timeouts,             u32 LE
  bytes17..18 poll period, u16 LE, ms
```

When `TEMP_VALID` is clear the two temperatures are NaN, not 0 — an explicit
not-a-number is much harder to mistake for a cold kiln than a plausible zero.

### LOG (task 5) — `App/drivers/bridge/uart_log_bridge.c`

Firmware → PC only, unsolicited (fire-and-forget; nothing ever sends a
request to this `task_id`). `uart_log_bridge_early_init()` installs an
`esp_log_set_vprintf()` hook as the very first thing in `app_main`, so
**every** `ESP_LOGx` call anywhere in the firmware — not just from
`uart_bridge.c` — is captured and forwarded here *instead of* (not in
addition to) the USB-Serial-JTAG console. Device log output is therefore
visible over the same always-on link used for control, without a second cable
or an active debugger session; it is why an `SX1509_start()` or `ILI9488_start()`
failure shows up as a `LOG` frame.

Log lines emitted before the `uart_protocol_t` exists — which covers all of
the driver bring-up in `app_main`, including the I2C bus, the expander, the
i2c scan, the thermocouples and the panel — are buffered in a queue and sent
once `uart_log_bridge_start()` runs, oldest first. Nothing from boot onward is
lost, only delayed until the link is up. If the queue itself fills up (PC not
connected for a while) further lines are dropped rather than blocking whichever
task tried to log; this channel is best-effort by design and never allowed to
stall real work.

One frame per log line (or truncated chunk of one line, never split across
frames):

| Field | Value |
|---|---|
| byte0 | level: `0x00 ERROR` `0x01 WARN` `0x02 INFO` `0x03 DEBUG` `0x04 VERBOSE` |
| bytes1..(length-1) | ASCII text `"TAG: message"`, **not** null-terminated |

### SYSTEM (task 6) — `uart_bridge.c: system_bridge_task`

Admin-style commands against the link itself, as opposed to a device on it.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `RESTART_UART` | none |
| `0x02` | `FACTORY_RESET` | byte1=scope (0=wifi 1=kiln 2=profiles 3=all) |

`FACTORY_RESET` mirrors `POST /api/factory_reset` (`factory_reset.c`)
exactly: same per-partition NVS erase, same unconditional reboot ~500ms
later (so this command's own ACK has a chance to leave first). No reply
frame either way — the ACK is the delivery confirmation, and the reboot
itself (a fresh unsolicited `GET_FW_VERSION` push from INFO) is the real
evidence the erase happened. An out-of-range scope byte is rejected with no
erase and no reboot.

`RESTART_UART` is an on-demand recovery lever for a link that's gotten stuck
(RX ring buffer overflow, line noise) without power-cycling the board — it
calls `uart_owner_restart()`, which flushes only the UART peripheral's *RX*
ring buffer and resets its rx-error counter. Deliberately RX-only: the ACK
for this very request is still sitting in the TX ring buffer when the
handler runs, and flushing that side too would eat the request's own reply
out from under it. In practice this rarely needs to be reached for by hand —
the framing layer already resyncs on the next `0x7E` delimiter regardless of
what garbage came before it, and `uart_owner.c`'s event task already runs
this same RX flush automatically the moment a HW FIFO/ring-buffer overflow
is detected — but it's available as an explicit last resort.

### INFO (task 3) — `uart_bridge.c: info_bridge_task`

A pure **query** channel: the request `DATA` frame is ACKed as usual (delivery
confirmation only), and the answer arrives as a *separate* `DATA` frame sent
back from `(ESP, task 3)` to whichever `(device, task_id)` the request came
from.

Request: byte0 = subcommand, no further args.

| Subcmd | Name |
|---|---|
| `0x01` | `GET_PIN_CONFIG` |
| `0x02` | `GET_FW_VERSION` |

**`GET_PIN_CONFIG` response** — mirrors `s_pin_config[]` in `uart_bridge.c`,
which is built from the same `settings.h`/Kconfig macros the drivers
themselves initialize from, so it can't drift out of sync with what's
actually wired up:

```
byte0            entry_count (N)
N * { u8 gpio, u8 function_id }
```

`function_id` values (`PIN_FUNC_*` in `uart_task_ids.h`):

| id | Meaning | id | Meaning |
|---|---|---|---|
| `0x01` | `I2C_SDA` | `0x09` | `SPI_MISO` |
| `0x02` | `I2C_SCL` | `0x0A` | `THERMO_FAULT` (`~FAULT`, active low) |
| `0x03` | `UART_TX` | `0x0B` | `EXPANDER_IRQ` (SX1509 `~INT`) |
| `0x04` | `UART_RX` | `0x0C` | `EXPANDER_RST` (SX1509 `~RESET`) |
| `0x05` | `SPI_SCLK` | `0x0D` | `SAFETY_TX` (isolated, non-inverting) |
| `0x06` | `SPI_MOSI` | `0x0E` | `SAFETY_RX` |
| `0x07` | `SPI_CS` | `0x0F` | `SAFETY_FAULT` (isolated fault line, output) |
| `0x08` | `LED_HEARTBEAT` | | |

Human-readable labels live PC-side only — the wire carries only the numeric
id, and the same id may legitimately appear several times (four `SPI_CS`
entries, three `THERMO_FAULT` entries).

**Only real ESP32-S3 GPIOs appear here.** The relay drives, the three `~DRDY`
inputs and the display's D/C and `~RESET` are SX1509 pins, not GPIOs, and are
reported through the IO task's `READ` instead. `LED_HEARTBEAT` is **absent by
default on this board** — there is no MCU-driven LED (D25/D28 are rail
indicators wired straight to 3.3V/5V) and `KILNCTL_HEARTBEAT_LED_GPIO` is -1,
which has no honest single-byte representation.

What the firmware reports on a default build:

| GPIO | function_id | | GPIO | function_id |
|---|---|-|---|---|
| 8  | `I2C_SDA` | | 18 | `SPI_CS` (thermo ch2) |
| 9  | `I2C_SCL` | | 21 | `SPI_CS` (display) |
| 43 | `UART_TX` | | 38 | `THERMO_FAULT` (ch0) |
| 44 | `UART_RX` | | 47 | `THERMO_FAULT` (ch1) |
| 12 | `SPI_SCLK` | | 48 | `THERMO_FAULT` (ch2) |
| 11 | `SPI_MOSI` | | 7  | `EXPANDER_IRQ` |
| 13 | `SPI_MISO` | | 10 | `EXPANDER_RST` |
| 14 | `SPI_CS` (thermo ch0) | | 5 | `SAFETY_TX` |
| 17 | `SPI_CS` (thermo ch1) | | 4 | `SAFETY_RX` |
| | | | 6 | `SAFETY_FAULT` |

**`GET_FW_VERSION` response** — built from `build_info.h`, which is
regenerated on *every* build (not just on a CMake reconfigure — see
`App/drivers/gen_build_info.cmake`), so it can never be stale relative to
what's actually flashed:

```
byte0-1        UART_PROTOCOL_VERSION, u16 LE -- fixed offset across every
               version of this protocol; always read and compare this
               first, before trusting anything else in the payload
byte2          dirty flag (0=clean, 1=dirty/unknown)
byte3          commit_len (N1)
N1 bytes       git commit, ASCII, not null-terminated
byte(4+N1)     datetime_len (N2)
N2 bytes       build date+time, ASCII "YYYY-MM-DD HH:MM:SSZ", not null-terminated
```

`UART_PROTOCOL_VERSION` (`uart_task_ids.h`, currently **7**) is a manually
maintained integer bumped whenever a wire-incompatible change is made
(task_id/subcommand renumbering, a payload's byte layout/length/endianness, or
the envelope itself). It is deliberately **not** an automatic hash of the
header: a hash would flag harmless edits — comments, reordering, adding an
unrelated new command — as incompatible just as readily as an actual break.
See the comment above the `#define` for the exact bump policy.

**This is a different number from `KILNLINK_PROTOCOL_VERSION`**
(`firmware/CommonFW/include/kilnlink/kilnlink_version.h`), which versions the
separate ESP<->Pico isolated safety link, not this PC<->ESP one. From
2026-08-17 to 2026-08-24 `UART_PROTOCOL_VERSION` was a plain C alias of that
other constant; the alias was removed after it silently bumped this number
whenever the isolated link's own contract changed for a reason that had
nothing to do with this protocol — most notably 2026-08-23's addition of a
`tx_dropped_sat` byte to the isolated link's status frame, which touched none
of what follows in this document but still got every PC command refused on
real hardware ("device speaks vN, pc_tools speaks vN-1"). The two numbers now
move **independently**: bump this one only when THIS document's contract
changes, never merely because `KILNLINK_PROTOCOL_VERSION` moved, and vice
versa. `tools/check_uart_version_independence.ps1` (repo root) is the CI grep
that keeps `UART_PROTOCOL_VERSION` from being re-aliased.

Compatibility should be treated as an **exact match**, not `>=`: any mismatch,
newer or older, means the two sides can disagree about numbering or layouts,
and there is no meaningful "forward compatible" case. A PC client should
refuse to send any device command until it has observed a matching version;
INFO queries themselves are exempt, since that is how compatibility gets
discovered in the first place.

Neither INFO response carries a subcommand/type byte back — a raw INFO reply
is therefore not self-describing by itself, and the PC side classifies it
structurally (its internal length fields either add up as a pin-config reply
or as a version reply, and in practice the two never collide). Every other
query response in this protocol *does* echo its subcommand in byte0; INFO is
the exception for historical reasons and is frozen that way.

### Boot-time version push

Once, right after the INFO task starts at boot, the firmware makes a
best-effort attempt to *send* the `GET_FW_VERSION` response payload
unsolicited to `(HOST, task 3)` — this only lands if a PC client is already
connected and has task 3 registered at that exact moment. If nobody's
listening (the common case — the PC usually connects after the board is
already up), it just fails silently after its own retries; nothing surfaces
to the firmware side as an error. A client should treat an unsolicited version
reply (one with nothing currently outstanding) as a **device reboot signal**.

### CONTROL (task 8) — `uart_bridge_ext.c: control_task`

Zone configuration reads plus narrow PID/plant-model writes. Byte-level
layout is authoritative in `uart_task_ids.h`'s `CONTROL_CMD_*` comment block
(right after `INFO_CMD_GET_WIFI_STATUS`) — this is a summary.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `GET_ZONES` | none — **query** |
| `0x02` | `SET_ZONE_PID` | byte1=zone, bytes2..5=kp f32 LE, bytes6..9=ki f32 LE, bytes10..13=kd f32 LE — **reply: ok/fail** |
| `0x03` | `SET_ZONE_MODEL` | byte1=zone, bytes2..5=k_dc f32 LE, bytes6..9=tau_s f32 LE, bytes10..13=dead_time_s f32 LE — **reply: ok/fail** |

`GET_ZONES` response: `byte0=0x01 byte1=thermo_count byte2=relay_count
byte3=count(N)`, then N × 31-byte records: `index(1) relay_mask(1)
control_mode(1) cal_offset_c(f32) pid_kp(f32) pid_ki(f32) pid_kd(f32)
max_ramp_c_per_hr(f32) max_temp_c(f32) min_temp_c(f32)`.

Unlike THERMO/IO's silent `SET_*`, `SET_ZONE_PID`/`SET_ZONE_MODEL` reply
`byte0=subcmd byte1=ok(0/1)` — a rejected zone index or out-of-range gain is
exactly what a GUI needs to know immediately.

Scope cap: `/api/zones`' full whole-page fields (zone naming, relay
assignment, heater window/min-on/min-off timing, cross-zone delta) are
**not** writable here — that endpoint validates ~20 fields per zone
together in one commit-or-reject pass that doesn't fit a 253-byte frame or
decompose safely into per-field writes. Use HTTP for those. Manual relay
control is IO task's `SET_RELAY`/`SET_RELAY_MASK` (task 2), not duplicated
here — see the task table above.

**Relays & Rules DSL (`/api/rules`) has no UART mirror in this pass** — the
rule text is free-form and can run to ~2KB, well past the 253-byte cap, and
this protocol has no chunking/multi-part convention to split it over (see
the framing section above). Use HTTP for reading or editing relay rules.

### PROFILES (task 9) — `uart_bridge_ext.c: profiles_task`

Fire profile storage (mirrors `profiles_http.c`) and execution control
(mirrors `dashboard_http.c`'s `/api/profile_exec*`). Full layout in
`uart_task_ids.h`'s `PROFILES_CMD_*` block.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `LIST` | byte1=`start_id` (optional) — **query, paged** |
| `0x02` | `GET` | byte1=id(0-7, or 128+n for a built-in) — **query** |
| `0x03` | `SAVE` | byte1=id(0-7, or `0xFF`=first free slot), byte2=name_len, name, zone_mask, segment_count, then 12 bytes/segment (target_c f32, ramp_c_per_hr f32, dwell_min u32) — **reply: ok/fail** |
| `0x04` | `DELETE` | byte1=id — **reply: ok/fail** |
| `0x05` | `GET_EXEC_STATUS` | none — **query** |
| `0x06` | `START` | byte1=id — **reply: ok/fail** |
| `0x07` | `STOP` | none — **reply: always ok** |
| `0x08` | `PAUSE` | none — **reply: ok/fail** |
| `0x09` | `RESUME` | none — **reply: ok/fail** |
| `0x0A` | `ACK_LAST_RUN` | none — **reply: ok/fail** |

`GET_EXEC_STATUS`'s per-zone block mirrors `/api/control`'s tuning-focused
shape (control_mode/actual_c/duty/relay_on/faulted), not
`/api/profile_exec`'s fuller one — the lifecycle fields (state,
segment_index/count, dwelling, target_c, ramp-lock) are still carried at the
top level; only the fault-reason strings and the `last_run` breadcrumb are
dropped for space. Every mutating command replies `byte0=subcmd
byte1=ok(0/1)`, plus a length-prefixed error string on failure (`SAVE`,
`START`) — same reasoning as CONTROL's `SET_*` replies: these can fail for
an operator-relevant reason the GUI needs immediately.

`0xFF` (`PROFILES_SAVE_ID_NEW`) as `SAVE`'s id byte requests "first free
slot", mirroring `POST /api/profile`'s empty/`-1`/out-of-range `id` field.

**`GET_EXEC_STATUS` answers inline on `profiles_task`, not the flash worker
(2026-09-02 fix).** Every other PROFILES subcommand dispatches through
`bx_run_on_internal_stack()` — the single-job internal-SRAM worker
`uart_bridge_ext.c` also uses for CONTROL/AUTOTUNE's flash-writing commands
and `safety_cfg_store`'s deferred NVS flush — because `SAVE`/`DELETE`/`START`/
etc. reach `nvs_*()` and need that worker's internal stack. `GET_EXEC_STATUS`
never touches flash (`profile_executor_get_status()` only takes
`s_exec.lock` and copies a snapshot), so it used to pay for that indirection
for nothing: queued behind whatever flash-writing job the worker was already
running, AND behind `s_exec.lock` itself, which `profile_executor`'s own 1 Hz
tick holds for the length of a full tick — PID/feedforward/guard math for
every active zone, plus an in-lock NVS write from
`relay_cycles_maybe_persist()` whenever it's due (at most once per 600 s).
Observed live: with three zones running, polling `GET_EXEC_STATUS` over
UART/MCP timed out ("ACKed but no reply within 3.0 s") roughly 40% of the
time, while the same data over HTTP (`GET /api/profile_exec`, which calls
`profile_executor_get_status()` directly from the httpd task, no worker/lock
queuing) was clean across a full firing. The reply was never lost — the
transport ACK happens on frame receipt, independent of the handler, so a
delayed handler still eventually sends a real reply, just possibly after the
caller's deadline. Fix: `profiles_task()` now special-cases `subcmd ==
PROFILES_CMD_GET_EXEC_STATUS` and answers it directly on its own stack,
before the worker dispatch — `thermo_read`'s bridge task already worked this
way (answers inline, no worker), which is why it never showed the symptom
under the same load. No host test covers this: `uart_bridge_ext.c` links no
host test at all (FreeRTOS task/queue machinery, not host-buildable — see
`test_bx_worker_reentrancy.c`'s own comment), so this was verified by
`build_kilnfw` plus code inspection only, not a red/green test.

**`LIST` is paged (2026-08-20).** A summary record is up to 19 bytes (id,
name_len, <=15-byte name, zone_mask, segment_count); 8 user slots plus the 28
shipped built-ins is ~532 bytes against the 253-byte `UART_PROTO_MAX_PAYLOAD`
— it no longer fits one frame. `LIST` takes an optional `start_id` byte and
enumerates every existing profile with `id >= start_id` in ascending order
(user slots 0–7, then the built-in catalogue at 128+), stopping when the
frame fills; the client re-asks with `last id + 1` until a reply comes back
with `count == 0`. Hidden built-ins are skipped, matching `GET /api/profiles`
— they stay reachable by a direct `GET`. The reply layout is unchanged, so an
old client sending no `start_id` byte still parses fine, it just sees the
first page. A single profile `GET` was never at risk: worst case (15-char
name, 12 segments) is 165 of 253 bytes.

### AUTOTUNE (task 10) — `uart_bridge_ext.c: autotune_task`

Mirrors `dashboard_http.c`'s `/api/autotune` (GET) and `/api/autotune/
start|abort|accept`. Full layout in `uart_task_ids.h`'s `AUTOTUNE_CMD_*`
block.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `GET_STATUS` | none — **query** |
| `0x02` | `START` | byte1=zone, byte2=method(0=step,1=relay), bytes3..6=step_duty-or-setpoint_c f32, bytes7..10=relay_d f32, bytes11..14=relay_h_c f32, byte15=rule(0=tl,1=zn) — **reply: ok/fail** |
| `0x03` | `ABORT` | none — **reply: always ok** |
| `0x04` | `ACCEPT` | none — **reply: ok/fail** |

`START` always sends all 16 argument bytes; fields the selected method
doesn't use are ignored (mirrors the HTTP form's server-side defaulting).

**Not mirrored**: `/api/autotune/matrix` (cross-zone coupling matrix + RGA)
and `/api/autotune/trace.csv`/`history.csv` (raw sample dumps) — both are
bulk/table data that don't fit one 253-byte frame and have no honest
truncated form (a partial coupling matrix or truncated CSV trace is
misleading, not merely incomplete). Use HTTP for those.

### WIFI (task 11) — `uart_bridge_ext.c: wifi_task`

Mirrors `wifi_provision_http.c`'s `GET /status`, `GET /scan`, `POST
/provision`, `GET /networks`, `POST /forget` — exists specifically so
Wi-Fi can be configured over a link that works even when Wi-Fi itself is
down or unconfigured. Never touches `kiln_io`/`relay_authority`/
`safety_link`. Full layout in `uart_task_ids.h`'s `WIFI_CMD_*` block.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `GET_STATUS` | none — **query** |
| `0x02` | `SCAN` | none — **query**, capped at 6 entries + truncated flag |
| `0x03` | `ADD_NETWORK` | ssid_len+ssid, password_len+password — **reply: ok/fail** |
| `0x04` | `SET_MODE` | byte1=mode(0=home,1=ap) — **reply: ok/fail** |
| `0x05` | `SET_AP_IDENTITY` | has_ssid(0/1)[+ssid_len+ssid], has_password(0/1)[+password_len+password] — **reply: ok/fail** |
| `0x06` | `GET_NETWORKS` | none — **query**, capped at 5 entries + truncated flag |
| `0x07` | `FORGET` | ssid_len+ssid — **reply: ok/fail** |

`SCAN`/`GET_NETWORKS` cap their result count (`WIFI_WIRE_MAX_SCAN_ENTRIES`=6,
`WIFI_WIRE_MAX_NETWORK_ENTRIES`=5) to fit one frame, setting a `truncated`
flag rather than splitting across frames (no existing multi-part convention
to reuse). Use HTTP for the complete list on a crowded RF environment.
`ADD_NETWORK` mirrors `wifi_prov_add_network()` (adds to, or updates the
password of, the saved-network list) — the `/provision` endpoint's
`ssid`/`password` form fields, not its `mode`/`ap_ssid`/`ap_password`
fields (those are `SET_MODE`/`SET_AP_IDENTITY`).
