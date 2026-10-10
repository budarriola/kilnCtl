"""RP2040 GPIO probe for the safety processor (A1), over SWD via
``debug_probe.py`` -- ``tools/PcTools/TODO.md`` capability 1b, the Pico-side
mirror of the ESP's ``gpio_probe.c`` (``firmware/KilnFW/App/drivers/
gpio_probe.c``).

Unlike the ESP probe, this needs **no firmware agent on the Pico at all**:
the RP2040's GPIO block (SIO) and pin-mux block (IO_BANK0/PADS_BANK0) are
plain memory-mapped peripherals, readable and writable directly over SWD with
the core halted, exactly the way ``coordinated_gpio_test.py`` already pokes
them for the ESP<->Pico link test. This module is that same register
approach, generalised into a reusable read/set_mode/write/read_all surface
instead of one script's inline helpers -- confirmed against the RP2040
datasheet (ch. 2.3 SIO, ch. 2.19 IO_BANK0, ch. 2.18 PADS_BANK0) before
writing it, same register addresses ``coordinated_gpio_test.py`` already
validated against real hardware this session (2026-08-18, the SIO_GPIO_IN
comment in ``debug_probe.py``).

**GPIO6 (`saftyRelay`) is permanently denied for SET_MODE and WRITE, with no
override.** GPIO6 drives Q4's gate into K4's coil -- the pilot relay for the
line contactor (`firmware/SaftyFW/docs/HARDWARE.md` §3): high energizes,
low is the fail-safe de-energized state, and it is the one pin in this whole
board whose misuse is silent and system-level rather than merely wrong. This
mirrors the ESP probe's treatment of its own safety-critical pin
(`SAFETY_FAULT_IO`, denied outright in `gpio_probe.c`), except **READ is not
denied here**: ``read()`` only touches ``SIO_GPIO_IN``, a passive register
that reflects the pin's actual level regardless of who (relay_owner,
firmware, or nothing at all) is driving it -- it cannot disturb whatever
state the pin is already in, and knowing K4's real state is exactly the kind
of read a bench diagnosis of "why did the relay not respond" needs. The
denial is enforced here, in Python, at the tool layer -- it does not rely on
any cooperation from SaftyFW, which is the point: this probe exists
precisely because SWD sees the chip's memory directly, with no firmware in
the loop to ask permission of.

What this module deliberately does **not** implement, and why:

- **No "refuse while ARMED" gate in this module.** The gate exists one layer
  up: ``mcp_server_pico_gpio_probe.py``'s ``set_mode``/``write`` tools call
  ``debug_probe.pico_armed_state()`` (the same fail-closed read
  ``debug_write_memory(peer="pico")`` uses) and refuse while the Pico is
  ARMED or when ARMED cannot be read. A caller using this module directly
  bypasses that gate; GPIO6's hard, unconditional deny here is the only guard
  rail at this level.
- **No profile-running check.** The ESP probe's `gpio_probe_run_blocked()`
  has a live `profile_executor` to ask; this module has no such source of
  truth on the Pico side and does not fabricate one.
- **No other pins denied.** `gpio_probe.c`'s ESP deny-list also protects
  SPI/I2C/the SX1509/the display/its own PC link -- there is nothing
  structurally equivalent to demand the same treatment on the Pico side yet
  (this bring-up tool predates SaftyFW having any of those peripherals
  wired to anything that would be silently misled by a probe poke). GPIO6 is
  the one pin this module treats as non-negotiable because it is the one
  whose misuse cannot be seen from outside.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict, List, Tuple

from . import debug_probe

# --- RP2040 register map (datasheet ch. 2.3 SIO, ch. 2.19 IO_BANK0,
# ch. 2.18 PADS_BANK0) -- same base addresses coordinated_gpio_test.py
# already exercises against real hardware.
IO_BANK0_BASE = 0x40014000
PADS_BANK0_BASE = 0x4001C000
SIO_BASE = 0xD0000000

SIO_GPIO_IN = SIO_BASE + 0x004
SIO_GPIO_OUT_SET = SIO_BASE + 0x014
SIO_GPIO_OUT_CLR = SIO_BASE + 0x018
SIO_GPIO_OE_SET = SIO_BASE + 0x024
SIO_GPIO_OE_CLR = SIO_BASE + 0x028

GPIO_FUNC_SIO = 5

# PADS_BANK0 GPIOx register bit positions (datasheet 2.19.6.2).
_PAD_PUE_BIT = 1 << 3  # pull-up enable
_PAD_PDE_BIT = 1 << 2  # pull-down enable

# GPIO6 = saftyRelay -> Q4 gate -> K4 coil. See module docstring: SET_MODE
# and WRITE are refused unconditionally for this pin, no override, no
# "confirm" flag. READ is allowed.
SAFTYFW_RELAY_GPIO = 6

MODE_INPUT = "input"
MODE_INPUT_PULLUP = "input_pullup"
MODE_INPUT_PULLDOWN = "input_pulldown"
MODE_OUTPUT = "output"

_VALID_MODES = {MODE_INPUT, MODE_INPUT_PULLUP, MODE_INPUT_PULLDOWN, MODE_OUTPUT}

_MEMRD_RE = re.compile(r"MEMRD\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)")


class PicoGpioProbeRefused(Exception):
    """Raised for a denied pin or an invalid pre-condition (e.g. WRITE before
    SET_MODE). Mirrors ``devices.GpioProbeRefused`` on the ESP side."""

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


@dataclass
class TrackedPin:
    gpio_num: int
    mode: str


# Per-process bookkeeping of pins this probe has configured via set_mode(),
# purely for read_all()'s convenience. There is no firmware on the Pico side
# to hold this for us (unlike gpio_probe.c's tracked_pin_t table) -- register
# pokes over SWD carry no state of their own between calls, so it lives here
# instead. Reset (or a second PC process) starts this back at empty; that is
# a bring-up-tool limitation, not a correctness problem, since read_all() is
# documented as "what this session configured," not "what the chip has."
_tracked: Dict[int, TrackedPin] = {}


def _pad_addr(gpio_num: int) -> int:
    return PADS_BANK0_BASE + 0x04 + 4 * gpio_num


def _ctrl_addr(gpio_num: int) -> int:
    return IO_BANK0_BASE + 8 * gpio_num + 4


def _read_word(peer: str, address: int) -> int:
    ok, out = debug_probe.read_memory(peer, address, count=1, width=32)
    if not ok:
        raise RuntimeError(f"read 0x{address:x} failed: {out}")
    match = _MEMRD_RE.search(out)
    if not match:
        raise RuntimeError(f"could not parse read_memory output: {out!r}")
    return int(match.group(1), 16)


def _write_word(peer: str, address: int, value: int) -> None:
    ok, out = debug_probe.write_memory(peer, address, value, width=32)
    if not ok:
        raise RuntimeError(f"write 0x{value:x} to 0x{address:x} failed: {out}")


def read(gpio_num: int, peer: str = debug_probe.PEER_PICO) -> bool:
    """Reads gpio_num's current input level via ``SIO_GPIO_IN``.

    Passive: touches nothing but that one read-only-in-effect register (it is
    architecturally read/write, but this call never writes it), so it never
    reconfigures funcsel/OE/pad state and is safe to call on any pin,
    including GPIO6 and pins currently owned by firmware.
    """
    value = _read_word(peer, SIO_GPIO_IN)
    return bool(value & (1 << gpio_num))


def set_mode(gpio_num: int, mode: str, peer: str = debug_probe.PEER_PICO) -> None:
    """Detaches gpio_num from whatever peripheral owns it and hands it to the
    plain SIO block, configured as input / input_pullup / input_pulldown /
    output. Refused outright for GPIO6."""
    if mode not in _VALID_MODES:
        raise ValueError(f"mode must be one of {sorted(_VALID_MODES)}, got {mode!r}")
    if gpio_num == SAFTYFW_RELAY_GPIO:
        raise PicoGpioProbeRefused(
            "gpio6 (saftyRelay / K4 pilot relay) is never settable through this probe -- read-only"
        )

    _write_word(peer, _ctrl_addr(gpio_num), GPIO_FUNC_SIO)

    pad = _read_word(peer, _pad_addr(gpio_num))
    pad &= ~(_PAD_PUE_BIT | _PAD_PDE_BIT)
    if mode == MODE_INPUT_PULLUP:
        pad |= _PAD_PUE_BIT
    elif mode == MODE_INPUT_PULLDOWN:
        pad |= _PAD_PDE_BIT
    _write_word(peer, _pad_addr(gpio_num), pad)

    if mode == MODE_OUTPUT:
        _write_word(peer, SIO_GPIO_OE_SET, 1 << gpio_num)
    else:
        _write_word(peer, SIO_GPIO_OE_CLR, 1 << gpio_num)

    _tracked[gpio_num] = TrackedPin(gpio_num, mode)


def write(gpio_num: int, level: bool, peer: str = debug_probe.PEER_PICO) -> None:
    """Drives gpio_num high or low. Refused for GPIO6 unconditionally, and
    refused for any other pin not already configured OUTPUT via
    :func:`set_mode` in this process."""
    if gpio_num == SAFTYFW_RELAY_GPIO:
        raise PicoGpioProbeRefused(
            "gpio6 (saftyRelay / K4 pilot relay) is never settable through this probe -- read-only"
        )
    tracked = _tracked.get(gpio_num)
    if tracked is None or tracked.mode != MODE_OUTPUT:
        raise PicoGpioProbeRefused(f"gpio{gpio_num} not configured OUTPUT via set_mode() first")
    addr = SIO_GPIO_OUT_SET if level else SIO_GPIO_OUT_CLR
    _write_word(peer, addr, 1 << gpio_num)


def read_all(peer: str = debug_probe.PEER_PICO) -> List[Tuple[int, str, bool]]:
    """Reads every pin this process has configured via :func:`set_mode`.
    Returns a list of (gpio_num, mode, level) tuples, cheapest useful
    "what does this session currently have configured" view -- one SIO read
    regardless of how many pins are tracked."""
    if not _tracked:
        return []
    value = _read_word(peer, SIO_GPIO_IN)
    return [
        (gpio_num, tracked.mode, bool(value & (1 << gpio_num)))
        for gpio_num, tracked in sorted(_tracked.items())
    ]
