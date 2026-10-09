"""LOG wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403


# ---------------------------------------------------------------------------
# LOG (task_id = UART_TASK_ID_LOG) -- firmware -> PC only, unsolicited
# ---------------------------------------------------------------------------
_LOG_LEVEL_LETTER: dict[LogLevel, str] = {
    LogLevel.ERROR: "E",
    LogLevel.WARN: "W",
    LogLevel.INFO: "I",
    LogLevel.DEBUG: "D",
    LogLevel.VERBOSE: "V",
}


@dataclass(frozen=True)
class LogLine:
    """One decoded LOG frame: a device-side ESP_LOGx call, captured and
    forwarded by ``uart_log_bridge.c`` instead of going to the USB-Serial-JTAG
    console (see App/drivers/bridge/uart_log_bridge.c)."""

    level: LogLevel
    text: str

    @property
    def letter(self) -> str:
        """Single-character level tag, e.g. 'E', matching ESP-IDF's own
        convention -- for display prefixes that don't have room for the full
        level name."""
        return _LOG_LEVEL_LETTER.get(self.level, "?")


def parse_log_frame(payload: bytes) -> LogLine:
    """Decode a LOG task payload: byte0 = level, the rest is ASCII text.

    Unknown level bytes (a newer firmware speaking a level we don't know
    about) fall back to INFO rather than raising -- a log line with a wrong
    color/severity tag is harmless to show, unlike a THERMO/IO payload where
    a wrong field is a real bug in disguise.
    """
    if len(payload) < 1:
        raise ValueError("log frame is empty")
    try:
        level = LogLevel(payload[0])
    except ValueError:
        level = LogLevel.INFO
    text = payload[1:].decode("ascii", errors="replace")
    return LogLine(level=level, text=text)


