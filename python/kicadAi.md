# KiCad project inspection for Cline

## What is included

This workspace now has a lightweight KiCad inspector that reads the PCB and netlist files directly without requiring a full KiCad runtime. It exposes a small MCP server so Cline can ask questions about the board.

## Main files

- [kicad_pcb_tool.py](kicad_pcb_tool.py) - parses KiCad PCB and netlist data into structured JSON-friendly output.
- [kicad_mcp_server.py](kicad_mcp_server.py) - exposes the KiCad tools over a local stdio MCP server.
- [cline-mcp-config.json](cline-mcp-config.json) - example config for Cline.
- [README-mcp.md](README-mcp.md) - quick usage guide.

## Available tools

The MCP server exposes these tools:

- `inspect_kicad_project` - returns board, component, and net summary information.
- `list_kicad_components` - lists component references and properties.
- `get_kicad_component` - fetches one component by reference.
- `list_kicad_nets` - lists nets and the connected pins.

## Example usage

From the workspace root:

```bash
C:/Users/budar/AppData/Local/Python/pythoncore-3.14-64/python.exe kicad_pcb_tool.py . --json
```

Or ask Cline to inspect the project with prompts such as:

- "Inspect the KiCad project in this workspace"
- "List the components on the PCB"
- "Show me component R1"

## Cline setup

Cline can be pointed at the included config file in [cline-mcp-config.json](cline-mcp-config.json). The server command uses the local Python interpreter and the workspace path.

## Notes

The parser is intentionally dependency-light and works with the KiCad S-expression files already present in this project.