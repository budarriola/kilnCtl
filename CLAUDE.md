# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**kilnCtl** is a KiCad-based electronics design project for a kiln controller. It includes a hierarchical schematic design, PCB layout, component library, and Python tools for programmatic access to project data.

## Where to start

**ROADMAP.md** at the repo root is the top-level plan spanning both firmwares
(`KilnFW` on the ESP32-S3, `SaftyFW` on the RP2040). Start tasks from there; it
links to the per-area plans that own the detail.

## Project Structure

All main-board KiCad project files live under **mainBoard/** (paths below are relative to that
directory unless noted). A second, independent board — the 5-channel thermocouple daughterboard —
lives under **ThermocoupleBoard/** with its own `.kicad_pro`/`.kicad_pcb`/`.kicad_sch`; several of
its sub-sheets (e.g. `Thermocouple.kicad_sch`) are copies of the same circuit used in the main
board, so a fix found in one project's copy often applies to the other's too.

### Schematics (mainBoard/)
- **mainBoard/kiln.kicad_sch** — Main schematic file; top-level hierarchy
- **mainBoard/MainControler.kicad_sch** — Arduino Nano-based main processor
- **mainBoard/Thermocouple.kicad_sch** — MAX31856 thermocouple interface (5 channels)
- **mainBoard/ADC.kicad_sch** — ADS1118 analog-to-digital converter
- **mainBoard/Power.kicad_sch** — Input power conditioning and protection
- **mainBoard/Regulators.kicad_sch** — 5V and 3.3V LDO regulators
- **mainBoard/5V_Regulator.kicad_sch** — Dedicated 5V regulation block
- **mainBoard/CurrentSense.kicad_sch** — Current monitoring circuitry
- **mainBoard/SSD.kicad_sch** — Seven-segment display interface
- **mainBoard/SaftyProcessor.kicad_sch** — Safety monitoring subsystem

### Board & Layout
- **mainBoard/kiln.kicad_pcb** — PCB layout and routing
- **mainBoard/kiln.kicad_prl** — KiCad project settings and layers configuration

### Component Data
- **mainBoard/kiln.csv** — Bill of Materials (BOM) with part numbers, values, datasheets, and footprints
- **mainBoard/parts/SamacSys_Parts.pretty/** — Component footprints
- **mainBoard/parts/SamacSys_Parts.3dshapes/** — 3D models for visualization and export
- **mainBoard/fp-lib-table** — Footprint library table

### Python Tools
`mykicadMcp/` is a separate git submodule (github.com/budarriola/mykicadMcp) holding the MCP server and its supporting tools:
- **mykicadMcp/kicad_pcb_tool.py** — Lightweight parser for PCB and netlist files; does not require KiCad runtime
- **mykicadMcp/kicad_mouser_tool.py** — Mouser Search API sourcing/stock/pricing lookups
- **mykicadMcp/kicad_ipc_tool.py** — Live-KiCad tools via the IPC API (`kicad-python`); requires a running KiCad session
- **mykicadMcp/kicad_mcp_server.py** — MCP server that exposes all KiCad tools over stdio or HTTP
- **mykicadMcp/requirements-mcp.txt** — Python dependencies (requires `mcp>=1.0.0`)
- **mykicadMcp/README.md** — Full setup guide and tool reference for the MCP server

### MCP Server Tools
The KiCad MCP server exposes 92 tools across 11 groups (inspection/netlist, schematic data,
Mouser sourcing, hierarchical groups, layout/placement, PCB groups, label positions, footprint
flips, live IPC tools, net classes/buses, and autorouter/routing). See **mykicadMcp/README.md** and `mykicadMcp/docs/mcp-tools/` for the full
reference. The net classes & buses group supports bus detection, net-class proposal/creation,
trace-cost scoring (with live deviation measurement), bus corridor-area measurement, capacitor voltage auditing, critical-net classification, connector detection, and `pcb_settings.json` management; the autorouter group covers the headline `route_kicad_board` orchestrator, Phase 7.3b detailed routing (windowed A*), zone inspection, plane-island analysis and costing, ratsnest calculation, layer/constraint querying, and undo. See **mykicadMcp/NETCLASS_PLAN.md** for the design doc.
A few commonly used tools:
- `inspect_kicad_project` — Get project-wide metrics and status
- `list_kicad_components` — List all components on the PCB
- `get_kicad_component` — Details for a specific component (reference, value, footprint)
- `get_kicad_component_connections` — Nets connected to a component
- `list_kicad_nets` — List all nets in the design
- `get_kicad_net` — Details for a specific net (connections, pin list)

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

Input protection uses TVS diodes (SMAJ24CA) and current-limiting resistors. See mainBoard/Power.kicad_sch and mainBoard/Regulators.kicad_sch.

### Safety Processor
A separate safety processor monitors critical parameters and can disable the main controller if needed. Isolated communication via relay feedback circuits.

### Data Access Pattern
Use Python tools (kicad_pcb_tool.py) when you need to read PCB data, netlist connectivity, or component properties without opening the GUI. The parser reads `.kicad_pcb` and auto-generated netlist files directly.

## Common Tasks

### View the PCB or Schematic
Open with KiCad:
```powershell
kicad mainBoard\kiln.kicad_pcb
kicad mainBoard\kiln.kicad_sch
```

### Route the Board
Use the MCP server `route_kicad_board` tool or the CLI:
```powershell
# Dry-run preview (no write)
python mykicadMcp\kicad_router_tool.py route mainBoard\kiln.kicad_pro

# Apply the routing
python mykicadMcp\kicad_router_tool.py route mainBoard\kiln.kicad_pro --write

# Undo (remove autorouter-owned copper)
python mykicadMcp\kicad_router_tool.py unroute mainBoard\kiln.kicad_pro --write

# Control effort (quick|balanced|best) and select nets
python mykicadMcp\kicad_router_tool.py route mainBoard\kiln.kicad_pro --write --effort best --nets /Power/VBUS /MainControler/CLK
```

The `route_kicad_board` orchestrator runs ratsnest → global route → detailed route (windowed A*)
in one call, with configurable rip-up aggressiveness. Always preview first (`write=false` is
default). Phase 7.5 (plane-aware), 7.6 (optimizer), and 7.5.6 (stitching) are M4 TODO hooks.

### Query Component or Net Information
Use the MCP server tools or call Python directly:
```powershell
python mykicadMcp\kicad_pcb_tool.py
```

### Inspect Zones & Plane Islands
Query copper pours and analyze fill islands:
```powershell
# List all zones (copper and keepout)
python -c "from kicad_router_tool import list_zones; import json; print(json.dumps(list_zones('mainBoard/kiln.kicad_pro'), indent=2))"

# Audit plane islands, costing, and stitching recommendations
python -c "from kicad_router_tool import audit_plane_islands; import json; print(json.dumps(audit_plane_islands('mainBoard/kiln.kicad_pro'), indent=2))"
```

### Update the BOM
Edit mainBoard/kiln.csv. This file is exported from KiCad's built-in BOM generator and includes part numbers, values, footprints, and Mouser links.

### Check Design Rule Violations
In KiCad: **Tools → Design Rule Checker** or press `Shift+I`. Refer to mainBoard/JLCPCB.kicad_dru.txt for manufacturing rules if fabricating at JLCPCB.

## Notes for AI Assistants

- The schematic files (`.kicad_sch`) are large text-based files; use Python tools to query data rather than reading raw files.
- Component designators (R1, U1, etc.) in mainBoard/kiln.csv match those on the schematic and PCB.
- Footprints are organized in `mainBoard/parts/SamacSys_Parts.pretty/`; do not modify these directly unless sourcing new parts.
- The MCP server is useful for scripting or integration with other tools; most editing should happen in KiCad GUI.
