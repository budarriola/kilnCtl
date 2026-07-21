# TODO

## Mouser stock / lifecycle report + BOM cost

Ran `generate_kicad_mouser_stock_report` (now with per-part JSON caching and BOM cost) — full report
written to [mouser_stock_report.md](mouser_stock_report.md). Checked all 44 unique parts (all have a
Mouser link now).

**Estimated one-board BOM cost: USD 179.64**, across 43 priced line items (Mouser's own quantity-break
pricing, ×the quantity actually used per board). 1 part (`C1` group, `0.1uf`, 28 references) has no
Mouser price-break data and is excluded from that total — see the report's "Excluded from cost total"
section.

Flagged items to fix:

- [x] **R47** — fixed: now `90.9k 1%`, Vishay `CRCW040290K9FKED` (correct MPN for the value) — **130,450 in stock** at Mouser, $0.10 ea (qty 1).
- [ ] **R77, R83, R89** — `1M`, Vishay `CRCW040247K5FKED` — same part number as R47, **out of stock**.
- [ ] **R96, R103** — `154k 1%`, Vishay `CRCW040247K5FKED` — same part number as R47/R77 group, **out
  of stock**.
- [ ] **Data quality issue, not just stock**: R47, R77/R83/R89, and R96/R103 have three different
  stated Values (`90.7k 1%`, `1M`, `154k 1%`) but all three currently link to the exact same Mouser
  part, `Vishay CRCW040247K5FKED` (a 47.5kΩ resistor — none of the three stated values). This looks
  like a stale/copy-pasted `Datasheet` property rather than the correct part for each value. Fix the
  links to point at the correct resistor for each stated value first (the `...FKEDHP`/`...FKEDC`
  packaging variants of the 47.5kΩ part are in stock, for reference if repackaging is ever relevant),
  then re-check stock/cost for the corrected parts.
- [ ] **C15, C59, C67** — `47uF 16V`, Murata `GRM32EC81C476KE15L` — Mouser lists this **End of Life**.
  Needs a replacement part before this design goes to fab.
