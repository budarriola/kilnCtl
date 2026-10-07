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
2. Kconfig help for `KILNCTL_THERMO_SPI_CLOCK_HZ` and SaftyFW
   `SPI_OWNER_BAUDRATE_HZ` still justify the 4 MHz cap by the deleted SimFW
   fixture. Cap kept in code (Kconfig range, `_Static_assert`); keep or lift?
3. Kconfig menu title and `docs/ILI9488.md` still frame the TFT35/ILI9488
   although the default panel is ST7796 (MSP4031).
4. Back-fed 3V3 on A1 (HARDWARE.md section 7) is a schematic property, unchanged
   by this audit; the USB-vs-IC5 contention warning stands unverified on the
   bench.

Not checked: the memory facts (R4 expander bit is not K4/heat enable, CT on
GPIO28, E-stop jumper fitted) were consistent with `kiln_io.c`
(`{0,3,2,1}` relay map) and the docs; no contradiction found.
