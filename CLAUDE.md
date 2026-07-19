# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**kilnCtl** is a KiCad-based electronics design project for a kiln controller. It includes a hierarchical schematic design, PCB layout, component library, and Python tools for programmatic access to project data.

## Project Structure

### Schematics
- **kiln.kicad_sch** — Main schematic file; top-level hierarchy
- **MainControler.kicad_sch** — Arduino Nano-based main processor
- **Thermocouple.kicad_sch** — MAX31856 thermocouple interface (5 channels)
- **ADC.kicad_sch** — ADS1118 analog-to-digital converter
- **Power.kicad_sch** — Input power conditioning and protection
- **Regulators.kicad_sch** — 5V and 3.3V LDO regulators
- **5V_Regulator.kicad_sch** — Dedicated 5V regulation block
- **CurrentSense.kicad_sch** — Current monitoring circuitry
- **SSD.kicad_sch** — Seven-segment display interface
- **SaftyProcessor.kicad_sch** — Safety monitoring subsystem

### Board & Layout
- **kiln.kicad_pcb** — PCB layout and routing
- **kiln.kicad_prl** — KiCad project settings and layers configuration

### Component Data
- **kiln.csv** — Bill of Materials (BOM) with part numbers, values, datasheets, and footprints
- **parts/SamacSys_Parts.pretty/** — Component footprints
- **parts/SamacSys_Parts.3dshapes/** — 3D models for visualization and export
- **fp-lib-table** — Footprint library table

### Python Tools
- **python/kicad_pcb_tool.py** — Lightweight parser for PCB and netlist files; does not require KiCad runtime
- **python/kicad_mcp_server.py** — MCP server that exposes KiCad inspection tools via stdio
- **python/requirements-mcp.txt** — Python dependencies (requires `mcp>=1.0.0`)
- **python/README-mcp.md** — Setup and usage guide for MCP server

### MCP Server Tools
The KiCad MCP server exposes these tools for programmatic access:
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
   python\.venv\Scripts\Activate.ps1
   ```
3. Install dependencies (if needed):
   ```powershell
   pip install -r python\requirements-mcp.txt
   ```
4. Test the MCP server:
   ```powershell
   python python\kicad_mcp_server.py
   ```

### Using MCP with Claude Code / Cline
- Configure MCP in your editor using the server path: `python python\kicad_mcp_server.py`
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

Input protection uses TVS diodes (SMAJ24CA) and current-limiting resistors. See Power.kicad_sch and Regulators.kicad_sch.

### Safety Processor
A separate safety processor monitors critical parameters and can disable the main controller if needed. Isolated communication via relay feedback circuits.

### Data Access Pattern
Use Python tools (kicad_pcb_tool.py) when you need to read PCB data, netlist connectivity, or component properties without opening the GUI. The parser reads `.kicad_pcb` and auto-generated netlist files directly.

## Common Tasks

### View the PCB or Schematic
Open with KiCad:
```powershell
kicad kiln.kicad_pcb
kicad kiln.kicad_sch
```

### Query Component or Net Information
Use the MCP server tools or call Python directly:
```powershell
python python\kicad_pcb_tool.py
```

### Update the BOM
Edit kiln.csv. This file is exported from KiCad's built-in BOM generator and includes part numbers, values, footprints, and Mouser links.

### Check Design Rule Violations
In KiCad: **Tools → Design Rule Checker** or press `Shift+I`. Refer to JLCPCB.kicad_dru.txt for manufacturing rules if fabricating at JLCPCB.

## Notes for AI Assistants

- The schematic files (`.kicad_sch`) are large text-based files; use Python tools to query data rather than reading raw files.
- Component designators (R1, U1, etc.) in kiln.csv match those on the schematic and PCB.
- Footprints are organized in `parts/SamacSys_Parts.pretty/`; do not modify these directly unless sourcing new parts.
- The MCP server is useful for scripting or integration with other tools; most editing should happen in KiCad GUI.
