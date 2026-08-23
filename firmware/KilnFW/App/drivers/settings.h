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
 * MAX31856.c. The part is rated to 5 MHz; the cap comes from the SimFW bench
 * fixture's slave-emulation first-byte deadline, and overrunning it silently
 * shifts a burst by one byte instead of faulting. Reasoning in MAX31856.c and
 * SimFW/docs/SPI_ACCESS_AUDIT.md section 9. Unrelated to DISPLAY_SPI_CLOCK_HZ,
 * which shares the bus but not this constraint. */
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
 * Relay4->K5/J11. */
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

/* --- Safety processor link (opto-isolated, inverted) ---
 * SAFETY_TX_IO (GPIO5) drives U2's LED through R12 (Pico RX); SAFETY_RX_IO
 * (GPIO4) is U3's collector (Pico TX), pulled up externally by R15 (1k to
 * 3.3V_Main); the internal pull-up is enabled too as belt-and-braces.
 * Both directions are inverted by the optocouplers -- see uart_task_ids.h
 * and docs/HARDWARE.md. SAFETY_FAULT_IO is an OUTPUT: high asserts the
 * isolated fault line into the safety processor. */
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
