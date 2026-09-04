"""Independent re-derivation of relay-feedback Ku/Tu from a raw capture, plus
a describing-function reconciliation against a FOPDT (step-test) model.

BACKGROUND. A completed relay-feedback autotune reports ``relay_ku``/
``relay_tu_s`` fitted on-device by ``pid_autotune_fit_relay()``
(firmware/KilnFW/App/drivers/pid_autotune.c). Those numbers can look
surprising against a FOPDT model identified separately by a step test (see
firmware/KilnFW/docs/PID_EXPANSION_PLAN.md section 2/3.6): naive
classical relay-feedback theory says the oscillation period Tu should sit
near ``4*L`` (dead time) and the phase crossover should be at exactly
-180 degrees, ignoring the relay's own hysteresis band. Both of those
shortcuts are wrong for a relay WITH hysteresis, and this module exists so
that check is done once, in code, against the raw trace, rather than by
eye against firmware-reported summary numbers.

THIS MODULE DOES TWO INDEPENDENT THINGS:

1. ``measure_relay_cycles()`` walks the raw capture (autotune_get_status +
   thermo_read poll pairs, see ``load_relay_capture()``) and re-implements
   the SAME crossing-detector ``pid_autotune_fit_relay()`` uses (tail-half
   midline, 10% deadband, upward-crossing bounded cycles) directly on the
   temperature trace, completely independent of the firmware's own fitted
   ``relay_ku``/``relay_tu_s`` numbers. This is a check that the firmware's
   period/amplitude measurement is not itself an artifact -- if this
   function's answer does not match the firmware's reported numbers to
   within measurement noise, that is itself evidence of a firmware bug.

2. ``predict_relay_oscillation()`` solves the two describing-function
   conditions (magnitude AND phase, the phase one including the relay's
   own hysteresis lag ``-arcsin(h/a)``, not just the plant's) for a given
   FOPDT model (K, tau, L) and relay parameters (d, h), to get the Tu/Ku a
   FOPDT model actually predicts. This is more work than the classical
   "period is near 4L" shortcut but it is the correct describing-function
   answer for a relay with hysteresis, and is the one to compare fitted
   Ku/Tu against.

CAPTURE FORMAT. Two independent 20 s pollers, JSON-lines, ``{"t": <unix
float>, "s": <text>}`` per line:

  * ``<name>_status.jsonl`` -- ``autotune_get_status`` reply text, e.g.
    ``state=relay_cycling method=1 zone=0 elapsed=63s samples=0
    actual=42.51C(valid) duty=0.00 model_valid=False model_settled=False
    proposed_gains=AutotuneGains(kp=0.0, ki=0.0, kd=0.0, rule=0)
    relay_valid=False``

  * ``<name>_thermo.jsonl`` -- ``thermo_read`` reply text, e.g.
    ``CH0: 26.70 C (CJ 27.11 C)\\nCH1: 26.74 C (CJ 27.38 C)\\nCH2: 26.70 C
    (CJ 27.44 C)``

The two pollers are NOT sample-aligned (same convention as
``coupling_pair_log.py``), so this module does not attempt to join them
row-for-row: ``measure_relay_cycles`` builds its own temperature trace
straight from the thermo file, restricted to the wall-clock span the
status file's ``state=relay_cycling`` rows cover (plus a small pad so a
crossing right at the boundary is not clipped), which is the same "cycling
window" the firmware itself fits over.
"""
from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass
from typing import Optional, Sequence

from .jsonl_util import iter_jsonl

# ---------------------------------------------------------------------------
# Capture loading
# ---------------------------------------------------------------------------

_STATE_RE = re.compile(r"state=(\S+)")
_DUTY_RE = re.compile(r"duty=([\-\d.]+)")
_CH_RE = re.compile(r"CH(\d+):\s*([\-\d.]+)\s*C")


@dataclass(frozen=True)
class StatusRow:
    t: float
    state: str
    duty: float


@dataclass(frozen=True)
class ThermoRow:
    t: float
    channels: dict[int, float]


@dataclass(frozen=True)
class RelayCapture:
    status: list[StatusRow]
    thermo: list[ThermoRow]


def _read_jsonl(path: str) -> list[tuple[float, str]]:
    out: list[tuple[float, str]] = []
    # on_error="raise": this call site never caught json.loads() before --
    # keep letting a malformed line raise rather than silently swallowing it.
    for d in iter_jsonl(path, on_error="raise"):
        out.append((float(d["t"]), str(d["s"])))
    return out


def parse_status_line(text: str) -> Optional[tuple[str, float]]:
    """Returns ``(state, duty)`` from one ``autotune_get_status`` line, or
    None if the line does not look like autotune-status text at all (so a
    cross-wired file, e.g. a thermo line fed to this parser, is rejected
    rather than silently misread).
    """
    m_state = _STATE_RE.search(text)
    if m_state is None:
        return None
    m_duty = _DUTY_RE.search(text)
    duty = float(m_duty.group(1)) if m_duty else float("nan")
    return m_state.group(1), duty


def parse_thermo_line(text: str) -> dict[int, float]:
    """Returns ``{channel: degC}`` parsed out of a ``thermo_read`` reply.
    Channels missing from the text (or the whole line being garbage) just
    yield an empty/partial dict -- callers index by channel and tolerate
    absence.
    """
    return {int(ch): float(val) for ch, val in _CH_RE.findall(text)}


def load_relay_capture(status_path: str, thermo_path: str) -> RelayCapture:
    status = [
        StatusRow(t=t, state=parsed[0], duty=parsed[1])
        for t, s in _read_jsonl(status_path)
        for parsed in [parse_status_line(s)]
        if parsed is not None
    ]
    thermo = [ThermoRow(t=t, channels=parse_thermo_line(s)) for t, s in _read_jsonl(thermo_path)]
    return RelayCapture(status=status, thermo=thermo)


# ---------------------------------------------------------------------------
# 1. Raw-trace crossing detector -- mirrors pid_autotune_fit_relay()
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class RelayCycle:
    period_s: float
    peak_to_peak_c: float

    @property
    def half_amplitude_c(self) -> float:
        return 0.5 * self.peak_to_peak_c


@dataclass(frozen=True)
class RelayMeasurement:
    channel: int
    cycling_window_s: float
    midline_c: float
    deadband_c: float
    upward_crossings_s: list[float]
    cycles: list[RelayCycle]

    @property
    def period_mean_s(self) -> float:
        return _mean(c.period_s for c in self.cycles)

    @property
    def period_stdev_s(self) -> float:
        return _pstdev(c.period_s for c in self.cycles)

    @property
    def peak_to_peak_mean_c(self) -> float:
        return _mean(c.peak_to_peak_c for c in self.cycles)

    @property
    def half_amplitude_mean_c(self) -> float:
        return 0.5 * self.peak_to_peak_mean_c


def _mean(xs) -> float:
    xs = list(xs)
    return sum(xs) / len(xs) if xs else float("nan")


def _pstdev(xs) -> float:
    xs = list(xs)
    if len(xs) < 2:
        return 0.0
    m = _mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / len(xs))


def _interp_crossing(t0: float, v0: float, t1: float, v1: float, level: float) -> float:
    if v1 == v0:
        return t1
    frac = (level - v0) / (v1 - v0)
    return t0 + frac * (t1 - t0)


def measure_relay_cycles(capture: RelayCapture, channel: int, pad_s: float = 25.0) -> RelayMeasurement:
    """Re-derives period and peak-to-peak amplitude per relay cycle directly
    from the raw thermo trace, using the SAME algorithm
    ``pid_autotune_fit_relay()`` uses on-device (tail-half min/max midline,
    10% deadband hysteretic upward-crossing detector, N crossings bound
    N-1 complete cycles). Deliberately does not read the firmware's own
    fitted ``relay_ku``/``relay_tu_s`` -- this is the independent check.
    """
    cycling = [r for r in capture.status if r.state == "relay_cycling"]
    if len(cycling) < 2:
        raise ValueError("no relay_cycling window found in status capture")
    t_start = cycling[0].t
    t_end = cycling[-1].t

    trace = sorted(
        (r.t - t_start, r.channels[channel])
        for r in capture.thermo
        if channel in r.channels and t_start - pad_s <= r.t <= t_end + pad_s
    )
    if len(trace) < 8:
        raise ValueError("not enough thermo samples in the cycling window")
    ts = [t for t, _ in trace]
    vs = [v for _, v in trace]

    n = len(vs)
    tail = vs[n // 2 :]
    midline = 0.5 * (max(tail) + min(tail))
    tail_pp = max(tail) - min(tail)
    if tail_pp < 1.0:
        raise ValueError("trace never oscillated (flat within noise), matching firmware's own guard")
    deadband = 0.10 * tail_pp

    armed = vs[0] < (midline - deadband)
    ups: list[float] = []
    for i in range(1, n):
        v = vs[i]
        if not armed:
            if v < midline - deadband:
                armed = True
            continue
        if v >= midline:
            ups.append(_interp_crossing(ts[i - 1], vs[i - 1], ts[i], vs[i], midline))
            armed = False

    cycles: list[RelayCycle] = []
    for i in range(len(ups) - 1):
        t0, t1 = ups[i], ups[i + 1]
        seg = [v for t, v in zip(ts, vs) if t0 <= t <= t1]
        if not seg:
            continue
        cycles.append(RelayCycle(period_s=t1 - t0, peak_to_peak_c=max(seg) - min(seg)))

    return RelayMeasurement(
        channel=channel,
        cycling_window_s=t_end - t_start,
        midline_c=midline,
        deadband_c=deadband,
        upward_crossings_s=ups,
        cycles=cycles,
    )


# ---------------------------------------------------------------------------
# 2. Firmware's Ku formula, and the correct describing-function prediction
#    for a FOPDT model under a relay WITH hysteresis
# ---------------------------------------------------------------------------


def relay_ku(d: float, a_half_c: float, h_c: float) -> float:
    """``pid_autotune_fit_relay()``'s exact formula: Ku = 4d / (pi *
    sqrt(a^2 - h^2)), where ``a`` is the HALF-amplitude of the oscillation
    (not peak-to-peak -- the firmware halves peak-to-peak into ``a`` before
    this point, see the "Here is the peak-to-peak -> amplitude halving"
    comment in pid_autotune.c) and ``h`` is the hysteresis half-width.
    Raises ValueError under the same conditions the firmware refuses the
    fit (a <= h), rather than returning a NaN/complex result.
    """
    if a_half_c <= h_c * 1.01:
        raise ValueError("oscillation amplitude <= hysteresis band -- identification meaningless")
    denom = math.sqrt(a_half_c * a_half_c - h_c * h_c)
    return 4.0 * d / (math.pi * denom)


@dataclass(frozen=True)
class FopdtModel:
    k_dc: float  # degC per unit duty
    tau_s: float
    dead_time_s: float


@dataclass(frozen=True)
class RelayPrediction:
    omega_rad_s: float
    tu_s: float
    half_amplitude_c: float
    ku: float


def predict_relay_oscillation(
    model: FopdtModel, relay_d: float, hysteresis_c: float,
    w_lo: float = 1e-4, w_hi: float = 0.5, tol: float = 1e-10, max_iter: int = 200,
) -> RelayPrediction:
    """Solves the two simultaneous describing-function conditions for a
    relay WITH hysteresis driving a FOPDT plant:

      magnitude:  |G(jw)| * |N(a)| = 1
      phase:      angle(G(jw)) + angle(N(a)) = -180 deg

    where G(jw) = K * exp(-jwL) / (1 + jw*tau) and the relay-with-hysteresis
    describing function N(a) = (4d/(pi*a)) * (sqrt(1-(h/a)^2) - j*(h/a)) has
    magnitude 4d/(pi*sqrt(a^2-h^2)) (the firmware's own Ku formula) and
    phase -arcsin(h/a).

    THIS IS THE CORRECT FORM. The classical shortcut "period is near 4*L,
    phase crossover is exactly -180 degrees" implicitly assumes h=0 (an
    ideal relay with no hysteresis) -- for h>0 the relay itself contributes
    a phase lag of -arcsin(h/a), so the PLANT's phase at the true
    oscillation frequency is only -180+arcsin(h/a), not -180. Comparing a
    measured Tu against the h=0 shortcut therefore always makes a
    hysteresis-band relay look like it is oscillating "too slowly" even
    when the FOPDT model is a perfect fit; this function removes that
    artifact by solving the real condition.

    Returns the predicted angular frequency, period, oscillation
    half-amplitude, and Ku at that operating point. Bisects on w over
    (w_lo, w_hi) for the first sign change of the phase-residual function,
    which is the physically relevant (lowest-frequency, highest-amplitude)
    solution for a stable FOPDT plant.
    """
    K, tau, L = model.k_dc, model.tau_s, model.dead_time_s
    d, h = relay_d, hysteresis_c

    def phase_residual_and_a(w: float) -> tuple[float, float]:
        mag_g = K / math.sqrt(1.0 + (w * tau) ** 2)
        # magnitude condition solved for sqrt(a^2-h^2), then for a:
        rhs = 4.0 * d * mag_g / math.pi
        a = math.sqrt(rhs * rhs + h * h)
        phase_g = math.atan(w * tau) + w * L  # magnitude of the (negative) phase
        phase_n = math.asin(h / a) if a > 0 else 0.0
        # angle(G)+angle(N) = -pi  <=>  phase_g + phase_n = pi
        return (phase_g + phase_n - math.pi), a

    # Scan for a sign change (coarse), then bisect.
    n_scan = 2000
    prev_w = w_lo
    prev_r, _ = phase_residual_and_a(prev_w)
    bracket = None
    for i in range(1, n_scan + 1):
        w = w_lo + (w_hi - w_lo) * i / n_scan
        r, _ = phase_residual_and_a(w)
        if prev_r == 0.0:
            bracket = (prev_w, prev_w)
            break
        if (prev_r < 0) != (r < 0):
            bracket = (prev_w, w)
            break
        prev_w, prev_r = w, r
    if bracket is None:
        raise ValueError("no sustained-oscillation solution found in the scanned frequency range")

    lo, hi = bracket
    r_lo, _ = phase_residual_and_a(lo)
    for _ in range(max_iter):
        mid = 0.5 * (lo + hi)
        r_mid, a_mid = phase_residual_and_a(mid)
        if abs(r_mid) < tol or (hi - lo) < 1e-12:
            lo = hi = mid
            break
        if (r_lo < 0) == (r_mid < 0):
            lo, r_lo = mid, r_mid
        else:
            hi = mid
    w_star = 0.5 * (lo + hi)
    _, a_star = phase_residual_and_a(w_star)
    ku_star = relay_ku(d, a_star, h)
    return RelayPrediction(
        omega_rad_s=w_star,
        tu_s=2.0 * math.pi / w_star,
        half_amplitude_c=a_star,
        ku=ku_star,
    )


# ---------------------------------------------------------------------------
# Tyreus-Luyben, mirroring pid_autotune_tune_from_relay()'s AUTOTUNE_RULE_
# TYREUS_LUYBEN branch exactly (kc = ku/3.2, ti = 2.2*tu, td = tu/6.3, then
# parallel-form ki = kc/ti, kd = kc*td).
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Gains:
    kp: float
    ki: float
    kd: float


def tyreus_luyben_gains(ku: float, tu_s: float) -> Gains:
    kc = ku / 3.2
    ti = 2.2 * tu_s
    td = tu_s / 6.3
    return Gains(kp=kc, ki=(kc / ti if ti > 0 else 0.0), kd=kc * td)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _cmd_check(argv: Sequence[str]) -> int:
    import argparse

    p = argparse.ArgumentParser(prog="relay_ku_tu_check check")
    p.add_argument("status_jsonl")
    p.add_argument("thermo_jsonl")
    p.add_argument("--channel", type=int, default=0)
    p.add_argument("--relay-d", type=float, required=True)
    p.add_argument("--hysteresis-c", type=float, required=True)
    p.add_argument("--k-dc", type=float, help="FOPDT gain, degC/duty, for the prediction step")
    p.add_argument("--tau-s", type=float)
    p.add_argument("--dead-time-s", type=float)
    args = p.parse_args(argv)

    cap = load_relay_capture(args.status_jsonl, args.thermo_jsonl)
    meas = measure_relay_cycles(cap, args.channel)
    print(f"cycling window: {meas.cycling_window_s:.1f} s")
    print(f"midline: {meas.midline_c:.3f} C  deadband: {meas.deadband_c:.3f} C")
    for i, c in enumerate(meas.cycles):
        print(f"  cycle {i}: period={c.period_s:.1f}s  pp={c.peak_to_peak_c:.3f}C  half-amp={c.half_amplitude_c:.3f}C")
    print(f"Tu mean/stdev: {meas.period_mean_s:.2f} / {meas.period_stdev_s:.2f} s "
          f"({100*meas.period_stdev_s/meas.period_mean_s:.1f}% CV)")
    print(f"peak-to-peak mean: {meas.peak_to_peak_mean_c:.3f} C  (half-amplitude {meas.half_amplitude_mean_c:.3f} C)")
    ku = relay_ku(args.relay_d, meas.half_amplitude_mean_c, args.hysteresis_c)
    print(f"Ku (firmware formula, from raw trace): {ku:.5f}")
    gains = tyreus_luyben_gains(ku, meas.period_mean_s)
    print(f"Tyreus-Luyben from raw-trace Ku/Tu: kp={gains.kp:.5f} ki={gains.ki:.7f} kd={gains.kd:.5f}")

    if args.k_dc is not None and args.tau_s is not None and args.dead_time_s is not None:
        model = FopdtModel(k_dc=args.k_dc, tau_s=args.tau_s, dead_time_s=args.dead_time_s)
        pred = predict_relay_oscillation(model, args.relay_d, args.hysteresis_c)
        print(f"FOPDT-predicted (hysteresis-correct DF): Tu={pred.tu_s:.2f}s  "
              f"a={pred.half_amplitude_c:.3f}C  Ku={pred.ku:.5f}")
        print(f"  ratio predicted/measured: Tu={pred.tu_s/meas.period_mean_s:.3f}  Ku={pred.ku/ku:.3f}")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    import sys

    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] != "check":
        print("usage: python -m kilnctrl.relay_ku_tu_check check <status.jsonl> <thermo.jsonl> "
              "--relay-d D --hysteresis-c H [--k-dc K --tau-s TAU --dead-time-s L]")
        return 2
    return _cmd_check(argv[1:])


if __name__ == "__main__":
    raise SystemExit(main())
