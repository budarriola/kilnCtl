# HARDWARE.md audit, 2026-10-06

Scope: `firmware/KilnFW/docs/HARDWARE.md`, `firmware/SaftyFW/docs/HARDWARE.md`
(plus the two stale lines in SaftyFW `ARCHITECTURE.md`), and
`firmware/UnitTestFw/UnitTest/docs/HARDWARE.md`. Sources: Kconfig and
`settings.h`, `kiln_io.c`, and the main-board netlist exported read-only with
`kicad-cli sch export netlist` (the kicad MCP returned empty nets). No
`.kicad_*` file was touched.

## Fixed (code or netlist proves the doc wrong)

- U6 (ADuM1201WT) channel pins were swapped. Netlist: Pico GP4 -> VIA (pin 7)
  -> VOA (pin 2) -> ESP GPIO4; ESP GPIO5 -> VIB (pin 3) -> VOB (pin 6) -> Pico
  GP5. Corrected in both HARDWARE.md files (GPIO rows, barrier tables, pin
  summaries).
- R7/R9/R12/R15 do not exist in the netlist and no pull-up sits on the
  DataFromSafty/PicoRx nets. Docs said R15/R9 were "still fitted"; reworded.
- Pico 3.3 V source named "IC3 (LT8631)"; netlist says IC5 (MAXM17572) makes
  `3.3v_Safty`, IC7 (MAXM17572) makes `5v_Safty`. Fixed in SaftyFW HARDWARE.md
  and ARCHITECTURE.md.
- R65 pull-down returns to `GND_Safty`, not "RelayGND" (HARDWARE.md and
  ARCHITECTURE.md).
- J7 pin 1 is 3.3 V (safety) via R51 0R, not "no connect" (KilnFW doc).
- KilnFW display section: MSP4031/ST7796 is the Kconfig default and fitted;
  backlight is GPIO15 flying wire with LEDC PWM; idle timeout default is 0
  (disabled), not 60 s.
- SimFW references to deleted files (`SPI_ACCESS_AUDIT.md`, `BENCH_RUNBOOK`)
  reworded as historical.
- SaftyFW "protocol still 12" now notes the current value, 16
  (`kilnlink_version.h`).

UnitTestFw HARDWARE.md checked against its Kconfig (pins 8/9, 43/44, 4/5/6, 18,
baud 115200, addresses 0x3C/0x20): all match, no change.

## Discrepancies needing owner review

1. Physical R15/R9 pull-ups: old docs said fitted, schematic has none. Is the
   built board populated with parts the schematic no longer has?
   **Owner: deferred 2026-10-06, ignore for now.**
2. Kconfig help for `KILNCTL_THERMO_SPI_CLOCK_HZ` and SaftyFW
   `SPI_OWNER_BAUDRATE_HZ` still justified the 4 MHz cap by the deleted SimFW
   fixture. **Resolved by owner decision 2026-10-06: kept at 4 MHz, rationale
   reworded** (4 MHz is ample for MAX31856 conversion rates; faster SPI is not
   needed). Help text, doc comments and docs reworded; no value changed.
3. RESOLVED 2026-10-06 (text only): Kconfig menu titles/choice labels and
   `firmware/KilnFW/docs/ILI9488.md` now name the ST7796 (MSP4031) as the
   fitted panel and ILI9488 as also supported. The doc keeps its filename
   (about 8 inbound links); no Kconfig symbol was renamed.
4. Back-fed 3V3 on A1 (HARDWARE.md section 7) is a schematic property, unchanged
   by this audit; the USB-vs-IC5 contention warning stands unverified on the
   bench. **Owner: ignore (2026-10-06).**

Not checked: the memory facts (R4 expander bit is not K4/heat enable, CT on
GPIO28, E-stop jumper fitted) were consistent with `kiln_io.c`
(`{0,3,2,1}` relay map) and the docs; no contradiction found.

## CT and E-stop netlist trace, 2026-10-07

Method: `kicad-cli sch export netlist` of `hardware/mainBoard/kiln.kicad_sch` (read-only,
output outside the repo; the kicad MCP `list_kicad_nets` still returned `[]`), parsed per net.
Pin names are net names from that export.

CT, per channel (sheet / jack / op-amp / ADC net / Pico pin):

| Ch | Sheet | Jack (DNP terminal) | Op-amp | Net | Pico pin |
|---|---|---|---|---|---|
| 1 | CurrentSense | J13 (J12) | U8 | `/SaftyProcessor/Current1` | A1 pin 31, GPIO26/ADC0 |
| 2 | CurrentSense1 | J15 (J14) | U7 | `/SaftyProcessor/Current2` | A1 pin 32, GPIO27/ADC1 |
| 3 | CurrentSense2 | J17 (J16) | U9 | `/SaftyProcessor/Current3` | A1 pin 34, GPIO28/ADC2 |

Chain, channel 3 (others identical): J17 tip (sleeve, ring = `GND_Safty`; TIP_SWITCH
unconnected) → R45 10k → U9 pin 2 (summing node, with C50 4.7 pF, D20/D21 BZX84C3V3 pair to
GND, rectifier diode D23) ; R115 1M tip-to-GND ; U9 pin 3 = GND ; U9 pin 1 → D22/D23 →
hold node U9 pin 5 (R89 1M ∥ C73 1 uF) ; U9B buffer (pin 7 tied to pin 6) = `Current3` →
GPIO28 ; R50 7.15k from `Current3` back to the summing node ; U9 supplied from `3.3v_Safty`.
R72/R78/R84 (burden), J12/J14/J16 (screw terminals) carry the DNP attribute; J13/J15/J17, R90
do not. Confirmed against the docs: pins 26/27/28 = Current1/2/3, DNP set, R43/R46 values,
`board_pins.h` `SAFTYFW_PIN_ADC*_GPIO` and `SAFTYFW_ADC_CH_CURRENT*` (26/27/28, ch 0/1/2). No
CT net reaches any ESP32 pin. Corrected: D12/D13 and C44 sit on the U8A summing node behind
R43, not on the jack (HARDWARE.md table, CURRENT_SENSE.md clamp wording); added the GPIO28
trace to SaftyFW HARDWARE.md section 9.

E-stop: net `/SaftyProcessor/estop` = A1 pin 12 (GPIO9), J1 pin 2, R10 (1k to `3.3v_Safty`),
C3 (10 nF to `GND_Safty`) and nothing else; J1 pin 1 = `GND_Safty`. `SAFTYFW_PIN_ESTOP` = 9
matches. The net does not touch K4, Q4, J10 or any ESP pin. K4 coil: pin 1 on `12v_Safty`,
pin 12 on Q4 drain (D8 flyback, D9 SMAJ24CA, C42 across it); Q4 gate from `saftyRelay` (A1 pin 9,
GPIO6) via R64 120R with R65 10k to `GND_Safty`; K4 contacts 8/9/10 go to J10 pins 1/2/3 only
(R66/R67 DNP; K4 pins 3/4/5 unconnected). So the netlist supports the doc: the fitted E-stop
jumper is a GPIO9 input only, there is no hardwired heat cut on the board, and the E-stop is
firmware-mediated (GPIO9 -> guard S7 -> GPIO6 -> K4, whose contact only reaches the J10
terminal for external wiring). Corrected: J10 header said "NC/COM/NO" against the pin 1 NO / 2
COM / 3 NC table below it; "contacts 3 and 5 unconnected" is 3, 4 and 5; J1 pin roles added.

Stale statements found by the same trace and fixed: `board_pins.h` UART comments and
`KilnFW/docs/SAFETY_LINK.md` table still had the ADuM1201 VIA/VIB channels swapped and an R9
pull-up (same error the first pass fixed in the two HARDWARE.md files).

Owner-review items (cannot be settled from the netlist):

1. J10 NO/COM/NC identity: the export carries no pin functions for K4 (blank names), so
   pin 1 NO / 2 COM / 3 NC remains symbol-drawing evidence. Needs a continuity check on the
   physical relay before wiring a contactor coil.
2. Physical presence of the E-stop jumper/contact and the CT: the netlist cannot show what
   is fitted. The bench facts (GPIO9 reads LOW; CT on J17/GPIO28, ~59 mV offset, 1 A:1 V) stay
   as measured, unverified here.
3. DNP flags are schematic attributes; confirm R72/R78/R84 and R66/R67 are actually
   unpopulated on the built board (this matters for the voltage-output-CT requirement).
4. Whether D14/D15 (D22/D23 on channel 3) orientation matches the CURRENT_SENSE.md
   description: the netlist gives pin numbers, not polarity; needs the symbol or a diode test.
