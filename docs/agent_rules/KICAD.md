# KiCad project, boards, mykicadMcp

Split out of CLAUDE.md 2026-10-14; history in docs/CLAUDE_HISTORY.md.

## Project Structure

Hardware and software halves; rationale in `docs/REPO_LAYOUT.md`.

```
hardware/   KiCad projects (mainBoard, ThermocoupleBoard, SaftyThermocoupleBoard,
            UnitTestFixture), shared lib/, datasheets/, simulation/, sourcing/
firmware/   KilnFW (ESP32-S3), SaftyFW (RP2040), CommonFW (shared link code)
tools/      PcTools (GUI + MCP for BOTH processors), mykicadMcp, pdfMcp
docs/       System-level documents spanning both halves
```

Main-board KiCad files live under **hardware/mainBoard/** (paths below relative to it).
**hardware/ThermocoupleBoard/** is the independent 3-channel daughterboard (MAX31856 U2-U4);
its `Thermocouple.kicad_sch` is a copy of the single-channel circuit also on disk (not in the
hierarchy) on the main board, so a fix in one copy often applies to the other.
**hardware/SaftyThermocoupleBoard/** carries the fourth MAX31856, for the RP2040. The main board
itself has no populated thermocouple channels.

Schematics: `kiln.kicad_sch` (top), `MainControler` (ESP32-S3-DevKitC), `Thermocouple`,
`Regulators` (input protection, 5V/3.3V), `5V_Regulator`, `CurrentSense`, `SSD` (7-segment),
`SaftyProcessor`. Layout: `kiln.kicad_pcb`, `kiln.kicad_prl`. Footprints/3D models in
`parts/SamacSys_Parts.pretty/` and `.3dshapes/` (do not modify unless sourcing new parts);
`fp-lib-table`. JLCPCB rules: `JLCPCB.kicad_dru`. Never edit `.kicad_*` files directly; most
editing happens in the KiCad GUI.

### Python / KiCad MCP

`tools/mykicadMcp/` is a git submodule (github.com/budarriola/mykicadMcp): `kicad_pcb_tool.py`
(PCB/netlist parser, no KiCad runtime), `kicad_mouser_tool.py`, `kicad_ipc_tool.py` (live KiCad
via IPC), `kicad_mcp_server.py` (HTTP 8766; `--transport stdio` available), `kicad_facade.py`,
`mcpkit_registry.py` (vendored from `tools/PcTools/src/mcpkit/registry.py`: edit the original,
re-vendor), `requirements-mcp.txt`, `README.md`, `NETCLASS_PLAN.md`. The server has 86 tools in
12 groups behind the facade; `inspect_kicad_project` and `get_kicad_ipc_status` are published
directly, the rest via `kicad_call`:

```
kicad_find(query="is this capacitor rated high enough")
kicad_call(name="inspect_kicad_project", args={"project_path":"hardware/mainBoard/kiln.kicad_pro"})
```

There is **no autorouter**. Replicate hand-routed work onto sibling instances of a repeated block
(e.g. the three ThermocoupleBoard channels) with `copy_kicad_component_routing`,
`diff_kicad_route_template`, `apply_kicad_route_template` (always diff before applying); the same
template pattern exists for placement, reference-designator positions and flips
(`kicad_find(query="template")`). Query components/nets via `list_kicad_components`,
`get_kicad_component`, `list_kicad_nets`, `get_kicad_net`, etc. Do not read the large `.kicad_sch`
files raw. No BOM CSV is checked in: regenerate via KiCad or `list_kicad_components`. DRC: KiCad
Tools > Design Rule Checker.

### Setup

Python MCP server: activate `tools\mykicadMcp\.venv\Scripts\Activate.ps1`; deps
`pip install -r tools\mykicadMcp\requirements-mcp.txt`; test `python tools\mykicadMcp\kicad_mcp_server.py`.
`tools/PcTools/uv.lock` is tracked: commit it with any `pyproject.toml` edit. `tools/pdfMcp/.venv`
holds the pdf-mcp server (`.mcp.json` points at it).

## Key Architecture Notes

- Hierarchical schematic: sub-sheets instantiated in the main schematic.
- Thermocouples: three MAX31856 (U2-U4, ThermocoupleBoard) feed the ESP-S3 zones, each with
  cold-junction compensation; a fourth (SaftyThermocoupleBoard) feeds the RP2040.
- Power: 12V input, 5V (logic, relay coils), 3.3V (MCU I/O, sensors); TVS (SMAJ24CA) and
  current-limiting input protection (`Regulators.kicad_sch`).
- Safety processor: a separate processor monitors critical parameters and can disable the main
  controller; isolated link via relay feedback circuits.
