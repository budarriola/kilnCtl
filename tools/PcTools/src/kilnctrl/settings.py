"""Persisted app settings (``tools/PcTools/src/kilnctrl/settings.json``).

Single settings file shared by the GUI and (where relevant) the MCP server:
the last-used serial port and the log retention count today, with room to
grow. Plain ``pathlib`` + ``json`` on purpose -- no config/appdirs
dependency for a handful of small values.

A missing or corrupt file must never stop the GUI or MCP server from
starting: failures here are logged and swallowed, falling back to defaults.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Optional

log = logging.getLogger(__name__)

#: tools/PcTools/src/kilnctrl/settings.json (next to this file)
SETTINGS_PATH = Path(__file__).resolve().with_name("settings.json")


def load(path: Path = SETTINGS_PATH) -> dict:
    """Read the JSON settings file, returning {} if missing or unreadable."""
    try:
        with path.open("r", encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError:
        return {}
    except (OSError, ValueError):
        log.warning("ignoring unreadable settings at %s", path, exc_info=True)
        return {}
    return data if isinstance(data, dict) else {}


def save(data: dict, path: Path = SETTINGS_PATH) -> None:
    """Write the JSON settings file, swallowing (but logging) I/O errors."""
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2)
            fh.write("\n")
    except OSError:
        log.warning("could not write settings to %s", path, exc_info=True)


def _update(key: str, value: object, path: Path = SETTINGS_PATH) -> None:
    data = load(path)
    data[key] = value
    save(data, path)


# ---------------------------------------------------------------------------
# last-used serial port
# ---------------------------------------------------------------------------
def get_last_port(path: Path = SETTINGS_PATH) -> Optional[str]:
    """The device name (e.g. "COM6") last successfully connected to, if any."""
    value = load(path).get("last_port")
    return value if isinstance(value, str) and value else None


def set_last_port(port: str, path: Path = SETTINGS_PATH) -> None:
    _update("last_port", port, path)


# ---------------------------------------------------------------------------
# log retention (session_log.py)
# ---------------------------------------------------------------------------
def get_keep_logs(default: int, path: Path = SETTINGS_PATH) -> int:
    value = load(path).get("keep_logs", default)
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def set_keep_logs(value: int, path: Path = SETTINGS_PATH) -> None:
    _update("keep_logs", int(value), path)
