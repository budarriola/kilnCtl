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
#define SAFTYFW_PIN_UART1_TX    4   // GP4, PicoTx -> R7 -> U3 -> ESP RX (GPIO4)
#define SAFTYFW_PIN_UART1_RX    5   // GP5, PicoRx <- U2 <- R12 <- ESP TX (GPIO5)

// --- The safety actuator (relay_owner). The ONLY code that may write this. -
#define SAFTYFW_PIN_RELAY       6   // GPIO6, saftyRelay -> Q4 gate -> K4 coil. High = energized.

// --- I2C0: wired to J7 but nothing answers today. Left uninitialised. ------
#define SAFTYFW_PIN_I2C0_SDA    7   // GPIO7
#define SAFTYFW_PIN_I2C0_SCL    8   // GPIO8

// --- Discretes (discrete_task) ----------------------------------------------
// GPIO9: ACTIVE HIGH for stop. R10 1k pull-up + C3 noise cap; normally-closed
// contact to GND_Safty, so LOW = healthy, HIGH = pressed/broken/unfitted.
// docs/HARDWARE.md section 5 forbids inverting this in firmware.
#define SAFTYFW_PIN_ESTOP       9
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
//
// CORRECTED 2026-08-21: these two were swapped relative to the silicon.
// RP2040 IO_BANK0 funcsel tables (pico-sdk
// src/rp2040/hardware_regs/include/hardware/regs/io_bank0.h) are explicit:
// GPIO16's funcsel 2 is UART0_TX, GPIO17's funcsel 2 is UART0_RX -- there is
// no funcsel on GPIO16 that produces UART0_RX or vice versa, so the Pico
// cannot be configured the other way regardless of what a #define says. This
// matches docs/HARDWARE.md section 7b's table (GP16 -> probe RX / Pico
// UART0 TX, GP17 -> probe TX / Pico UART0 RX), which was already correct;
// only this header disagreed with it.
#define SAFTYFW_PIN_UART0_TX     16 // GP16, Pico UART0 TX -> probe's RX
#define SAFTYFW_PIN_UART0_RX     17 // GP17, Pico UART0 RX <- probe's TX

// --- Heartbeat LED (watchdog_task), TODO.md Phase 2 "physical heartbeat" ---
// GPIO25 is NOT part of A1's own schematic/net list above -- it is never
// routed off the Pico module, so it has no entry in docs/HARDWARE.md's I/O
// map the way every other pin here does. It is real hardware nonetheless:
// docs/HARDWARE.md section 2 confirms A1 is `PICO_BOARD=pico` (a stock
// Raspberry Pi Pico module, not pico_w/pico2/a custom board file), and on
// that stock module GPIO25 is hard-wired module-side to the Pico's own
// onboard LED (pico-sdk's PICO_DEFAULT_LED_PIN for `PICO_BOARD=pico`) --
// unlike GPIO0-13/16/17/26-28 above, nothing on A1's board needs to agree
// with this pin for it to work, so there is no "check the schematic" step
// left to do here the way there was for every other constant in this file.
#define SAFTYFW_PIN_HEARTBEAT_LED 25 // GP25, Pico module onboard LED, module-internal (no A1 net)

#endif // SAFTYFW_BOARD_PINS_H
