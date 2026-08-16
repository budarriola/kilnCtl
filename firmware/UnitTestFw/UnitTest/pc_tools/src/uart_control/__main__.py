"""``python -m uart_control`` -> launch the GUI (``-m uart_control.mcp_server``
runs the MCP server instead)."""

from __future__ import annotations

from .gui import main

if __name__ == "__main__":
    raise SystemExit(main())
