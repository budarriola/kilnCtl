"""DUT readback providers: "what does the safety processor say each CT
channel is reading right now."

**This is the one part of the whole tool that cannot be exercised against
`virtual_simfw` and cannot be proven against real hardware today either.**
See this package's README ("Readback path" / "Be honest about limits") for
the full account. Short version:

- `KilnctrlSafetyReadback` is believed to be the correct, real path -- it
  wraps `tools/PcTools/src/kilnctrl/safety.py`'s `SafetyClient.get_status()`,
  whose `current_a` field is exactly `SaftyFW`'s `current_task`/ADC readout
  as seen by the ESP over the existing isolated UART link, itself polled by
  the PC over the existing kilnctrl USB link. That is precisely "the
  existing kilnctrl MCP path" DESIGN_NOTES.md 3.3 names. It has been read, not run:
  no RP2040 in this repo answers `SAFETY_CMD_GET_STATUS` with real current
  data yet (`tools/PcTools/scripts/current_sense_commissioning.py`'s module
  docstring says the same thing about the exact same link, for the exact
  same reason).
- `SyntheticDutReadback` is a host-only stand-in used for tests and for
  exercising this tool's procedure/fit/crosstalk logic against
  `virtual_simfw` today. It is explicitly not a hardware model -- it applies
  a made-up per-channel gain/offset/noise/crosstalk to whatever the fixture
  last reported having commanded, so the *arithmetic* (sweep -> fit ->
  reject-if-bad, drive-one -> confirm-one-responds) can be verified now,
  while the *physical* accuracy claim is left entirely to real bench day.
"""

from __future__ import annotations

import random
from dataclasses import dataclass
from typing import Optional, Protocol


class DutReadbackError(RuntimeError):
    """Raised when the DUT's current-sense reading could not be obtained."""


class DutReadback(Protocol):
    """Everything the calibration runner needs from "read the DUT's
    current-sense channels" -- deliberately just this one method, so a
    synthetic stand-in and the real kilnctrl-backed client are
    interchangeable to `calibrate_ct.py`."""

    def read_currents(self) -> "tuple[float, float, float]":
        """Return the three per-channel current readings, amps, in the same
        channel order as `SafetyStatus.current_a` / the fixture's own CT
        channel numbering (0, 1, 2)."""
        ...

    def close(self) -> None:
        ...


class KilnctrlSafetyReadback:
    """Real DUT readback over the existing kilnctrl SAFETY link.

    Thin wrapper -- all it does is call `SafetyClient.get_status().current_a`
    and translate a query failure into `DutReadbackError`. Deliberately does
    not manage the underlying `RemoteUartLink`'s lifetime beyond `close()`;
    callers are expected to obtain the link the same way
    `current_sense_commissioning.py` does (`kilnctrl.link_hub.get_shared_link`)
    so this tool and any other bench script sharing the same physical link
    don't fight over it.
    """

    def __init__(self, safety_client) -> None:
        # `safety_client` is a `kilnctrl.safety.SafetyClient` -- not
        # type-annotated against that class directly so this module (and
        # therefore `fit.py`/`crosstalk.py`/`calibration_table.py`, which
        # import nothing from kilnctrl) stays importable even in a
        # kilnctrl-less environment; only constructing this class needs it.
        self._safety = safety_client

    def read_currents(self) -> "tuple[float, float, float]":
        try:
            status = self._safety.get_status()
        except Exception as exc:  # noqa: BLE001 - re-raised as our own type
            raise DutReadbackError(f"SAFETY GET_STATUS failed: {exc}") from exc
        return status.current_a

    def close(self) -> None:
        self._safety.close()


@dataclass
class SyntheticChannelModel:
    """A made-up linear-with-noise DUT channel, for `SyntheticDutReadback`.

    `commanded -> gain*commanded + offset + noise`, clamped at 0 (a real
    current-sense channel per CURRENT_SENSE.md Sec.5 clamps at 0 too:
    "the result at 0" is the documented convention). `leak_into` optionally
    bleeds a fraction of this channel's signal into another channel's
    reading, purely so the crosstalk-rejection test path has something
    real to reject.
    """

    gain: float
    offset: float
    noise_sigma: float = 0.0
    leak_into: "Optional[dict[int, float]]" = None


class SyntheticDutReadback:
    """Host-only stand-in DUT for testing this tool's procedure against
    `virtual_simfw` without real current-sense hardware.

    Takes a per-channel "what was last commanded" value from `commanded`
    (a plain `{channel: float}` dict the caller mutates directly, e.g.
    `calibrate_ct.py` updates it right after each real `fixture.set_amps()`
    call) and runs it through a per-channel synthetic model.

    This deliberately does NOT re-query the fixture's own `CT GET_STATE`
    for the "last commanded" value, even though that would seem to close
    the loop more realistically -- an earlier version of this class did
    exactly that, and it introduced a real race: `GET_STATE` and the
    preceding `SET_AMPS` are two independent request/reply round trips
    over the same link, and reading back before the fixture's own
    zero-crossing-gated apply had landed produced a spurious one-step-stale
    "measured" value with no analog cause at all -- purely an artifact of
    this test harness, not of the fixture or the DUT. A real DUT's
    analog reading never depends on interrogating the fixture's protocol
    state in the first place, so removing that round trip is also the more
    faithful design, not just the more convenient one.
    """

    def __init__(
        self,
        channel_models: "dict[int, SyntheticChannelModel]",
        commanded: "dict[int, float]",
        n_channels: int = 3,
        seed: int = 0,
    ) -> None:
        self._models = channel_models
        self._commanded = commanded
        self._n = n_channels
        self._rng = random.Random(seed)

    def read_currents(self) -> "tuple[float, float, float]":
        commanded = [self._commanded.get(c, 0.0) for c in range(self._n)]
        out = [0.0] * self._n
        for ch in range(self._n):
            model = self._models.get(ch)
            if model is None:
                continue
            value = model.gain * commanded[ch] + model.offset
            if model.noise_sigma:
                value += self._rng.gauss(0.0, model.noise_sigma)
            out[ch] += value
        # Apply leakage as a second pass so it reads from the *clean*
        # per-channel values above, not a partially-summed running total.
        for ch in range(self._n):
            model = self._models.get(ch)
            if not model or not model.leak_into:
                continue
            base = model.gain * commanded[ch] + model.offset
            for target, frac in model.leak_into.items():
                if 0 <= target < self._n:
                    out[target] += base * frac
        return tuple(max(0.0, v) for v in out)  # type: ignore[return-value]

    def close(self) -> None:
        pass
