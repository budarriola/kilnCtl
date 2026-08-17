// board_pins.h -- the RP2040 GPIO/ADC map for A1 on the kilnCtl main board.
// Single source of truth so no task file re-derives a pin number from memory.
// Traced and verified in docs/HARDWARE.md section "Pico I/O map" (also ARCHITECTURE.md
// section 3's module table) -- if this disagrees with either, the docs win and this
// file is wrong.
//
// Deliberately a plain header, not a driver: it holds only pin numbers, not
// initialisation code, so every hardware-owner task can include it without
// pulling in another task's setup.
#ifndef SAFTYFW_BOARD_PINS_H
#define SAFTYFW_BOARD_PINS_H

// --- SPI0: MAX31856 thermocouple ADC (spi_owner) ---------------------------
#define SAFTYFW_PIN_SPI0_MISO   0   // GPIO0, SPI0 RX from the MAX31856
#define SAFTYFW_PIN_SPI0_CS0    1   // GPIO1, MAX31856 ~CS, active low, driven as plain GPIO
#define SAFTYFW_PIN_SPI0_SCK    2   // GPIO2, SPI0 SCK
#define SAFTYFW_PIN_SPI0_MOSI   3   // GPIO3, SPI0 TX

// --- UART1: the opto-isolated link to the ESP (uart_owner / link_task) -----
// NEVER referenced from safety_core.c or relay_owner.c -- see the CI grep
// check in tools/check_isolation.ps1.
#define SAFTYFW_PIN_UART1_TX    4   // GPIO4, PicoTx -> U3 -> ESP RX (GPIO5)
#define SAFTYFW_PIN_UART1_RX    5   // GPIO5, PicoRx <- U2 <- ESP TX (GPIO4)

// --- The safety actuator (relay_owner). The ONLY code that may write this. -
#define SAFTYFW_PIN_RELAY       6   // GPIO6, saftyRelay -> Q4 gate -> K4 coil. High = energized.

// --- I2C0: wired to J7 but nothing answers today. Left uninitialised. ------
#define SAFTYFW_PIN_I2C0_SDA    7   // GPIO7
#define SAFTYFW_PIN_I2C0_SCL    8   // GPIO8

// --- Discretes (discrete_task) ----------------------------------------------
#define SAFTYFW_PIN_ESTOP       9   // GPIO9, active low, R10 pull-up + C3 debounce cap
#define SAFTYFW_PIN_MAIN_FAULT  10  // GPIO10, U1 collector, active low

// --- Thermocouple front end (thermo_task, via spi_owner) -------------------
#define SAFTYFW_PIN_THERMO_FAULT 11 // GPIO11, MAX31856 ~FAULT, active low
#define SAFTYFW_PIN_THERMO_DRDY  12 // GPIO12, MAX31856 ~DRDY, active low, real IRQ (Phase 3)

// --- Current sense (current_task, via adc_owner) ----------------------------
// RP2040 SAR ADC input numbers, not GPIO numbers -- adc_select_input() takes
// these directly. GPIO26/27/28 must still be adc_gpio_init()'d (ARCHITECTURE.md
// section 8: "adc_gpio_init() disables the digital functions").
#define SAFTYFW_PIN_ADC0_GPIO    26 // GPIO26, ADC0, Current1
#define SAFTYFW_PIN_ADC1_GPIO    27 // GPIO27, ADC1, Current2
#define SAFTYFW_PIN_ADC2_GPIO    28 // GPIO28, ADC2, Current3
#define SAFTYFW_ADC_CH_CURRENT1  0
#define SAFTYFW_ADC_CH_CURRENT2  1
#define SAFTYFW_ADC_CH_CURRENT3  2
// ADC3 (GPIO29) reads VSYS/3 on a standard Pico -- USELESS here, VSYS is
// unconnected on this board (docs/HARDWARE.md section 6). Do not sample it.
// ADC4 is the internal die temperature sensor -- a free sanity check
// (ARCHITECTURE.md section 8), not part of this phase's skeleton.

// --- Console UART0, debug-probe bridge only (see docs/ARCHITECTURE.md sec 1)
// UART1 is taken by the isolated link, so the console/RTT-adjacent bench path
// uses UART0 on the one free pin pair this board has.
#define SAFTYFW_PIN_UART0_TX     17 // GP17 -> probe UART0 RX
#define SAFTYFW_PIN_UART0_RX     16 // GP16 <- probe UART0 TX

#endif // SAFTYFW_BOARD_PINS_H
