"""Local cache of the last Wi-Fi credentials the user entered in the GUI's
Wi-Fi Settings popup (gui.py), so the MCP server (mcp_server.py) can rejoin
the same network automatically without re-prompting each session.

Gitignored (/tools/PcTools/wifi_credentials.json) -- this is a plaintext
secret on disk, scoped to local dev machines only. Never commit it.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Optional

log = logging.getLogger(__name__)

# tools/PcTools/wifi_credentials.json, regardless of cwd this module is
# imported from.
_STORE_PATH = Path(__file__).resolve().parents[2] / "wifi_credentials.json"


def save(ssid: str, password: str) -> None:
    """Remember the most recently submitted SSID/password. Called from
    gui.py's wifi_connect_async() every time the user submits the Wi-Fi
    Settings form, overwriting whatever was saved before."""
    if not ssid:
        return
    try:
        _STORE_PATH.write_text(
            json.dumps({"ssid": ssid, "password": password}, indent=2),
            encoding="utf-8",
        )
    except OSError:
        log.warning("wifi_credentials: failed to save to %s", _STORE_PATH, exc_info=True)


def load() -> Optional[dict]:
    """Return {"ssid": ..., "password": ...} from the local cache, or None
    if nothing has been saved yet."""
    try:
        data = json.loads(_STORE_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    ssid = data.get("ssid")
    if not ssid:
        return None
    return {"ssid": ssid, "password": data.get("password", "")}
