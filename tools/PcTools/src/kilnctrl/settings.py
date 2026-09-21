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
import os
import tempfile
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
    """Write the JSON settings file, swallowing (but logging) I/O errors.

    Written via a tempfile in the SAME directory + ``os.replace`` rather
    than truncating ``path`` in place: several independent processes (the
    GUI, the MCP server, an ad-hoc script) can call this concurrently, and
    a plain truncate-then-write leaves a window where a reader (``load``)
    sees a truncated/partial file and silently treats it as ``{}`` --
    quietly dropping every OTHER key already on disk (``last_port``,
    ``openocd_exe``, ...), not just the one this call meant to set.
    ``os.replace`` is atomic on both POSIX and Windows, so a reader always
    sees either the old, complete file or the new, complete one.
    """
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp_name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent))
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump(data, fh, indent=2)
                fh.write("\n")
            os.replace(tmp_name, path)
        except BaseException:
            try:
                os.remove(tmp_name)
            except OSError:
                pass
            raise
    except OSError:
        log.warning("could not write settings to %s", path, exc_info=True)


def _update(key: str, value: object, path: Path = SETTINGS_PATH) -> None:
    data = load(path)
    if data.get(key) == value:
        return  # unchanged -- skip the write (and the churn/race window)
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


# ---------------------------------------------------------------------------
# OpenOCD executable path override (openocd_util.py)
# ---------------------------------------------------------------------------
def get_openocd_path(path: Path = SETTINGS_PATH) -> Optional[str]:
    """User-set override for openocd.exe, if autodetection doesn't find it."""
    value = load(path).get("openocd_exe")
    return value if isinstance(value, str) and value else None


def set_openocd_path(value: str, path: Path = SETTINGS_PATH) -> None:
    _update("openocd_exe", value, path)


# ---------------------------------------------------------------------------
# last-seen board host (host_resolve.py)
# ---------------------------------------------------------------------------
def get_last_host(path: Path = SETTINGS_PATH) -> Optional[str]:
    """The host (bare IP/hostname, no scheme or port) that most recently
    answered a real HTTP request from any kilnctrl client, if any."""
    value = load(path).get("last_host")
    return value if isinstance(value, str) and value else None


def set_last_host(value: str, path: Path = SETTINGS_PATH) -> None:
    _update("last_host", value, path)
