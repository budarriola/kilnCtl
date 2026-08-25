# KilnCtrl drivers

Device drivers for the kilnCtl main board, plus the ESP-IDF peripheral
"owner" wrappers they sit on. `docs/HARDWARE.md` at the project root is the
authority on what is wired to what; `settings.h` here turns the Kconfig
options in `Kconfig` into the plain names these drivers use.

| File | Part | Doc |
|------|------|-----|
| `MAX31856.c/.h` | 3x thermocouple front end on the J6 daughterboard (SPI) | `docs/MAX31856.md` |
| `SX1509.c/.h` | 16-channel I2C I/O expander, U5 at 0x3E | `docs/SX1509.md` |
| `kiln_io.c/.h` | Board layer over the expander: relays, IO_1..IO_7, ~DRDY, display D/C and ~RESET | `docs/SX1509.md` |
| `ILI9488.c/.h` | 480x320 SPI TFT on J2 (BIGTREETECH TFT35 SPI V2.1) | `docs/ILI9488.md` |
| `safety_link.c/.h` | Isolated UART (ADuM1201 digital isolator) + opto-isolated fault line to the RP2040 safety processor | `docs/SAFETY_LINK.md` |
| `uart_bridge.c/.h` | One task per wire task_id, turning protocol frames into driver calls | `docs/UART_PROTOCOL.md` |
| `uart_log_bridge.c/.h` | Forwards every `ESP_LOGx` line to the PC over the same link | `docs/UART_PROTOCOL.md` |
| `i2c_scan.c/.h` | One-shot bus probe at boot | — |
| `uart_task_ids.h` | The frozen wire contract (protocol version 2) | `docs/UART_PROTOCOL.md` |

`espInterfaces/` holds the peripheral owners — `i2c_owner`, `esp_spi_owner`,
`uart_owner` — each a task that serializes every transfer on its bus, plus
`uart_protocol`, the framed/CRCed/ACKed message layer used for both the PC
link and the isolated link to the safety processor.
