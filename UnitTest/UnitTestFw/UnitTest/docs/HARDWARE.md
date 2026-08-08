# Hardware configuration (Kconfig)

All pin/bus/protocol hardware settings are `idf.py menuconfig`-configurable
under **"kilnCtl Hardware Configuration"**, defined in `App/drivers/Kconfig`.
`App/drivers/settings.h` is just a thin translation layer from the resulting
`CONFIG_KILNCTL_*` macros to the plain names the rest of the code uses —
nothing downstream needs to know or care that they're Kconfig-backed.

After adding or changing options in `Kconfig`, run `idf.py reconfigure` (or
`idf.py menuconfig`) once — a plain `idf.py build` only re-detects **new**
Kconfig files on a full reconfigure, not on an incremental build.

## Board pinout

Measured against an ESP32-S3-WROOM-1 compact devkit (two USB-C ports; see
`Datasheets/` for the source pinout image). Defaults below match what's
actually wired on the reference board.

| Signal | GPIO | Kconfig option | Notes |
|---|---|---|---|
| I2C SDA | 8 | `KILNCTL_I2C_SDA_IO` | shared: MCP4728 DAC + SSD1306 OLED |
| I2C SCL | 9 | `KILNCTL_I2C_SCL_IO` | shared: MCP4728 DAC + SSD1306 OLED |
| UART TX | 43 | `KILNCTL_UART_TX_IO` | labeled "TX" on the board; routed through the dedicated USB-UART bridge chip, **not** the native USB-Serial-JTAG port |
| UART RX | 44 | `KILNCTL_UART_RX_IO` | labeled "RX" on the board |
| AD9833 SCLK | 4 | `KILNCTL_AD9833_SCLK_IO` | |
| AD9833 MOSI | 5 | `KILNCTL_AD9833_MOSI_IO` | |
| AD9833 CS | 6 | `KILNCTL_AD9833_CS_IO` | FSYNC, bit-banged by `spi_owner` |
| Heartbeat LED | 18 | `KILNCTL_HEARTBEAT_LED_GPIO` | |

The board's native USB / USB-Serial-JTAG port (commonly GPIO19/20) is used
for flashing and debugging and is **not** the link this firmware's UART
protocol runs over — see `docs/UART_PROTOCOL.md`.

## Other Kconfig options

| Option | Default | Notes |
|---|---|---|
| `KILNCTL_UART_PORT_CHOICE` | UART0 | which hardware UART peripheral |
| `KILNCTL_UART_BAUD_RATE` | 115200 | |
| `KILNCTL_AD9833_SPI_HOST_CHOICE` | SPI2 | which hardware SPI peripheral |
| `KILNCTL_AD9833_MCLK_HZ` | 25000000 | **must match the actual crystal wired to the AD9833's MCLK pin** — a wrong value skews every programmed frequency |
| `KILNCTL_SSD1306_I2C_ADDR` | 0x3C | 0x3D on some boards |
| `KILNCTL_SSD1306_WIDTH` / `_HEIGHT` | 128 / 64 | pixels; height must be a multiple of 8 |

## Live pin config over UART

The running firmware's actual pin usage (built from these same Kconfig
values, so it can never drift out of sync with what's flashed) is queryable
live over the UART protocol — see `INFO_CMD_GET_PIN_CONFIG` in
`docs/UART_PROTOCOL.md`, and the `pc_tools` GUI's **About → Pin
Configuration...** window, which overlays the live reply on a pinout
diagram.
