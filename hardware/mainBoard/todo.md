# TODO

## Next board revision -- silkscreen (owner request, 2026-08-27)
- Silkscreen labels next to each relay terminal block (J3/J4/J8/J11) noting
  its **logical relay number** (R1-R4, or whatever numbering firmware ends
  up using after the R1-4 -> R0-3 rename below) AND its **NO/COM/NC**
  terminal identification. Bench testing this session found relay 2 and
  relay 4 physically swapped vs the schematic's Relay1..Relay4 net order
  (compensated in firmware, see `firmware/KilnFW/App/drivers/kiln_io.c`'s
  `kiln_relay_logical_to_pin_bit` table) -- panel silkscreen matching the
  logical number would make that kind of mismatch obvious on sight instead
  of needing a bench toggle-and-watch test to find.
- Silkscreen labels next to each thermocouple connector/screw-terminal
  noting its **logical channel number** (0-2 on the 3-channel daughterboard
  this session tested against). Same bench session found channel 0/1/2
  rotated vs. the physical terminal order (compensated in firmware, see
  `MAX31856_start_all()`'s `cs_pins`/`fault_pins` comment) -- same
  "physical label should match what firmware reports" reasoning as relays
  above.

## 2026-07-21 recheck summary
- Schematic integrity: clean (0 duplicate refs, 0 missing Value, 0 missing Footprint).
- Capacitor voltages (50V project default): 5 parts with no stated voltage (assumed default), 2
  groups intentionally rated lower (C15/C59/C67 and C46/C49/C52 at 16V) — no action needed.
- Part specs vs. Mouser link: 0 mismatches across 44 parts. FB1–FB8 (ferrite beads) previously
  flagged because their Value field held the MPN string (`74269244182`) instead of an electrical
  value; updated to `1.8k` (matching the project's resistor-value convention, e.g. R41) per
  Mouser's stated 1800ohm spec. R41 group relink and all prior mismatches also confirmed resolved.
- Stock sufficiency: all 44 parts covered for board quantity 1.

See [mouser_stock_report.md](mouser_stock_report.md) for the last Mouser stock/lifecycle/cost pass
(44 parts, USD 179.64 estimated BOM cost) and [schematic_health_report.md](schematic_health_report.md)
for the full audit.
