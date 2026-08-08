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
| `devices.py` | DAC / AD9833 / INFO payload builders + INFO response parsers |
| `info.py` | `InfoClient`: owns task 3, queries pin config / FW version, spots boot pushes |
| `pin_overlay.py` | Measured badge coordinates in `assets/pinout.png` + overlay drawing |
| `session_log.py` | Per-session log files, semantic rollover, retention setting |
| `settings.py` | Persisted app settings (`settings.json`): last-used port, log retention |
| `actions.py` | Named-action registry (one entry per GUI button) backing `press_button` |
| `mcp_server.py` | MCP tools over stdio, incl. bespoke DAC/AD9833/OLED/INFO tools and the generic `press_button`/`list_buttons` pair |
| `gui.py` | Tkinter GUI: `manualCtrl`, `Logs` and `About` menus |

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
