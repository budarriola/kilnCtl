# KilnCtrl drivers

Device drivers for the kilnCtl main board, plus the ESP-IDF peripheral
"owner" wrappers they sit on. `docs/HARDWARE.md` at the project root is the
authority on what is wired to what; `settings.h` here turns the Kconfig
options in `Kconfig` into the plain names these drivers use.

| File | Part | Doc |
|------|------|-----|
| `hw/MAX31856.c/.h` | 3x thermocouple front end on the J6 daughterboard (SPI) | `docs/MAX31856.md` |
| `hw/SX1509.c/.h` | 16-channel I2C I/O expander, U5 at 0x3E | `docs/SX1509.md` |
| `owners/kiln_io.c/.h` | Board layer over the expander: relays, IO_1..IO_7, ~DRDY, display D/C and ~RESET | `docs/SX1509.md` |
| `hw/panel_spi.c/.h` (formerly `ILI9488.c/.h`), `hw/panel_codec.c/.h`, `hw/panel_detect.c/.h`, `hw/st7796_panel.c/.h` | 480x320 SPI TFT on J2, panel-agnostic driver parameterised by a `panel_desc_t` (BIGTREETECH TFT35 SPI V2.1 / ILI9488 today; LCDWIKI/Elecrow MSP4031 / ST7796S selectable, not yet bench-verified) | `docs/ILI9488.md`, `docs/DISPLAY_ST7796_PLAN.md` |
| `safety/safety_link.c/.h` | Isolated UART (ADuM1201 digital isolator) + opto-isolated fault line to the RP2040 safety processor | `docs/SAFETY_LINK.md` |
| `bridge/uart_bridge.c/.h` | One task per wire task_id, turning protocol frames into driver calls | `docs/UART_PROTOCOL.md` |
| `bridge/uart_log_bridge.c/.h` | Forwards every `ESP_LOGx` line to the PC over the same link | `docs/UART_PROTOCOL.md` |
| `hw/i2c_scan.c/.h` | One-shot bus probe at boot | — |
| `common/uart_task_ids.h` | The frozen wire contract (protocol version 2) | `docs/UART_PROTOCOL.md` |

`owners/` holds the peripheral owners — `i2c_owner`, `esp_spi_owner`,
`uart_owner` — each a task that serializes every transfer on its bus, plus
`uart_protocol`, the framed/CRCed/ACKed message layer used for both the PC
link and the isolated link to the safety processor. (Formerly the flat
`espInterfaces/` subfolder; the drivers/ layering reorg flattened it into
`owners/` alongside the rest of the bus/IO-arbitration layer -- see
`tools/drivers_reorg/mapping.csv`.)
