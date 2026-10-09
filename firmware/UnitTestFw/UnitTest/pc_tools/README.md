# pc_tools / uart_control

PC-side counterpart to the ESP32-S3 hardened UART protocol implemented in
`App/drivers/espInterfaces/uart_protocol.{c,h}` and addressed by
`App/drivers/uart_task_ids.h`.

## Physical connection

The board has **two USB-C ports**. This protocol runs on UART0 (115200 8N1, no
flow control) exposed through the **USB-UART bridge** port (CP210x / CH340 /
FTDI), *not* the ESP32-S3's native USB-Serial-JTAG port. Port autodiscovery
scores JTAG-looking ports negatively for exactly this reason.

## Usage

```powershell
# Tkinter manual-control GUI
uv run --project pc_tools uart-control-gui

# MCP server (stdio transport)
uv run --project pc_tools uart-mcp-server
```

Both are also VS Code tasks ("UART Control: Open GUI" / "...: Run MCP Server").

## Layout

| module | role |
| --- | --- |
| `protocol.py` | SLIP framing, CRC-16/CCITT-FALSE, `Frame`, enums, task IDs |
| `serial_link.py` | `UartLink` (reader thread, retry/ACK logic), port discovery |
| `devices.py` | DAC / AD9833 / OLED / PCF8575 / INFO payload builders + response parsers |
| `info.py` | `InfoClient`: owns task 3, queries pin config / FW version, spots boot pushes |
| `expander.py` | `ExpanderClient`: owns task 7, PCF8575 writes plus the read-port / address-scan queries |
| `pin_overlay.py` | Measured badge coordinates in `assets/pinout.png` + overlay drawing |
| `session_log.py` | Per-session log files, semantic rollover, retention setting |
| `settings.py` | Persisted app settings (`settings.json`): last-used port, log retention |
| `actions.py` | Named-action registry (one entry per GUI button) backing `press_button` |
| `mcp_server.py` | MCP tools over stdio, incl. bespoke DAC/AD9833/OLED/INFO tools and the generic `press_button`/`list_buttons` pair |
| `gui.py` | Tkinter GUI: `manualCtrl`, `Logs` and `About` menus |
| `logic_capture.py` | Saleae Logic 2 automation-API (gRPC) client: device list, timed digital capture |

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
uv run --project pc_tools python -m uart_control.logic_capture devices
uv run --project pc_tools python -m uart_control.logic_capture rates --channels 0,1
uv run --project pc_tools python -m uart_control.logic_capture capture --channels 0,1 --seconds 2 --out logs/saleae
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

## Generic button press (MCP)

Every bespoke tool (`dac_set_channel_percent`, `oled_print`, ...) also has a
same-named entry in `actions.py`'s registry, reachable generically:

```
list_buttons()                          # every action name + params + description
press_button("OLED: Write & Show", {"text": "hi"})
press_button("DAC: Set Channel Percent", {"channel": 0, "percent": 50.0})
```

Action names match the GUI's own button/menu-item labels 1:1, so a future
GUI button gets MCP coverage the moment it's added to `actions.py`, without a
matching `@mcp.tool()` having to be hand-written. `gui.py`'s own widget
callbacks are untouched -- this is a second, generic front door onto the
same underlying `devices.py`/`info.py` calls, not a replacement.

## INFO task (task 3)

`UART_TASK_ID_INFO` is the one *query* channel in the protocol. The request
DATA frame is ACKed like any other (delivery only); the answer comes back as a
**separate DATA frame** from `(ESP, task 3)` addressed to whoever asked, so the
PC has to be registered as task 3 itself. `InfoClient` does that at
construction — not lazily — because the firmware also pushes its version
*unsolicited* exactly once at boot (`info_boot_push_task`), and a push that
arrives with no task registered is NACKed and lost.

Because a boot push and a query reply are byte-identical, "nobody asked" is
what identifies a reboot — which is why `InfoClient` owns the inbox with a
single consumer rather than letting callers drain the queue.

* **About → Pin Configuration...** shows the live `GET_PIN_CONFIG` reply drawn
  over `assets/pinout.png`, plus a legend of what each pin does.
* The firmware version appears in the status bar, refreshed on connect and on
  every detected reboot.

## PCF8575 expander task (task 7)

Mostly ordinary write commands, but `READ_PORT` and `SCAN` are queries with the
same request/reply shape as INFO — hence `ExpanderClient`, which owns task 7's
inbox. Two differences from `InfoClient`: nothing is ever pushed unsolicited on
this task, and the replies echo their subcommand in byte0, so they're
self-describing rather than classified structurally.

All eight addresses the part's A2/A1/A0 pins can select (0x20–0x27) are
reachable: **manualCtrl → PCF8575 Expander...** has an address combobox, an
address scan, 16 write-through pin toggles, whole-port and mask operations, and
a read-back row. Note the quasi-bidirectional pin semantics (a pin driven low
always reads back 0) documented in [`../docs/PCF8575.md`](../docs/PCF8575.md).

## Protocol version compatibility

`GET_FW_VERSION` carries `UART_PROTOCOL_VERSION` (see `uart_task_ids.h`) at a
*fixed* byte offset that never moves across versions, so it can always be read
and compared before trusting the rest of the payload. `InfoClient.compatible`
is `None` until a version has actually been observed (query reply or boot
push), `True`/`False` after. Both the GUI and the MCP server refuse to send
any DAC/AD9833/OLED command — blocking on `None` too, not just `False` — until
compatibility is confirmed `True`; INFO queries themselves are always allowed,
since that's how compatibility gets discovered in the first place. The GUI
also pops an error dialog once per connection on a confirmed mismatch and
turns the status-bar FW line red.

There's no automatic way to tell whether an arbitrary firmware change would
actually break the wire format, so `UART_PROTOCOL_VERSION` is a manually
maintained integer (bump policy documented next to it in `uart_task_ids.h`),
not a hash of the header — a hash would flag harmless edits (comments,
reordering) as incompatible just as readily as a real break.

## Connecting does not reset the board

Two things had to be right for this, and both were wrong before:

* `UartLink.connect()` configures DTR/RTS **while the port is still closed**,
  then opens it. Passing `port=` to `serial.Serial()` opens with those lines
  at their driver defaults, which pulses the board's auto-reset circuit —
  deasserting them afterwards is too late.
* The starting `MSG_INDEX` is randomized per connection. The firmware's dedup
  ring outlives any host session, so restarting the host at index 0 made its
  first sends look like retransmits: re-ACKed, never delivered. See
  [`../docs/UART_PROTOCOL.md`](../docs/UART_PROTOCOL.md).

Together these are why the first query after a connect used to fail with
"ACKed but no reply arrived", recovering only when the firmware's
once-per-boot version push happened to land right after.

## Session logs

Both the GUI and the MCP server write one log file per session to
`pc_tools/logs/session_*.log`. A new file starts when a connect succeeds or
when a device reboot is detected — both semantic triggers, hence a
hand-swapped `FileHandler` rather than one of `logging`'s size/time rotating
handlers. **Logs → Keep Logs...** sets how many files to retain (persisted in
`src/uart_control/settings.json`, along with the last-used serial port, which
is preferred over the autodiscovered recommendation the next time a port is
still present); **Logs → Show Log...** is the in-app view.

## Known limitation

`uart_bridge.c` only ACKs/NACKs at the protocol layer for the DAC and AD9833
tasks; it never sends an application-level status frame back. A send result of
`ok` therefore means "delivered to the ESP task's inbox", **not** that the
underlying I2C/SPI device operation succeeded. Device-level failures are only
visible on the ESP's own console log. (Task INFO is the exception: it really
does answer with data.)

## Self-check

```powershell
uv run --project pc_tools python pc_tools/selfcheck.py
```

Runs framing/CRC/payload-layout checks and cross-wires two `UartLink`s over a
fake port — including a stub `info_bridge_task` — so the INFO query flow and
the boot push are exercised without hardware.
