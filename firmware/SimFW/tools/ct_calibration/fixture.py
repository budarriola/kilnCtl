"""Thin wrapper over `kilnsim`'s CT command group -- the fixture side of the
calibration loop.

Deliberately not a new abstraction: this just calls
`kilnsim.link.SimLink.send_command(CommandGroup.CT, ...)` with the real
payload shapes from `tools/PcTools/src/kilnsim/payloads.py` (`ct amps`,
`ct mode`, `ct state`, per that module's own CLI help text), so anything
this file does is exactly what `kilnsim ct ...` on the command line would
do. Kept separate from `calibrate_ct.py` only so the sweep/crosstalk loop
in that file reads as bench procedure, not protocol plumbing.
"""

from __future__ import annotations

import sys
from pathlib import Path

# tools/PcTools/src is not on sys.path by default outside that project's own
# entry points -- add it once, matching the pattern
# tools/PcTools/scripts/current_sense_commissioning.py already uses for the
# analogous kilnctrl import.
_PCTOOLS_SRC = Path(__file__).resolve().parents[4] / "tools" / "PcTools" / "src"
if str(_PCTOOLS_SRC) not in sys.path:
    sys.path.insert(0, str(_PCTOOLS_SRC))

from kilnsim.link import SimLink  # noqa: E402
from kilnsim.protocol import CommandGroup, CtCmd  # noqa: E402


def set_mode_manual(link: SimLink, channel: int) -> None:
    """MANUAL mode: the channel's amplitude is exactly whatever the last
    `set_amps` call set it to, not derived from the thermal model. The
    calibration sweep only makes sense in this mode."""
    link.send_command(CommandGroup.CT, CtCmd.SET_MODE, {"channel": channel, "mode": "manual"})


def set_amps(link: SimLink, channel: int, amps: float) -> None:
    link.send_command(CommandGroup.CT, CtCmd.SET_AMPS, {"channel": channel, "amps": amps})


def get_state(link: SimLink, channel: int) -> dict:
    """Returns the fixture's own decoded `CT GET_STATE` reply -- `mode`,
    `amps` (last commanded), `last_pwm_scale` (what the driver actually
    handed the PWM peripheral, i.e. `ct_wave_amps_to_pwm_scale()`'s current
    -- today identity -- output), `distortion`, `valid`.

    This is the fixture's *own* view of what it commanded, never the DUT's
    reading -- see `readback.py` for that half.
    """
    return link.send_command(CommandGroup.CT, CtCmd.GET_STATE, {"channel": channel})
