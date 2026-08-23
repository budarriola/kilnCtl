"""kilnsim -- PC-side toolset for SimFW, the RP2040 bench-test fixture.

Sibling package to :mod:`kilnctrl` under ``tools/PcTools/src/``. See
``firmware/SimFW/docs/PLAN.md`` for the fixture's design; this package is the
"PC side: MCP server, CLI, GUI" it describes in section 6.

Fresh, self-contained toolset (DESIGN_NOTES.md sec 6): nothing here is ported
from ``firmware/UnitTestFw``'s ``pc_tools`` -- that stack was deleted with its
project (2026-08-23, DESIGN_NOTES.md sec 12). The wire-level encode/decode in
:mod:`kilnsim.link` speaks the real protocol lifted from ``UnitTestFw`` into
``firmware/CommonFW`` as ``benchproto``; everything above the link layer
(:mod:`kilnsim.protocol`, :mod:`kilnsim.scenario`, :mod:`kilnsim.report`, the
CLI, the MCP server, the GUI) is written against the stable command/payload
*shape* DESIGN_NOTES.md sections 5 and 8 describe, not against any particular
byte layout.
"""

from __future__ import annotations

__all__ = ["__version__"]

__version__ = "0.1.0"
