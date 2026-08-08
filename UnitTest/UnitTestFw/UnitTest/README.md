# kilnCtl UnitTest firmware

ESP32-S3 test-bench firmware (ESP-IDF) driving an MCP4728 DAC, an AD9833
waveform generator, and an SSD1306 OLED, all remotely controllable from a PC
over a hardened, reliable UART protocol — plus a Python GUI/MCP server on
the PC side to drive them.

## Quick start

```powershell
# Firmware: build + flash + monitor
idf.py -p PORT flash monitor

# PC control GUI (also a VS Code task / action button)
uv run --project pc_tools uart-control-gui
```

## Documentation

| Doc | Covers |
|---|---|
| [`docs/UART_PROTOCOL.md`](docs/UART_PROTOCOL.md) | The wire protocol: framing, CRC, reliability/retry/dedup, task registration, and every command payload (DAC, AD9833, OLED, INFO queries) |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | Pin/bus configuration via `idf.py menuconfig`, the board's pinout, and the live pin-config query |
| [`docs/AD9833.md`](docs/AD9833.md) | AD9833 driver notes: wiring, API, MCLK caveat |
| [`docs/SSD1306.md`](docs/SSD1306.md) | SSD1306 driver notes: shared I2C bus, API, font |
| [`docs/MCP4728.md`](docs/MCP4728.md) | MCP4728 DAC command-format defaults and driver API |
| [`pc_tools/README.md`](pc_tools/README.md) | The PC-side Python package: GUI, MCP server, session logging, layout |

## Firmware layout

- `App/main.c` — `app_main`, kept to single-line "start" calls; all task/init
  choreography lives in each driver's own file.
- `App/drivers/` — `DcDac`, `AD9833`, `SSD1306` device drivers; `i2c_owner`,
  `esp_spi_owner`, `uart_owner` (per-bus request-queue owners);
  `uart_protocol` (the reliable addressed-message layer); `uart_bridge`
  (wires each device up as a UART-addressable task); `Kconfig` /
  `settings.h` (hardware configuration); `gen_build_info.cmake` (firmware
  version info, regenerated on every build).
- `App/monitor_task.c` — heartbeat LED.

## PC tools layout

See [`pc_tools/README.md`](pc_tools/README.md) for the full breakdown —
`uart_control` package: `protocol.py`, `serial_link.py`, `devices.py`,
`info.py`, `pin_overlay.py`, `session_log.py`, `mcp_server.py`, `gui.py`.
