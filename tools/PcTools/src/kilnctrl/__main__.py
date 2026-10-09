"""``python -m kilnctrl`` -> launch the GUI (``-m kilnctrl.mcp_server``
runs the MCP server instead)."""

from __future__ import annotations

from .gui import main

if __name__ == "__main__":
    raise SystemExit(main())
