# DISPLAY_ST7796 (MSP4031) — board-to-LCD wiring and module rework

Bench wiring sheet for connecting the MSP4031 4.0" ST7796 module to the kiln
main board. Derived from `DISPLAY_ST7796_PLAN.md` §3.1–3.4 (harness design and
hazards) and verified against `hardware/mainBoard/kiln.kicad_pcb` (U4 and J2
pad-to-net maps) on 2026-09-03.

The module's connector does not mate with J2 — JST XH 10-way on the board versus
a 14-pin 2.54 mm header on the module — so a custom harness is required
regardless. Two circuits deliberately cross inside that harness.

## Harness: J2 (JST XH 2.50 mm, 10-way) → module 14-pin header

| Wire | From | Board net / driver | To | Module name | Note |
|---|---|---|---|---|---|
| 1 | **J2-1** | `LCD_IORQ` = SX1509 IO14, R18 10k pull-**up** | **mod 4** | LCD_RST | **Crosses.** Pull-up releases the panel from reset while the expander floats. |
| 10 | J2-2 | `SDA` = ESP GPIO8, R17 2.2k pull-up to 3.3V_Main | mod 12 | CTP_SDA | Touch. **Gated on the hazard check below** — meter first, lift R4/R6 if needed. |
| 11 | J2-3 | `SCL` = ESP GPIO9, R16 2.2k pull-up to 3.3V_Main | mod 10 | CTP_SCL | Touch. **Gated on the hazard check below** — meter first, lift R4/R6 if needed. |
| 2 | **J2-4** | `LCD_Reset` = SX1509 IO15, R22 10k pull-**down** | **mod 5** | LCD_RS (D/C) | **Crosses.** Pull-down defaults to "command". |
| 3 | J2-5 | `CS3` = ESP GPIO21 | mod 3 | LCD_CS | |
| 4 | J2-6 | `CLK` = ESP GPIO12 | mod 7 | SCK | |
| 5 | J2-7 | `MOSI` = ESP GPIO11 | mod 6 | SDI (MOSI) | |
| 6 | J2-8 | `MISO` = ESP GPIO13 | mod 9 | SDO (MISO) | R30 10k pull-down on the board. Makes RDDID readback wireable. |
| 7 | J2-9 | `GND_Main` | mod 2 | GND | |
| 8 | J2-10 | `5V_Main` | mod 1 | VCC | Adds ~103 mA to 5V_Main |

## Backlight bodge (not through J2)

| Wire | From | To | Module name | Note |
|---|---|---|---|---|
| 9 | ESP32-S3 DevKitC **GPIO15** (U4 pad 8 = header J1 pin 8) | mod 8 | LED | Active HIGH into the BSS138 gate (R7 10k pull-up to the module's VCC3.3). 3.3 V push-pull is fine here, and the pin is LEDC-PWM-able. Fallback: GPIO16. |

Leaving module pin 8 open instead is legal and reproduces today's behaviour
exactly: backlight permanently on.

## Touch reset bodge (not through J2)

| Wire | From | To | Module name | Note |
|---|---|---|---|---|
| 12 | ESP32-S3 DevKitC **3V3** (header J1 pin 1) | mod 11 | CTP_RST | Required whenever wires 10/11 are fitted. Holds the FT6336U's active-low reset released. |

CTP_RST is an unterminated input of the module's U2 74LVC245 if left open —
undefined level, crowbar current, and it drives the touch controller's reset.

Tie it to **3.3 V from the DevKit, not to module pin 1 (VCC)**. Pin 1 is the raw
5 V rail whenever the module is fed from 5 V, and a 5 V strap is only legal if U2
is genuinely a 74LVC245 (5 V-tolerant inputs) rather than a 74HC245 (clamps at
VCC+0.5 V) — a marking you would have to read off the part and trust. Driving the
input from 3.3 V is a valid HIGH for either device, so this routing is correct
without that check. It also matches the module's own U2 supply, which is the
on-module VCC3.3, not VCC.

Cost: a second flying wire to the DevKit header, alongside wire 9. J2 carries no
3.3 V, so there is no way to do this inside the harness.

## Module modifications (on the MSP4031 PCB)

Only needed if touch is wired (wires 10-12). Display-only wiring needs no module
rework at all.

| Part | Action | Why |
|---|---|---|
| **R4** (10k, module pin 12 / CTP_SDA) | **Remove or lift one end** | Header-side pull-up to **raw VCC**. At VCC = 5 V it drives the shared I2C bus above the ESP32-S3 and SX1509 abs-max. See the hazard section. |
| **R6** (10k, module pin 10 / CTP_SCL) | **Remove or lift one end** | Same. |

These two are the only parts that must come off, because they are the only ones
on the module that tie a J2-facing line to raw VCC. Everything else on that
header pulls to the module's own VCC3.3 (the XC6206 output) and is harmless:

| Part | Leave fitted | Note |
|---|---|---|
| R3, R5 | **Yes — required** | Module-side pull-ups of the CTP BSS138 level translators, on the FT6336U side. Removing these kills touch I2C. |
| R7 (10k, pin 8 LED) | Yes | To VCC3.3, into the BSS138 gate. Wire 9 drives it fine. |
| R8 (10k, pin 14 SD_CS) | Yes | To VCC3.3. Pin 14 is left open anyway. |

After lifting R4/R6 the board's R16/R17 (2.2k to 3.3V_Main) are the only pull-ups
on the bus, which is correct for 100 kHz across the module's added capacitance.

Module pins 9 (SDO) and 13 (CTP_INT) are unbuffered outputs referenced to VCC3.3,
so no 5 V appears there regardless of this rework.

This part list comes from the vendor's V1.0 schematic (`DISPLAY_ST7796_PLAN.md`
S3.2), not from the module in hand. The meter check below is the actual gate.

## Leave unconnected

| Module pin | Name | Why |
|---|---|---|
| 13 | CTP_INT | Touch polls instead of using INT |
| 14 | SD_CS | No SD card use |

Every J2 pin is now used. Display-only builds omit wires 10–12 and leave module
pins 10–14 open; that variant is safe with no hazard check and is still the
right starting point on a fresh bench.

Orientation: J2 is the 10-way header on `MainControler.kicad_sch`; pin 1 is
`LCD_IORQ`, the one with R18 10k up to 3.3V_Main. Module pin 1 is VCC.

## Hazard — check module pins 10 and 12 before fitting wires 10/11

The module's capacitive-touch I²C pull-ups R4/R6 (10k each) go to **raw VCC**,
not to the module's own 3.3 V rail, and the vendor recommends powering the
module from 5 V. With VCC = 5 V, module pins 10 and 12 idle at 5 V. In the
recommended harness those land on J2-3 and J2-2, which are the same physical
I²C bus as the SX1509 expander and ESP32-S3 GPIO8/GPIO9, already pulled to
3.3 V by R16/R17 (2.2k). The two supplies fight across those resistors and
settle at roughly

    (5 / 10k + 3.3 / 2.2k) / (1 / 10k + 1 / 2.2k) ~= 3.6 V

which is at or above the absolute-maximum input voltage for both the ESP32-S3
and the SX1509, and it backfeeds current into the 3.3 V rail continuously.
**This damages the main board, not the cheap module.**

Before fitting wires 10 and 11: power the module from 5 V standalone with
nothing else connected to it, and meter module pin 10 and pin 12 to ground.
Do this with the harness unplugged from J2 — the measurement is the gate, not a
formality.

- Reads ~3.3 V — the module was built differently from the V1.0 schematic and
  the hazard does not apply. Record the reading, then fit wires 10 and 11.
- Reads ~5 V — hazard confirmed. Lift or remove R4 and R6 on the module, re-meter
  to confirm the pins now idle at 3.3 V (the board's R16/R17 then supply the only
  bus pull-ups, which is correct for 100 kHz), and only then fit wires 10 and 11.

Display-only wiring (module pins 10–14 open) is safe either way, and is a
legitimate way to make bench progress before this is resolved.

## Why GPIO15 is the backlight pin

Board side, from the PCB's U4 pad-to-net map: pad 8 (GPIO15/ADC2_CH4/32K_P) is
`unconnected-(U4-GPIO15…-Pad8)`. Nothing on the main board touches it — no net,
no passive, no J6/J7 route. GPIO16 (pad 9), GPIO2 (pad 40) and GPIO1 (pad 41)
are likewise unconnected. J6, the 20-way socket carrying the real signals, has
no GPIO15 pin at all, which is why this has to be a flying wire to the DevKit
header rather than another harness circuit.

DevKit side, ESP32-S3-DevKitC-1U-N8R8 (U4's Mouser part): J1 pin 8 is
`RTC_GPIO15, GPIO15, U0RTS, ADC2_CH4, XTAL_32K_P`. It is not a strapping pin
(those are GPIO0, 3, 45, 46) and not a PSRAM pin (GPIO35–37, unavailable on this
octal-PSRAM part and already unconnected here). U0RTS is only an alternate
function; the DevKitC-1 USB-UART bridge wires TX/RX on GPIO43/44 plus DTR/RTS to
EN and GPIO0 for auto-reset, and does not route hardware flow control to GPIO15.
XTAL_32K_P applies only with an external 32.768 kHz crystal configured — the
DevKitC-1 has none and KilnFW does not select one.

The onboard RGB LED is GPIO38 (v1.1) or GPIO48 (v1.0), never GPIO15. Removing
that LED is required on this board for an unrelated reason: GPIO38 is
`thermoFault_0` and GPIO48 is `thermoFault_2`.

ESP pins already in use, from the same map: GPIO4/5 safety link, GPIO6 Fault,
GPIO7 IO_Expander_IRQ, GPIO8/9 I²C, GPIO10 IO_Expander_RST, GPIO11/12/13 SPI,
GPIO14/17/18/21 CS0–CS3, GPIO38/47/48 thermoFault_0/1/2.

## Firmware state

The signal ordering above is `KILNCTL_DISPLAY_SWAP_DC_RESET = n`, the current
default, so no Kconfig pin change is needed for the display itself.

The backlight firmware path landed in commit `ad35720`
(DISPLAY_ST7796_PLAN.md Phase 7): `App/drivers/backlight_pwm.c` drives LEDC and
follows `screen_idle_get_state()`. It is gated behind a **default-off** Kconfig
flag, so wire 9 does nothing until that flag is turned on — with it off the
backlight stays on via the module's own 10k pull-up, exactly as before.

To use wire 9, set in `menuconfig` (or `sdkconfig.defaults` — note
`sdkconfig` itself is gitignored, so edit the Kconfig default or the defaults
file, not the generated config):

| Symbol | Value |
|---|---|
| `KILNCTL_BACKLIGHT_PWM_ENABLE` | `y` |
| `KILNCTL_BACKLIGHT_GPIO` | `15` (matches wire 9; `16` is the fallback) |
| `KILNCTL_BACKLIGHT_ON_PERCENT` | screen-awake duty |
| `KILNCTL_BACKLIGHT_IDLE_PERCENT` | duty after `KILNCTL_TOUCH_IDLE_TIMEOUT_MS` |

The gate is active HIGH into the module's BSS138, so 0% is dark and 100% is
full brightness. Leaving the flag off is still the correct state on any board
where wire 9 is not soldered.
