# KiCad MCP Server - Tool Reference

`python/kicad_mcp_server.py` exposes 61 tools over stdio MCP for working with this project's
KiCad files (and, for a handful of tools, a live-running KiCad session). This is the full
reference, organized into groups; see [python/README-mcp.md](../../README-mcp.md) for setup
and narrated example workflows instead.

Most tools take a `project_path` (a KiCad project directory, `.kicad_pro`, `.kicad_pcb`, or
`.kicad_sch` path - any of these resolve to the same project). Anything that edits a file
defaults to `write: false` (a dry run that returns a preview) - inspect the result, then call
again with `write: true` to actually save. Board-editing tools also accept
`allow_while_open: false`, which by default refuses to write while KiCad has the board open
for editing (to avoid racing the GUI's own unsaved state); see
[05-layout-and-placement.md](05-layout-and-placement.md) for detail on that guard.

## Groups

| Group | File | What it covers |
|---|---|---|
| 1. Project Inspection & PCB/Netlist | [01-inspection-and-netlist.md](01-inspection-and-netlist.md) | Read-only board/netlist queries: components, nets, pads, pin positions - no KiCad runtime required. |
| 2. Schematic Data & Property Maintenance | [02-schematic-data.md](02-schematic-data.md) | Reading/writing schematic symbol properties (Value, Footprint, Manufacturer_Part_Number, etc), integrity/capacitor-voltage/part-spec audits. |
| 3. Mouser Sourcing & BOM | [03-mouser-sourcing.md](03-mouser-sourcing.md) | Mouser link discovery, live stock/price lookups, alternate-link ranking, stock sufficiency, stock/lifecycle reports, buy-list generation, and the one-call schematic health check. Most of these REQUIRE `MOUSER_API_KEY`. |
| 4. Hierarchical Groups & Sibling Discovery | [04-hierarchical-groups.md](04-hierarchical-groups.md) | Finding which footprints belong to one repeated sub-circuit instance, and matching members across sheets/instances. |
| 5. Layout & Placement | [05-layout-and-placement.md](05-layout-and-placement.md) | Moving/aligning footprints, copying a known-good instance's layout onto siblings, collision checks. |
| 6. PCB Groups (Ctrl+G) | [06-pcb-groups.md](06-pcb-groups.md) | Creating/listing/deleting the board-file grouping construct KiCad's GUI uses for "select as one unit." |
| 7. Silkscreen Label Position Templates | [07-label-position-templates.md](07-label-position-templates.md) | Copying a hand-decluttered text property's (Reference/Value label) offset onto sibling instances. |
| 8. Footprint Flip Templates | [08-footprint-flip-templates.md](08-footprint-flip-templates.md) | Copying a correctly front/back-flipped footprint's full flip state onto siblings that need the same treatment. |
| 9. Live KiCad IPC Tools | [09-live-ipc-tools.md](09-live-ipc-tools.md) | Tools that talk to a *running* KiCad instance (real geometry, GUI selection/highlighting) instead of parsing files on disk. |

## Picking the right group

- Just need to read board/schematic data? Start with **Group 1** (PCB/netlist) or **Group 2**
  (schematic symbol properties) depending on whether you need footprint/net facts or the
  properties KiCad stores on the schematic symbol itself (Datasheet, Mouser fields, MPN, etc).
- Working on parts sourcing, stock, pricing, or a buy list? **Group 3**.
- Checking the schematic for errors before ordering parts or sending the board to fab? Start
  with `audit_kicad_schematic_health` in **Group 3** - it runs Group 2's integrity/voltage/spec
  audits and Group 3's stock-sufficiency check in one call and writes a Markdown summary.
- Making a repeated sub-circuit (relay channel, thermocouple input, regulator block, etc)
  consistent across instances? Start with **Group 4** to find/match members, then **Group 5**
  (position), **Group 7** (label offsets), and/or **Group 8** (front/back flip) depending on
  what needs copying.
- Want a human reviewing a change to see it happen live, or want to point at a component in
  the GUI instead of typing its reference? **Group 9** - but it needs KiCad open with the IPC
  API enabled (Preferences > Plugins > Enable IPC API); everything else works from the files
  on disk alone.
