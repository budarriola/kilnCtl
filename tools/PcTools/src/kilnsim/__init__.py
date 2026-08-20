"""kilnsim -- PC-side toolset for SimFW, the RP2040 bench-test fixture.

Sibling package to :mod:`kilnctrl` under ``tools/PcTools/src/``. See
``firmware/SimFW/docs/PLAN.md`` for the fixture's design; this package is the
"PC side: MCP server, CLI, GUI" it describes in section 6.

Fresh, self-contained toolset (PLAN.md sec 6, sec 12): nothing here is ported
from ``firmware/UnitTestFw``'s ``pc_tools`` -- that stack is deleted with its
project. The wire-level encode/decode in :mod:`kilnsim.link` is a placeholder
until the real protocol is lifted from ``UnitTestFw`` into ``firmware/CommonFW``
(PLAN.md sec 12); everything above the link layer (:mod:`kilnsim.protocol`,
:mod:`kilnsim.scenario`, :mod:`kilnsim.report`, the CLI, the MCP server, the
GUI) is written against the stable command/payload *shape* PLAN.md sections 5
and 8 describe, not against any particular byte layout, so it does not need to
change when that layer lands.
"""

from __future__ import annotations

__all__ = ["__version__"]

__version__ = "0.1.0"
