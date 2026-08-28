"""Shared MCP facade toolkit for the PcTools servers.

``kilnctrl`` registers a large flat tool surface (~135 tools). Exposing all of
them over MCP costs 200-500 tokens of JSON schema *each*, spent in every
context window before the model has read a single line of the user's request
-- roughly 20k tokens of pure preamble.

:func:`mcpkit.registry.collapse` replaces that surface with five facade tools
(``help`` / ``find`` / ``describe`` / ``call`` / ``batch``). The full tool set
stays intact and callable; it is merely *discovered on demand* instead of being
broadcast up front. Cost drops to about 1.5k tokens, and ``batch`` collapses a
multi-step hardware sequence into a single round trip.

See ``docs/MCP_SERVERS.md`` for the operator-facing story: how the servers are
started and stopped, and why they now speak HTTP instead of stdio.
"""

from .registry import ToolEntry, collapse
from .serve import serve

__all__ = ["ToolEntry", "collapse", "serve"]
