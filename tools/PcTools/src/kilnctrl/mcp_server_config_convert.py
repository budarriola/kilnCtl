"""CONFIG CONVERT -- MCP surface for kilnctrl.config_convert (owner request
2026-09-17: "converting any config version to any other in best effort
style, but keep the board at one version at a time"). Part of the
mcp_server.py split pattern -- see that module's docstring for the overall
map. This tool never writes to a board; it converts a document already in
hand (typically pasted from a backup file, or from a raw NVS dump) to a
target version of its own store. See kilnctrl/config_convert.py's module
docstring for exactly which stores are supported today (kilnctl_backup,
kilnctl_profile_blob) and which are deliberately refused
(kilnctl_kiln_cfg_package, safety_config_store).
"""
from __future__ import annotations

import json

from . import config_convert
from . import mcp_server as _srv


@_srv._tool()
def convert_config(document_json: str, to_version: int) -> str:
    """Best-effort convert a kilnCtl config document to another version of
    its own store, without ever touching a board.

    ``document_json`` is the exact text of a document this tool already
    knows how to place (auto-detected from its own ``"kind"`` field):
    ``kilnctl_backup`` (the JSON ``GET /api/backup/export`` produces) or
    ``kilnctl_profile_blob`` (this tool's own wrapper -- ``{"kind":
    "kilnctl_profile_blob", "version": N, "blob_hex": "<raw profN NVS
    record, hex>"}`` -- for the raw on-flash profile record, which firmware
    itself never exposes as JSON). ``to_version`` is the target version
    number for that store.

    Forward conversion (to a newer version than the document already is)
    mirrors firmware's own migration exactly -- every new field gets
    precisely the default firmware's migration code documents. Backward
    conversion is best-effort: a field the older version's layout cannot
    express at all is dropped, and every drop is named in the returned
    report, never silent.

    Returns the converted document as JSON, followed by the per-field
    report. Refuses (returns ``error: ...``) for a document this tool
    cannot place, a target version it does not know, or an unsupported
    store (kiln_cfg_store's package format and SaftyFW's raw config_store
    record are not implemented yet -- see kilnctrl/config_convert.py's
    module docstring for why).
    """
    doc = json.loads(document_json)
    out_doc, report = config_convert.convert_document(doc, to_version)
    return json.dumps(out_doc, indent=2) + "\n\n" + report.render()
