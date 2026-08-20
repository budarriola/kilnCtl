"""Pure least-squares fitting for the CT calibration sweep.

No I/O, no fixture, no DUT link -- everything here is plain floats in,
plain dataclasses out, so it is fully testable without hardware
(`tools/PcTools/tests/test_ct_calibration.py`).

Context: `firmware/SimFW/docs/PLAN.md` section 3.3's calibration procedure
sweeps the fixture's *commanded* CT amplitude (today, per
`wave_owner.c`'s `TODO(M-D calibration)` identity placeholder, this is the
same 0..1 number the driver hands the PWM as a duty-scale fraction) and
records what `SaftyFW`'s current-sense channel reports back, in real amps.
This module fits that pair of sequences to a line

    measured_amps = gain * commanded + offset

and, since what the firmware actually needs is the *inverse* -- "given a
target real-amps figure, what commanded value produces it" -- also exposes
that inverse directly off the fit result, so a caller never has to invert
`gain`/`offset` by hand and risk a sign error.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Sequence

#: PLAN.md 3.3 says "sweep ... across ~10 points"; a fit from fewer than
#: this is not what that procedure describes, regardless of how clean the
#: numbers look, so `fit_linear` refuses to run below it rather than
#: silently returning a 2-point "fit".
MIN_SWEEP_POINTS = 4

#: Below this, treat the fit as not credible. 0.98 is deliberately strict:
#: this pairs a digitally-generated sine amplitude with a filtered ADC
#: reading, both clean signals with no reason to scatter -- a real
#: hardware run landing much below this line is telling you something is
#: wrong (bad connection, wrong channel, saturation), not that the
#: relationship is merely noisy.
DEFAULT_MIN_R2 = 0.98

#: A gain at or below this is functionally "the readback does not move
#: when the command does" -- a flat line, a disconnected CT, or a channel
#: stuck at a rail. Rejected regardless of what R² says (a perfectly flat
#: line has R² undefined/degenerate, but this catches the near-flat case
#: R² alone might miss).
MIN_GAIN = 1e-6


class FitError(ValueError):
    """Raised for input that cannot be fit at all (not merely a bad fit --
    see `LinearFit.ok` / `evaluate_fit` for that)."""


@dataclass(frozen=True)
class LinearFit:
    """Result of an ordinary least-squares fit of one channel's sweep.

    `gain`/`offset` are the forward relationship (commanded -> measured);
    `to_command(amps)` gives the inverse a caller actually needs to drive
    the fixture at a target real-amps value.
    """

    gain: float
    offset: float
    r2: float
    n: int
    residuals: "tuple[float, ...]"
    max_abs_residual: float

    def predict(self, commanded: float) -> float:
        """What the linear model predicts the DUT would read for a given
        commanded value."""
        return self.gain * commanded + self.offset

    def to_command(self, target_amps: float, lo: float = 0.0, hi: float = 1.0) -> float:
        """Inverse of the fit, clamped to `[lo, hi]` -- the commanded value
        (today, the fixture's raw 0..1 PWM-scale input) that should make the
        DUT read `target_amps`, per this fit.

        This is exactly what a calibrated `ct_wave_amps_to_pwm_scale()`
        needs to compute per PLAN.md 3.3 (see this package's README for the
        precise firmware change) -- `gain`/`offset` alone are the fit as
        measured; this method is the fit as *used*.
        """
        if self.gain == 0:
            raise FitError("cannot invert a fit with zero gain")
        raw = (target_amps - self.offset) / self.gain
        return min(hi, max(lo, raw))


def fit_linear(commanded: Sequence[float], measured: Sequence[float]) -> LinearFit:
    """Ordinary least-squares fit of `measured = gain*commanded + offset`.

    Raises `FitError` for structurally unusable input (mismatched lengths,
    too few points, zero variance in `commanded` -- a vertical-line fit is
    undefined, not just a bad one). Does NOT reject a *bad* fit (low R²,
    non-positive gain) -- that is `evaluate_fit`'s job, kept separate so a
    caller can always see the numbers even when it means to reject them.
    """
    n = len(commanded)
    if n != len(measured):
        raise FitError(f"commanded/measured length mismatch: {n} vs {len(measured)}")
    if n < MIN_SWEEP_POINTS:
        raise FitError(
            f"need at least {MIN_SWEEP_POINTS} sweep points to fit, got {n} "
            "(PLAN.md 3.3: 'sweep commanded amplitude across ~10 points')"
        )

    mean_x = sum(commanded) / n
    mean_y = sum(measured) / n
    sxx = sum((x - mean_x) ** 2 for x in commanded)
    sxy = sum((x - mean_x) * (y - mean_y) for x, y in zip(commanded, measured))

    if sxx == 0.0:
        raise FitError(
            "commanded values have zero variance -- cannot fit a line "
            "through a single x value (did the sweep actually change amps?)"
        )

    gain = sxy / sxx
    offset = mean_y - gain * mean_x

    residuals = tuple(y - (gain * x + offset) for x, y in zip(commanded, measured))
    ss_res = sum(r * r for r in residuals)
    ss_tot = sum((y - mean_y) ** 2 for y in measured)
    # ss_tot == 0 means every measured value was identical -- a flat
    # readback. That is caught by MIN_GAIN in evaluate_fit; r2 is reported
    # as 0.0 here (not 1.0, which sum-of-squares math would otherwise give
    # for a perfect-looking fit to a constant) so evaluate_fit's R² gate
    # also flags it rather than waving it through as "perfect".
    r2 = 0.0 if ss_tot == 0.0 else max(0.0, 1.0 - ss_res / ss_tot)

    max_abs_residual = max((abs(r) for r in residuals), default=0.0)

    return LinearFit(
        gain=gain, offset=offset, r2=r2, n=n,
        residuals=residuals, max_abs_residual=max_abs_residual,
    )


@dataclass(frozen=True)
class FitVerdict:
    ok: bool
    reasons: "tuple[str, ...]" = ()

    def describe(self) -> str:
        if self.ok:
            return "OK"
        return "REJECTED: " + "; ".join(self.reasons)


def evaluate_fit(
    fit: LinearFit,
    min_r2: float = DEFAULT_MIN_R2,
    min_gain: float = MIN_GAIN,
) -> FitVerdict:
    """Decide whether `fit` is credible enough to store in a calibration
    table. This is the gate that keeps implausible data (flat readback,
    negative/inverted gain, a noisy/bad fit) from silently producing a
    confident-looking calibration -- see this package's README, "Be honest
    about limits".
    """
    reasons: "list[str]" = []

    if fit.gain <= min_gain:
        reasons.append(
            f"gain {fit.gain:.6g} <= {min_gain:.2g} -- readback did not "
            "move with the commanded amplitude (flat/disconnected channel, "
            "or gain went negative/inverted)"
        )
    if not math.isfinite(fit.r2):
        reasons.append("R² is not finite")
    elif fit.r2 < min_r2:
        reasons.append(f"R² {fit.r2:.4f} < required {min_r2:.4f}")

    return FitVerdict(ok=not reasons, reasons=tuple(reasons))
