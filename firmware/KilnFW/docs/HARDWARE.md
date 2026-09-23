# KilnCtrl Hardware Map

Everything here was traced from `hardware/mainBoard/kiln.kicad_sch` (netlist exported with
`kicad-cli sch export netlist`), plus `hardware/ThermocoupleBoard/` and
`hardware/SaftyThermocoupleBoard/` for the two daughterboards. Where a schematic *net
name* disagrees with what the silicon actually does, this file follows the
silicon and says so.

The controller is an **ESP32-S3-DevKitC** (U4) plugged into the main board. A
second processor, a **Raspberry Pi Pico / RP2040** (A1), sits in an isolated
ground domain as the safety processor; its firmware lives at
`firmware/SaftyFW` in this repository, not in this firmware's tree.

## ESP32-S3 pin assignments

| GPIO | Signal (schematic net) | Direction | What it is |
|------|------------------------|-----------|------------|
| 4  | `DataFromSafty`      | in  | Safety link **RX**, driven by U6's VOB output (a push-pull CMOS output, not an open collector). Not inverted in software — U6 does not invert either. R15 1k pull-up still present but no longer what defines the idle level. |
| 5  | `DataToSafty`        | out | Safety link **TX** (drives U6's VIB input). Not inverted — U6 is non-inverting, and the `UART_SIGNAL_TXD_INV` this pin used to carry (to cancel the old optocoupler's inversion) has been removed. |
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
J6 and the display on J2. The thermocouple parts run SPI mode 1 and are **capped
at 4 MHz** — the part is rated to 5 MHz, but the SimFW bench fixture's slave
emulation cannot reliably meet the first-byte deadline above 4 MHz, and missing
it silently shifts a register burst by one byte instead of faulting (see
`firmware/SimFW/docs/SPI_ACCESS_AUDIT.md` §9). The cap is enforced by a `range`
on `KILNCTL_THERMO_SPI_CLOCK_HZ` and a `_Static_assert` in `MAX31856.c`.

The ILI9488 runs mode 0 and much faster (`KILNCTL_DISPLAY_SPI_CLOCK_HZ`, 20 MHz),
and is **not** subject to that cap: each device gets its own device config on the
shared bus, and the two clock symbols are independent.

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
- **Relay 2 and relay 4's physical outputs are swapped vs. this table.**
  Bench testing 2026-08-27 found a real PCB-level miswire (Relay1/Relay3 read
  correctly): the terminal block that this table's `Relay2` row names (K1/J3)
  is actually driven by IO3, and the one this table's `Relay4` row names
  (K5/J11) is actually driven by IO1. `kiln_io.c`
  (`kiln_relay_logical_to_pin_bit`/`kiln_io_remap_relay_bits`) compensates in
  software so that `kiln_io_set_relay(io, 2, ...)` energizes IO3/K5/J11 and
  `kiln_io_set_relay(io, 4, ...)` energizes IO1/K1/J3 -- i.e. the firmware's
  relay 2/4 numbering matches the panel silkscreen, not this table's raw net
  names. See `firmware/KilnFW/App/drivers/owners/kiln_io.h` for the full detail.
- **The display's D/C and reset are on I2C.** Every command/data transition on
  the ILI9488 costs an I2C transfer to the expander. The display driver batches
  each command's payload into one SPI transaction to keep the number of
  transitions proportional to commands, not to bytes.

Relays are 12 V coils (EE2-12NUH) switched low-side by BSS138 MOSFETs, so an
expander pin high = relay energized. J3/J4/J8/J11 are 3-pin terminal blocks
(NC/COM/NO). K4 is on the safety domain and belongs to the Pico, not the ESP32.

### Relay type and contact-life budget

`docs/RELAY_LIFE_BUDGET.md`. Each relay (the four heater relays K1/K2/K3/K5,
plus the safety relay K4) has a **type** — `ssr | contactor | mercury` — that
selects a rated contact-life budget. The board's own heater relays are the
EE2-12NUH electromechanical parts above, but the type is a per-installation
setting, not a fact about this schematic: a builder who re-fits SSRs in
place of K1/K2/K3/K5 sets the zone's relay type to `ssr` and the budget
indication turns off for those relays.

- Heater-relay type is chosen per zone, in the zones page (`relay_type`
  field, `ZONES_CFG_VERSION` 20), and applies to every relay in that zone's
  `relay_mask`. Default `ssr`.
- Safety-relay (K4) type is chosen in the safety/commissioning settings on
  the ESP, options `contactor | mercury` only — `ssr` is not offered, since
  K4 is a mechanical permit relay by design. Default `contactor`.

**Rated-life table** (`relay_cycles.h`):

| Type | Rated cycles |
|---|---|
| `ssr` | none — no budget, never shown as a percent |
| `contactor` | 100,000 |
| `mercury` | 1,000,000 |

These are industry-typical placeholders, not datasheet numbers for a
specific part — no contactor or mercury datasheet is in this tree. Each
relay also has an optional `rated_override` (0 = use the table above) so a
real datasheet figure can be typed in per relay without touching the table.

**Counting.** The four heater relays are counted in `relay_cycles.c` from
transitions `heater_output.c` already tracks per zone. The safety relay K4
is counted on the **ESP**, not the Pico: the ESP observes K4's reported
state on every safety-status frame and increments a fifth slot
(`RELAY_CYCLES_SAFETY_INDEX`) on an observed edge. This can miss a
transition that happens while the ESP is rebooting, which is acceptable for
an indication against a 100,000+ cycle budget when a firing produces on the
order of 10 K4 transitions — see
`firmware/SaftyFW/docs/RELAY_WEAR_ANALYSIS.md` for why K4 itself still has
no counter or persistence on the Pico side. Counts persist in NVS (`kiln_nvs`,
blob `relay_cyc`), written at most once per 600 s plus a flush at firing
stop / autotune end, same flash-wear discipline as the rest of this module.

**Thresholds and indication — indication only, never blocks heating.**
Budget percent is computed on read, never stored. At **≥ 80 %** used, a
persistent WARNING appears; at **≥ 90 %**, a persistent ERROR. Firing,
autotune, and every other operation stay allowed regardless of tier — this
is purely informational. The persistence (shown continuously rather than
once) is deliberate here even though it otherwise runs against this
project's usual "an always-present warning is a warning nobody reads" rule.

Where it shows up:
- **LCD home page topbar** — a second warning-symbol icon next to the
  existing topbar icons, using the existing warn/error accent colours (no
  new LCD colour tokens). Hidden below 80 % on any relay, shown persistently
  above.
- **Web dashboard** — `#relayLifeIcon` next to the profile-feasibility icon
  on the main page, tooltip naming the relay and percent; `.relay-life-error`
  styling for the error tier.
- **LCD Diagnostics → Relay Life** — a sub-page listing each relay's type,
  cycle count, rated life, and percent.
- **Web Diagnostics** — a table with the same per-relay type/cycles/rated/
  percent columns, plus a reset control per relay.

**Resetting a count after replacing a relay.** A reset only zeroes the
cycle count — the configured type/override is left alone, since replacing a
contact does not change what kind of relay is fitted.
- Web: a "Reset count" button per relay in the diagnostics table, gated by
  `kcConfirm()` (the same confirm dialog used elsewhere in the web UI).
- LCD: no dialog widget exists on this page, so the LCD Diagnostics → Relay
  Life page uses a two-tap confirm — pressing "Reset" turns the button into
  "Confirm?" for 5 seconds; a second press within that window commits the
  reset, otherwise it reverts to "Reset" with nothing changed.

Both paths call `relay_cycles_reset()`, which writes through the same
flash-worker-guarded persist path as the periodic write (never called
directly from an LVGL callback on the PSRAM-backed task stack) and logs an
INFO event naming the old count.

Autotune's relay identification switches faster than a normal firing's PWM
window, so a tuning campaign spends contact-life budget faster than the
same wall-clock time spent firing — worth knowing when reading a percent
that jumped after a tuning session.

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

**The screw-terminal-to-chip mapping is rotated one position vs. the table
above.** Confirmed on the bench by unplugging each terminal in turn and
noting which logical channel faulted: terminal 2 faults ch1, terminal 1
faults ch0, terminal 0 faults ch2 -- a fixed rotation, not a random miswire.
Firmware (`MAX31856.c`'s `cs_pins`/`fault_pins` arrays) compensates in
software so logical channel index *i* always reads the chip actually fed by
terminal *i* (`{THERMO_CS2_IO, THERMO_CS0_IO, THERMO_CS1_IO}` for CS, same
rotation for fault); every downstream consumer (zone `thermo_mask` bits, the
dashboard, LCD, MCP tools) sees the corrected, terminal-accurate numbering
and needs no compensating remap of its own.

### Physical zone arrangement (test kiln)

The three heating elements are **stacked vertically** as rings up the chamber
wall, and **zone 2 is the BOTTOM element, zone 0 the TOP** (zone 1 middle).
Owner-confirmed 2026-09-03.

This had never been written down anywhere in the repo, which mattered more than
it looks: the identified zone coupling matrix is asymmetric, and until the
physical order was known there was no way to tell whether that asymmetry was
real thermal transport or a fit artifact/transposed table. With the order
confirmed it checks out. Summing each zone's column in the live matrix
(`coupling_coeff[affected][stepped]`, so a column is the influence that zone
exerts on the others):

| Zone | Position | Influence exerted on the other two |
|---|---|---|
| z2 | bottom | 43.87 |
| z1 | middle | 39.74 |
| z0 | top    | 22.63 |

Heat rises, so the bottom element should dominate and the top should be the
weakest influencer -- which is exactly the ordering measured. Note this runs
opposite to the intuition that zone 0 is the bottom.

**Use this as a sanity check on any future re-identification.** A matrix whose
column sums put z0 above z2 is asserting that the top element heats the bottom
of the kiln more than the reverse; treat that as a transposed table until
proven otherwise, not as a new measurement.

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

**As of 2026-08-25, the two UART lines and the fault line no longer share the
same kind of part.** The UART pair (`DataToSafty`/`DataFromSafty`) used to
cross through a TCMT1109 optocoupler pair, U2/U3, along with R7/R12/R15; that
pair has been desoldered and replaced by **U6, a single ADuM1201WT digital
isolator** with one channel per direction. The fault line is unchanged and
still crosses through its own TCMT1109 optocoupler, U1:

| Part | Driven by | Output | Meaning |
|------|---------------|--------------------|---------|
| U6 (VIB/VOB) | Pico `PicoTx` (GP4, safety side) | ESP GPIO4 (`DataFromSafty`) | Pico -> ESP data |
| U6 (VIA/VOA) | ESP GPIO5 (`DataToSafty`) | Pico `PicoRx` (GP5), R9 1k pull-up | ESP -> Pico data |
| U1 (opto) | ESP GPIO6 (`Fault`) via R11 390R | Pico `mainFault` (GPIO10), R8 1k pull-up | ESP -> Pico fault assert |

Things that follow from that change:

1. **U6's channel assignment matches the ESP's direction directly** —
   `DataToSafty` (GPIO5) is TX into VIA, `DataFromSafty` (GPIO4) is RX out of
   VOB. (Historical note: the old optocoupler U3 was drawn mirrored relative
   to U1/U2, LED on the *safety* side instead of the *main* side, which made
   the direction easy to get backwards from the symbol alone — see "How this
   was measured, 2026-08-23" below for that now-retired part and the
   measurement that caught it. U6 has no such trap: each channel is
   unidirectional by pin, VIx in on one side, VOx out on the other, with no
   symbol ambiguity to misread.)
2. **Neither direction needs software inversion any more.** The ADuM1201 is
   non-inverting: a high at a VIx input is a high at the matching VOx output.
   Both firmwares used to invert their own TX to cancel their respective
   optocoupler's inversion (`UART_SIGNAL_TXD_INV` in `KilnFW`'s
   `safety_link.c`, `gpio_set_outover(SAFTYFW_PIN_UART1_TX,
   GPIO_OVERRIDE_INVERT)` in `SaftyFW`'s `uart_owner.c`); both of those have
   been removed. Re-adding either now would invert an already-correct signal
   and break the link.
3. **R15 no longer defines the idle level on ESP GPIO4.** VOB is a push-pull
   CMOS output, not an open collector, so it drives GPIO4 to a definite level
   on its own; R15's 1k pull-up to 3.3V_Main is still fitted but is now
   belt-and-braces only, same role as the ESP's internal pull-up. (Under the
   old optocoupler, R15 together with U3's collector was the *only* thing
   defining that level.)
4. **GPIO6 is still an output.** The ESP asserts fault *to* the safety
   processor through U1, unchanged by any of the above. There is no hardware
   path for the Pico to signal the ESP — everything coming back does so over
   the isolated UART.

**The old 9600 baud ceiling was a property of the TCMT1109/R15 pair, not of
either firmware or the UART peripherals**, and does not apply to U6. A baud
sweep with U6 fitted is in progress; see `CONFIG_KILNCTL_SAFETY_BAUD_RATE` in
`KilnFW/App/drivers/Kconfig` for the current measured value rather than
assuming either the old 9600 figure or any other number quoted elsewhere in
this repo's history. A static GPIO high/low test across the barrier passes at
*any* baud rate regardless of which part is fitted — both an optocoupler and
a digital isolator carry a static DC level perfectly well — so that kind of
test proves the wiring and nothing about the rate the link can actually
carry; only a real UART framing exchange does that. See `docs/SAFETY_LINK.md`'s
"Transport" section for the historical optocoupler measurement table.

### How this was measured, 2026-08-23 (applies to the retired optocoupler pair)

**History — this measurement was taken against U2/U3, the TCMT1109
optocoupler pair that has since been removed and replaced by U6. It no longer
describes the polarity of the fitted hardware on the two UART lines; it is
kept here because it is the record of how the old direction/inversion
mapping was established, and because the fault-line row (U1) is still
current.**

Not traced, not inferred from symbols — driven and read on the bench, with the
PC reaching each processor by a path that is not the link under test (ESP over
its USB-serial bridge and JTAG, Pico over SWD). Every crossing inverted, as
predicted, back when U2/U3 were fitted:

| Step | Driven | Read | Result |
|---|---|---|---|
| A | ESP GPIO5 = high | Pico GP5 | **low** — U2's LED lit |
| A | ESP GPIO5 = low | Pico GP5 | **high** — R9 pulls up |
| B | Pico GP4 = high | ESP GPIO4 | **low** — U3's LED lit |
| B | Pico GP4 = low | ESP GPIO4 | **high** — R15 pulls up |
| control | Pico GP5 driven either way | ESP GPIO4 | **no change** — it is the Pico's own receiver |
| C | ESP GPIO6 = high (fault asserted) | Pico GP10 | **low** — U1's LED lit |
| C | ESP held in reset (GPIO6 high-Z) | Pico GP10 | **high** — R8 pulls up |

Confirmed at register level as well as through the probe API: ESP
`GPIO_IN_REG` (0x6000403C) moved `0xAC000381` -> `0xAC000391`, exactly bit 4,
when Pico GP4 was driven low; Pico `SIO_GPIO_IN` (0xD0000004) moved
`0x02031B82` -> `0x02031BA2`, exactly bit 5, when ESP GPIO5 was driven low.
ESP `GPIO_OUT_REG` (0x60004004) reads `0x00064740` — bit 6 set — while the
firmware reports the fault line asserted.

> **The fault line fails de-asserted.** Step C's second row is not just a
> polarity check: with the ESP unpowered, in reset, or otherwise dead, U1's LED
> is dark and R8 holds the Pico's `mainFault` **high**, which reads as "the
> main controller is fine". The safety processor must infer a dead main
> controller from UART silence; this line cannot tell it. (This row is still
> current — U1 was not touched by the 2026-08-25 rework.)

The previous version of this section had the two data pins swapped, and so did
`KilnFW`'s `Kconfig` defaults. See `SAFETY_LINK.md` for how that happened.

## Safety processor (A1, RP2040) — for reference

Not driven by this firmware, listed so the isolated protocol has something to
describe. GPIO0/1/2/3 = MISO/CS0/CLK/MOSI to the safety thermocouple board;
GP4/GP5 = TX/RX across the barrier through U6 (GP4 drives U6's VIB input,
GP5 receives U6's VOA output); GPIO6 = `saftyRelay` (Q4 -> K4 -> J10);
GPIO7/8 = SDA/SCL; GPIO9 = `estop` (J1 terminal, 1k pull-up, 0.01uF);
GPIO10 = `mainFault` in from U1; GPIO11/12 = safety `thermoFault`/`thermoDrdy`;
ADC0/1/2 (GPIO26/27/28) = `Current1..3` from three AD8542 current-sense stages
fed by the J13/J15/J17 3.5 mm current-transformer jacks.

## Display (J2)

10-pin JST XH on the main board: 1 `LCD_IORQ`, 2 `SDA`, 3 `SCL`, 4 `LCD_Reset`,
5 `CS3`, 6 `CLK`, 7 `MOSI`, 8 `MISO`, 9 GND, 10 5V. The panel is a
**BIGTREETECH TFT35 SPI V2.1** (3.5", 480x320, **ILI9488**, 3.3 V logic / 5 V
supply, backlight apparently hardwired on with no control pin).

> **Replacement panel.** The MSP4031 (ST7796, 4.0") is the intended successor to
> the TFT35 described here. Its connection to J2 needs a custom harness with two
> crossovers, two flying wires to the DevKit header (backlight, touch reset), and
> **removal of R4 and R6 on the module** before touch may share this board's I2C
> bus. All of that is specified in
> [`DISPLAY_ST7796_WIRING.md`](DISPLAY_ST7796_WIRING.md); the design rationale is
> in [`DISPLAY_ST7796_PLAN.md`](DISPLAY_ST7796_PLAN.md). Nothing below changes
> until that panel is fitted.

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
(SPI) that third-party listings often claim.

The firmware now drives the NS2009 (App/drivers/hw/NS2009.c) on the same I2C bus
as the SX1509 expander, polled by screen_idle_task (App/drivers/
screen_idle.c) to auto-blank the panel after
`CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS` of no touches (default 60s). There is
no backlight control line (above), so a true "screen and backlight out" is
not possible from firmware — `ILI9488_set_power(false)` (display-off +
sleep-in) was tried first and rejected: on this panel it blanks to a bright
WHITE page (bench finding, 2026-08-17), the opposite of the goal. "Blank"
instead means painting the frame solid black (`ILI9488_clear`), which blocks
far more of the backlight than white without needing a hardware change.
NS2009_start probes both addresses the part can
answer at and logs a warning rather than failing boot if neither does, which
covers both "no touch controller populated" and the SDA/SCL-swap question
above — check the boot log's `i2c_scan_bus()` output to tell which. Screen
auto-blank and the UART bridge's TOUCH_CMD_INJECT (synthetic touches, for
PC/MCP-driven UI testing without the physical glass) both work with no
NS2009 present; only real touch input is contingent on it answering. The
touch-pressure threshold (`CONFIG_KILNCTL_TOUCH_Z1_MAX_THRESHOLD`) has not
been calibrated against real hardware — see its Kconfig help text.

## Power

12 V in on J18 (main) and J19 (safety), each with an SMAJ24CA TVS. Each domain
has its own 5 V switcher and 3.3 V rail; the two domains share no ground. Relay
coils run from the 12 V rail of their own domain. D25/D28 (main) and D27/D29
(safety) are rail indicator LEDs tied through 2.2k to 3.3V/5V — none of them is
MCU-driven, which is why the firmware has no heartbeat LED by default.

## Wanted for the next board revision

Owner wishlist, collected here so it is not scattered across bench notes.
Nothing in this section exists on the current board; firmware has no
support for any of it yet.

- **NTC temperature sensors on the solid-state relays.** One thermistor per
  SSR heatsink so the controller can see SSR temperature directly, alarm on
  an overheating relay, and drive the relay fan from a measurement rather
  than an assumption. Needs ADC inputs (or an I2C ADC) plus the divider
  network. Most ESP32-S3 pins on U4 are unconnected on the current board
  (pin table above), and ADC1 channels live on GPIO1-10 of which only 4-10
  are used, so GPIO1-3 are free ADC1 inputs; an I2C ADC is the fallback if
  more channels are wanted.
- **Two PWM outputs.** One for **relay (SSR) fan control**, one for **case fan
  control**, each intended for a 4-wire PWM fan or a low-side MOSFET driver.
  Should come straight off free ESP32-S3 GPIO through LEDC rather than the
  SX1509 expander, so fan speed does not depend on an I2C write and keeps
  working through an expander reset.

Related next-revision notes already recorded elsewhere: the LCD SDA/SCL swap
and the missing backlight control line (Display section above), and the
second independent thermocouple and contactor feedback contact proposed in
`firmware/SaftyFW/docs/SAFETY_MODEL.md`.
