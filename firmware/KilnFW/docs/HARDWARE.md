# KilnCtrl Hardware Map

Everything here was traced from `mainBoard/kiln.kicad_sch` (netlist exported with
`kicad-cli sch export netlist`), plus `ThermocoupleBoard/` and
`SaftyThermocoupleBoard/` for the two daughterboards. Where a schematic *net
name* disagrees with what the silicon actually does, this file follows the
silicon and says so.

The controller is an **ESP32-S3-DevKitC** (U4) plugged into the main board. A
second processor, a **Raspberry Pi Pico / RP2040** (A1), sits in an isolated
ground domain as the safety processor; its firmware is not part of this
repository.

## ESP32-S3 pin assignments

| GPIO | Signal (schematic net) | Direction | What it is |
|------|------------------------|-----------|------------|
| 4  | `DataToSafty`        | in  | Safety link **RX** (U3 collector). Inverted, no external pull-up. |
| 5  | `DataFromSafty`      | out | Safety link **TX** (drives U2's LED). Inverted. |
| 6  | `Fault`              | out | Isolated fault line to the safety processor (drives U1's LED). |
| 7  | `IO_Expander_IRQ`    | in  | SX1509 `~INT`, active low. |
| 8  | `SDA`                | i/o | I2C data (SX1509, and J2/J6 pass-through). |
| 9  | `SCL`                | out | I2C clock. |
| 10 | `IO_Expander_RST`    | out | SX1509 `~RESET`, active low. |
| 11 | `MOSI`               | out | Shared SPI. |
| 12 | `CLK`                | out | Shared SPI clock. |
| 13 | `MISO`               | in  | Shared SPI. |
| 14 | `CS0`                | out | MAX31856 channel 0 `~CS`. |
| 17 | `CS1`                | out | MAX31856 channel 1 `~CS`. |
| 18 | `CS2`                | out | MAX31856 channel 2 `~CS`. |
| 21 | `CS3`                | out | ILI9488 display `~CS` (J2). |
| 38 | `thermoFault_0`      | in  | MAX31856 ch0 `~FAULT`, active low. |
| 47 | `thermoFault_1`      | in  | MAX31856 ch1 `~FAULT`, active low. |
| 48 | `thermoFault_2`      | in  | MAX31856 ch2 `~FAULT`, active low. |
| 43/44 | (dev board only)  | out/in | UART0 to the dev board's USB-UART bridge — the PC link. Unconnected on the main board. |

Every other ESP32-S3 pin on U4 is unconnected on the main board. `~DRDY` for the
three thermocouple channels does **not** reach the ESP32 — see the expander
below.

## SPI bus

One bus (SCLK 12 / MOSI 11 / MISO 13), four chip selects: three MAX31856s across
J6 and the display on J2. The thermocouple parts run SPI mode 1 at up to 5 MHz;
the ILI9488 runs mode 0 and much faster, so each device gets its own device
config on the shared bus.

## I2C bus

SDA 8 / SCL 9. The only device on the main board is the SX1509 (U5) at **0x3E**
(`ADDR1`/`ADDR0` both to GND). SDA/SCL are also carried out to J2 (display) and
J6 (thermocouple board) for whatever those modules want to hang there — nothing
on either board currently answers.

## SX1509 I/O expander (U5, 0x3E)

`~INT` -> GPIO7, `~RESET` <- GPIO10.

| Pin | Net | Function |
|-----|-----|----------|
| IO0  | `Relay1` | Q3 -> **K3** -> J8 |
| IO1  | `Relay2` | Q1 -> **K1** -> J3 |
| IO2  | `Relay3` | Q2 -> **K2** -> J4 |
| IO3  | `Relay4` | Q5 -> **K5** -> J11 |
| IO4  | `IO_1` | Opto-isolated **input** from J24 (U13), 2.2k pull-up to 3.3V |
| IO5  | `IO_2` | Drives the opto-isolated **output** to J25 (U14) through a 390R |
| IO6  | `IO_3` | J20 pin 1, direct |
| IO7  | `IO_4` | J20 pin 2, direct |
| IO8  | `thermoDrdy_0` | MAX31856 ch0 `~DRDY` (active low) |
| IO9  | `thermoDrdy_1` | MAX31856 ch1 `~DRDY` |
| IO10 | `thermoDrdy_2` | MAX31856 ch2 `~DRDY` |
| IO11 | `IO_5` | J21 pin 1 |
| IO12 | `IO_6` | J21 pin 2 |
| IO13 | `IO_7` | J23 pin 1 (J23 pin 2 is GND) |
| IO14 | `LCD_IORQ` | J2 pin 1 — see the display section; probably the touch IRQ, not D/C |
| IO15 | `LCD_Reset` | J2 pin 4 — see the display section; probably the panel's D/C (RS) |

Two consequences worth stating plainly:

- **Relay numbering is not K numbering.** `Relay1` is K3, `Relay2` is K1,
  `Relay3` is K2, `Relay4` is K5. The firmware exposes the schematic's
  `Relay1..4` numbering and documents the K/J mapping rather than silently
  renumbering.
- **The display's D/C and reset are on I2C.** Every command/data transition on
  the ILI9488 costs an I2C transfer to the expander. The display driver batches
  each command's payload into one SPI transaction to keep the number of
  transitions proportional to commands, not to bytes.

Relays are 12 V coils (EE2-12NUH) switched low-side by BSS138 MOSFETs, so an
expander pin high = relay energized. J3/J4/J8/J11 are 3-pin terminal blocks
(NC/COM/NO). K4 is on the safety domain and belongs to the Pico, not the ESP32.

## Thermocouple daughterboard (J6 -> J5)

Main board **J6** is a 1x20 socket; thermocouple board **J5** is a 1x20 header.
They mate in **reverse pin order** (J6 pin 1 = J5 pin 20), which is correct for
these two horizontal parts — every signal lines up:

| J6 (main) | J5 (TC board) | Signal |
|-----------|---------------|--------|
| 20 | 1 | 5V |
| 19 | 2 | 3.3V (unused on the TC board — it makes its own 3.3V with an LT1962) |
| 18/17 | 3/4 | `thermoFault_0` / `thermoDrdy_0` |
| 16/15 | 5/6 | `thermoFault_1` / `thermoDrdy_1` |
| 14/13 | 7/8 | `thermoFault_2` / `thermoDrdy_2` |
| 12 | 9 | GND |
| 11/10/9 | 10/11/12 | MOSI / MISO / CLK |
| 8/7/6 | 13/14/15 | CS0 / CS1 / CS2 |
| 5/4 | 16/17 | SDA / SCL |
| 3/2/1 | 18/19/20 | GND |

The board carries three MAX31856s (U2/U3/U4) on `CS0`/`CS1`/`CS2`, each with its
own `~DRDY`/`~FAULT` pair, and an LT1962-3.3 analog regulator.

## Safety thermocouple board (J7 -> J1)

Main board **J7** (2x06, 1.27 mm) to safety board **J1** (2x06), straight
pin-for-pin (1->1 … 12->12). This connector is on the **safety** domain: its
SPI/I2C lines come from the Pico (A1), not the ESP32.

| Pin | Signal | | Pin | Signal |
|-----|--------|-|-----|--------|
| 1 | (no connect) | | 2 | 5V (safety) |
| 3 | `thermoFault` | | 4 | `thermoDrdy` |
| 5 | `MOSI` | | 6 | `SDA` |
| 7 | `MISO` | | 8 | `SCL` |
| 9 | `CLK` | | 10 | GND (safety) |
| 11 | `CS0` | | 12 | GND (safety) |

The board is the same design as the main thermocouple board with one MAX31856
populated.

## Isolation barrier (main <-> safety)

Three TCMT1109 optocouplers, and nothing else, cross between `GND_Main` and
`GND_Safty`:

| Part | LED driven by | Collector (output) | Meaning |
|------|---------------|--------------------|---------|
| U3 | Pico `PicoTx` (GPIO4) via R7 390R | ESP GPIO4 (`DataToSafty`) | Pico -> ESP data |
| U2 | ESP GPIO5 (`DataFromSafty`) via R12 390R | Pico `PicoRx` (GPIO5), R9 1k pull-up | ESP -> Pico data |
| U1 | ESP GPIO6 (`Fault`) via R11 390R | Pico `mainFault` (GPIO10), R8 1k pull-up | ESP -> Pico fault assert |

Three things follow, and all three are easy to get wrong:

1. **The data net names are backwards.** `DataToSafty` is the ESP's receive and
   `DataFromSafty` is its transmit. Trace U2/U3 above if this ever looks wrong.
2. **Both data directions are logically inverted.** A high on the driving side
   lights the LED, which pulls the receiving collector low. An idle-high UART
   line therefore arrives idle-low; the firmware calls
   `uart_set_line_inverse(UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV)` rather
   than trying to fix this in software.
3. **GPIO6 is an output.** The ESP asserts fault *to* the safety processor.
   There is no hardware path for the Pico to signal the ESP — everything coming
   back does so over the isolated UART.

R15 (1k to 3.3V_Main) idles the TX net high; GPIO4 has no external pull-up, so
the internal one must be enabled.

## Safety processor (A1, RP2040) — for reference

Not driven by this firmware, listed so the isolated protocol has something to
describe. GPIO0/1/2/3 = MISO/CS0/CLK/MOSI to the safety thermocouple board;
GPIO4/5 = TX/RX across the barrier; GPIO6 = `saftyRelay` (Q4 -> K4 -> J10);
GPIO7/8 = SDA/SCL; GPIO9 = `estop` (J1 terminal, 1k pull-up, 0.01uF);
GPIO10 = `mainFault` in from U1; GPIO11/12 = safety `thermoFault`/`thermoDrdy`;
ADC0/1/2 (GPIO26/27/28) = `Current1..3` from three AD8542 current-sense stages
fed by the J13/J15/J17 3.5 mm current-transformer jacks.

## Display (J2)

10-pin JST XH on the main board: 1 `LCD_IORQ`, 2 `SDA`, 3 `SCL`, 4 `LCD_Reset`,
5 `CS3`, 6 `CLK`, 7 `MOSI`, 8 `MISO`, 9 GND, 10 5V. The panel is a
**BIGTREETECH TFT35 SPI V2.1** (3.5", 480x320, **ILI9488**, 3.3 V logic / 5 V
supply, backlight apparently hardwired on with no control pin).

**Two unresolved discrepancies — check both against the physical connector
before powering the panel.** Published pinouts for this module (single-source,
community-maintained, and labelled V2.2) give the 10 pins as: 1 IORQ (touch
interrupt), 2 SCL, 3 SDA, 4 RS, 5 NSS, 6 SCK, 7 MOSI, 8 MISO, 9 GND, 10 +5V.
Pins 5-10 line up with the main board exactly. Pins 1-4 do not:

1. **D/C is probably J2 pin 4, not pin 1.** The module's RS (register select =
   data/command) is pin 4, which the main board names `LCD_Reset`; the module's
   pin 1 is the touch controller's interrupt, which the main board names
   `LCD_IORQ`. There appears to be no panel reset on the connector at all. The
   firmware defaults to the module's reading (D/C on expander IO15) and has a
   menuconfig switch, `KILNCTL_DISPLAY_SWAP_DC_RESET`, to take the schematic's
   naming instead.
2. **SDA and SCL may be swapped.** The main board puts SDA on pin 2 and SCL on
   pin 3; the module pinout has them the other way round. This only affects the
   touch controller, which the firmware does not drive, so it is a note for the
   next board revision rather than a firmware problem.

Also worth recording: BIGTREETECH's own documentation and hardware repository
say the touch controller on this module is an **NS2009** (I2C), not the XPT2046
(SPI) that third-party listings often claim. Either way the firmware drives the
display only — touch is unsupported.

## Power

12 V in on J18 (main) and J19 (safety), each with an SMAJ24CA TVS. Each domain
has its own 5 V switcher and 3.3 V rail; the two domains share no ground. Relay
coils run from the 12 V rail of their own domain. D25/D28 (main) and D27/D29
(safety) are rail indicator LEDs tied through 2.2k to 3.3V/5V — none of them is
MCU-driven, which is why the firmware has no heartbeat LED by default.
