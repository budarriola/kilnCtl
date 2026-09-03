# ST7796 panel support, panel auto-detection, and SPI DMA

Status: **Phases 1, 2, 3 and 5 landed 2026-09-01/02** (§12). Phase 4's
detection *logic* (`panel_detect.c`, host-tested, wired into `panel_spi.c`)
is also landed — it is correctly inert today because both descriptors'
`id_matches` stay `NULL` until the bench RDDID bytes in §4 are recorded, so
it always falls back to the Kconfig default and changes nothing observable.
Phase 6 (SPI DMA/async): 9.2 (`max_transfer_sz` raised to 32768,
`KILNCTL_SPI_MAX_TRANSFER_SZ`), 9.5 (MAX31856 on `spi_device_polling_transmit`
via `esp_spi_owner.c`'s `use_polling` flag) and 9.9 (bounded owner-transfer
wait) are landed. 9.3 (`SPI_TRANS_DMA_USE_PSRAM`) and 9.4 (hardware CS) are
now also landed, compiled-in but **default OFF** behind
`CONFIG_KILNCTL_SPI_DMA_USE_PSRAM` / `CONFIG_KILNCTL_SPI_HARDWARE_CS` — see
§12 Phase 6 for exactly what each changes and how to soak it. 9.1
(flush-duration measurement), 9.6 (async flush), 9.7 (ST7796 zero-copy) and
9.8 (clock step-up) remain open — see §12 Phase 6 for why they stay unbuilt
rather than merely unmeasured. Phase 0's blocking
hardware measurements (§4) are still open — nothing here has touched real
MSP4031 hardware yet; Phases 1-3/5 were built and host-tested against the
existing ILI9488 panel and the transcribed ST7796 init table only.
Written 2026-08-30, revised the same day after a research pass over the board
netlist, the vendor schematic, the ESP-IDF 6.0.2 source tree and the LVGL 9.5
source.

Goal: run KilnFW on either the existing BIGTREETECH TFT35 (ILI9488, resistive
NS2009 touch) or the LCDWIKI/Elecrow MSP4031 (ST7796S, capacitive FT6336U),
decided at runtime; and move the display SPI path onto asynchronous DMA so the
flush stops blocking the LVGL task. The end state is a display layer good enough
to build a much more polished UI on top of — that, not the second panel itself,
is the point.

Vendor package (pruned, ~11 MB of the original 394 MB):
`firmware/KilnFW/Datasheets/4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0_Keep/`

---

# STOP — read this before connecting the MSP4031 to J2

**Do not plug this module into J2 until one measurement has been taken and
reported back.**

The MSP4031's capacitive-touch I²C pull-ups **R4 and R6 (10 k each) go to raw
VCC**, not to the module's own 3.3 V rail. The vendor recommends powering the
module from **5 V**. If you do, module pins 10 (CTP_SCL) and 12 (CTP_SDA) idle at
**5 V** — and in the recommended harness those two pins land on J2 pins 3 and 2,
which are the **same physical I²C bus as the SX1509 expander and the ESP32-S3's
GPIO8/GPIO9**, already pulled to 3.3 V by R16/R17 (2.2 k).

Two supplies fighting across those resistors settle at roughly

```
(5 V / 10 k + 3.3 V / 2.2 k) / (1 / 10 k + 1 / 2.2 k)  ≈  3.6 V
```

That is at or above the absolute-maximum input voltage for both the ESP32-S3 and
the SX1509, and it also **backfeeds current into the 3.3 V rail** continuously.
This is the one item in this document that damages hardware rather than
software, and the damage is to the main board, not to the cheap module.

### The measurement to take first

- [ ] **With the module powered from 5 V, standalone, nothing else connected —
      measure module pin 10 and module pin 12 to ground.**
      - **Reads ≈ 3.3 V** → the module was built differently from the V1.0
        schematic; the hazard does not apply. Report the reading, then continue.
      - **Reads ≈ 5 V** → hazard confirmed. **Do not connect touch to J2.** Pick a
        fix from §3.4.2 first: (a) lift/remove R4 and R6 on the module
        (recommended), (b) power the module from 3.3 V, or (c) wire the display
        only and leave touch disconnected.

Display-only wiring (touch pins 10/11/12/13 left unconnected) is safe either way
and is a legitimate way to make progress before this is resolved.

Full detail in §3.4.2. The remaining hardware checks are in §4.

---

Also before touching hardware: two more wires are missing and one input must not
float — §3.4.1 (backlight), §3.4.3 (CTP_RST).

---

## 0. Decisions already locked

These are settled; do not re-open them inside this plan.

- **Exactly one task owns the panel.** Owner requirement, 2026-08-30. This is
  already why the UART `DISPLAY` task was deleted (`uart_bridge.c:1393-1400`,
  "two independent draw-call owners proved to be an unresolvable race",
  TODO.md 10.1). LVGL's task is that owner. See §8 — there is currently one
  violation to clean up.
- **No third-party library is vendored as a git submodule.** See §5.
- **PSRAM holds the LVGL draw buffers** (TODO 9.1a, done). An R8 PSRAM part is a
  hard requirement on any board reorder.
- **The `display_*` MCP tools were deleted** (ROADMAP.md:1071-1083). TODO.md
  1215-1222 is stale on this point.

---

## 1. What we have today

*(This table describes the pre-Phase-1 baseline, as a reference point. See §12
for what has since landed — `ILI9488.c` is now `panel_spi.c`, parameterised by
a `panel_desc_t`, with `panel_codec.c` and `st7796_panel.c` alongside it.)*

| Piece | Where | Notes |
|---|---|---|
| Panel driver | `App/drivers/ILI9488.c` (1651 lines) | No framebuffer; draws straight to panel GRAM. COLMOD fixed `0x66` (18 bpp) — ILI9488 cannot take RGB565 over 4-line SPI. |
| RGB conversion | `ILI9488.c:307` | One function, `ili9488_rgb565_to_rgb666()`. Every pixel widened 2 → 3 bytes. |
| Scratch / chunking | `ILI9488.h:73`, `ILI9488.c:710` | 1440 B (`480 × 3`), `MALLOC_CAP_DMA`. |
| D/C and ~RESET | `ILI9488.c:357`, `:581` | SX1509 IO15 / IO14 by default (~100 µs I²C per toggle), shadowed. Direct-GPIO bench mode via `KILNCTL_DISPLAY_DC_RESET_DIRECT_GPIO`. |
| Panel ID read | `ILI9488.c:1603` | RDDID `0x04` on a second 4 MHz device handle. **Nothing branches on it today.** |
| LVGL | `components/lvgl` 9.5.0, vendored local component | `LV_COLOR_FORMAT_RGB565`, `RENDER_MODE_PARTIAL`, two 480×40×2 = 38400 B buffers in PSRAM (`lvgl_port.c:544`), custom PSRAM allocator. |
| Flush | `lvgl_port.c:120` | Fully synchronous. The double buffer buys nothing today. |
| Touch | `App/drivers/NS2009.c` | 4-wire resistive, I²C 0x48/0x49 (probed). 3×3 affine calibration in NVS. PENIRQ unused. |
| SPI bus | `App/main.c:753-761` | **SPI2**, `SPI_DMA_CH_AUTO`, `max_transfer_sz = 1440`. SCLK 12 / MOSI 11 / MISO 13 — **these are exactly the SPI2 IOMUX pins**, so clock and data are already on IOMUX, not the GPIO matrix. Display CS = GPIO21 (matrix, fine). |
| SPI arbitration | `espInterfaces/esp_spi_owner.c` | One owner task + queue; **software CS** (`spics_io_num = -1` everywhere); blocking `spi_device_transmit()`. Display borrows the thermocouples' owner (`main.c:906`). |
| I²C | `main.c:634`, `i2c_owner.c` | New `i2c_master` driver. SX1509 at 0x3E. Each driver builds its own `i2c_owner_t`. |
| Build | ESP-IDF 6.0.2, esp32s3 | Managed components: `espressif__mdns` only. No submodules under KilnFW. All C11. |
| Host tests | `App/test/build_host_tests.ps1` | MSVC, `/std:c11`, type-only stubs. No SPI/I²C transaction mock exists. |

**External callers of the ILI9488 API are only six functions**, which is far less
coupling than the 30-function header suggests:
`blit_begin` / `blit_data` / `blit_end` / `get_dimensions` (`lvgl_port.c:143-146`,
`:408`, `:525`), `ILI9488_clear` (`screen_idle.c:114`), `ILI9488_start`
(`main.c:906`). Plus `ILI9488_SCRATCH_BYTES` at `main.c:759` and
`ILI9488_PANEL_WIDTH/HEIGHT` at `uart_bridge.c:1538-1543`. Everything else
(text engine, primitives, `read_id`, `set_power`, `set_rotation`) has **zero**
external callers — `ILI9488_start` uses the text engine internally for the
boot splash (`ILI9488.c:802-804`), which is the only reason it must keep
compiling.

---

## 2. What the new panel is

From `2-Specification/ST7796_Init.txt`, the ST7796S datasheet, and the FT6336U
datasheet in the `_Keep` tree.

- **ST7796S, 320×480 native** — same geometry as the ILI9488, so landscape is
  480×320 either way and the UI layout budget survives.
- **It accepts RGB565 over SPI.** COLMOD `3Ah = 05h`, confirmed by the
  datasheet's 4-SPI color-format section (§8.4.2.3) and by the vendor init.
  The 565 → 666 widening disappears and every flush ships **two thirds of the
  bytes**. This is the single most valuable difference.
- Init needs the `F0 C3` / `F0 96` unlock pair and the matching `F0 3C` / `F0 69`
  lock. Gamma tables are 14 bytes each (ILI9488's are 15). No `INVON`;
  inversion is left off via `B4 = 0x00`.
- MADCTL per rotation, BGR bit `0x08` always set: `0x48` / `0x28` / `0x88` /
  `0xE8`. Landscape (rotation 1, our default) is `0x28`.
- `2A`/`2B`/`2C`/`2E` windowing is standard — that whole half of the driver
  carries over unchanged.
- **Datasheet SPI timing, pages 53-54: write cycle min 66 ns = 15.15 MHz; read
  cycle min 150 ns = 6.67 MHz.** We currently run the display at 20 MHz. That is
  already above the ST7796's rated write clock, and the vendor's own demo runs
  80 MHz — everybody overclocks these panels. Treat 15 MHz as the spec, 40 MHz
  as the pragmatic target, and 80 MHz as an experiment. Never read back above
  6.67 MHz.
- **MSP4030 has no touch; MSP4031 has capacitive FT6336U.** Neither is
  resistive. We are assuming MSP4031.
- **FT6336U**: I²C 7-bit `0x38` (no conflict with the SX1509 at 0x3E),
  single-byte register addressing, 10–400 kHz. `0x02` = touch count,
  `0x03..0x08` = point 1, `0x09..0x0E` = point 2;
  X = `(r0 & 0x0F) << 8 | r1`, Y = `(r2 & 0x0F) << 8 | r3`. Identity:
  `0xA8` = 0x11, `0xA3` = 0x64, `0xA0` = 0x02 for the "U" part. Reports two
  points; LVGL's pointer indev takes one. Active-low `/INT`, active-low `RSTN`.
- Module operating temperature **−10 to +50 °C** (touch panel −20 to +70 °C).
  In a kiln-controller enclosure that deserves a measurement.
- Module PCB is **60.88 × 108.00 mm** with an 11.17 mm header stack, versus the
  current 3.5" panel. Enclosure change.

---

## 3. Hardware: how the MSP4031 would connect

### 3.0 Change budget — how little has to change

Owner constraint, 2026-08-30: *reuse the existing pins, change as little as
possible; changes are acceptable but only where they are actually needed.*

Good news — **the main board needs no modification at all.** All ten J2 circuits
are reused as they are, and no Kconfig pin setting changes:

- Power, ground, CS, SCK, MOSI and MISO map straight through.
- D/C and RESET are handled entirely by **how the harness is wired** (J2 pins 1
  and 4 cross over to module pins 4 and 5), and that crossover lands D/C on
  SX1509 IO15 — which is `KILNCTL_DISPLAY_SWAP_DC_RESET = n`, **today's
  default**. No firmware pin change.
- Touch I²C reuses J2 pins 2/3, again crossing in the cable only.
- `KILNCTL_DISPLAY_CS_IO` (21), `KILNCTL_SPI_SCLK/MOSI/MISO_IO` (12/11/13),
  `KILNCTL_DISPLAY_WIDTH/HEIGHT` (320/480) and `KILNCTL_DISPLAY_ROTATION` (1) are
  all correct for the new panel unchanged.

So the change list, smallest first:

| # | Change | Where | Needed? |
|---|---|---|---|
| 1 | Custom harness with two crossovers | cable only | **Required** — the connectors do not mate regardless (JST XH 10-way vs 14-pin 2.54 mm header). |
| 2 | Strap module pin 11 (CTP_RST) to module pin 1 (VCC) | inside the harness | **Required if touch is used** — a floating 74LVC245 input (3.4.3). Zero board impact; it is one extra crimp in the same connector shell. |
| 3 | Lift/remove R4 and R6 on the **module** | module PCB | **Required if touch is used and VCC = 5 V** (3.4.2). Two 0402/0603 resistors off the module — nothing on the main board. The alternative, running the module at 3.3 V, is *more* invasive (there is no 3.3 V on J2). |
| 4 | Leave module pins 8, 13, 14 unconnected | — | **No change.** Backlight stays permanently on exactly as today; touch polls instead of using INT; no SD card. |
| 5 | One flying wire, spare ESP32 GPIO → module pin 8 | bodge wire | **Optional.** Buys real backlight dim/off + PWM. Skip it and behaviour matches today's panel exactly. |

**If you want the absolute minimum: items 1 and 2, plus item 3 only after
metering pins 10/12 (§4).** Item 5 is the one discretionary change, and it is
purely additive — nothing else in the plan depends on it.

The one thing this cannot avoid is the enclosure: the module is 60.88 × 108 mm
against the current 3.5" panel.

### 3.1 Board side — J2, read from the generated netlist (not from the doc)

J2 is a **JST XH B10B-XH-AM, 2.50 mm, 10 circuits**, on `MainControler.kicad_sch`.

| J2 pin | Net | Goes to | Passive |
|---|---|---|---|
| 1 | `LCD_IORQ` | **SX1509 U5 pin 21 = IO14** | R18 10k **pull-UP** to 3.3V_Main |
| 2 | `SDA` | ESP GPIO8, SX1509 SDA, J6.5 | R16 2.2k pull-up 3.3V |
| 3 | `SCL` | ESP GPIO9, SX1509 SCL, J6.4 | R17 2.2k pull-up 3.3V |
| 4 | `LCD_Reset` | **SX1509 U5 pin 22 = IO15** | R22 10k **pull-DOWN** to GND |
| 5 | `CS3` | ESP GPIO21 | R19 10k pull-up |
| 6 | `CLK` | ESP GPIO12, J6.9 | R31 10k pull-up |
| 7 | `MOSI` | ESP GPIO11, J6.11 | R28 10k pull-up |
| 8 | `MISO` | ESP GPIO13, J6.10 | R30 10k **pull-down** |
| 9 | `GND_Main` | — | — |
| 10 | `5V_Main` | IC6 (5 V switcher), J6.20 | — |

**There is no 3.3 V pin on J2** (3.3V_Main reaches J6 pin 19 only). **There is
no backlight net anywhere in the design** — grepping the netlist for
BL/LED/backlight returns only the rail-indicator LEDs.

**Spare pins — the hard constraint.** All 16 SX1509 I/O are allocated:
IO0-3 Relay1-4, IO4-7 IO_1..4, IO8-10 thermoDrdy_0..2, IO11-13 IO_5..7,
IO14 LCD_IORQ, IO15 LCD_Reset. Only `OSCIO` is unconnected. **Zero spare
expander pins.** On the ESP32-S3, GPIO35/36/37 go to octal PSRAM, GPIO39-42 are
the JTAG path we flash over, GPIO43/44 are UART0, GPIO19/20 are USB, and
GPIO0/3/45/46 are strapping — leaving **GPIO15, GPIO16, GPIO1, GPIO2** as safe
spares, none of which route to J2. Any use is a flying wire.

### 3.2 Module side — MSP4031, one 14-pin header

Display, touch and microSD all share the single 14-pin 2.54 mm header (the 0.5 mm
14P FPC is an alternative to it, not an addition).

| Pin | Name | Path on module |
|---|---|---|
| 1 | VCC | → U1 XC6206 LDO → VCC3.3 (0.1 µF only on the output) |
| 2 | GND | — |
| 3 | LCD_CS | → U2 74LVC245PW A1 → panel CS |
| 4 | LCD_RST | → U2 A0 → panel RESET (pin 38) |
| 5 | LCD_RS | → U2 A2 → panel DCX (pin 9) — this is D/C |
| 6 | SDI(MOSI) | → U2 A4 → panel SDA, **and SD card CMD** |
| 7 | SCK | → U2 A3 → panel WRX/SCL, **and SD card CLK** |
| 8 | **LED** | R1 0R → gate of Q1 BSS138; **R7 10k pull-up to VCC3.3**; drain → R2 2R → LEDK |
| 9 | SDO(MISO) | **unbuffered**, straight from panel SDO, **and SD card DAT0** |
| 10 | CTP_SCL | BSS138 translator; header side **R6 10k to raw VCC** |
| 11 | CTP_RST | → U2 A5 → FT6336U RSTN. **No pull-up at the header.** |
| 12 | CTP_SDA | BSS138 translator; header side **R4 10k to raw VCC** |
| 13 | CTP_INT | **unbuffered**, straight from FT6336U INT, active low |
| 14 | SD_CS | → U2 A6; R8 10k pull-up to VCC3.3 |

U2 is a `74LVC245PW` with `VCC = VCC3.3`, `DIR = VCC3.3` (**A→B only**),
`OE = GND` (always on). So the seven host-driven lines are buffered, 5 V-tolerant
inputs; MISO and CTP_INT are raw 3.3 V outputs.

Supply: vendor recommends 5 V (*"When connected to 3.3 V the backlight
brightness will be slightly dim"* — the backlight runs off the LDO output, which
drops out at 3.3 V in). Backlight current **103 mA**, total 0.5 W.

Backlight: schematic block *"背光控制电路（可IO或者PWM控制）"* — backlight control
circuit, IO or PWM controllable, with *"LED不接或者接高电平，背光常亮"* = **pin left
unconnected or driven HIGH → backlight always on**. So it is **active HIGH,
PWM-able, and defaults ON** when unwired.

### 3.3 Recommended harness: J2 → module

The ordering below is deliberate — note pins 1 and 4 cross.

| J2 | Board net / driver | → Module | Module name | Verdict |
|---|---|---|---|---|
| 10 | `5V_Main` | 1 | VCC | OK. Adds ~103 mA to 5V_Main. |
| 9 | `GND_Main` | 2 | GND | OK |
| 5 | `CS3` = GPIO21 | 3 | LCD_CS | OK |
| **1** | `LCD_IORQ` = SX1509 **IO14**, R18 10k **pull-up** | **4** | LCD_RST | OK — pull-up releases the panel from reset while the expander floats. |
| **4** | `LCD_Reset` = SX1509 **IO15**, R22 10k **pull-down** | **5** | LCD_RS (D/C) | OK — pull-down defaults to "command". |
| 7 | `MOSI` = GPIO11 | 6 | SDI | OK |
| 6 | `CLK` = GPIO12 | 7 | SCK | OK |
| — | **nothing** | 8 | **LED** | **Missing.** Leave open → backlight permanently on, same as today. See 3.4.1. |
| 8 | `MISO` = GPIO13 | 9 | SDO | OK. RDDID readback is wireable. |
| **3** | `SCL` = GPIO9 | **10** | CTP_SCL | **Crosses in the cable**, and see 3.4.2. |
| — | **nothing** | 11 | **CTP_RST** | **Missing and must not float.** See 3.4.3. |
| **2** | `SDA` = GPIO8 | **12** | CTP_SDA | Crosses. See 3.4.2. |
| — | **nothing** | 13 | CTP_INT | Missing. Polling works; no functional loss. |
| — | **nothing** | 14 | SD_CS | Safe open (R8 keeps the card deselected). Do not fit a card. |

**Why 1↔4 cross.** This puts D/C on IO15, which is exactly
`KILNCTL_DISPLAY_SWAP_DC_RESET = n`, today's default — **no Kconfig change
needed**. It also lands the 10k pull-**up** on the panel's active-low RESET and
the 10k pull-**down** on D/C, both correct polarities. Straight-through would
require flipping the Kconfig *and* would hold the panel in reset via R22 whenever
the expander pins are inputs.

**A real gain:** unlike the BIGTREETECH panel — which per HARDWARE.md brings out
no panel reset at all — the MSP4031 **does** expose RESET. `kiln_io_lcd_reset()`
becomes a live recovery lever for the first time.

### 3.4 Mismatches, stated as problems

**3.4.1 — No backlight pin exists on the board, and no expander pin is free.**
Leaving module pin 8 open reproduces today's behaviour exactly (backlight always
on, HARDWARE.md:278-287). Real backlight control needs an 11th wire from a spare
ESP32 GPIO (15, 16, 1 or 2) direct to module pin 8: a 3.3 V push-pull drive into
a 10k-pulled-up MOSFET gate, trivially compatible and LEDC-PWM-able. **This is
the single highest-value bodge available**, and it closes ROADMAP.md:437-438
("HW change: LCD backlight control — no GPIO/PWM path exists yet") plus the
`KILNCTL_TOUCH_IDLE_TIMEOUT_MS` help text that currently says a real blank is
impossible.

**3.4.2 — 5 V on the shared I²C bus. The sharpest electrical problem.**
The module's header-side touch-I²C pull-ups R4/R6 (10k) go to **raw VCC**, not to
the on-module 3.3 V. With VCC = 5 V as the vendor recommends, module pins 10 and
12 idle at **5 V**. Those land on `SDA`/`SCL`, the same physical bus as the
SX1509, already pulled to 3.3 V by R16/R17 (2.2k). Two conflicting rails give an
idle of roughly

```
(5/10k + 3.3/2.2k) / (1/10k + 1/2.2k) ≈ 3.6 V
```

on ESP32-S3 GPIO8/GPIO9 and on the SX1509's I²C pins — at or over abs-max — plus
continuous backfeed into 3.3V_Main. Three ways out, none free:

- **(a) Lift or remove R4 and R6 on the module.** Cleanest. The board's 2.2k
  pull-ups then serve the whole bus and the BSS138 translators still work (their
  module-side pull-ups R3/R5 remain).
- **(b) Feed the module from 3.3 V.** Needs a bodge (no 3.3 V on J2), makes the
  backlight dim by the vendor's own admission, and pushes 103 mA through an
  XC6206 in dropout.
- **(c) Display only, no touch.** Loses the point of the MSP4031.

**3.4.3 — CTP_RST (module pin 11) is a floating 74LVC245 input if left open.**
The vendor pulled up SD_CS at the header but not CTP_RST. An unterminated LVC
input is undefined and draws crowbar current, and it drives the FT6336U's
active-low reset. With no spare board pin the practical answer is to strap module
pin 11 to module pin 1 (VCC) in the harness — legal at 5 V *if* U2 really is a
74LVC245 (5 V-tolerant inputs) and not a 74HC245 (clamps at VCC+0.5 V). Check the
marking; see §4.

**3.4.4 — Connector.** JST XH 2.50 mm 10-way on the board versus a 14-pin
2.54 mm male header on the module. Nothing off the shelf mates them; a custom
harness with both crossovers is mandatory. Buzz it out before applying power.

**3.4.5 — Thermal.** The 103 mA backlight runs from the module's XC6206 output,
so at VCC = 5 V the LDO burns ≈ (5 − 3.3) × 0.103 ≈ **175 mW** in a SOT-23, with
a 0.1 µF output cap and no bulk, inside a kiln-controller enclosure rated to
+50 °C.

**3.4.6 — Shared MISO.** Module pin 9 is common to the panel SDO *and* the
microSD data line, on a net that already carries three MAX31856s. Do not fit a
card; treat any MISO weirdness as a candidate cause.

**3.4.7 — Firmware knobs that go inert.** `KILNCTL_TOUCH_Z1_MAX_THRESHOLD` and
`KILNCTL_TOUCH_CAL_SWAP_XY` / `_INVERT_X` / `_INVERT_Y` are resistive concepts.
The rotation/axis part of them is still needed (§7); the pressure threshold is
not.

### 3.5 Every display/touch/LVGL Kconfig symbol today

| Line | Symbol | Default | Range |
|---|---|---|---|
| `Kconfig:168` | `KILNCTL_DISPLAY_CS_IO` | 21 | 0-48 |
| `:173` | `KILNCTL_DISPLAY_SPI_CLOCK_HZ` | 20000000 | — |
| `:177` | `KILNCTL_DISPLAY_WIDTH` (unrotated) | 320 | — |
| `:181` | `KILNCTL_DISPLAY_HEIGHT` (unrotated) | 480 | — |
| `:185` | `KILNCTL_DISPLAY_SWAP_DC_RESET` | n | — |
| `:199` | `KILNCTL_DISPLAY_ROTATION` | 1 | 0-3 |
| `:206` | `KILNCTL_DISPLAY_DC_RESET_DIRECT_GPIO` | n | — |
| `:222` | `KILNCTL_DISPLAY_DC_GPIO` | 0 | 0-48 |
| `:228` | `KILNCTL_DISPLAY_RESET_GPIO` | 1 | 0-48 |
| `:236` | `KILNCTL_TOUCH_IDLE_TIMEOUT_MS` | **0 (disabled)** | 0-3600000 |
| `:260` | `KILNCTL_TOUCH_Z1_MAX_THRESHOLD` | 200 | 0-4095 |
| `:287` | `KILNCTL_LVGL_BUF_ROWS` | 40 | 4-480 |
| `:301` | `KILNCTL_TOUCH_CAL_SWAP_XY` | n | — |
| `:314` | `KILNCTL_TOUCH_CAL_INVERT_X` | n | — |
| `:318` | `KILNCTL_TOUCH_CAL_INVERT_Y` | n | — |

---

## 4. Questions only the owner can answer

Hardware questions needing a meter, an eyeball, or a decision. Nothing in
Phase 1+ should start before the ones marked **blocking** are closed.

### Electrical — do these before applying power (all blocking)

**Question 1, and nothing else starts until it is answered:**

- [ ] **Measure module pin 10 (CTP_SCL) and pin 12 (CTP_SDA) to ground, module
      powered from 5 V, standalone.** ≈ 5 V confirms the hazard in the STOP block
      at the top of this document and means the module must not be connected to
      J2's I²C until R4/R6 are lifted (or the module is run at 3.3 V, or touch is
      left unwired). ≈ 3.3 V clears it. **Record the reading here:**
      `pin 10 = ____ V, pin 12 = ____ V`
- [ ] **Does the physical module match the V1.0 schematic?** Are R4/R6 fitted,
      and do they go to raw VCC or to VCC3.3? Meter module pin 10/12 to pin 1 and
      to a 3.3 V test point, unpowered. (This is the same hazard from the other
      direction — it explains *why* question 1 reads what it reads.)
- [ ] **Read the marking on U2.** Schematic says `74LVC245PW` (5 V-tolerant
      inputs). A substituted `74HC245` clamps at VCC+0.5 and would be damaged by
      a 5 V strap on CTP_RST. This gates the 3.4.3 recommendation.
- [ ] **Is there a pull-up on module pin 11 (CTP_RST)?** Meter pin 11 to VCC3.3
      unpowered. If absent: strap to VCC, or spend a spare GPIO.
- [ ] **Read the marking on U1**, confirm XC6206**P332**, then measure VCC3.3 and
      the LDO case temperature with the backlight on, at 5 V in and at 3.3 V in.
      This decides whether 3.4.2 option (b) is viable.
- [ ] **Does 5V_Main have 103 mA + panel logic of headroom?** Measure the rail
      under load; IC6 is the 5 V switcher.
- [ ] **Buzz out the finished harness** before power — the risk is the module
      end and the two crossovers (J2 1↔4, and J2 2/3 → module 12/10).

### Decisions

- [ ] **Which fix for the 5 V I²C hazard: (a) lift R4/R6, (b) run the module at
      3.3 V, or (c) display-only?** Everything about touch depends on this.
- [ ] **Do we fit the backlight bodge (3.4.1)?** One wire from GPIO15 or GPIO16
      to module pin 8 buys real dim/off, PWM brightness, and closes an open
      ROADMAP item. Recommend yes.
- [ ] **Which GPIO for the backlight, if yes?** GPIO15 or GPIO16 preferred; both
      need confirming as genuinely free on the physical DevKitC (the octal-PSRAM
      allocation is config-derived — confirm by part number).
- [ ] **Where does the panel physically go?** 60.88 × 108 mm plus an 11.17 mm
      header stack. Enclosure question, but it blocks bench work.
- [ ] **Is the goal to run both panels forever, or is the MSP4031 a replacement?**
      Permanent dual support justifies the full descriptor/detection design in
      §6; a straight swap would justify something much smaller. This plan assumes
      permanent dual support because that is what was asked for.
- [ ] **Do we want the microSD slot?** It shares MOSI/MISO/SCK and needs a CS.
      No board pin is free. Assumed no.

### Bench facts to record before writing code (blocking for Phase 3)

*(Phase 3's driver code has since landed by transcribing the vendor init table
exactly, without these — but RDDID matching for Phase 4's auto-detection, and
any DMA/flush work in Phase 6, still need the real numbers below.)*

- [ ] **RDDID (`0x04`) bytes from the ILI9488** on this wiring, recorded here.
- [ ] **RDDID (`0x04`) bytes from the ST7796** on this wiring, recorded here.
      If ambiguous, also try `0xD3` (RDID4). All-`0x00`/all-`0xFF` means MISO
      undriven, i.e. "no panel", not "panel 1" (`ILI9488.c:1641`).

  **How to actually read these bytes (2026-09-02):** the previous version of
  this checklist, and a comment in `panel_detect.h`, both said "boot the
  board as it is today (`KILNCTL_DISPLAY_PANEL=ili9488`, the default) and
  read the WARN logged on fallback" — that is wrong. `panel_spi.c`'s
  `ILI9488_start()` only calls `ILI9488_read_id()` inside the
  `CONFIG_KILNCTL_DISPLAY_PANEL_AUTO` branch; the explicit-ILI9488 default
  branch never reads RDDID at all, so a default-config boot logs nothing to
  record. The corrected one-step procedure, using a checked-in debug-only
  sdkconfig fragment (`firmware/KilnFW/sdkconfig.paneldetect.defaults`,
  never referenced by `sdkconfig.defaults` itself, so it changes nothing
  unless explicitly passed):
  1. `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.paneldetect.defaults" build`
     (or `kiln_call(name="build_kilnfw")` with that override) — selects AUTO
     instead of the shipped ILI9488 default. AUTO's own bootstrap panel is
     ILI9488, so reading the ILI9488's ID needs no wiring change.
  2. Flash and read the boot log for `panel auto-detect: no RDDID match
     (read 0x.. 0x.. 0x.. ...)` from `panel_spi.c` — that is the exact
     triple to record above. (An all-`0x00`/all-`0xFF` read still means
     "MISO not driven", not a real ID — see the bullet above.)
  3. For the ST7796 row: wire the MSP4031 to J2 (only after the STOP-block
     5 V hazard check earlier in this document) and rebuild with
     `CONFIG_KILNCTL_DISPLAY_PANEL_ST7796` forced explicitly instead of the
     AUTO fragment, so the panel actually initializes while its ID is read
     the same way.
  4. Rebuild WITHOUT the fragment (plain `idf.py build`) before flashing
     again — AUTO is a bench detection tool, not the shipped configuration.

  **Filling in the table once both rows exist (one step, ready now):**
  `panel_detect.h` has a new `panel_detect_id_equals(id, b0, b1, b2)` helper
  (host-tested, rejects all-`0x00`/all-`0xFF` centrally so no per-descriptor
  matcher has to remember to). For each panel, replace `.id_matches = NULL,`
  in `panel_spi.c` (`ili9488_panel_desc`) / `st7796_panel.c`
  (`st7796_panel_desc`) with a matcher built on it:
  ```c
  static bool ili9488_id_matches(const uint8_t id[3]) {
      return panel_detect_id_equals(id, 0x??, 0x??, 0x??); /* this row's bytes */
  }
  ```
  and point `.id_matches` at it. `test_panel_detect.c`'s
  `test_recorded_id_matcher_pattern()` already proves this exact recipe
  against a synthetic ID, and
  `test_id_equals_guard_beats_pathological_recorded_zero()` proves a
  transcribed-wrong or all-`0x00`/all-`0xFF` recorded value is caught rather
  than silently matching — both green today, ahead of the real bytes.
- [ ] **Does `0x38` answer** on the shared I²C bus with the module attached?
      The existing `i2c_scan_bus()` boot output already prints this.
- [ ] **Does the ST7796 blank black or white on DISPOFF + sleep-in?** The
      ILI9488 blanks *white*, which is why `screen_idle` paints black instead
      (bench finding 2026-08-17, `screen_idle.h:1-12`). If the ST7796 blanks
      black, `set_power(false)` becomes the better strategy on that panel — which
      means **the blank strategy belongs in the panel descriptor**, not in
      `screen_idle.c`.
- [ ] **Current flush duration**, measured, before any DMA work. 480×40×2 =
      38400 B at 20 MHz is ~15 ms of pure clock time split across 27 chunks of
      1440 B. Record the real number here; it is the budget everything else is
      judged against.

---

## 5. Third-party libraries — adopt none of them as submodules

### TFT_eSPI — skip

The blocker is architectural, not legal. `Processors/TFT_eSPI_ESP32*.h` includes
`soc/spi_reg.h` and drives the peripheral directly —
`WRITE_PERI_REG(SPI_MOSI_DLEN_REG(...))`, `SET_PERI_REG_MASK(SPI_CMD_REG(...),
SPI_USR)` with a busy-wait — configuring the bus once and then **owning** it. It
does not arbitrate. On a bus shared with three MAX31856s their clock and mode get
stomped mid-transaction. It is also C++ with hard `Arduino.h` / `SPI.h` /
`Print.h` includes (needs arduino-as-component), and it is configured by editing
`User_Setup.h` *inside the library*, which fights a submodule by design. License
is a stack of MIT + BSD-3 + BSD-2 + separate font terms.

→ If we ever want an off-the-shelf ST7796 driver, it is
**`espressif/esp_lcd_st7796`** (Apache-2.0, plain C), which is an ordinary
`spi_master` device.

### TJpg_Decoder — skip, we already ship it

A C++ Arduino wrapper around ChaN's `tjpgd.c` shaped to feed
`TFT_eSPI::pushImage()`; last commit 2023-11. LVGL 9.5 already bundles the same
ChaN engine: `LV_USE_TJPGD` in `lv_conf.h` auto-registers a `.jpg` decoder
(confirmed in the vendored copy's `lv_conf_template.h:981-1018`). Adding the
submodule would ship a second copy of `tjpgd.c`.

Note for later: LVGL's TJpgDec work buffer is hardcoded at 4096 bytes
(`src/libs/tjpgd/lv_tjpgd.c:25`) and `JD_FASTDECODE` is fixed at 1. Neither is
user-tunable without patching the component.

→ **Effort: one config flag.**

### Arduino-FT6336U — skip

Clean MIT, but ~300 lines of I²C register reads wrapped in `Wire`/`Arduino.h`,
and the only real asset (the register map) is in the datasheet we already keep.
Options: `lambage/esp_lcd_touch_ft6336u` from the component registry, or
hand-write it in the shape of `NS2009.c`. **Hand-writing is preferred** while we
stay off `esp_lcd`, because `esp_lcd_touch` drags in the whole `esp_lcd` object
model for a device that needs about twelve register reads.

The vendor's own two-file `FT6336.cpp`/`.h` is kept in the `_Keep` tree as a
register-usage reference. It is not to be compiled.

### Should we adopt `esp_lcd` at all?

The real fork in the road.

- **Option A — hand-written `ST7796.c` mirroring `ILI9488.c`.** Keeps the
  `spi_owner_t` arbitration, keeps the SX1509 D/C batching (which no upstream
  driver knows about), reuses ~80 % of the existing driver. Cost: a second large
  driver to maintain, and async DMA built by hand.
- **Option B — `esp_lcd` + `esp_lvgl_port`.** Async DMA flush, double buffering
  and the touch binding come for free. Cost, now measured rather than guessed:
  `esp_lcd_panel_io_spi` creates **its own** `spi_device_handle_t` and takes the
  IDF bus lock around each of `tx_param` / `rx_param` / `tx_color`
  (`esp_lcd_panel_io_spi.c:217-260`, `:273-315`, `:327-415`). It would not go
  through `esp_spi_owner`, and it holds the bus for the duration of each call —
  a burst of init commands stalls thermocouple reads. Worse, when a color
  transfer exceeds `max_transfer_sz` it splits into chunks with
  `SPI_TRANS_CS_KEEP_ACTIVE` on every non-final chunk, while the bus lock is
  released — so another device can in principle be scheduled between chunks with
  CS still asserted. Mitigation is to size `max_transfer_sz` ≥ one full flush so
  there is exactly one chunk.

**Recommendation: Option A now, B reachable later.** Write the byte-generation
half as a separate host-tested file (§6 step 1) so the encoding survives whichever
transport wins. Revisit B after §9 establishes whether the owner can lease the
bus safely.

---

## 6. Panel abstraction and auto-detection

### Step 1 — factor a codec

Follow the `max31856_codec.c` precedent: a `panel_codec.c/.h` with no
`spi_owner_transfer` in it, holding

- RGB565 → RGB666 widening (ILI9488) and the null conversion (ST7796);
- CASET/PASET/RAMWR byte generation;
- the MADCTL value for a given rotation, per panel;
- chunk splitting and window bounds/overrun arithmetic, lifted out of
  `ILI9488_blit_data`.

Pure functions, so they get host tests — and per the standing rule, **each new
check needs a negative test proving it can fail** before it counts as done.

### Step 2 — a panel descriptor

```c
typedef struct {
    const char *name;
    uint16_t panel_width, panel_height;   /* native, unrotated */
    uint8_t  colmod;                      /* 0x66 ILI9488, 0x55 ST7796 */
    uint8_t  bytes_per_pixel;             /* 3 or 2 */
    const uint8_t *init_seq; size_t init_len;
    const uint8_t madctl[4];              /* per rotation */
    bool (*id_matches)(const uint8_t id[3]);
    bool blank_via_power_off;             /* see the DISPOFF question in §4 */
} panel_desc_t;
```

`ILI9488.c` becomes `panel_spi.c` parameterised by a `const panel_desc_t *`.
Deliberately a data descriptor, not a function-pointer-per-operation interface:
only one panel is live per boot, and indirect calls in the pixel path would cost
more than they buy.

Note `lvgl_port.c:145` currently hardcodes `w*h*2` for the blit length — that
constant becomes `bytes_per_pixel` from the LVGL color format, not from the
panel.

### Step 3 — detection

1. **Kconfig override** — `KILNCTL_DISPLAY_PANEL` = `auto` (default) / `ili9488`
   / `st7796`. An explicit setting skips probing. Escape hatch for a panel whose
   ID register lies.
2. **RDDID (`0x04`)** on the existing slow read device, as `ILI9488_read_id()`
   already does; try each descriptor's `id_matches`. **Write the matchers against
   the bytes recorded in §4, never against datasheet nominal values** — the
   repeat failure mode in this repo is a consumer whose producer was assumed.
3. **Touch controller as corroboration.** NS2009 at `0x48`/`0x49` versus FT6336U
   at `0x38`, using the existing `i2c_master_probe` idiom (`NS2009.c:116`). If
   the SPI ID is ambiguous, the touch probe breaks the tie. Log loudly when the
   two signals disagree.
4. **Fallback**: no match → WARN naming the bytes read, then boot the
   Kconfig-configured default. A kiln controller with a blank screen is worse
   than one with a wrong gamma table.

Detection runs once in `main.c` before the panel starts. The resolved panel name
goes into the boot log, the diagnostics page, and the `READ_ID` reply.

### Step 4 — the UI geometry problem

`UI_THEME_PAGE_CONTENT_BUDGET_PX` (`ui_theme.h:155`, currently 268 px) is a
**compile-time `#define` derived from `CONFIG_KILNCTL_DISPLAY_WIDTH`**, and four
pages enforce it with `_Static_assert` (`ui_page_temperature.c:150`,
`ui_page_network.c:191,195`, `ui_page_network_manage.c:56`), mirrored by the
grep-based lint `App/test/check_ui_budget_asserts.ps1`.

**Runtime panel selection is fundamentally incompatible with static-assert
enforcement if the two panels ever differ in geometry.** They do not — both are
320×480 native, 480×320 landscape — so this survives *as long as that stays
true*. Record it as an explicit invariant: a future third panel with different
geometry breaks the enforcement mechanism, not just the layout. Do not silently
weaken the asserts to accommodate one.

---

## 7. Touch abstraction

`lvgl_port.c` types the touch device concretely as `NS2009Class *` (`:29`, `:396`)
and calls `NS2009_read()` directly. Introduce a `touch_dev_t` with
`read(x, y, pressed)` plus a `self_calibrating` flag.

- **NS2009**: unchanged, keeps the affine fit from `touch_cal_store`.
- **FT6336U**: reports panel coordinates, so it **bypasses** `touch_cal_*`
  entirely — but it still needs the rotation/axis mapping that today lives only
  inside the uncalibrated fallback (`lvgl_port.c:417-423`, driven by the
  `TOUCH_CAL_SWAP_XY`/`INVERT_*` Kconfig). Reuse that path, not the affine fit.
- Discard the second touch point; the LVGL indev is `LV_INDEV_TYPE_POINTER`
  (`lvgl_port.c:571`).

Things that break and must be handled, not discovered later:

- **`kiln_ui.c:270-273` forces a first-boot calibration flow** when
  `touch_cal_store_is_calibrated()` is false. With a self-calibrating controller
  that is permanently false, so the board would boot into a 3×3 target grid that
  can never complete. **This is a hard boot-path breakage.** Gate it on
  `self_calibrating`.
- **`lvgl_port.c:680-686`** logs a WARN every boot about running the
  "known-inaccurate bootstrap mapping". It would become permanently wrong.
- **`ui_page_touch_cal.c`** should say "this controller self-calibrates" rather
  than present a grid that does nothing.
- **`lvgl_port_get_last_raw_touch()`** (`lvgl_port.h:62`) returns raw ADC plus Z1
  pressure, consumed by `ui_page_touch_test.c` and exposed over
  `TOUCH_CMD_GET_STATE` (`uart_bridge.c:1482`). **FT6336U has no pressure.**
  Decide what that field reports — a fixed sentinel is better than a plausible
  lie.
- **Unaffected, by explicit design:** the touch-injection path
  (`lvgl_port.c:157-200`, `:315-388`) enters in screen pixels post-transform, so
  `TOUCH_CMD_INJECT` and the whole MCP-driven UI test harness carry over
  untouched. Keep it that way.
- Touch-target sizing (`ui_theme.c:17`, `:74`, `:105`;
  `UI_THEME_MIN_TOUCH_TARGET_PX` = 72) was tuned for a resistive stylus-ish
  contact. Capacitive finger contact has a different accuracy profile — worth
  re-tuning once it works, not before.
- There is an **open, unreproduced bug**: "the LCD back buttons do not work"
  (ROADMAP.md:213-219, TODO.md:1300), with `touch_calibrated: true` ruling out
  the obvious explanation. A controller swap will either fix this or make it
  permanently unreproducible. Try to characterise it on the ILI9488 **before**
  swapping, or the evidence is gone.

---

## 8. The single-owner invariant

**Exactly one task may issue draw calls to the panel.** That task is the LVGL
task (`lvgl_port_task`, `lvgl_port.c:475`, priority 4, static 8192-byte stack).
This is not a style preference — two independent draw-call owners was the
unresolvable race that got the UART DISPLAY task deleted on 2026-08-27
(`uart_bridge.c:1393-1400`).

Current state of the invariant:

- [x] **Violation: `screen_idle.c:114` calls `ILI9488_clear()` from the
      `screen_idle` task**, outside LVGL. **DONE (Phase 1):** `screen_idle`
      now sets a flag and the LVGL task performs the blank on the flush
      callback's off→on edge (`lvgl_port.c:126-136`).
- [x] **`main.c:906` `ILI9488_start()` draws the boot splash** from the main
      task. **DONE (Phase 1):** the ordering is now documented in `main.c` as
      a deliberate invariant — `ILI9488_start()` must run strictly before
      `lvgl_port_start()`.
- [ ] **`screen_idle` also reads touch** (`screen_idle.c:66`), which is why
      `main.c:914` passes `touch = NULL` deliberately: LVGL is the only reader.
      Same invariant, one bus down. Preserve it.
- [ ] Any async/DMA work in §9 must not turn the SPI completion callback into a
      second drawing context. The callback's entire job is: raise CS if software
      CS is still in use, and call `lv_display_flush_ready()`. Nothing else.

---

## 9. SPI: DMA, async, and hardware CS

Precision matters here, because part of the goal is already true, part is cheap,
and part is a bad idea.

**Already DMA.** The bus is created with `SPI_DMA_CH_AUTO` (`main.c:761`) and
`max_transfer_sz = 1440`, and the scratch is `MALLOC_CAP_DMA`. Every display
transfer over 64 bytes is already moved by DMA. **What is missing is
asynchrony**: `esp_spi_owner.c:33` calls `spi_device_transmit()`, which blocks
until completion, so the owner task idles during the transfer and the LVGL task
blocks behind it. The double buffer is doing nothing.

**MAX31856 stays non-DMA and should move to polling.** Its transfers are ≤ 17
bytes, which fit the FIFO. Measured IDF overhead on S3 is **11 µs for
`spi_device_polling_transmit` versus 26 µs for the interrupt path** — for short
register pokes, polling is strictly better. Note `MAX31856.c:342` asks for
`SPI_DMA_DISABLED` but never wins the race (`main.c` initializes the bus first,
deliberately); that comment is right about intent and misleading about outcome
and should be corrected when touched. **Do not "fix" it by letting MAX31856
initialize the bus** — `max_transfer_sz = 17` with no DMA caps transfers at 64
bytes and breaks every display push.

### Facts established for ESP-IDF 6.0.2 on ESP32-S3

- **SPI2 has 6 hardware CS lines, SPI3 has 3.** `soc_caps.h:284`:
  `SOC_SPI_PERIPH_CS_NUM(i) = ((i)==0)?2:(((i)==1)?6:3)`. The widely repeated
  "three CS pins" line in `driver/spi_master.h:173` is a stale ESP32-era comment
  and is wrong for the S3.
- **We are on SPI2** (`Kconfig:18` default) with 4 devices. They fit in hardware
  CS with room to spare.
- Hardware-CS and software-CS devices coexist on one host but share six lock
  slots: HW-CS fills from slot 0 up, SW-CS from slot 5 down
  (`spi_bus_lock.c:612-634`).
- **SCLK 12 / MOSI 11 / MISO 13 are the SPI2 IOMUX pins** — we are already on
  IOMUX for clock and data. CS on GPIO21 goes through the matrix, which does not
  force the other signals off IOMUX. Below 40 MHz the IDF docs say matrix and
  IOMUX behave identically anyway.
- **Absolute clock ceiling is 80 MHz** (APB clock source); the panel's own
  datasheet says 15.15 MHz write / 6.67 MHz read.
- **`max_transfer_sz` hard cap is 32768 bytes** (`SPI_LL_DMA_MAX_BIT_LEN = 1<<18`
  bits). Cost of raising it is **24 bytes of internal DRAM per 4092 bytes**
  (a TX and an RX descriptor array, 12 bytes each). 20480 → 144 B; 32768 → 216 B.
  **This is far cheaper than the previous draft assumed** — raising
  `max_transfer_sz` does *not* allocate a buffer, only descriptors.
- **PSRAM can be DMA'd directly** (`SOC_PSRAM_DMA_CAPABLE`) with the
  `SPI_TRANS_DMA_USE_PSRAM` transaction flag. **Without that flag every in-flight
  transaction is silently bounce-copied into a freshly allocated internal
  buffer** — precisely the internal-DRAM exhaustion mode this board has already
  hit. Cost with the flag is a fixed per-transaction `esp_rom_delay_us` of about
  11 µs at 40 MHz SPI / 80 MHz PSRAM; negligible over a 20 kB chunk, expensive
  over many small ones.
- `esp_dma_capable_malloc` was **removed in IDF 6.0**. Use
  `spi_bus_dma_memory_alloc(host, size, extra_caps)` (it handles cache and
  hardware alignment and is freed with plain `free()`), or
  `heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED)`.
- Alignment: internal DRAM needs 4-byte alignment (and 4-byte length for RX);
  PSRAM RX needs data-cache-line alignment (`CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE`,
  default 32) on both address and length. **PSRAM TX — our flush case — has no
  extra alignment requirement.**
- **No public `spi_device_*` API is ISR-safe**; `queue_trans` uses `xQueueSend`,
  not the FromISR variant.
- `pre_cb`/`post_cb` run **in the ISR for queued transactions but in the calling
  task for polling transactions** — the same callback, two contexts. Write for
  both. Mark them `IRAM_ATTR`, and use `gpio_ll_set_level(&GPIO, pin, lvl)`
  rather than `gpio_set_level()`, which lives in flash by default
  (`CONFIG_GPIO_CTRL_FUNC_IN_IRAM` defaults to n) and will fault with the cache
  disabled. This is exactly how `esp_lcd` drives D/C
  (`esp_lcd_panel_io_spi.c:419-431`).
- **`lv_display_flush_ready()` is ISR-safe** — it is a single volatile store
  (`lv_display.c:656`), and LVGL's threading doc names it and `lv_tick_inc()` as
  the only two functions callable from any context. Calling it from the SPI
  completion callback is the sanctioned pattern.
- LVGL does **not** block inside `flush_cb`; it blocks on the *next* flush into
  the same buffer, and by default that block is a **busy-wait**
  (`lv_refr.c:1458`, `while(disp->flushing);`). Register
  `lv_display_set_flush_wait_cb()` to replace it with a semaphore take, or a core
  spins whenever rendering outruns SPI.
- **`SPI_DEVICE_HALFDUPLEX` with both read and write phases is not supported on
  S3** (`SOC_SPI_HD_BOTH_INOUT_SUPPORTED` undefined). MAX31856 register reads
  must stay full-duplex.
- New in 6.0: `spi_transaction_t::override_freq_hz` changes clock per
  transaction (~30 µs cost) — a cleaner way to do slow panel reads than keeping a
  whole second device handle.

### Work items, in order

- [ ] **9.1 Measure first.** Flush duration and `lvgl_port_task` CPU, before any
      change. Record in §4.
- [ ] **9.2 Raise `max_transfer_sz` to one full LVGL buffer** (480×40×2 = 38400,
      or clamp `KILNCTL_LVGL_BUF_ROWS` so one flush fits under the 32768-byte
      hardware cap — 34 rows = 32640 B). Collapses 27 transactions per flush into
      one. Costs ~200 bytes of internal DRAM, not 38 kB. Cheapest real win.
- [x] **9.3 Set `SPI_TRANS_DMA_USE_PSRAM`** on flush transactions so LVGL's PSRAM
      buffers are DMA'd in place instead of bounce-copied into internal DRAM.
      **LANDED 2026-09-02, default OFF** behind `CONFIG_KILNCTL_SPI_DMA_USE_PSRAM`.
      `spi_owner_t::dma_use_psram` (set at `spi_owner_init()` time, plumbed
      from the Kconfig option, not read as a raw `#if` inside
      `esp_spi_owner.c` itself — kept a plain, host-testable struct field) —
      when true, `spi_owner_task()` sets the flag on every non-polling
      (display flush) transaction only; MAX31856 polling transfers never
      carry it, since they never touch a PSRAM buffer. Host-tested
      (`test_esp_spi_owner.c`): flag reaches the queued-path transaction when
      the owner was initialized with it on, and is absent both when it is
      off and on the polling path regardless. Not yet flash-verified — the
      plan's own `SPI_TRANS_DMA_TX_FAIL` check happens on real hardware, not
      host tests; enabling it and reading the 9.1 flush-duration number
      before/after is the confirmation step.
- [x] **9.4 Move the display and thermocouple devices to hardware CS**
      (`spics_io_num = <gpio>`). **LANDED 2026-09-02, default OFF** behind
      `CONFIG_KILNCTL_SPI_HARDWARE_CS`. With it on, `panel_spi.c` and
      `MAX31856.c` skip the manual CS `gpio_config()`/`gpio_set_level()` calls
      and instead set each device's real GPIO (display GPIO21, thermo
      GPIO14/17/18) on `spics_io_num`; `disp->cs_gpio`/`ch->cs_gpio` are set
      to -1 so every `spi_owner_transfer()`/`spi_owner_transfer_polling()`
      call site passes the sentinel `esp_spi_owner.c` now checks
      (`request.cs_pin >= 0`) before bit-banging — with it on, the owner task
      never touches that GPIO at all, leaving the SPI peripheral to
      assert/deassert it with correct setup/hold timing. Off (the default),
      this changes nothing: `bitbang_cs` is `true` for every call site in
      today's tree, proven by a host test alongside the negative-control test
      that a real `cs_pin` still bit-bangs exactly twice per transfer.
      **Not flash-verified** — this is the item the plan calls out as
      touching the live bus shared with three energized-heater thermocouple
      channels, so the STOP-block reasoning applies: verify with nothing
      energized (RDDID / MAX31856 register-read sanity, then a 9.1
      flush-duration re-measurement) before ever enabling it on a board with
      elements connected.
- [ ] **9.5 Thermocouple transfers → `spi_device_polling_transmit`.** 11 µs
      versus 26 µs. Do not mix polling and queued transactions on the *same*
      device; across devices the bus lock handles it.
- [ ] **9.6 Async flush.** Add `spi_owner_transfer_async(..., done_cb)` using
      `spi_device_queue_trans` / `get_trans_result` with `queue_size >= 2`. The
      owner task keeps arbitrating — a queued display transaction must still
      complete before a MAX31856 request is dequeued, or the guarantee the owner
      exists for is lost. The gain is that the *caller* returns early, not that
      transfers interleave. `lv_display_flush_ready()` moves into the completion
      callback; register `lv_display_set_flush_wait_cb()` so LVGL sleeps rather
      than spins. **Drain the result queue** — un-reaped transactions hold their
      bounce buffers.
- [ ] **9.7 ST7796 zero-copy flush.** With COLMOD `0x55` the LVGL buffer *is* the
      wire format: DMA straight out of it, no scratch, no per-chunk conversion.
      This is where the ST7796's two-thirds byte count actually pays.
- [ ] **9.8 Then reconsider clock speed.** Per-device clocks are independent, so
      the display can be raised alone. Step 20 → 26 → 40 MHz, verifying with
      `READ_ID` and a known-pattern blit at each step; watch the ribbon. The
      panel is rated 15 MHz, so this is knowingly out of spec — do not go past
      40 MHz without a scope.
- [x] **9.9 Fix `spi_owner_transfer()`'s `portMAX_DELAY` wait** (TODO.md:364).
      **DONE (Phase 1, landed ahead of the rest of §9):** bounded to a 1000 ms
      timeout backed by a heap slot pool, `wedged` surfaced on `/api/status`.
- [ ] **9.10 I²C is out of scope.** The SX1509 D/C toggle is latency-bound, not
      throughput-bound; the fix there is the existing direct-GPIO option or a
      board revision, not DMA.

**DMA async is a nice-to-have, not a requirement.** If 9.2, 9.3 and the ST7796's
byte reduction already bring the flush inside budget, stop there rather than
taking on the arbitration risk of 9.6. Precedent worth remembering: on the safety
UART link, DMA was evaluated and rejected — the real bug was elsewhere
(ROADMAP.md:69, LINK_PROTOCOL.md §3). Measure before believing DMA is the answer.

---

## 10. Internal DRAM budget

This constrains everything above, and the numbers are tight.

| Constant (`dram_margin.h`) | Value |
|---|---|
| `KILN_DRAM_FREE_ALARM_BYTES` | 11903 (measured at the 2026-08-22 failure) |
| `KILN_DRAM_LARGEST_ALARM_BYTES` | 8704 (same event) |
| `KILN_DRAM_FREE_KNOWN_BYTES` | 22123 (current measured trough, 2026-08-27) |
| `KILN_DRAM_FREE_FLOOR_BYTES` | 20480 (**owner policy floor**) |

**Headroom above the policy floor is about 1.6 kB.** The 2026-08-22 incident was
`/api/status` at 8000 s uptime with free=11903: a browser loading `/app.js` got
`ERR_CONNECTION_RESET` and every page sat on "Loading..." forever.

What this permits and forbids:

- **Permitted:** raising `max_transfer_sz` (9.2) — ~200 bytes of descriptors.
- **Permitted and actively good:** `SPI_TRANS_DMA_USE_PSRAM` (9.3), which
  *removes* per-transaction internal bounce buffers.
- **Not affordable:** moving a 19.2 kB LVGL buffer from PSRAM to internal DRAM.
  Do not propose it without showing where the bytes come from.
- **Known-fatal:** putting a task stack in PSRAM. An 8192-byte PSRAM stack
  crashed the board on `esp_task_stack_is_sane_cache_disabled()` and was reverted
  the same day (`lvgl_port.c:604-660`). A PSRAM stack cannot be running when the
  flash cache is disabled.
- `dram_margin.h:88-96` requires the KNOWN figures to be updated **in the same
  commit** as any change that moves the trough, in either direction — a stale
  regression check is the vacuous-check failure this repo has shipped three times.

---

## 11. Datasheet retention — done

- [x] Pruned vendor package copied to
      `Datasheets/4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0_Keep/`
      (~11 MB of 394 MB): specs (MSP4030/4031 EN, QD3958 glass), `ST7796_Init.txt`
      and `ST7796 initialization.md`, 4 mechanical drawings, ST7796S and FT6336U
      datasheets, the FT6336U register `.xlsx`, the module schematic PDF, the
      **Altium library** (`4.0inch_48P_0.8mm.PcbLib`, `QD3958_48P.SCHLIB`), the
      user manual, and under `Demo_ESP32/`: the instructions PDF, the vendor's
      `User_Setup.h` + `lv_conf.h`, the two-file `FT6336-arduino` reference
      source, and all 9 example sketches (`.ino`/`.h` only).
- [x] Commit the `_Keep` tree (34 files tracked; `41bb192`).
- [ ] Delete the original 394 MB `..._V1.0/` directory now that `_Keep` is
      committed. Confirmed safe: `..._V1.0/` is untracked and gitignored
      (`.gitignore:80`), so removing it is filesystem cleanup, not a git
      operation — plain `rm -rf`, no commit needed. Not done in this pass:
      this session's sandbox refuses `rm -rf` on a 394 MB tree; needs a
      session with that permission, or the owner doing it by hand.

Deliberately not kept: the `Image2Lcd` and `PCtoLCD2002` binaries (marked
破解版 / 完美版 — cracked, must not enter git), the opaque `.rar`, 199 MB of
bundled upstream libraries (pin versions instead: the demo targets LVGL 8.3.6 and
TFT_eSPI 2.5.43 — note **we are on LVGL 9.5**, so the vendor `lv_conf.h` is a
reference, not a drop-in), the 23 MB Espressif flash-download tool (we flash over
OpenOCD), the C51/CH32/STM32/UNO demos with their Keil build droppings, the
screenshot and sample-image sets, the stale Office `~$` lock files, and the CN
duplicates of every EN PDF.

---

## 12. Sequencing

Each phase ends somewhere the firmware still boots and drives the existing panel.

### Phase 0 — bench facts and hardware decisions
- [ ] **The STOP-block measurement at the top of this document, first.** Nothing
      else in Phase 0 happens until module pins 10/12 have been metered at 5 V
      and the reading recorded in §4.
- [ ] Close every remaining **blocking** item in §4.
- [ ] Record both panels' RDDID bytes, the DISPOFF blank colour, and the current
      flush time in this document.
- [ ] Characterise the "LCD back buttons don't work" bug on the ILI9488 while
      that panel is still the only one (§7).

### Phase 1 — invariants and cleanup (no new panel) — DONE 2026-09-01/02
- [x] Move `screen_idle`'s blank into the LVGL task (§8) — a live latent bug.
- [x] Fix `spi_owner_transfer()`'s unbounded wait (9.9).
- [x] Comment the `ILI9488_start`-before-`lvgl_port_start` ordering as an
      invariant.

### Phase 2 — codec split — DONE 2026-09-01/02
- [x] `panel_codec.c/.h` extracted, host-tested, with negative tests.
- [x] ILI9488 refactored onto it; behaviour byte-identical.
- [x] New test file wired into `build_host_tests.ps1` (`$sources` array plus a
      `run_test_*` declaration and call in `test_main.c`).

### Phase 3 — descriptor and ST7796 driver — DONE 2026-09-01/02
- [x] `panel_desc_t` introduced; `ILI9488.c` becomes `panel_spi.c`.
- [x] ST7796 init table transcribed from `ST7796_Init.txt` (byte-for-byte; see
      `st7796_panel.c`'s COLMOD note — vendor writes `0x05`, the plan's `0x55`
      lives only as descriptor metadata).
- [x] RGB565 fast path for ST7796.
- [x] Panel selectable by Kconfig only (`KILNCTL_DISPLAY_PANEL`, ILI9488
      default), no detection yet.

### Phase 4 — auto-detection — LOGIC LANDED, TABLE BLOCKED
- [x] `panel_detect.c/.h`: pure `panel_detect_choose()` — SPI-ID match count,
      touch-kind tiebreak/corroboration, disagreement flagging, Kconfig
      fallback. Host-tested (`test_panel_detect.c`), wired into
      `panel_spi.c:902-907`.
- [ ] Both descriptors' `id_matches` stay `NULL` (`panel_spi.c:1848`,
      `st7796_panel.c:137`) — **blocked on the recorded RDDID bytes in §4**.
      With both NULL, `panel_detect_choose()` always falls back to the
      Kconfig default; this is inert on the currently-attached ILI9488 by
      construction, not by omission.
- [x] **Tooling to take the bench measurement, and the one-step fill-in once
      it exists — LANDED 2026-09-02.** §4 above now has the corrected
      procedure (the old "read the WARN on a default boot" claim was wrong;
      that WARN only fires in `CONFIG_KILNCTL_DISPLAY_PANEL_AUTO`) plus a
      checked-in debug-only sdkconfig fragment
      (`sdkconfig.paneldetect.defaults`) that reuses the existing, already
      owner-arbitrated `ILI9488_read_id()`/`spi_owner_transfer()` path —
      no new SPI code, no change to MAX31856 timing or CS behavior. The
      fill-in step is `panel_detect_id_equals()` (new, host-tested helper in
      `panel_detect.h`) plus a one-line matcher per descriptor; the recipe
      and its negative case are proven now against a synthetic ID in
      `test_panel_detect.c` (`test_recorded_id_matcher_pattern()`,
      `test_id_equals_guard_beats_pathological_recorded_zero()`), so only
      the real bytes are still missing, not the code path that consumes
      them.

### Phase 5 — FT6336U and touch abstraction — DONE 2026-09-01/02
- [x] `touch_dev_t` with a `self_calibrating` flag.
- [x] FT6336U driver in the `NS2009.c` shape, polled — **UNVALIDATED ON
      HARDWARE**; identity is verified (`FOCALTECH_ID`/`CIPHER_MID`/
      `CIPHER_HIGH`) rather than accepting any device answering at 0x38.
- [x] `kiln_ui.c:273` forced-calibration boot path gated for self-calibrating
      devices.
- [x] `ui_page_touch_cal` and the raw-touch/pressure reporting handled (§7) —
      shows a notice instead of the 3×3 grid on self-calibrating devices.

### Phase 6 — SPI async/DMA — PARTIALLY LANDED, REST HARDWARE-GATED
- [x] 9.2 `max_transfer_sz` raised to 32768 (`KILNCTL_SPI_MAX_TRANSFER_SZ`,
      `settings.h:179`, `main.c:777`). Changes nothing observable today —
      the ILI9488 codec still chunks every flush at `ILI9488_SCRATCH_BYTES`
      (1440 B) regardless of the host's ceiling.
- [x] 9.5 MAX31856 → `spi_device_polling_transmit` (`esp_spi_owner.c`'s
      `use_polling` request flag, dispatched only ever from the owner task's
      own thread — never an ISR, never a caller's task).
- [x] 9.9 bounded owner-transfer wait (landed Phase 1, see above).
- [x] 9.3, 9.4 **LANDED 2026-09-02, default OFF.** Written blind (no flash,
      no scope) in a pass whose owner was mid-firing — safe only because
      both are gated behind Kconfig options that generate no observable
      behavior change until explicitly turned on
      (`CONFIG_KILNCTL_SPI_DMA_USE_PSRAM`, `CONFIG_KILNCTL_SPI_HARDWARE_CS`),
      host-tested for the plumbing and guards each one touches (10 tests
      total in `test_esp_spi_owner.c`, each proven to fail under a targeted
      mutation), and confirmed to leave `build_kilnfw`/`run_repo_checks`
      green with the options at their sdkconfig default. Neither is
      flash-verified — see 9.3/9.4's own entries above for exactly what
      "confirmed to work" looks like for each, to be done on a bench cycle
      with nothing energized before either is ever turned on near a live
      kiln.
- [ ] 9.1, 9.6, 9.7, 9.8 remain open, and none of them is a safe default-off
      flag the way 9.3/9.4 were:
      - **9.1** is a real-hardware measurement by definition — no config
        option or host test can produce it. First hardware step, unchanged
        from before this pass: take it once the board is between profiles.
      - **9.6 (async flush)** does not reduce to a flag. It replaces
        `spi_owner_transfer()`'s synchronous "queue, then block on a
        completion semaphore" contract with a callback-driven one
        (`spi_device_queue_trans`/`get_trans_result`, `queue_size >= 2`,
        `lv_display_set_flush_wait_cb()`), which is new control flow on the
        exact bus arbitration this plan's own §13 flags as the standing risk
        ("Any async work needs a test that can *fail* — a deliberately
        interleaved transfer the arbiter rejects — before it is trusted").
        A default-off flag would just be an unexercised second code path
        through the owner task, not a de-risked one. Per §9's own "DMA async
        is a nice-to-have, not a requirement" note, the right next step is
        the 9.1 measurement first — if 9.2+9.3 (and the ST7796's byte
        reduction, eventually) already bring flush time inside budget, 9.6
        may not be worth the arbitration risk at all.
      - **9.7 (ST7796 zero-copy)** is scoped to a panel that has **never run
        on hardware** (`st7796_panel.c`, STOP block at the top of this
        document) — there is no working ST7796 bring-up yet for a zero-copy
        flush to be verified against, default-off or not.
      - **9.8 (clock step-up)** is a bench tuning procedure, not new code:
        `KILNCTL_DISPLAY_SPI_CLOCK_HZ` is already a plain Kconfig int
        (default 20 MHz, `Kconfig` under "ILI9488 Display"), so the
        mechanism this item wants already exists. What is missing is the
        scope-verified bench step itself (`READ_ID` + known-pattern blit at
        each of 20/26/40 MHz, watching the ribbon) — there is nothing here
        for a firmware pass to build.

### Phase 7 — the actual point: a better UI
- [ ] `LV_USE_TJPGD` if images are wanted.
- [ ] Backlight PWM (dim/off on idle, touch-driven wake) if the bodge in 3.4.1
      was fitted — this is what `KILNCTL_TOUCH_IDLE_TIMEOUT_MS` has been waiting
      for.
- [ ] Theme/style pass (TODO.md:1223-1225).

---

## 13. Risks

- **The 5 V I²C hazard is the top risk in this document** — see the STOP block at
  the top and §3.4.2. It can damage the SX1509 **and** the ESP32-S3, i.e. the
  main board rather than the replaceable module, and it is the only item here
  that breaks hardware rather than software. Meter module pins 10 and 12 before
  the module is connected to J2.
- **RDDID may not distinguish the panels.** If both answer alike, or the ST7796
  leaves MISO undriven on this wiring, detection falls back to the I²C touch
  signal alone. Phase 0 settles it.
- **Internal DRAM (§10)** — 1.6 kB above the policy floor. The cheap DMA items
  fit; the expensive ones do not.
- **Bus arbitration under async.** The MAX31856s' correctness depends on nothing
  interleaving. Any async work needs a test that can *fail* — a deliberately
  interleaved transfer the arbiter rejects — before it is trusted.
- **Two panels, one static UI budget.** Both are 480×320 landscape so
  `UI_THEME_PAGE_CONTENT_BUDGET_PX` survives, but the enforcement mechanism is
  compile-time and a future panel with different geometry breaks it (§6 step 4).
  The no-scroll rule (TODO.md:1264-1269, UI_PLAN.md:44-46) leaves no room to
  absorb a smaller page.
- **Scope creep into `esp_lcd`.** Option B is attractive and would rewrite the
  arbitration model at the same time as adding a panel. Keep them separate.

---

## 14. Documents to update when this lands

- [ ] `docs/HARDWARE.md` — the display section, once the harness and the two
      pin-1/pin-4 ambiguities are resolved on real hardware.
- [ ] `docs/ILI9488.md` — either generalise to a panel doc or add an ST7796 twin.
- [ ] `App/drivers/README.md:14` — the ILI9488 row.
- [x] `TODO.md` — the stale 1215-1222 entry corrected to point at
      ROADMAP.md:1140; 364-365 already closed with 9.9.
- [ ] `ROADMAP.md:437-438` — the backlight-control HW item, if the bodge lands.
- [ ] `Datasheets/README.md` — add the MSP4031 package.
