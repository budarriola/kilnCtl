"""GPIO_PROBE wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403


# ---------------------------------------------------------------------------
# GPIO_PROBE (task_id = UART_TASK_ID_GPIO_PROBE)
# ---------------------------------------------------------------------------
# Only answered on a firmware built with CONFIG_KILNCTL_ENABLE_GPIO_PROBE
# (default off, gpio_probe.c). Every accepted call is deny-list-checked and
# refused while a profile is RUNNING or PAUSED on the firmware side -- this
# client does not duplicate that policy, it just carries the refusal reason
# back.


class GpioProbeResponseError(ValueError):
    """Raised when a GPIO_PROBE response payload does not match its wire layout."""


class GpioProbeRefused(RuntimeError):
    """Raised when the firmware answered but refused the request (ok=0):
    an unknown mode, a deny-listed pin, a profile running/paused, or a
    WRITE to a pin never SET_MODE'd as OUTPUT. ``reason`` is the firmware's
    own ASCII explanation."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


@dataclass(frozen=True)
class GpioProbePin:
    gpio_num: int
    mode: int
    level: int

    @property
    def mode_name(self) -> str:
        return {
            GPIO_PROBE_MODE_INPUT: "input",
            GPIO_PROBE_MODE_INPUT_PULLUP: "input_pullup",
            GPIO_PROBE_MODE_INPUT_PULLDOWN: "input_pulldown",
            GPIO_PROBE_MODE_OUTPUT: "output",
        }.get(self.mode, f"unknown(0x{self.mode:02X})")


def gpio_probe_set_mode(gpio_num: int, mode: int) -> bytes:
    """0x01 SET_MODE: gpio_num, mode (GPIO_PROBE_MODE_*). Replies ok/fail."""
    return struct.pack("<BBB", GPIO_PROBE_CMD_SET_MODE, gpio_num, mode)


def gpio_probe_write(gpio_num: int, level: bool) -> bytes:
    """0x02 WRITE: gpio_num, level. Firmware refuses unless gpio_num was
    already SET_MODE'd OUTPUT. Replies ok/fail."""
    return struct.pack("<BBB", GPIO_PROBE_CMD_WRITE, gpio_num, 1 if level else 0)


def gpio_probe_read(gpio_num: int) -> bytes:
    """0x03 READ: gpio_num (query)."""
    return struct.pack("<BB", GPIO_PROBE_CMD_READ, gpio_num)


def gpio_probe_read_all() -> bytes:
    """0x04 READ_ALL: no args (query). Returns every pin SET_MODE'd since
    the probe task started, up to GPIO_PROBE_MAX_TRACKED."""
    return struct.pack("<B", GPIO_PROBE_CMD_READ_ALL)


def _unpack_gpio_probe_reason(payload: bytes, offset: int) -> str:
    if offset >= len(payload):
        raise GpioProbeResponseError("response is missing its reason length byte")
    length = payload[offset]
    start = offset + 1
    end = start + length
    if end > len(payload):
        raise GpioProbeResponseError("reason string overruns payload")
    return payload[start:end].decode("ascii", errors="replace")


def parse_gpio_probe_response(payload: bytes) -> "tuple[int, object]":
    """Decode a GPIO_PROBE reply into ``(subcmd, value)``.

    Raises :class:`GpioProbeRefused` (not just returning False) when the
    firmware answered ok=0, since a refusal here always carries a reason
    worth surfacing rather than a bare boolean.
    """
    if len(payload) < 2:
        raise GpioProbeResponseError("GPIO_PROBE response is missing its ok byte")
    subcommand = payload[0]

    if subcommand in (GPIO_PROBE_CMD_SET_MODE, GPIO_PROBE_CMD_WRITE):
        ok = bool(payload[1])
        if not ok:
            raise GpioProbeRefused(_unpack_gpio_probe_reason(payload, 2))
        return subcommand, True

    if subcommand == GPIO_PROBE_CMD_READ:
        ok = bool(payload[1])
        if not ok:
            raise GpioProbeRefused(_unpack_gpio_probe_reason(payload, 2))
        if len(payload) < 3:
            raise GpioProbeResponseError("READ response is missing its level byte")
        return subcommand, bool(payload[2])

    if subcommand == GPIO_PROBE_CMD_READ_ALL:
        if len(payload) < 2:
            raise GpioProbeResponseError("READ_ALL response is missing its count byte")
        count = payload[1]
        offset = 2
        pins = []
        for i in range(count):
            if offset + 3 > len(payload):
                raise GpioProbeResponseError(f"READ_ALL entry {i} is truncated")
            pins.append(
                GpioProbePin(
                    gpio_num=payload[offset], mode=payload[offset + 1], level=payload[offset + 2]
                )
            )
            offset += 3
        return subcommand, pins

    raise GpioProbeResponseError(f"unknown GPIO_PROBE response subcommand 0x{subcommand:02X}")
