# pc_tools / kilnctrl

PC-side counterpart to the kilnCtl main board's hardened UART protocol,
implemented in `App/drivers/espInterfaces/uart_protocol.{c,h}` and addressed by
`App/drivers/uart_task_ids.h`. That header is the wire contract: every task id,
subcommand and payload layout here matches it byte for byte.

What is on the other end (see [`../docs/HARDWARE.md`](../docs/HARDWARE.md) for
the full trace): an **ESP32-S3-DevKitC** driving three **MAX31856**
thermocouple channels over J6, an **SX1509** I/O expander (four relays, seven
digital I/O, the three `~DRDY` inputs, and the display's D/C and `~RESET`), an
**ILI9488** 480x320 TFT on J2, and an opto-isolated link to an **RP2040**
safety processor.

## Physical connection

The dev board has **two USB-C ports**. This protocol runs on UART0 (115200 8N1,
no flow control) exposed through the **USB-UART bridge** port (CP210x / CH340 /
FTDI), *not* the ESP32-S3's native USB-Serial-JTAG port. Port autodiscovery
scores JTAG-looking ports negatively for exactly this reason.

## Usage

```powershell
# Tkinter manual-control GUI
uv run --project pc_tools kilnctrl-gui

# MCP server (stdio transport)
uv run --project pc_tools kilnctrl-mcp-server
```

Both are also VS Code tasks ("KilnCtrl: Open GUI" / "...: Run MCP Server").

## Layout

| module | role |
| --- | --- |
| `protocol.py` | SLIP framing, CRC-16/CCITT-FALSE, `Frame`, enums, task ids, every subcommand constant |
| `serial_link.py` | `UartLink` (reader thread, retry/ACK logic), port discovery |
| `link_hub.py` | Lets several pc_tools processes share one physical port |
| `devices.py` | Payload builders + response parsers for all seven tasks |
| `thermo.py` | `ThermoClient`: owns task 1, MAX31856 queries + the auto-report push |
| `io_expander.py` | `IoClient`: owns task 2, SX1509 queries + the auto-report push |
| `info.py` | `InfoClient`: owns task 3, pin config / FW version, spots boot pushes |
| `display.py` | `DisplayClient`: owns task 4, READ_ID plus the blit stream; Pillow image conversion and a test-pattern generator |
| `device_log.py` | `LogClient`: owns task 5, the firmware's forwarded ESP_LOGx output |
| `safety.py` | `SafetyClient`: owns task 7, the isolated RP2040 link |
| `pin_overlay.py` | Badge coordinates in `assets/pinout.png` + overlay drawing |
| `session_log.py` | Per-session log files, semantic rollover, retention setting |
| `settings.py` | Persisted app settings (`settings.json`): last-used port, log retention |
| `actions.py` | Named-action registry (one entry per GUI button) backing `press_button` |
| `mcp_server.py` | MCP tools over stdio, plus the generic `press_button`/`list_buttons` pair |
| `gui.py` | Tkinter GUI: `manualCtrl`, `Logs` and `About` menus |
| `logic_capture.py` | Saleae Logic 2 automation-API (gRPC) client: device list, timed digital capture |

The `uart_*` naming inside those modules still refers to the UART protocol
itself, which has not changed; only the package identity did (`uart_control` ->
`kilnctrl`).

## Device pages / tool groups

Each device gets one GUI page, one action group and one MCP tool group, all
three built on the same `devices.py` builders.

### Thermocouples -- task 1 (`manualCtrl -> Thermocouples`)

Per-channel type (B/E/J/K/N/R/S/T plus the two raw voltage modes), averaging,
50/60 Hz filter, one-shot vs automatic conversion, TC and cold-junction
thresholds, cold-junction offset, fault read/clear, and raw register
read/write.

The live temperature readout is fed by the firmware's **auto-report push**
(`SET_AUTO_REPORT`), not by a polling loop: one frame per period carries all
three channels, and the page turns reporting off again when it closes. Faults
are shown as decoded text -- "open circuit (no thermocouple?)" -- never as a
hex byte.

`~DRDY` does **not** reach the ESP32 on this board. It goes to the SX1509, so
conversion-ready is reported by the I/O page, not here.

### I/O & Relays -- task 2 (`manualCtrl -> I/O & Relays`)

**Relay numbering is not K numbering**, and the UI says so on every row:

| control | schematic net | contactor | terminal block |
| --- | --- | --- | --- |
| Relay 1 | `Relay1` | **K3** | J8 |
| Relay 2 | `Relay2` | **K1** | J3 |
| Relay 3 | `Relay3` | **K2** | J4 |
| Relay 4 | `Relay4` | **K5** | J11 |

The firmware exposes the schematic's `Relay1..4` numbering rather than silently
renumbering, so the mapping is spelled out in the GUI rows, in every action
description and in every MCP tool docstring. Also on the page: digital I/O 1-7
with direction and pull-up control and a live level read, a DRDY indicator per
thermocouple channel, and a raw-register section (read/write, dir / pull-up /
open-drain / debounce / interrupt masks, the LED driver, soft and hard reset,
and an I2C scan). Expander state is pushed on a period *and* immediately on
every `~INT` edge, so an input change appears without waiting out the period.

### Display -- task 4 (`manualCtrl -> Display`)

Reset (soft or via the expander's `~RESET`), power, rotation, invert, clear,
fill/draw rect, draw line, text cursor/style/print, and `READ_ID` -- the only
way to tell a wired-up panel from drawing commands vanishing into an
unconnected connector.

**Send an image** is implemented: "Open Image..." loads a PNG/JPEG, scales it
to the panel (letterboxed by default so nothing is silently stretched),
converts to RGB565 and streams it with `BLIT_BEGIN`/`BLIT_DATA`/`BLIT_END`,
with a progress bar. Pillow is a declared dependency for this, imported lazily
so the rest of the package still works without it. **Test Pattern** streams
colour bars plus a grey ramp through the identical path and needs neither an
image file nor Pillow -- the bars make a swapped colour channel obvious and the
ramp catches an RGB565->RGB666 expansion that lost its low bits.

Expect this to be slow: RGB565 fits 63 pixels per protocol frame, so a full
480x320 image is ~2440 frames, on the order of a minute at 115200 baud. The
page says so rather than letting it look like a hang.

### Safety processor -- task 7 (`manualCtrl -> Safety Processor`)

Status of the isolated link, the fault line, E-stop, safety relay K4, heating
enable, the safety thermocouple and the three current-sense channels, plus
request-enable, ping, poll period and a manual fault-line override.

Two things this page is careful about, both easy to get backwards:

* **The isolated fault line is an ESP *output*.** GPIO6 drives U1's LED, which
  pulls the Pico's `mainFault` input low: it is this firmware telling the
  safety processor that the main controller has faulted. There is no hardware
  path for the Pico to signal the ESP at all. The firmware asserts it by itself
  on PC-link loss, a thermocouple fault or a watchdog trip; `SET_FAULT_OUT` is
  a manual override of that.
* **"Link down" is the expected state today.** The RP2040 firmware that would
  answer this protocol does not exist in this repository yet, so `link_up = 0`
  with age "never received" is normal. The page leads with a banner saying so
  and colours that state amber rather than red -- painting a known-absent
  peer as a fault just trains people to ignore the colour that should mean
  something.

`GET_STATUS` never blocks on the far side: the ESP polls on its own schedule
and caches the last good answer, so a dead link is a *successful* query
reporting `link_up = 0`, and a raised `SafetyQueryError` means the **ESP**
didn't answer -- a different fault entirely.

## Generic button press (MCP)

Every bespoke tool (`thermo_read`, `io_set_relay`, ...) also has a same-named
entry in `actions.py`'s registry, reachable generically:

```
list_buttons()                          # every action name + params + description
press_button("IO: Set Relay", {"relay": 1, "on": true})
press_button("Thermo: Read All")
```

Action names match the GUI's own button/menu-item labels 1:1, so a future GUI
button gets MCP coverage the moment it's added to `actions.py`, without a
matching `@mcp.tool()` having to be hand-written. `gui.py`'s own widget
callbacks are untouched -- this is a second, generic front door onto the same
underlying `devices.py` calls, not a replacement.

## Query channels and unsolicited pushes

`UART_TASK_ID_INFO` was the only query channel on the old fixture; this board
has five. In all of them the request DATA frame is ACKed like any other
(delivery only), and the answer comes back as a **separate DATA frame** from
`(ESP, that task)` addressed to whoever asked -- so the PC has to be registered
on that task itself. All five clients register at app construction, not lazily
when a window opens.

Two differences worth knowing:

* INFO replies carry **no** subcommand byte, so `parse_info_response()`
  classifies structurally. Every other task echoes its subcommand in byte0.
* Three tasks push **unsolicited** frames: INFO (the once-per-boot version
  push, which is how a reboot is detected), THERMO and IO (auto-reports). A
  push is byte-identical to a query answer, so "nobody asked" is the only thing
  that distinguishes them -- which is why each client owns its task's inbox
  with a single consumer thread instead of letting callers drain the queue.

The MCP server buffers the two auto-report streams into `thermo_get_reports()`
and `io_get_reports()`, since an MCP client has nowhere to receive a push.

## Protocol version compatibility

`GET_FW_VERSION` carries `UART_PROTOCOL_VERSION` (see `uart_task_ids.h`) at a
*fixed* byte offset that never moves across versions, so it can always be read
and compared before trusting the rest of the payload. **This board is version
2.** Version 1 is the unit-test fixture, and the two are not merely different:
they reuse the same task ids for different hardware, so a v1 firmware would
accept a thermocouple command on task 1 and interpret it as an MCP4728 DAC
write. Hence a hard equality check, not `>=`.

`InfoClient.compatible` is `None` until a version has actually been observed
(query reply or boot push), `True`/`False` after. Both the GUI and the MCP
server refuse to send **any** device command -- blocking on `None` too, not
just `False` -- until compatibility is confirmed; INFO queries themselves are
always allowed, since that's how compatibility gets discovered in the first
place. The GUI also pops an error dialog once per connection on a confirmed
mismatch and turns the status-bar FW line red.

There's no automatic way to tell whether an arbitrary firmware change would
actually break the wire format, so `UART_PROTOCOL_VERSION` is a manually
maintained integer (bump policy documented next to it in `uart_task_ids.h`),
not a hash of the header -- a hash would flag harmless edits (comments,
reordering) as incompatible just as readily as a real break.

## Connecting does not reset the board

Two things had to be right for this, and both were wrong before:

* `UartLink.connect()` configures DTR/RTS **while the port is still closed**,
  then opens it. Passing `port=` to `serial.Serial()` opens with those lines
  at their driver defaults, which pulses the board's auto-reset circuit --
  deasserting them afterwards is too late.
* The starting `MSG_INDEX` is randomized per connection. The firmware's dedup
  ring outlives any host session, so restarting the host at index 0 made its
  first sends look like retransmits: re-ACKed, never delivered. See
  [`../docs/UART_PROTOCOL.md`](../docs/UART_PROTOCOL.md).

Together these are why the first query after a connect used to fail with
"ACKed but no reply arrived", recovering only when the firmware's
once-per-boot version push happened to land right after.

## About window / pinout diagram

**About -> Pin Configuration...** shows the live `GET_PIN_CONFIG` reply drawn
over `assets/pinout.png`, plus a legend of what each pin does. The image is a
stock ESP32-S3-DevKitC-1 pin list, which is the module this board carries, so
it did not need replacing -- what changed is which badges get highlighted
(nineteen GPIOs here against the fixture's eight) and the function-id table
behind them, which gained `SPI_MISO`, `THERMO_FAULT`, `EXPANDER_IRQ`,
`EXPANDER_RST`, `SAFETY_TX`, `SAFETY_RX` and `SAFETY_FAULT`.

Only real ESP32-S3 GPIOs appear there. The relay drives, the DRDY inputs and
the display's D/C and `~RESET` are SX1509 pins, reported through the I/O page
instead. See `pin_overlay.py` for which badge coordinates were measured
against the PNG and which were derived from the row pitch.

## Saleae Logic 2

Logic 2 runs two local servers, both toggled from **Preferences** and both
already enabled here (`%APPDATA%\Logic\config.json`:
`automationServerEnabled`, `mcpServerEnabled`):

| server | endpoint | used by |
| --- | --- | --- |
| automation (gRPC) | `127.0.0.1:10430` | `logic_capture.py` / `logic2-automation` |
| MCP (HTTP) | `127.0.0.1:10530/mcp` | registered as `saleae` in the repo's `.mcp.json` |

Logic 2 must already be running — neither server can launch it. Prefer the MCP
server for interactive, agent-driven capture; use `logic_capture.py` when the
capture has to be interleaved with UART traffic from a single process (arm the
analyzer, drive the DUT, export the decode).

```powershell
uv run --project pc_tools python -m kilnctrl.logic_capture devices
uv run --project pc_tools python -m kilnctrl.logic_capture rates --channels 0,1
uv run --project pc_tools python -m kilnctrl.logic_capture capture --channels 0,1 --seconds 2 --out logs/saleae
```

`SALEAE_AUTOMATION_HOST` / `SALEAE_AUTOMATION_PORT` override the endpoint.

Two device quirks the API doesn't surface as queries, both handled in
`logic_capture.py`:

- the sample rate must be one of a fixed set that depends on the enabled
  channel count — hence the `rates` subcommand, which recovers the legal set
  from the backend's rejection message (default is 25 MS/s);
- the attached Logic 16 exposes threshold *ranges* (1.8–3.6 V, 3.6–5.0 V) and
  rejects any explicit value, so `--threshold` is unset by default and Logic's
  own setting is used.

## Session logs

Both the GUI and the MCP server write one log file per session to
`pc_tools/logs/session_*.log`. A new file starts when a connect succeeds or
when a device reboot is detected — both semantic triggers, hence a
hand-swapped `FileHandler` rather than one of `logging`'s size/time rotating
handlers. **Logs → Keep Logs...** sets how many files to retain (persisted in
`src/kilnctrl/settings.json`, along with the last-used serial port, which is
preferred over the autodiscovered recommendation the next time a port is still
present); **Logs → Show Log...** is the in-app view.

## Known limitation

For everything that is not a query subcommand, `uart_bridge.c` only ACKs/NACKs
at the protocol layer; it never sends an application-level status frame back. A
send result of `ok` therefore means "delivered to the ESP task's inbox",
**not** that the underlying SPI/I2C transfer to the MAX31856, SX1509 or ILI9488
succeeded. Device-level failures surface only as firmware log lines — the GUI's
**Device Console** window, or the MCP server's `get_device_log()`.

## Self-check

```powershell
uv run --project pc_tools python pc_tools/selfcheck.py
```

Runs framing/CRC/payload-layout checks for all seven tasks and cross-wires two
`UartLink`s over a fake port with stub bridge tasks, so every query flow, both
auto-report pushes, the boot push, the blit stream and the action registry's
version gate are exercised without hardware.
