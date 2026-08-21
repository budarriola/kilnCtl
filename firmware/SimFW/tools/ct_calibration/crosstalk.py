"""Crosstalk / channel-mapping check: DESIGN_NOTES.md 3.3's "[the calibration
procedure] doubles as the `CURRENT_SENSE.md` §5 commissioning check (one
relay commanded -> exactly one channel responds)."

The §5 procedure drives that check by commanding one *relay*; on the SimFW
bench this calibration tool doesn't have a relay in the loop at all -- it
drives the fixture's CT channel directly (`kilnsim ct amps`). The logic is
identical either way: command exactly one channel to a level well above the
noise floor, everything else at zero, and confirm the DUT reports exactly
one channel above threshold, and that it is the right one. A swapped CT
jack, a shared/leaky transformer coupling, or a mis-wired ADC mux all show
up here as either zero, more than one, or the wrong channel responding --
and per CURRENT_SENSE.md §5, any of those must block the calibration from
being trusted, the same way it blocks S3/S4 from being enabled.

Pure decision logic, no I/O -- see `tools/PcTools/tests/test_ct_calibration.py`.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Sequence


@dataclass(frozen=True)
class CrosstalkResult:
    driven_channel: int
    responded: "tuple[int, ...]"
    passed: bool
    message: str


def check_crosstalk(
    driven_channel: int,
    readback_a: Sequence[float],
    threshold_a: float,
) -> CrosstalkResult:
    """`readback_a` is the DUT's per-channel current reading (same index
    order as `SafetyStatus.current_a`: channel 0/1/2) while `driven_channel`
    is the only CT channel commanded to a nonzero test amplitude, all
    others held at zero. Exactly one channel must read above `threshold_a`,
    and it must be `driven_channel`.
    """
    responded = tuple(i for i, a in enumerate(readback_a) if a > threshold_a)

    if len(responded) == 0:
        return CrosstalkResult(
            driven_channel, responded, False,
            f"no channel responded to channel {driven_channel} being driven "
            f"(all readings <= {threshold_a:.3f} A) -- check the fixture "
            "actually commanded a nonzero amplitude and the transformer/RC "
            "path into that CT jack is connected",
        )
    if len(responded) > 1:
        return CrosstalkResult(
            driven_channel, responded, False,
            f"channels {list(responded)} all responded to channel "
            f"{driven_channel} alone being driven -- crosstalk between CT "
            "channels, a shared conductor, or leakage between transformer "
            "windings",
        )
    got = responded[0]
    if got != driven_channel:
        return CrosstalkResult(
            driven_channel, responded, False,
            f"channel {got} responded when channel {driven_channel} was "
            "driven -- a swapped CT jack or a mis-mapped ADC channel "
            "(CURRENT_SENSE.md Sec.5: 'this is the only way to discover a "
            "CT plugged into the wrong jack')",
        )
    return CrosstalkResult(
        driven_channel, responded, True,
        f"channel {got} responded as expected, no crosstalk",
    )


def all_channels_clean(results: Sequence[CrosstalkResult]) -> bool:
    """Whether every channel's crosstalk check passed -- the gate this
    tool applies before it will fit or store *any* channel's calibration.
    Mirrors CURRENT_SENSE.md §5's "until it passes on all three channels,
    S3 and S4 must be left disabled": a calibration built on top of an
    unproven channel mapping is worse than no calibration.
    """
    return len(results) > 0 and all(r.passed for r in results)
