# BIGTREETECH TFT35 SPI V2.1 — research notes

Researched 2026-08-09. Compiled from public vendor documentation, the vendor's
GitHub hardware repo, and one third-party pinout diagram. Where sources
disagree or a claim could not be independently confirmed, this is called out
explicitly — do not treat the flagged items as certain.

## Display controller

- **ILI9488** — a-Si TFT LCD single-chip driver, 480(RGB)x320 resolution,
  16.7M colors. Confirmed consistently by BIGTREETECH's own docs (GitHub
  `bigtreetech/docs`, `TFT35 SPI.md`) and the official V2.1 User Manual
  (`bigtreetech/TFT35-SPI` repo, `v2/BIGTREETECH TFT35 SPI V2.1 User
  Manual.pdf`, section 1.2 "Product Parameters": *"Driver IC for SPI
  Display: ILI9488"*).
  - Source: https://github.com/bigtreetech/docs/blob/master/docs/TFT35%20SPI.md
  - Source: https://github.com/bigtreetech/TFT35-SPI/tree/master/v2

## Touch controller — IMPORTANT DISCREPANCY

- The task brief assumed **XPT2046**. Every official BIGTREETECH source found
  (the `docs` repo page, the BTT wiki `global.bttwiki.com/TFT35 SPI.html`,
  and the official V2.1 User Manual PDF) instead states the touch driver IC
  is **NS2009**: *"Driver IC for Touch: NS2009"*. The vendor's hardware repo
  even ships an `NS2009.PDF` datasheet directly alongside the V2.1 schematic
  (`bigtreetech/TFT35-SPI/v2/Hardware/NS2009.PDF`).
  - We could not find any official BIGTREETECH source for the V2.1 revision
    that names XPT2046.
  - It's possible the confusion comes from other/older BTT touchscreens
    (e.g. the non-SPI TFT35 V3.0, TFT24, TFT28 series used with Marlin) that
    do use XPT2046-family touch controllers, or from generic "TFT35" 3.5"
    SPI panels sold by other vendors (Waveshare, generic AliExpress modules)
    that commonly pair ILI9488 with XPT2046. Those are a **different
    product** from the BTT TFT35 **SPI** V2.1 covered here.
  - **Confidence: high that NS2009, not XPT2046, is correct for the
    BIGTREETECH TFT35 SPI V2.1 specifically.** The XPT2046.pdf datasheet was
    still downloaded per the task instructions, but treat it as
    reference-only / possibly not applicable to this exact board unless the
    physical unit in hand is confirmed otherwise (check silkscreen marking
    on the touch IC).
  - Sources: https://github.com/bigtreetech/docs/blob/master/docs/TFT35%20SPI.md ,
    https://global.bttwiki.com/TFT35%20SPI.html ,
    https://github.com/bigtreetech/TFT35-SPI/blob/master/v2/BIGTREETECH%20TFT35%20SPI%20V2.1%20User%20Manual.pdf ,
    https://github.com/bigtreetech/TFT35-SPI/blob/master/v2/Hardware/NS2009.PDF

## Logic / interface voltage

- **3.3V logic**, **5V input power** to the module. Confirmed by the same
  official sources: *"Input Voltage: DC 5V"*, *"Logic Voltage: DC 3.3V"*.
  - Source: https://github.com/bigtreetech/docs/blob/master/docs/TFT35%20SPI.md

## 10-pin connector pinout

The official User Manual (`v2/BIGTREETECH TFT35 SPI V2.1 User Manual.pdf`,
section 2.2 "Pin-out") documents both an XH2.54 10-pin connector and a 1.0mm
pitch 9-pin FPC connector, but the manual presents this **only as an image**
(a photo/diagram of the connector with silkscreened labels), not as text or
a table, so it could not be extracted/OCR'd by the tools available here.

The best available *text* source for the pin order is a third-party,
community-maintained pinout-diagram project (not BIGTREETECH-authored):

**Source: https://dids.github.io/btt-pinouts-diagrams/** (page titled "BIGTREETECH CB1 + TFT35 SPI (+ UART)")

This diagram labels itself **"TFT35 SPI V2.2"** (not V2.1) and gives, reading
the connector pin 1 -> pin 10:

| Pin | Signal | Notes |
|-----|--------|-------|
| 1 | IORQ (touch IRQ) | wired to host GPIO labelled `SOC_TP_IRQ` in the diagram |
| 2 | SCL | I2C clock (touch) |
| 3 | SDA | I2C data (touch) |
| 4 | RS | display register-select / D-C (data/command) |
| 5 | NSS | display SPI chip-select |
| 6 | SCK | shared SPI clock (display) |
| 7 | MOSI | SPI data out (display) |
| 8 | MISO | SPI data in (display) |
| 9 | GND | ground |
| 10 | +5V | module power in |

This is internally consistent with BIGTREETECH's stated feature description
("SPI is used for LCD display, and I2C is used for touch... The touch
screen is controlled by the chip on the motherboard") — i.e. touch (IRQ/SCL/
SDA) goes to the host's I2C + GPIO, and the display (RS/NSS/SCK/MOSI/MISO)
goes to the host's SPI bus, sharing power/ground.

**Confidence: medium.** The signal *set* and *grouping* (touch I2C+IRQ vs.
display SPI vs. power/ground) is well corroborated by BIGTREETECH's own
feature description. The exact **pin-1-to-pin-10 physical order** above
comes from a single third-party source labelled for a slightly different
revision string ("V2.2" vs the "V2.1" this project uses), and could not be
cross-checked against the vendor's own image-only pinout diagram or the
vendor schematic PDF (`v2/Hardware/BTT TFT35-SPI V2.1_SCH.pdf`, which is a
PCB/schematic export whose text layer is not usable as a clean pin table
either — also downloaded to
`C:\Users\budar\AppData\Local\Temp\tftresearch\SCH.pdf` during this
research but not copied into the repo since it's a secondary/derived
source, not the vendor's own textual pinout).
**Before wiring, verify pin 1 against the physical silkscreen "1" marking
on the connector and against continuity/multimeter checks — do not rely on
this table alone for a build.**

## Backlight control

No dedicated backlight enable/PWM/dimming signal appears among the 10
connector pins identified above (power, ground, SPI x4, I2C x2, touch IRQ —
no separate BL line). This is corroborated by a user report on BIGTREETECH's
own issue tracker:

- https://github.com/bigtreetech/TFT35-SPI/issues/18 — a user running a
  TFT35 SPI V2.1 24/7 on a Raspberry Pi found the panel got very hot, and
  that standard Linux display-power-management commands (`xset s 30`,
  `xset dpms force off`) blanked the screen contents but did **not** turn
  off the backlight. No resolution/fix is recorded in that issue.

**Conclusion (medium-high confidence): the backlight is most likely hardwired
on (tied to the 5V rail, always on whenever the module is powered), with no
software or dedicated-pin control exposed via the 10-pin connector.** This
was not explicitly stated by BIGTREETECH in the sources checked — it is an
inference from (a) the absence of a BL pin in the available pinout and (b)
the reported inability to turn the backlight off in software.

## Other notes

- Readme in the vendor hardware repo (`v2/readme.md`) gives a Raspberry Pi
  `fbtft` overlay wiring example that uses the **`ili9486`** kernel driver
  name (not `ili9488`) for this panel — a common practical quirk where the
  closest available Linux `fbtft` driver is used even though the physical
  chip is ILI9488. Wiring given there for a *direct GPIO* (non-connector)
  hookup: `SPI_MOSI=GPIO10`, `SPI_MISO=GPIO9`, `SPI_CLK=GPIO11`,
  `TFT35_SPI_CS=GPIO4`, `TFT35_SPI_RS=GPIO25`. This is Raspberry-Pi-specific
  pin numbering, not the module's own connector pin numbering, and is a
  different wiring path than the 10-pin XH2.54/FPC connector table above.
  - Source: https://github.com/bigtreetech/TFT35-SPI (raw `v2/readme.md`)
- Vendor hardware files available (for reference, not copied into this repo):
  schematic `v2/Hardware/BTT TFT35-SPI V2.1_SCH.pdf`, board IO drawing
  `v2/Hardware/BTT TFT35-SPI V2.1_IO.pdf`, dimensions `..._SIZE.pdf`, 3D
  step file, and the `NS2009.PDF` touch-controller datasheet — all at
  https://github.com/bigtreetech/TFT35-SPI/tree/master/v2

## Sources cited in this file

- https://github.com/bigtreetech/docs/blob/master/docs/TFT35%20SPI.md
- https://global.bttwiki.com/TFT35%20SPI.html
- https://github.com/bigtreetech/TFT35-SPI
- https://github.com/bigtreetech/TFT35-SPI/tree/master/v2
- https://github.com/bigtreetech/TFT35-SPI/blob/master/v2/BIGTREETECH%20TFT35%20SPI%20V2.1%20User%20Manual.pdf
- https://github.com/bigtreetech/TFT35-SPI/blob/master/v2/Hardware/NS2009.PDF
- https://github.com/bigtreetech/TFT35-SPI/blob/master/v2/Hardware/BTT%20TFT35-SPI%20V2.1_SCH.pdf
- https://dids.github.io/btt-pinouts-diagrams/
- https://github.com/bigtreetech/TFT35-SPI/issues/18
