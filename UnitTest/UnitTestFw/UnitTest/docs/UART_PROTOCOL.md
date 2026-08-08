# Hardened UART protocol — wire format reference

The ESP32-S3 firmware and the PC-side `pc_tools` package talk to each other
over a single UART link using a custom, reliable, addressed message
protocol built on top of `uart_owner` (the raw serialized-transaction layer).

Source of truth:
- Firmware: `App/drivers/espInterfaces/uart_protocol.h` / `.c`, `App/drivers/uart_task_ids.h`, `App/drivers/uart_bridge.c`
- PC: `pc_tools/src/uart_control/protocol.py`, `serial_link.py`, `devices.py`, `info.py`

## Physical layer

UART0, 115200 baud (both Kconfig-configurable — see `docs/HARDWARE.md`), 8N1,
no flow control. On boards with two USB-C ports, this is the one routed
through the dedicated USB-UART bridge chip (e.g. CH343/CP210x), **not** the
native USB-Serial-JTAG port used for flashing/debugging.

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
| 3 | `SRC_DEVICE` | 1 | `ESP=0`, `HOST=1` |
| 4 | `SRC_TASK` | 1 | |
| 5 | `DST_DEVICE` | 1 | |
| 6 | `DST_TASK` | 1 | |
| 7 | `LENGTH` | 1 | payload length, 0–128 |
| 8..8+LEN-1 | `PAYLOAD` | LEN | |
| 8+LEN..+1 | `CRC16` | 2 (BE) | CRC-16/CCITT-FALSE over bytes `[0, 8+LEN)` |

CRC-16/CCITT-FALSE: poly `0x1021`, init `0xFFFF`, no reflection, no xorout.
Check value: `crc16(b"123456789") == 0x29B1`.

Note the deliberate endianness split: header fields are big-endian, but the
multi-byte fields *inside* command payloads (below) are little-endian —
that's the natural layout for a `memcpy` straight into an ESP32 `float`/
`double`, or a Python `struct.pack("<...")`. Don't "fix" this asymmetry.

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
- If a registered task's inbox is full, the receiver withholds the `ACK`
  (does not record the dedup entry either) so the sender's retry gives the
  task time to drain its queue — this doubles as natural backpressure.

Return values callers can expect from a send:

| Result | Meaning |
|---|---|
| `OK` | ACKed — delivered to the destination task's inbox |
| `UNDELIVERABLE` (NACK) | destination task not registered there |
| `TIMEOUT` | no reply after all retries — link or peer down |

## Task registration

Any task on either side can register a `task_id` (1 byte) to get an inbox
queue for messages addressed to it. Task IDs only need to be unique within
their own device (ESP task 1 and HOST task 1 are unrelated).

| task_id | Owner | Purpose |
|---|---|---|
| 1 | `UART_TASK_ID_DAC` | MCP4728 DAC commands |
| 2 | `UART_TASK_ID_AD9833` | AD9833 waveform generator commands |
| 3 | `UART_TASK_ID_INFO` | device info queries (pin config, FW version) |
| 4 | `UART_TASK_ID_OLED` | SSD1306 OLED commands |
| 5 | `UART_TASK_ID_LOG` | firmware console output (ESP_LOGx), forwarded unsolicited |
| 6 | `UART_TASK_ID_SYSTEM` | link-recovery commands (restart) |

The PC side registers the same numeric IDs for symmetry (see
`pc_tools/src/uart_control/protocol.py`).

## Command payloads

### DAC (task 1) — `App/drivers/uart_bridge.c: dac_bridge_task`

byte0 = subcommand:

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `SET_CHANNEL_PERCENT` | byte1=channel(0-3), bytes2..5=percent `f32` LE |
| `0x02` | `SET_ALL_PERCENT` | bytes1..16=percent[4] `f32` LE (ch0..ch3) |
| `0x03` | `POWER_DOWN` | byte1=channel(0-3), byte2=power_mode(0-3) |

### AD9833 (task 2) — `App/drivers/uart_bridge.c: ad9833_bridge_task`

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `SET_FREQUENCY` | byte1=reg(0/1), bytes2..9=freq_hz `f64` LE |
| `0x02` | `SET_PHASE` | byte1=reg(0/1), bytes2..9=degrees `f64` LE |
| `0x03` | `SET_WAVEFORM` | byte1=waveform (0=sine,1=triangle,2=square,3=square÷2) |
| `0x04` | `SELECT_FREQ_REG` | byte1=reg(0/1) |
| `0x05` | `SELECT_PHASE_REG` | byte1=reg(0/1) |
| `0x06` | `RESET` | byte1=hold(0/1) |
| `0x07` | `SLEEP` | byte1=dac_power_down(0/1), byte2=mclk_power_down(0/1) |

### SSD1306 OLED (task 4) — `App/drivers/uart_bridge.c: oled_bridge_task`

`CLEAR`/`SET_CURSOR`/`PRINT` only touch the firmware's in-RAM framebuffer;
nothing reaches the panel until `DISPLAY`.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `CLEAR` | none |
| `0x02` | `SET_CURSOR` | byte1=col, byte2=row (text-cell coordinates) |
| `0x03` | `PRINT` | bytes1..(length-1)=ASCII text, **not** null-terminated |
| `0x04` | `DISPLAY` | none — flushes the framebuffer to the panel |
| `0x05` | `SET_CONTRAST` | byte1=contrast(0-255) |
| `0x06` | `SET_INVERT` | byte1=invert(0/1) |
| `0x07` | `SET_POWER` | byte1=on(0/1) |

### LOG (task 5) — `App/drivers/uart_log_bridge.c`

Firmware → PC only, unsolicited (fire-and-forget; nothing ever sends a
request to this `task_id`). `uart_log_bridge_early_init()` installs an
`esp_log_set_vprintf()` hook as the very first thing in `app_main`, so
**every** `ESP_LOGx` call anywhere in the firmware — not just from
`uart_bridge.c` — is captured and forwarded here *instead of* (not in
addition to) the USB-Serial-JTAG console. This means device log output is
visible over the same always-on link used for control, without a second
cable or an active debugger/monitor session, and it's why the OLED's own
`ESP_LOGE` on an `SSD1306_start()` failure — previously invisible unless
something was attached to the JTAG console — now shows up as a `LOG` frame.

Log lines emitted before the `uart_protocol_t` exists (i.e. before
`uart_protocol_init()` runs in `app_main`, which covers all of the driver
bring-up above it, including `SSD1306_start()`) are buffered in a queue and
sent once `uart_log_bridge_start()` runs, oldest first — nothing from boot
onward is lost, only delayed until the link is up. If the queue itself fills
up (PC not connected/listening for a while) further lines are dropped rather
than blocking whichever task tried to log; this channel is best-effort by
design and never allowed to stall real work.

One frame per log line (or truncated chunk of one line, never split across
frames):

| Field | Value |
|---|---|
| byte0 | level: `0x00 ERROR` `0x01 WARN` `0x02 INFO` `0x03 DEBUG` `0x04 VERBOSE` |
| bytes1..(length-1) | ASCII text `"TAG: message"`, **not** null-terminated |

The GUI's Logs → "Device Console..." window shows these live, color-coded by
level; `mcp_server.py`'s `get_device_log` tool exposes the same buffered
history for pull-based (non-GUI) access.

### SYSTEM (task 6) — `App/drivers/uart_bridge.c: system_bridge_task`

Admin-style commands against the link itself, as opposed to a device on it.

| Subcmd | Name | Args |
|---|---|---|
| `0x01` | `RESTART_UART` | none |

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
is detected — but it's available as an explicit last resort (GUI: "Restart
UART" button next to Connect; MCP: `restart_uart` tool).

### INFO (task 3) — `App/drivers/uart_bridge.c: info_bridge_task`

Unlike the others, this is a **query** channel: the request `DATA` frame is
ACKed as usual (delivery confirmation only, no payload), and the actual
answer arrives as a *separate* `DATA` frame sent back from `(ESP, task 3)`
to whichever `(device, task_id)` the request came from. The requester must
therefore itself be registered to receive it.

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

`function_id` values (`PIN_FUNC_*` in `uart_task_ids.h`): `I2C_SDA=0x01`,
`I2C_SCL=0x02`, `UART_TX=0x03`, `UART_RX=0x04`, `SPI_SCLK=0x05`,
`SPI_MOSI=0x06`, `SPI_CS=0x07`, `LED_HEARTBEAT=0x08`. Human-readable labels
live PC-side only (`devices.py: PIN_FUNCTION_LABELS`) — the wire carries
only the numeric id.

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

`UART_PROTOCOL_VERSION` (`uart_task_ids.h`) is a manually maintained integer
bumped whenever a wire-incompatible change is made (task_id/subcommand
renumbering, a payload's byte layout/length/endianness, or the envelope
itself). It is deliberately **not** an automatic hash of the header: a hash
would flag harmless edits — comments, reordering, adding an unrelated new
command — as incompatible just as readily as an actual break, which would
force a version bump on changes that never affected the wire format. See the
comment above the `#define` for the exact bump policy.

On the PC side, `FirmwareVersion.compatible` (`devices.py`) is `True` only on
an exact match — not `>=` — since any mismatch, newer or older, means the two
sides can disagree about numbering or layouts; there's no meaningful "forward
compatible" case. `InfoClient.compatible` (`info.py`) is `None` until a
version has actually been observed (via a query reply or a boot push), so
callers can distinguish "not yet known" from "confirmed incompatible". Both
`gui.py` and `mcp_server.py` refuse to send any DAC/AD9833/OLED command while
`compatible` is anything other than `True` — INFO queries themselves are
exempt, since that's how compatibility gets discovered in the first place.
The GUI additionally pops an error dialog once per connection on a confirmed
mismatch.

Neither response carries a subcommand/type byte back — a raw INFO reply is
therefore not self-describing by itself. The PC side classifies it
structurally (its internal length fields either add up as a pin-config
reply or as a version reply, and in practice the two never collide); see
`devices.py: parse_info_response()` for the exact logic and rationale.

### Boot-time version push

Once, right after the INFO task starts at boot, the firmware makes a
best-effort attempt to *send* the `GET_FW_VERSION` response payload
unsolicited to `(HOST, task 3)` — this only lands if a PC client is already
connected and has task 3 registered at that exact moment. If nobody's
listening (the common case — the PC usually connects after the board is
already up), it just fails silently after its own retries; nothing surfaces
to the firmware side as an error. `pc_tools` treats an unsolicited version
reply (one with nothing currently outstanding) as a **device reboot
signal** — see `info.py` and the session-logging rollover in `gui.py`.
