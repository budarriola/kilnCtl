# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**kilnCtl** is a KiCad-based electronics design project for a kiln controller. It includes a hierarchical schematic design, PCB layout, component library, and Python tools for programmatic access to project data.

## Where to start

Fresh clone: run `tools/setup.ps1`, then open `kilnCtl.code-workspace` (not the
folder). `docs/SETUP.md` explains what it generates and why those files are
gitignored — **edit `templates/`, never the generated output.**


**ROADMAP.md** at the repo root is the top-level plan spanning both firmwares
(`KilnFW` on the ESP32-S3, `SaftyFW` on the RP2040). Start tasks from there; it
links to the per-area plans that own the detail.

## Tooling: always go through the MCP facade

**Anything involving the boards, the bench fixture, or the KiCad project starts
with one of these calls.** Do not conclude a capability is missing because you
cannot see a tool for it — each server publishes about six tools and keeps the
rest behind a search facade (135 tools for `kilnctrl`, 86 for `kicad`, 45 for
`kilnsim`).

```
kiln_help()                      # kilnctrl: main board (ESP32-S3) + RP2040 safety processor
simfw_help()                     # kilnsim:  the SimFW bench fixture
kicad_help()                     # kicad:    the schematic, board and sourcing data
kiln_find(query="read the temperature")
kiln_call(name="thermo_read")
kiln_batch(calls=[{"name":"safety_get_status"},{"name":"safety_get_link_stats"}])
```

`*_batch` is the right form for any sequence of two or more hardware
operations — one round trip, and it stops at the first failure.

All three speak HTTP (`kicad` on 8766, `kilnctrl` on 8767, `kilnsim` on 8768)
and must be running. If a call fails to connect:

```powershell
.\tools\PcTools\scripts\mcp_servers.ps1 status   # or: start | stop | restart
```

The same four actions are status-bar buttons in `kilnCtl.code-workspace`, and
the servers auto-start when the workspace opens.

Firmware builds and host tests are tools too — `build_kilnfw`,
`build_saftyfw_host_tests`, `build_simfw_host_tests`, `run_pctools_tests` — so
the toolchain invocations do not have to be rediscovered. Flashing is
`debug_program(peer="esp"|"pico"|"sim")`, always OpenOCD, never esptool.

Full rationale, token measurements, and how to add a tool: **docs/MCP_SERVERS.md**.

## Project Structure

The tree is split into hardware and software halves. See `docs/REPO_LAYOUT.md`
for the rationale and the move history.

```
hardware/   KiCad projects (mainBoard, ThermocoupleBoard, SaftyThermocoupleBoard,
            UnitTestFixture), shared lib/, datasheets/, simulation/, sourcing/
firmware/   KilnFW (ESP32-S3), SaftyFW (RP2040), CommonFW (shared link code),
            SimFW (RP2040 bench-test fixture)
tools/      PcTools (GUI + MCP for BOTH processors)
docs/       System-level documents spanning both halves
```

`mykicadMcp/` and `pdfMcp/` are still at the repo root: both had running MCP
server processes holding the directories open when the move was done, and they
belong under `tools/`. Paths in this file reflect where things are **now**.

All main-board KiCad project files live under **hardware/mainBoard/** (paths below are relative to that
directory unless noted). A second, independent board — the 5-channel thermocouple daughterboard —
lives under **hardware/ThermocoupleBoard/** with its own `.kicad_pro`/`.kicad_pcb`/`.kicad_sch`; several of
its sub-sheets (e.g. `Thermocouple.kicad_sch`) are copies of the same circuit used in the main
board, so a fix found in one project's copy often applies to the other's too.

### Schematics (hardware/mainBoard/)
- **hardware/mainBoard/kiln.kicad_sch** — Main schematic file; top-level hierarchy
- **hardware/mainBoard/MainControler.kicad_sch** — Arduino Nano-based main processor
- **hardware/mainBoard/Thermocouple.kicad_sch** — MAX31856 thermocouple interface (5 channels)
- **hardware/mainBoard/ADC.kicad_sch** — ADS1118 analog-to-digital converter
- **hardware/mainBoard/Power.kicad_sch** — Input power conditioning and protection
- **hardware/mainBoard/Regulators.kicad_sch** — 5V and 3.3V LDO regulators
- **hardware/mainBoard/5V_Regulator.kicad_sch** — Dedicated 5V regulation block
- **hardware/mainBoard/CurrentSense.kicad_sch** — Current monitoring circuitry
- **hardware/mainBoard/SSD.kicad_sch** — Seven-segment display interface
- **hardware/mainBoard/SaftyProcessor.kicad_sch** — Safety monitoring subsystem

### Board & Layout
- **hardware/mainBoard/kiln.kicad_pcb** — PCB layout and routing
- **hardware/mainBoard/kiln.kicad_prl** — KiCad project settings and layers configuration

### Component Data
- **hardware/mainBoard/kiln.csv** — Bill of Materials (BOM) with part numbers, values, datasheets, and footprints
- **hardware/mainBoard/parts/SamacSys_Parts.pretty/** — Component footprints
- **hardware/mainBoard/parts/SamacSys_Parts.3dshapes/** — 3D models for visualization and export
- **hardware/mainBoard/fp-lib-table** — Footprint library table

### Python Tools
`mykicadMcp/` is a separate git submodule (github.com/budarriola/mykicadMcp) holding the MCP server and its supporting tools:
- **mykicadMcp/kicad_pcb_tool.py** — Lightweight parser for PCB and netlist files; does not require KiCad runtime
- **mykicadMcp/kicad_mouser_tool.py** — Mouser Search API sourcing/stock/pricing lookups
- **mykicadMcp/kicad_ipc_tool.py** — Live-KiCad tools via the IPC API (`kicad-python`); requires a running KiCad session
- **mykicadMcp/kicad_mcp_server.py** — MCP server for the KiCad tools; HTTP on 8766 by default,
  `--transport stdio` still available
- **mykicadMcp/kicad_facade.py** — search taxonomy (groups, keywords, synonyms) for the facade
- **mykicadMcp/mcpkit_registry.py** — vendored copy of `tools/PcTools/src/mcpkit/registry.py`;
  edit the original and re-vendor, never this copy
- **mykicadMcp/requirements-mcp.txt** — Python dependencies (requires `mcp>=1.0.0`)
- **mykicadMcp/README.md** — Full setup guide and tool reference for the MCP server

### MCP Server Tools
The KiCad MCP server holds 86 tools in 12 groups: inspection/netlist, schematic data, Mouser
sourcing, audits, hierarchical sheet groups, layout/placement, PCB groups, net classes & buses,
nets, route templates, live IPC tools, and server control. Like the hardware servers it publishes
a search facade, not the whole set — start with `kicad_help()` or `kicad_find(query=...)` and
reach the rest through `kicad_call` / `kicad_batch`:

```
kicad_help()
kicad_find(query="is this capacitor rated high enough")
kicad_call(name="inspect_kicad_project", args={"project_path":"hardware/mainBoard/kiln.kicad_pro"})
```

`inspect_kicad_project` and `get_kicad_ipc_status` stay directly published. Everything else —
`list_kicad_components`, `get_kicad_component`, `get_kicad_component_connections`,
`list_kicad_nets`, `get_kicad_net` and the rest — is one `kicad_call` away. See
**mykicadMcp/README.md** and `mykicadMcp/docs/mcp-tools/` for the full reference, and
**mykicadMcp/NETCLASS_PLAN.md** for the net-class design doc.

The net classes & buses group supports bus detection, net-class proposal/creation, trace-cost
scoring (with live deviation measurement), bus corridor-area measurement, capacitor voltage
auditing, critical-net classification, connector detection, and `pcb_settings.json` management.

There is **no autorouter**: it was removed (`mykicadMcp` commit "remove autorouter engine, keep
reference-copy/template tools"). What remains is the reference-copy family —
`copy_kicad_component_routing`, `apply_kicad_route_template`, `diff_kicad_route_template` — which
replicates routing already drawn on one instance of a repeated block onto its siblings.

## Development Setup

### Python MCP Server
1. Ensure Python 3 is installed and in `PATH`
2. Activate the virtual environment:
   ```powershell
   mykicadMcp\.venv\Scripts\Activate.ps1
   ```
3. Install dependencies (if needed):
   ```powershell
   pip install -r mykicadMcp\requirements-mcp.txt
   ```
4. Test the MCP server:
   ```powershell
   python mykicadMcp\kicad_mcp_server.py
   ```

### Using MCP with Claude Code
- Configure MCP in your editor using the server path: `python mykicadMcp\kicad_mcp_server.py`
- Example tools: "List the components on the PCB", "Show me component R1 and its connections", "Provide details for net /MainControler/CLK"

## Key Architecture Notes

### Hierarchical Schematic Design
The project uses a hierarchical schematic structure where sub-sheets (ADC, Thermocouple, etc.) are instantiated in the main schematic. This allows modular design and easier debugging of subsystems.

### Multi-Channel Thermocouple Interface
Five MAX31856 converters (U10–U14) provide independent thermocouple monitoring with built-in cold-junction compensation. Each is on a separate schematic page for clarity.

### Power Distribution
Three voltage rails:
- **12V input** (from external supply)
- **5V** (main logic and relay coils)
- **3.3V** (microcontroller I/O and sensors)

Input protection uses TVS diodes (SMAJ24CA) and current-limiting resistors. See hardware/mainBoard/Power.kicad_sch and hardware/mainBoard/Regulators.kicad_sch.

### Safety Processor
A separate safety processor monitors critical parameters and can disable the main controller if needed. Isolated communication via relay feedback circuits.

### Data Access Pattern
Use Python tools (kicad_pcb_tool.py) when you need to read PCB data, netlist connectivity, or component properties without opening the GUI. The parser reads `.kicad_pcb` and auto-generated netlist files directly.

## Common Tasks

### View the PCB or Schematic
Open with KiCad:
```powershell
kicad hardware\mainBoard\kiln.kicad_pcb
kicad hardware\mainBoard\kiln.kicad_sch
```

### Replicate Routing Across Repeated Blocks
There is no autorouter in this repo any more -- it was removed from `mykicadMcp`, and
`kicad_router_tool.py` no longer exists. Route by hand in KiCad, then replicate that work onto the
sibling instances of a repeated block (the five identical thermocouple channels, for example):

```
kicad_call(name="copy_kicad_component_routing", args={
  "project_path": "hardware/mainBoard/kiln.kicad_pro",
  "template_reference": "U10", "target_reference": "U11"})
kicad_call(name="diff_kicad_route_template", args={...})   # preview before applying
kicad_call(name="apply_kicad_route_template", args={...})
```

Always diff before applying. The same template pattern exists for placement
(`*_layout_template`), reference-designator positions (`*_property_position_template`) and
footprint flips (`*_flip_template`) -- `kicad_find(query="template")` lists them all.

### Query Component or Net Information
Use the MCP server tools or call Python directly:
```powershell
python mykicadMcp\kicad_pcb_tool.py
```

### Update the BOM
Edit hardware/mainBoard/kiln.csv. This file is exported from KiCad's built-in BOM generator and includes part numbers, values, footprints, and Mouser links.

### Check Design Rule Violations
In KiCad: **Tools → Design Rule Checker** or press `Shift+I`. Refer to hardware/mainBoard/JLCPCB.kicad_dru.txt for manufacturing rules if fabricating at JLCPCB.

## Notes for AI Assistants

- The schematic files (`.kicad_sch`) are large text-based files; use Python tools to query data rather than reading raw files.
- Component designators (R1, U1, etc.) in hardware/mainBoard/kiln.csv match those on the schematic and PCB.
- Footprints are organized in `hardware/mainBoard/parts/SamacSys_Parts.pretty/`; do not modify these directly unless sourcing new parts.
- The MCP server is useful for scripting or integration with other tools; most editing should happen in KiCad GUI.
