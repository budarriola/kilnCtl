#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

/* All hardware pin/bus settings below are `idf.py menuconfig`-configurable
 * (see App/drivers/Kconfig, under "KilnCtrl Hardware Configuration") rather
 * than hardcoded here -- this file just translates the resulting CONFIG_*
 * macros into the plain names the rest of the drivers/app code uses, so
 * nothing downstream needs to know or care that they're Kconfig-backed.
 *
 * The defaults match the kilnCtl main board (mainBoard/kiln.kicad_sch,
 * ESP32-S3-DevKitC = U4). docs/HARDWARE.md is the authority on what each
 * signal is actually wired to; if the two ever disagree, the schematic wins
 * and both should be corrected. */

/* How many times an I2C write is attempted before the driver gives up.
 * Every attempt is a full write plus (where the part allows it) a read-back
 * that confirms the device actually holds what was written; a mismatch is
 * logged and counts as a failed attempt just like a NACK or a timeout. This
 * sits on top of the single bus-reset-and-retry that i2c_owner already does
 * per transaction, so a wedged bus gets reset up to this many times before
 * the error is reported to the caller. */
#define I2C_WRITE_RETRY_ATTEMPTS 5

/* --- I2C bus: SX1509 expander (U5) is currently its only device --- */
#define I2C_MASTER_SCL_IO      CONFIG_KILNCTL_I2C_SCL_IO
#define I2C_MASTER_SDA_IO      CONFIG_KILNCTL_I2C_SDA_IO
#define I2C_MASTER_FREQ_HZ     CONFIG_I2C_MASTER_FREQUENCY

/* --- Shared SPI bus: 3x MAX31856 through J6, plus the ILI9488 on J2 ---
 * One bus, four chip selects. The thermocouple parts want SPI mode 1 and are
 * held to 4 MHz (see THERMO_SPI_CLOCK_HZ below); the display wants mode 0 and
 * much faster, so each device is added to the bus with its own
 * spi_device_interface_config_t rather than the bus being configured once for
 * everyone. The two clocks are independent Kconfig symbols -- capping the
 * thermocouples does not slow the display. */
#if CONFIG_KILNCTL_SPI_HOST_SPI3
#define KILN_SPI_HOST          SPI3_HOST
#else
#define KILN_SPI_HOST          SPI2_HOST
#endif
#define KILN_SPI_SCLK_IO       CONFIG_KILNCTL_SPI_SCLK_IO
#define KILN_SPI_MOSI_IO       CONFIG_KILNCTL_SPI_MOSI_IO
#define KILN_SPI_MISO_IO       CONFIG_KILNCTL_SPI_MISO_IO

/* DISPLAY_ST7796_PLAN.md 9.3/9.4, both default OFF. Normalized to a real 0/1
 * macro rather than aliased straight to CONFIG_KILNCTL_*, same reasoning as
 * TOUCH_CAL_SWAP_XY below: a bool Kconfig option that is OFF generates no
 * CONFIG_* macro at all, so a bare alias would leave these expanding to an
 * undeclared identifier wherever code uses them as an ordinary C token (a
 * ternary argument, an initializer) rather than only inside an `#if`. */
#if CONFIG_KILNCTL_SPI_DMA_USE_PSRAM
#define KILNCTL_SPI_DMA_USE_PSRAM 1
#else
#define KILNCTL_SPI_DMA_USE_PSRAM 0
#endif
#if CONFIG_KILNCTL_SPI_HARDWARE_CS
#define KILNCTL_SPI_HARDWARE_CS 1
#else
#define KILNCTL_SPI_HARDWARE_CS 0
#endif
/* DISPLAY_ST7796_PLAN.md 9.6, default OFF -- same normalization reasoning as
 * the two macros above. */
#if CONFIG_KILNCTL_SPI_ASYNC_FLUSH
#define KILNCTL_SPI_ASYNC_FLUSH 1
#else
#define KILNCTL_SPI_ASYNC_FLUSH 0
#endif
/* DISPLAY_ST7796_PLAN.md 9.7, default OFF. Only meaningful on a panel whose
 * descriptor already carries bytes_per_pixel == 2 (ST7796, COLMOD 0x55) --
 * see panel_spi.c's ili9488_blit_data()/ili9488_push_scratch() for what this
 * skips when on. Never affects the ILI9488 (bytes_per_pixel == 3), the only
 * panel that has ever run on this board's hardware. */
#if CONFIG_KILNCTL_DISPLAY_ZERO_COPY_FLUSH
#define KILNCTL_DISPLAY_ZERO_COPY_FLUSH 1
#else
#define KILNCTL_DISPLAY_ZERO_COPY_FLUSH 0
#endif

/* --- MAX31856 thermocouple channels (thermocouple daughterboard via J6) ---
 * CS and ~FAULT are real ESP32-S3 GPIOs; ~DRDY is not -- it lands on the
 * SX1509 (IO8/IO9/IO10), so the thermocouple driver takes an optional
 * expander handle to observe it. */
#define THERMO_CS0_IO          CONFIG_KILNCTL_THERMO_CS0_IO
#define THERMO_CS1_IO          CONFIG_KILNCTL_THERMO_CS1_IO
#define THERMO_CS2_IO          CONFIG_KILNCTL_THERMO_CS2_IO
#define THERMO_FAULT0_IO       CONFIG_KILNCTL_THERMO_FAULT0_IO
#define THERMO_FAULT1_IO       CONFIG_KILNCTL_THERMO_FAULT1_IO
#define THERMO_FAULT2_IO       CONFIG_KILNCTL_THERMO_FAULT2_IO
/* Capped at 4 MHz by Kconfig `range`, re-checked by a _Static_assert in
 * MAX31856.c. The part is rated to 5 MHz; the cap is an owner decision
 * (2026-10-06): 4 MHz is ample for the MAX31856 and faster SPI is not needed.
 * Reasoning in MAX31856.c. Unrelated to DISPLAY_SPI_CLOCK_HZ, which shares the
 * bus but not this constraint. */
#define THERMO_SPI_CLOCK_HZ    CONFIG_KILNCTL_THERMO_SPI_CLOCK_HZ
/* Expander pins carrying ~DRDY for channels 0/1/2. */
#define THERMO_DRDY0_EXP_PIN   8
#define THERMO_DRDY1_EXP_PIN   9
#define THERMO_DRDY2_EXP_PIN   10

/* --- SX1509 I/O expander (U5) ---
 * ADDR1/ADDR0 are both strapped to GND, giving 0x3E. The other three
 * addresses the part supports (0x3F/0x70/0x71) are still selectable at
 * runtime, same as the old expander driver, since nothing but the strapping
 * stops a differently-populated board from using them. */
#define SX1509_I2C_ADDR        CONFIG_KILNCTL_SX1509_I2C_ADDR
#define SX1509_IRQ_IO          CONFIG_KILNCTL_SX1509_IRQ_IO
#define SX1509_RESET_IO        CONFIG_KILNCTL_SX1509_RESET_IO

/* Expander pin assignments, fixed by the board (see docs/HARDWARE.md). The
 * relay bit order is the schematic's Relay1..Relay4, which is NOT the
 * K-designator order: Relay1->K3/J8, Relay2->K1/J3, Relay3->K2/J4,
 * Relay4->K5/J11.
 *
 * These four are the real expander pins wired to each schematic net --
 * DO NOT reorder them to compensate for the "commanding relay 2 moves
 * position 4" panel-numbering swap found on the bench (2026-08-27); that fix
 * belongs in kiln_io.c's relay<->pin lookup (kiln_relay_logical_to_pin_bit,
 * applied by kiln_io_remap_relay_bits() in kiln_io_set_relay_mask()'s write
 * path and kiln_io_resync_relay_shadow()'s read-back), which is the one
 * place the compensating swap actually lives. Renaming
 * these instead would desync every OTHER consumer of these specific defines
 * (KILN_IO_RELAY_MASK below, gpio_probe.c's pin table, uart_bridge.c's
 * PIN_FUNC map) from the real hardware pins without fixing anything, since
 * KILN_IO_RELAY_MASK just ORs them together order-independent. */
#define SX1509_RELAY1_PIN      0
#define SX1509_RELAY2_PIN      1
#define SX1509_RELAY3_PIN      2
#define SX1509_RELAY4_PIN      3
#define SX1509_IO1_PIN         4   /* opto-isolated input from J24 */
#define SX1509_IO2_PIN         5   /* drives the opto-isolated output to J25 */
#define SX1509_IO3_PIN         6   /* J20 pin 1 */
#define SX1509_IO4_PIN         7   /* J20 pin 2 */
#define SX1509_IO5_PIN         11  /* J21 pin 1 */
#define SX1509_IO6_PIN         12  /* J21 pin 2 */
#define SX1509_IO7_PIN         13  /* J23 pin 1 */
/* Display control pins on the expander. Which of IO14/IO15 is data/command
 * and which is reset is NOT settled -- the main board names them LCD_IORQ
 * (J2 pin 1) and LCD_Reset (J2 pin 4), but every pinout found for the
 * BIGTREETECH TFT35 SPI module says pin 1 is the *touch* interrupt and pin 4
 * is RS (= D/C), with no reset pin brought out at all. Those sources are
 * single-origin and for a "V2.2" silkscreen, so neither reading is proven.
 *
 * Default here follows the module pinout (D/C on IO15, i.e. J2 pin 4), since
 * a display with no D/C cannot work at all while a display with no reset
 * merely loses a recovery lever. Set KILNCTL_DISPLAY_SWAP_DC_RESET in
 * menuconfig to take the other interpretation; the driver reads whichever is
 * configured and nothing else in the firmware assumes an order. Confirm
 * against the physical connector before trusting either. */
#if CONFIG_KILNCTL_DISPLAY_SWAP_DC_RESET
#define SX1509_LCD_DC_PIN      14  /* LCD_IORQ, J2 pin 1 */
#define SX1509_LCD_RESET_PIN   15  /* LCD_Reset, J2 pin 4 */
#else
#define SX1509_LCD_DC_PIN      15  /* LCD_Reset net, J2 pin 4 = module RS */
#define SX1509_LCD_RESET_PIN   14  /* LCD_IORQ net, J2 pin 1 = module touch IRQ */
#endif

/* --- ILI9488 TFT (BIGTREETECH TFT35 SPI V2.1) on J2 ---
 * Only CS is a GPIO; D/C and ~RESET are expander pins (above), which is why
 * the display driver needs the SX1509 handle and why it batches every
 * command's data into a single SPI transaction. */
#define DISPLAY_CS_IO          CONFIG_KILNCTL_DISPLAY_CS_IO
#define DISPLAY_SPI_CLOCK_HZ   CONFIG_KILNCTL_DISPLAY_SPI_CLOCK_HZ
#define DISPLAY_WIDTH          CONFIG_KILNCTL_DISPLAY_WIDTH
#define DISPLAY_HEIGHT         CONFIG_KILNCTL_DISPLAY_HEIGHT
#define DISPLAY_ROTATION       CONFIG_KILNCTL_DISPLAY_ROTATION

/* Bench-wiring override: D/C and ~RESET driven straight off two ESP32 GPIOs
 * instead of through the SX1509 expander -- see the Kconfig help text for
 * why (bring-up without a working/present expander). DISPLAY_DC_GPIO/
 * DISPLAY_RESET_GPIO are only meaningful when this is set; ILI9488_start
 * passes -1 for each otherwise, which ILI9488_init reads as "use the
 * expander (io) for this line," the normal main-board wiring. */
#if CONFIG_KILNCTL_DISPLAY_DC_RESET_DIRECT_GPIO
#define DISPLAY_DC_GPIO        CONFIG_KILNCTL_DISPLAY_DC_GPIO
#define DISPLAY_RESET_GPIO     CONFIG_KILNCTL_DISPLAY_RESET_GPIO
#else
#define DISPLAY_DC_GPIO        (-1)
#define DISPLAY_RESET_GPIO     (-1)
#endif

/* --- NS2009 touch controller (on the TFT35 SPI panel, J2) ---
 * No fixed I2C address setting here: NS2009_start probes both addresses the
 * part can answer at (A0 strapped either way) rather than assuming one, same
 * reasoning as SX1509_scan sweeping all four of its addresses. */
#define TOUCH_IDLE_TIMEOUT_MS  CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS
#define TOUCH_Z1_MAX_THRESHOLD CONFIG_KILNCTL_TOUCH_Z1_MAX_THRESHOLD

/* --- LVGL touchscreen UI (lvgl_port.c, TODO.md section 10) ---
 * Raw-to-pixel touch calibration is unproven bench guesswork, same honesty
 * as TOUCH_Z1_MAX_THRESHOLD above -- see the Kconfig help text. */
#define LVGL_BUF_ROWS          CONFIG_KILNCTL_LVGL_BUF_ROWS

/* DISPLAY_ST7796_PLAN.md 9.2: the shared SPI host's per-transaction ceiling
 * (main.c's spi_bus_config_t::max_transfer_sz). The plan's target is one
 * full default LVGL draw buffer (480 x 40 x 2B RGB565 = 38400B) so a future
 * zero-copy flush (9.7) can DMA a whole buffer in one transaction, but the
 * hard SPI DMA ceiling documented in §9 (SPI_LL_DMA_MAX_BIT_LEN) is 32768B
 * -- below that -- so this is clamped to the hard cap itself rather than to
 * the buffer size, which also makes it independent of whatever
 * KILNCTL_LVGL_BUF_ROWS is tuned to later. Raising max_transfer_sz costs
 * only DMA descriptor arrays (~24B per 4092B of ceiling per §9, ~200B
 * total here), not a 32 KB allocation -- see main.c's spi_config comment for
 * the full accounting against §10's ~1.6 kB internal-DRAM headroom. Still
 * comfortably above today's ILI9488_SCRATCH_BYTES (1440B) chunk size, so
 * this alone changes nothing observable on the currently-attached panel. */
#define KILNCTL_SPI_MAX_TRANSFER_SZ 32768u

/* Bool Kconfig options that are OFF generate no CONFIG_* macro at all (not
 * "defined as 0") -- valid inside an `#if`, but a bare `#define X
 * CONFIG_KILNCTL_...` alias then leaves X expanding to an undeclared
 * identifier the moment it's used as a real C token (lvgl_port.c's
 * `TOUCH_CAL_SWAP_XY ? a : b`), not just inside a preprocessor conditional --
 * discovered building 2026-08-18. Normalize to a real 0/1 macro instead,
 * matching DISPLAY_DC_GPIO's #if/#else pattern above. */
#if CONFIG_KILNCTL_TOUCH_CAL_SWAP_XY
#define TOUCH_CAL_SWAP_XY 1
#else
#define TOUCH_CAL_SWAP_XY 0
#endif

#if CONFIG_KILNCTL_TOUCH_CAL_INVERT_X
#define TOUCH_CAL_INVERT_X 1
#else
#define TOUCH_CAL_INVERT_X 0
#endif

#if CONFIG_KILNCTL_TOUCH_CAL_INVERT_Y
#define TOUCH_CAL_INVERT_Y 1
#else
#define TOUCH_CAL_INVERT_Y 0
#endif

/* Same normalize-to-0/1 reasoning as the three above, for the FT6336U
 * (capacitive, self_calibrating) mapping -- kept as its own knob set
 * (Kconfig's own comment explains why) rather than reusing
 * TOUCH_CAL_SWAP_XY/INVERT_X/INVERT_Y, which stay the NS2009-tuned values. */
#if CONFIG_KILNCTL_TOUCH_CAP_SWAP_XY
#define TOUCH_CAP_SWAP_XY 1
#else
#define TOUCH_CAP_SWAP_XY 0
#endif

#if CONFIG_KILNCTL_TOUCH_CAP_INVERT_X
#define TOUCH_CAP_INVERT_X 1
#else
#define TOUCH_CAP_INVERT_X 0
#endif

#if CONFIG_KILNCTL_TOUCH_CAP_INVERT_Y
#define TOUCH_CAP_INVERT_Y 1
#else
#define TOUCH_CAP_INVERT_Y 0
#endif

/* --- PC link UART ---
 * GPIO43/44 are the ESP32-S3-DevKitC's own UART0 pins, wired to the module's
 * USB-UART bridge and its "UART" USB-C port -- distinct from the native
 * USB-Serial-JTAG port used for flashing/debugging. The main board leaves
 * both unconnected, so they belong to the dev board alone. */
#if CONFIG_KILNCTL_UART_PORT_1
#define UART_OWNER_PORT_NUM    UART_NUM_1
#else
#define UART_OWNER_PORT_NUM    UART_NUM_0
#endif
#define UART_OWNER_TX_IO       CONFIG_KILNCTL_UART_TX_IO
#define UART_OWNER_RX_IO       CONFIG_KILNCTL_UART_RX_IO
#define UART_OWNER_BAUD_RATE   CONFIG_KILNCTL_UART_BAUD_RATE
#define UART_OWNER_QUEUE_LEN         CONFIG_KILNCTL_UART_OWNER_QUEUE_LEN
#define UART_OWNER_TASK_PRIORITY     CONFIG_KILNCTL_UART_OWNER_TASK_PRIORITY
#define UART_OWNER_STACK_SIZE        CONFIG_KILNCTL_UART_OWNER_STACK_SIZE
#define UART_PROTOCOL_TASK_PRIORITY  CONFIG_KILNCTL_UART_PROTOCOL_TASK_PRIORITY
#define UART_PROTOCOL_STACK_SIZE     CONFIG_KILNCTL_UART_PROTOCOL_STACK_SIZE

/* --- Safety processor link (isolated via U6, an ADuM1201WT digital isolator) ---
 * SAFETY_TX_IO (GPIO5) drives U6's VIA input (Pico RX side); SAFETY_RX_IO
 * (GPIO4) is driven by U6's VOA output (Pico TX side), a push-pull CMOS
 * output; the internal pull-up is enabled too as belt-and-braces even though
 * it no longer defines the idle level. Neither direction is inverted -- the
 * ADuM1201 is non-inverting -- see uart_task_ids.h and docs/HARDWARE.md.
 * SAFETY_FAULT_IO is an OUTPUT: high asserts the isolated (opto, U1) fault
 * line into the safety processor. */
#define SAFETY_UART_PORT_NUM   UART_NUM_1
#define SAFETY_TX_IO           CONFIG_KILNCTL_SAFETY_TX_IO
#define SAFETY_RX_IO           CONFIG_KILNCTL_SAFETY_RX_IO
#define SAFETY_FAULT_IO        CONFIG_KILNCTL_SAFETY_FAULT_IO
#define SAFETY_UART_BAUD_RATE  CONFIG_KILNCTL_SAFETY_BAUD_RATE
#define SAFETY_POLL_PERIOD_MS  CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS

/* --- Wi-Fi provisioning (AP fallback / station / AP mode) ---
 * The AP is the fallback path, not the normal one: it comes up whenever
 * there's no working station connection, and stays up (permanently, not just
 * until the next boot) when AP mode (WIFI_PROV_MODE_AP, the 2026-08-11
 * rename of the old "local-only" concept) is set. See wifi_prov.h. */
#define WIFI_AP_SSID               CONFIG_KILNCTL_WIFI_AP_SSID
#define WIFI_AP_DEFAULT_PASSWORD   CONFIG_KILNCTL_WIFI_AP_DEFAULT_PASSWORD
#define WIFI_AP_CHANNEL            CONFIG_KILNCTL_WIFI_AP_CHANNEL
#define WIFI_STA_CONNECT_TIMEOUT_MS CONFIG_KILNCTL_WIFI_STA_CONNECT_TIMEOUT_MS

/* Heartbeat LED. The main board has no MCU-driven LED (D25/D28 are rail
 * indicators wired straight to 3.3V/5V), and the dev board's addressable RGB
 * LED is removed on this build -- so this is -1/disabled by default and the
 * monitor task simply reports over the log link instead of blinking. */
#define HEARTBEAT_LED_GPIO     CONFIG_KILNCTL_HEARTBEAT_LED_GPIO

#endif // SETTINGS_H
