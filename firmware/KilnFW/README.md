# KilnCtrl firmware

ESP32-S3 firmware (ESP-IDF) for the **kilnCtl main board**: three MAX31856
thermocouple channels, an SX1509 I/O expander driving four relays and seven
digital I/Os, a 480x320 ILI9488 TFT, and an opto-isolated link to an RP2040
safety processor — all remotely controllable from a PC over a hardened,
reliable UART protocol, plus a Python GUI/MCP server on the PC side to drive
them.

The controller is an ESP32-S3-DevKitC (U4) plugged into the main board.
`docs/HARDWARE.md` is the authority on what is wired to what; it was traced
from `hardware/mainBoard/kiln.kicad_sch` and the two thermocouple daughterboards.

This firmware started as the unit-test fixture's and was retargeted at this
board. The fixture's devices (MCP4728 DAC, AD9833, SSD1306, PCF8575) are gone;
their wire `task_id`s are reused, which is why the protocol version is now
**2**.

## Quick start

```powershell
# ESP-IDF v6.0.2 environment (this machine's export.ps1 is broken; use this)
& 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'

idf.py set-target esp32s3
idf.py build

# Flash over JTAG with OpenOCD, then monitor
idf.py monitor

# PC control GUI
uv run --project pc_tools kilnctrl-gui
```

Hardware configuration (every pin, bus, clock and timeout) lives under
`idf.py menuconfig` → **KilnCtrl Hardware Configuration**; `App/drivers/settings.h`
translates those into the plain names the drivers use.

### Building against the simulated plant

For developing the control stack without a kiln (or without a thermocouple
daughterboard), `idf.py menuconfig` → **KilnCtrl Hardware Configuration** →
**Simulated plant (development only)** → **KILNCTL_SIM_PLANT** (default off)
compiles the host tests' thermal model into the firmware. The profile
executor, autotune engine, and dashboard then read simulated temperatures
instead of the MAX31856 channels, and faults can be injected on the running
board:

```powershell
# Inject: fault = none|element_dead|relay_welded|tc_detached|tc_frozen|tc_open
curl -X POST -d "zone=0&fault=element_dead" http://<board-ip>/api/sim

# Report each zone's true element temperature, reported reading, relay state, fault
curl http://<board-ip>/api/sim
```

**Never flash a sim build to a board wired to elements.** It reports
fabricated temperatures — the UI will show a healthy firing with nothing
plugged in — and relay writes still go out to a real expander if one is
attached. As of 2026-08-12 this build compiles but has not been flashed or
run on hardware. See [`docs/PID_CONTROL.md`](docs/PID_CONTROL.md) and
[`docs/GUARD_TEST_MATRIX.md`](docs/GUARD_TEST_MATRIX.md) for the details.

## Documentation

| Doc | Covers |
|---|---|
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | The board: every ESP32-S3 pin, the SPI and I2C buses, the expander's 16 pins, both daughterboard connectors, the isolation barrier, and the display connector's unresolved pinout |
| [`docs/UART_PROTOCOL.md`](docs/UART_PROTOCOL.md) | The wire protocol (version 2): framing, CRC, reliability/retry/dedup, task registration, and every command payload |
| [`docs/MAX31856.md`](docs/MAX31856.md) | Thermocouple driver: wiring, registers and conversions, API, THERMO subcommands |
| [`docs/SX1509.md`](docs/SX1509.md) | Expander and `kiln_io` board layer: pin map, relay/K numbering, boot order, API, IO subcommands |
| [`docs/ILI9488.md`](docs/ILI9488.md) | Display driver: the D/C-vs-reset ambiguity, RGB666 over SPI, API, DISPLAY subcommands |
| [`docs/SAFETY_LINK.md`](docs/SAFETY_LINK.md) | The isolated link to the RP2040, the contract its firmware must implement, and the fault line |
| [`docs/SAFETY_MODEL.md`](docs/SAFETY_MODEL.md) | The safety-wins policy: what actually blocks a relay from energizing today, and what still doesn't |
| [`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md) | What's done and verified vs. outstanding, across firmware and PC tooling |
| [`../../tools/PcTools/README.md`](../../tools/PcTools/README.md) | The PC-side Python package: GUI, MCP server, session logging, layout |

## Firmware layout

- `App/main.c` — `app_main`, kept to single-line "start" calls; all task/init
  choreography lives in each driver's own file. Bring-up order is I2C bus →
  SX1509 + `kiln_io` (relays come up **off**) → i2c scan → shared SPI bus →
  MAX31856 channels → ILI9488 → safety link → log bridge → device bridges.
  Only the UART owner and protocol stack abort boot on failure; everything
  else is logged and stepped over.
- `App/drivers/` — `MAX31856`, `SX1509` + `kiln_io`, `ILI9488`, `safety_link`
  device drivers; `espInterfaces/` holds `i2c_owner`, `esp_spi_owner`,
  `uart_owner` (per-bus request-queue owners) and `uart_protocol` (the
  reliable addressed-message layer); `uart_bridge` (one task per wire
  `task_id`); `uart_log_bridge` (forwards every `ESP_LOGx` to the PC);
  `uart_task_ids.h` (the frozen wire contract); `Kconfig` / `settings.h`
  (hardware configuration); `gen_build_info.cmake` (firmware version info,
  regenerated on every build).
- `App/monitor_task.c` — heartbeat. This board has no MCU-driven LED, so by
  default it reports over the log link instead of blinking.

## Two things that bite

- **The shared SPI bus is initialized in `app_main`, not by a driver.** Only
  the first `spi_bus_initialize()` on a host takes effect, and the two drivers
  on that bus want incompatible configurations — `MAX31856_bus_init` asks for
  DMA-disabled with `max_transfer_sz` 17, which would cap the display's
  transfers at 64 bytes. `app_main` brings the bus up once with the display's
  requirements (DMA, 1440-byte transfers); both drivers then find the host
  already up, which each handles.
- **Relay pins high = relays energized**, and the expander's data latch powers
  up at 1. `kiln_io_init` loads all four relay bits low *before* it turns
  those pins into outputs, which is the only ordering that does not click
  every element on for the duration of one I2C transfer at each boot.

## PC tools layout

See [`../../tools/PcTools/README.md`](../../tools/PcTools/README.md) for the full breakdown.
